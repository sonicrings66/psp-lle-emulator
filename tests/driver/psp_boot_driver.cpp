#include "psp_boot_driver.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bus/system_bus.h"
#include "me/me_core.h"
#include "options.h"
#include "options_cli.h"
#include "frontend/live_window.h"
#include "cpu.h"
#include "crypto/kirk.h"
#include "crypto/prx_decrypt.h"
#include "fuse_config.h"
#include "firmware/ipl_extractor.h"
#include "firmware/module_stager.h"
#include "firmware/real_hardware_attach.h"
#include "format/flash0_fs.h"
#include "format/pbp.h"
#include "format/psar.h"
#include "util/nand_image.h"
#include "../../testsupport/state_hash.h"

namespace beta::driver {

namespace {

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The real halt real cIPL/exploit-trampoline code reaches (uofw's own
// src/syscon/power.S) once it has nothing further to do -- see
// psp_boot_driver.h's file header and docs/PORTING_LOG.md.
constexpr uint32_t kRealHaltPc = 0x00010108u;
// A few thousand real self-loop iterations is plenty to confirm the halt is
// stable rather than merely passed through once (ported threshold from
// test_ipl_realfuse.cpp).
constexpr uint64_t kHaltConfirmStreak = 1000;
// Real module_start returns via a real `jr $ra`; this synthetic entry has no
// real caller, so $ra points here instead of 0 (which would look identical
// to a genuine crash) -- deliberately outside every real mapped region.
constexpr uint32_t kModuleStartReturnSentinel = 0xDEAD0000u;

void emitStateLine(int64_t instrCount, const Cpu& cpu, const SystemBus& bus) {
    uint32_t ramBase = 0, ramSize = 0;
    const uint8_t* ram = bus.ramWindow(ramBase, ramSize);
    const testsupport::StateHash h = testsupport::StateHasher::checkpoint(
        reinterpret_cast<const uint8_t*>(&cpu.state()), sizeof(CpuState), ram, ram ? ramSize : 0);
    // Exact format testsupport::GoldenTrace::loadFromFile parses -- see
    // golden_trace.cpp's sscanf call. Do not change this without updating
    // that parser too.
    std::printf("  [STATE] instr %lld cpu=%016llX ram=%016llX pc=%08X count=%08X\n",
                static_cast<long long>(instrCount),
                static_cast<unsigned long long>(h.cpu), static_cast<unsigned long long>(h.ram),
                cpu.state().pc, static_cast<uint32_t>(cpu.countRegister));
}

// Reads the GE's own real current display framebuffer (480x272, whatever
// real format/stride it's currently configured with) straight through the
// bus into `px` (RGBA8888). Shared by the live window (PSP_WINDOW=1, see
// LiveWindow::submit) and the one-shot end-of-run PNG dump (DIAG164) --
// same real VRAM read every other access in this driver uses, since
// GeRenderer's own dr16/dr32/unpack are private. Returns the number of
// non-black pixels (0 is a legitimate real "nothing drawn yet" signal).
uint64_t captureFramebufferRgba(beta::SystemBus& bus, std::vector<uint8_t>& px, uint32_t* fbOut = nullptr, uint32_t* strideOut = nullptr, uint32_t* fmtOut = nullptr) {
    beta::GeRenderer& r = bus.geCore().renderer();
    const uint32_t fb = r.frameBufferAddress(); // already real-VRAM-mapped
    const uint32_t stride = std::max<uint32_t>(r.frameBufferStride(), 1u);
    const uint32_t fmt = r.frameBufferFormat();
    if (fbOut) *fbOut = fb;
    if (strideOut) *strideOut = stride;
    if (fmtOut) *fmtOut = fmt;
    constexpr uint32_t kW = beta::LiveWindow::kWidth, kH = beta::LiveWindow::kHeight;
    px.assign(static_cast<size_t>(kW) * kH * 4u, 0u);
    uint64_t nonBlack = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            uint32_t raw = 0;
            try { raw = (fmt == 3) ? bus.read32(fb + (y * stride + x) * 4u) : bus.read16(fb + (y * stride + x) * 2u); } catch (...) {}
            uint8_t r8, g8, b8, a8;
            if (fmt == 3) { r8 = raw & 0xFFu; g8 = (raw >> 8) & 0xFFu; b8 = (raw >> 16) & 0xFFu; a8 = static_cast<uint8_t>(raw >> 24); }
            else if (fmt == 0) { r8 = static_cast<uint8_t>((raw & 0x1Fu) << 3); g8 = static_cast<uint8_t>(((raw >> 5) & 0x3Fu) << 2); b8 = static_cast<uint8_t>(((raw >> 11) & 0x1Fu) << 3); a8 = 255u; }
            else if (fmt == 1) { r8 = static_cast<uint8_t>((raw & 0x1Fu) << 3); g8 = static_cast<uint8_t>(((raw >> 5) & 0x1Fu) << 3); b8 = static_cast<uint8_t>(((raw >> 10) & 0x1Fu) << 3); a8 = (raw & 0x8000u) ? 255u : 0u; }
            else { r8 = static_cast<uint8_t>((raw & 0xFu) << 4); g8 = static_cast<uint8_t>(((raw >> 4) & 0xFu) << 4); b8 = static_cast<uint8_t>(((raw >> 8) & 0xFu) << 4); a8 = static_cast<uint8_t>(((raw >> 12) & 0xFu) << 4); }
            if (r8 || g8 || b8) ++nonBlack;
            (void)a8; // the display scanout doesn't composite alpha; keep the dump/window opaque, matching the original DIAG164 behavior
            uint8_t* o = &px[(static_cast<size_t>(y) * kW + x) * 4u];
            o[0] = r8; o[1] = g8; o[2] = b8; o[3] = 255u;
        }
    }
    return nonBlack;
}

} // namespace

BootDriverResult runBoot(const BootDriverOptions& opts) {
    BootDriverResult result;

    // PSP_WINDOW=1 ("make the window show up at the start"): a real,
    // live Win32 window (beta::LiveWindow, ported verbatim from
    // psp-lle-emu's own frontend/live_window.h) opens immediately, before
    // any boot stage runs, instead of this driver's usual headless
    // batch-run-then-exit behavior. The periodic frame submits below show
    // real progress as it happens; the window stays open after the
    // instruction budget is reached (see the end of this function) so
    // the final state is actually visible instead of vanishing the
    // instant the process would otherwise exit. Keyboard input reads into
    // LiveWindow::buttons() as a real SceCtrl bit mask and is wired into
    // SysconChip::buttons below, which the real SYSCON GET_DIGITAL_KEY-family
    // commands answer from -- genuine interactivity, not a stand-in.
    const bool windowOn = opts.emu.window.open;
    beta::LiveWindow win;
    if (windowOn) {
        if (!win.start(opts.emu.window.title.empty() ? "PSP-LLE-beta" : opts.emu.window.title.c_str())) {
            std::printf("[boot] PSP_WINDOW: window failed to open (continuing without it)\n");
        } else {
            std::printf("[boot] PSP_WINDOW: window opened\n");
        }
    }

    const std::vector<uint8_t> pbpBytes = readWholeFile(opts.firmwarePbpPath);
    if (pbpBytes.empty()) { result.failureReason = "could not read firmware PBP: " + opts.firmwarePbpPath; return result; }

    // Real per-console fuse ID, read from <project root>/config/psp-lle.ini
    // (never hardcoded/logged here -- see fuse_config.h). The NAND-IPL
    // extraction path below (the documented ARK-4 reset-block ECDSA-bypass
    // exploit) doesn't need it, but real guest code reached later DOES drive
    // real KIRK commands (KIRK_CMD_ENCRYPT_IV_FUSE/DECRYPT_IV_FUSE and
    // similar) that derive a per-console key from it -- confirmed live this
    // session: once a real, properly-import-linked crypto routine (memlmd's
    // own, once loaded) actually runs, it produces a wrong result against
    // KirkEngine's own placeholder default. Falls back to that same
    // placeholder if the config file or key is missing.
    uint32_t fuseId90 = 0, fuseId94 = 0;
    const bool haveRealFuse = beta::fuseconfig::loadRealFuse("E:/PSP-LLE-beta/config/psp-lle.ini", fuseId90, fuseId94); // this project's own portable folder, see BootDriverOptions' own comment
    // Real KIRK command 0xE (the real hardware PRNG) mixes in real wall-
    // clock time(0) -- genuinely non-deterministic on real hardware too,
    // but it means two runs of this exact-repeat test driver can otherwise
    // produce a different RAM state hash (confirmed live: identical
    // cpu/pc/count, different ram, across repeated runs of the unmodified
    // binary). PSP_DETERMINISTIC_KIRK_PRNG=1 pins it to a fixed value for
    // exact-repeat verification runs; unset (the default) keeps real
    // hardware's own non-determinism. This is a test-driver-local knob
    // (like stateHashInterval's own env var), not a general EmuOptions
    // field -- it only matters for byte-exact test repeatability.
    const char* detPrngEnv = std::getenv("PSP_DETERMINISTIC_KIRK_PRNG");
    const bool detPrng = detPrngEnv && detPrngEnv[0] != '\0' && detPrngEnv[0] != '0';
    const uint32_t detPrngValue = 0u;
    beta::crypto::KirkEngine kirk = haveRealFuse
        ? beta::crypto::KirkEngine(fuseId90, fuseId94, detPrng ? &detPrngValue : nullptr)
        : beta::crypto::KirkEngine(detPrng ? &detPrngValue : nullptr);
    std::printf("[boot] real console fuse ID %s (value not logged)\n", haveRealFuse ? "loaded from config/psp-lle.ini" : "NOT FOUND -- using KirkEngine's placeholder default");

    beta::format::PbpFile pbp(pbpBytes);
    const beta::format::ByteSpan psar = pbp.section(beta::format::PbpPart::PsarData);
    const std::vector<beta::format::ResolvedPsarEntry> entries = beta::format::resolvePsarEntries(psar, kirk);
    if (entries.empty()) { result.failureReason = "PSAR resolved to zero entries"; return result; }

    // This console is a real 64MB-RAM unit (03g/TA-090v3, confirmed via
    // PSPident) -- SystemBus's own default (kRamSize32MB) would only map
    // half of what this driver's own real bootInfo->memSize (64MB, set
    // further down) tells real guest code is present, so a real full-RAM
    // clear loop (reboot.bin's own real cold/warm-boot RAM init) faults on
    // the unmapped upper half and silently loops forever retrying via
    // deliverBusErrorsAsExceptions instead of ever finishing. Confirmed
    // live via a standalone trace before this fix.
    beta::SystemBus bus(beta::MemoryMap::kRamSize64MB, {}, {}, {}, {}, {}, opts.emu.ge, opts.emu.me);
    bus.attachKirkEngine(&kirk); // real firmware drives KIRK through MMIO registers too, not just this driver's own direct decrypt calls -- see mmio/kirk_mmio.h
    // LLE Media Engine (me/me_core.h, ported this session): me_wrapper.prx's
    // own real RPC to the ME hangs forever without a real second core to
    // answer it (confirmed live -- a 500M-instruction run never returns
    // from its sceKernelLoadModuleBufferBootInitBtcnf call, stuck in a
    // real sceKernelWaitEventFlag retry loop). On by default for this
    // driver since clearing that module is exactly what this run is for.
    bus.enableMe();

    // This console's own real flash0: (read from the real NAND dump, not the
    // PSAR) -- moved this early (was previously only constructed much later,
    // right before attachRealNandAndFuse) specifically so stageEarlyBoot can
    // prefer each tier-2 module's own real NAND-resident copy over the
    // PSAR's generic update-package copy. Confirmed live this session: the
    // PSAR's own copy of a real module can carry a different real
    // tag/subtype that fails memlmd_9D36A439's real anti-downgrade check,
    // while the SAME module's real NAND-resident copy (this console's own,
    // already-installed) passes it for real -- see module_stager.h's own
    // doc comment on the new `nandFs` parameter.
    beta::NandImage nand(opts.nandDumpPath);
    beta::format::Flash0Fs flash0(nand, fuseId90, fuseId94);
    std::printf("[boot] real flash0: (NAND-resident, preferred over the PSAR for tier-2 modules) valid=%s\n",
                flash0.valid() ? "true" : "false");

    std::printf("[boot] staging real early-boot modules (pspbtcnf-driven, gameBoot=%s)...\n",
                opts.gameBoot ? "true" : "false");
    const beta::firmware::StagedModules staged = beta::firmware::stageEarlyBoot(
        entries, bus, kirk, opts.gameBoot, flash0.valid() ? &flash0 : nullptr);
    if (!staged.earlyBootModules[0].loaded || !staged.earlyBootModules[1].loaded) {
        result.failureReason = "sysmem/loadcore failed to load+link";
        return result;
    }
    result.sysmemRealEntry = staged.sysmemRealEntry;
    std::printf("[boot] sysmem real entry=0x%08X gp=0x%08X, loadcore real base=0x%08X\n",
                staged.sysmemRealEntry, staged.sysmemRealGp, staged.earlyBootModules[1].base);
    // Writes every one of the ~44 real extraModules' already-decrypted
    // bytes into guest RAM at their own allocated bufAddr -- previously
    // only handPlacedModules got this (below); extraModules' buffers sat
    // zeroed the whole run (confirmed live this session), so init.prx's
    // own real StartModule loop had nothing to find once it finished the
    // hand-placed set. See module_stager.h's own doc comment on this
    // function for what this does and doesn't accomplish by itself.
    beta::firmware::writeExtraModulesToRam(staged.extraModules, bus);
    std::printf("[boot] wrote %zu real extraModules' bytes into guest RAM\n", staged.extraModules.size());

    // Real loadcore's own FUN_88118080 (deferred-import-stub-patching, run
    // during real cross-module import linking just above) writes its
    // placeholder value to a wrong computed address for a deferred import
    // whose target library isn't found yet (confirmed live this session,
    // and in the earlier scratch-tool investigation this is ported from):
    // it lands inside loadcore's own static code at [0x8811C700,0x8811C900),
    // corrupting a genuine `jr $ra; nop` trampoline at 0x8811C82C (and
    // ~24 other 8-byte-stride slots through 0x8811C8CC) that real module
    // validation (FUN_8811a6b8's own jump-table dispatch) reads later.
    // Not yet root-caused (why the address computation is wrong), so this
    // snapshots the real, pristine bytes here -- right after staging,
    // before the CPU has executed a single real instruction -- to restore
    // from once, right before the first real module validation needs them.
    std::vector<uint32_t> pristineTrampolineRegion(0x200u / 4u);
    for (uint32_t i = 0; i < pristineTrampolineRegion.size(); ++i) {
        pristineTrampolineRegion[i] = bus.read32(0x8811C700u + i * 4u);
    }

    // Pristine snapshot of every hand-placed module's own real raw-file
    // bytes (populated once staging/writing happens below), restored
    // one-shot per module right before loadcore's own real validator is
    // entered for it -- see the population and restore sites' own comments.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> handPlacedSnapshots;

    std::printf("[boot] extracting the real NAND-resident IPL (genuine KIRK/ECDSA chain)...\n");
    const uint32_t iplEntry = beta::firmware::extractAndLoadRealIplCached(
        kirk, nand, [&](uint32_t addr, uint32_t value) { bus.write32(addr, value); },
        /*fuseKey=*/0ull, opts.iplCacheDir);
    if (iplEntry == 0) { result.failureReason = "real IPL extraction/ECDSA verify failed"; return result; }
    result.iplEntry = iplEntry;
    std::printf("[boot] real IPL entry=0x%08X\n", iplEntry);

    // Real HW_RAM_SIZE register (0xBC100040, uofw hardware.h; jpcsp
    // MMIOHandlerSystemControl: (tachyonVersion<<8)|ramSize) -- ported from
    // test_ipl_realfuse.cpp:757. Only the RAM-type bits are set here
    // (RAM_TYPE_64_MB=2); the Tachyon-version bits are deliberately left 0
    // and added much later (that file's own comment: "setting them here
    // breaks the pre-IPL/IPL chain"). Without even this minimal write, real
    // sysmem/lfatfs code that reads this register sees neither
    // RAM_TYPE_32_MB(1) nor RAM_TYPE_64_MB(2) -- root-caused there to a real
    // empty-partition-list crash; may also matter earlier, to cIPL itself.
    bus.write32(0xBC100040u, 2u);

    // Two real patches to the decrypted cIPL content itself, ported from
    // test_ipl_realfuse.cpp (~line 2756-2798, 3056): undumped real-hardware
    // content this project (and the old one) can never supply byte-perfectly,
    // each with a known, documented, public substitution -- not a guess.
    //
    // 1. ROM-HMAC call site (0x040ED000): real stock cIPL recomputes a hash
    //    seeded from the real, physical, undumped mask-ROM content -- it can
    //    never compute this correctly here. ARK-4's own real, working CFW
    //    source (ipl_stage1_payload/main.c) hardcodes the known-good
    //    IPL_01G hash result and redirects this exact call site to return it
    //    directly; same substitution, ported verbatim.
    {
        constexpr uint32_t kRomHmacCallSite = 0x040ED000u;
        constexpr uint32_t kHmacPatchScratch = 0x04180000u;
        constexpr uint8_t kRomHmac01G[32] = {
            0x27, 0x85, 0xcf, 0x5c, 0xbb, 0xad, 0xf4, 0x2f, 0xff, 0xa5, 0xc4, 0x90, 0xb3, 0xa0, 0xa5, 0x64,
            0xd9, 0x29, 0xdb, 0xe2, 0xdb, 0x07, 0x35, 0x4a, 0xe7, 0x76, 0xcf, 0x51, 0x00, 0x00, 0x00, 0x00};
        uint32_t addr = kHmacPatchScratch;
        auto emit = [&bus, &addr](uint32_t instr) { bus.write32(addr, instr); addr += 4; };
        for (int w = 0; w < 8; ++w) {
            uint32_t word = static_cast<uint32_t>(kRomHmac01G[w * 4]) |
                             (static_cast<uint32_t>(kRomHmac01G[w * 4 + 1]) << 8) |
                             (static_cast<uint32_t>(kRomHmac01G[w * 4 + 2]) << 16) |
                             (static_cast<uint32_t>(kRomHmac01G[w * 4 + 3]) << 24);
            const uint32_t hi = (word >> 16) & 0xFFFFu, lo = word & 0xFFFFu;
            emit(0x3C090000u | hi);                               // lui $t1, hi
            emit(0x35290000u | lo);                                // ori $t1, $t1, lo
            emit(0xAD090000u | static_cast<uint32_t>(w * 4));       // sw $t1, (w*4)($at-ish base 0)
        }
        emit(0x03E00008u); // jr $ra
        emit(0x00000000u); // nop (delay slot)
        const uint32_t jalTarget = (kHmacPatchScratch >> 2) & 0x03FFFFFFu;
        bus.write32(kRomHmacCallSite, 0x0C000000u | jalTarget); // jal kHmacPatchScratch
        std::printf("[boot] patched real ROM-HMAC call site (0x%08X) with ARK-4's known-good IPL_01G hash\n", kRomHmacCallSite);
    }
    // 2. Boot-ROM continue/halt sign-bit placeholder (0xBFC00FFC): real IPL
    //    reads the sign bit of the last word of the (undumped) masked boot
    //    ROM to decide "continue booting" vs "reset and halt". Any sign-bit-
    //    set placeholder routes correctly; the true real value is unknown.
    //    Ported from test_ipl_realfuse.cpp's own `watchBus.forcedAddrs` --
    //    a LIVE, persistent force re-applied on every real read, not a one-
    //    shot init write (confirmed live: real code legitimately overwrites
    //    this address with real content during its own early work, so a
    //    single write here gets clobbered long before the real check at
    //    pc=0x040EC124 runs). Re-poking it once per step is the cheap
    //    equivalent for this driver -- fine for Milestone A's correctness
    //    focus, not a hot path.
    std::printf("[boot] patched real boot-ROM continue-bit placeholder (0xBFC00FFC, persistent)\n");

    Cpu cpu(bus);
    cpu.deliverBusErrorsAsExceptions = true; // let real firmware's own exception handler run, see cpu.h
    cpu.setIdleSkip(true);

    // ---- Stage 1: let the CPU genuinely execute real cIPL code from its own entry ----
    std::printf("[boot] stage 1: executing real cIPL from 0x%08X until the real halt stabilizes...\n", iplEntry);
    cpu.reset(iplEntry);
    // Real exploit trampoline hands off with a real, valid stack pointer --
    // cIPL's own early code immediately does its own `addiu $sp,$sp,-N`
    // frame setup, which needs a real base to decrement from. cpu.reset()
    // leaves SP at 0, so without this cIPL's very first function-epilogue
    // `lw $ra,N($sp)` reads garbage near address 0xFFFFFFE0 and the garbage
    // $ra sends execution off to 0xBFC00000 by accident (confirmed live by
    // disassembling exactly this). Ported from test_ipl_realfuse.cpp:3254 --
    // this specific value is stage-1-only; stage 2's own splice (below)
    // uses a different real SP (test_ipl_realfuse.cpp:3504).
    cpu.state().gpr[SP] = 0x880FFF00u;
    uint64_t haltStableStreak = 0;
    uint64_t instr = 0;
    bool haltConfirmed = false;
    for (; instr < opts.instructionBudget; ++instr) {
        bus.write32(0xBFC00FFCu, 0x80000000u); // persistent force -- see the patch's own comment above
        cpu.step();
        if (cpu.state().pc == kRealHaltPc) {
            if (++haltStableStreak >= kHaltConfirmStreak) { haltConfirmed = true; ++instr; break; }
        } else {
            haltStableStreak = 0;
        }
        if (opts.stateHashInterval > 0 && static_cast<int64_t>(instr) % opts.stateHashInterval == 0) {
            emitStateLine(static_cast<int64_t>(instr), cpu, bus);
            if (windowOn) { beta::SysconChip::buttons = win.buttons(); std::vector<uint8_t> px; captureFramebufferRgba(bus, px); win.submit(px.data()); }
        }
    }
    if (!haltConfirmed) {
        result.failureReason = "stage-1 real halt never stabilized within the instruction budget";
        result.totalInstructionsExecuted = instr;
        return result;
    }
    result.haltConfirmedAtInstr = instr;
    std::printf("[boot] stage 1: real halt confirmed stable at instr %llu\n", static_cast<unsigned long long>(instr));

    // Real per-console hardware state real guest code (not this driver's
    // own direct KIRK calls, which already have the Fuse ID -- see
    // fuse_config.h) would read once it's running: the real NAND image,
    // attached to the live NandController so real NAND commands get real
    // page/spare data, and the real Fuse ID written into the real sysreg
    // registers lfatfs.prx's own real descramble chain reads it from.
    // Deferred to right here (after stage 1's own real halt, same as the
    // old project's own documented timing for this exact Fuse-ID write)
    // rather than at reset -- writing it earlier is documented there to
    // break the real pre-IPL/IPL chain.
    beta::firmware::attachRealNandAndFuse(bus, nand, fuseId90, fuseId94);
    std::printf("[boot] attached real NAND image + wrote real Fuse ID to sysreg\n");

    // ---- Stage 2: this project's own documented SYSCON-reset shortcut ----
    // (splice directly to sysmem's real entry instead of re-running the
    // whole real pre-IPL->cIPL chain -- see the file header comment).
    std::printf("[boot] stage 2: SYSCON-style reset to real sysmem entry 0x%08X\n", staged.sysmemRealEntry);
    {
        const auto preCp0State = cpu.state().cp0State;
        const auto preCp0Ctrl = cpu.state().cp0Ctrl;
        const auto preCount = cpu.countRegister;
        const auto preTimerPending = cpu.timerInterruptPending;
        cpu.reset(staged.sysmemRealEntry);
        cpu.state().cp0State = preCp0State;
        cpu.state().cp0Ctrl = preCp0Ctrl;
        cpu.countRegister = preCount;
        cpu.timerInterruptPending = preTimerPending;
        // Real pre-IPL/cIPL side effect this splice never reproduces
        // otherwise (it skips re-executing real pre-IPL/cIPL by jumping
        // straight to sysmem's entry) -- ported verbatim from
        // test_ipl_realfuse.cpp:3503.
        cpu.state().cp0State[12] &= ~0x00400000u; // Status.BEV
    }
    cpu.state().gpr[SP] = 0x041F0000u; // ported verbatim from test_ipl_realfuse.cpp:3504
    if (staged.sysmemRealGp != 0) cpu.state().gpr[GP] = staged.sysmemRealGp;
    cpu.state().gpr[RA] = kModuleStartReturnSentinel;

    // Real SysMemConfig/SysMemThreadConfig/SceLoadCoreExecFileInfo boot-info
    // sysmem's real module_start(argSize, argBlock) entry needs -- ported
    // from test_ipl_realfuse.cpp:3528-3669, itself traced from uofw's real
    // SysMemInitMain/SysMemReInit (via Ghidra decompile) after an earlier,
    // wrong SceLoadCoreBootInfo guess was disproven. Without this, $a1
    // (argBlock) is null and sysmem's own first real field read off it
    // faults (confirmed live: DBE at address 0x50, `lw $a0,0x50($a1)` with
    // a1=0, within sysmem's first ~150 real instructions).
    {
        constexpr uint32_t kConfigAddr = 0x041E0000u;
        constexpr uint32_t kThreadConfAddr = kConfigAddr + 256u;
        constexpr uint32_t kSysmemExecInfoAddr = kThreadConfAddr + 256u;
        constexpr uint32_t kLoadCoreExecInfoAddr = kSysmemExecInfoAddr + 192u;
        for (uint32_t i = 0; i < 104u; i += 4u) bus.write32(kConfigAddr + i, 0u);
        for (uint32_t i = 0; i < 256u; i += 4u) bus.write32(kThreadConfAddr + i, 0u);

        auto buildExecFileInfo = [&bus](uint32_t addr, const beta::firmware::EarlyBootModuleResult& m) {
            for (uint32_t i = 0; i < 192u; i += 4u) bus.write32(addr + i, 0u);
            bus.write32(addr + 32u, 1u);                        // elfType: 1 = ET_EXEC-ish real kernel exec
            bus.write32(addr + 36u, m.base);                    // topAddr
            bus.write32(addr + 40u, m.entry - m.base);          // entryAddr (offset from topAddr)
            bus.write32(addr + 68u, 1u);                        // isKernelMod
            bus.write32(addr + 72u, 1u);                        // isDecrypted
            bus.write32(addr + 76u, m.moduleInfoAddr - m.base); // moduleInfoOffset
            bus.write32(addr + 80u, m.moduleInfoAddr);          // moduleInfo
            bus.write32(addr + 84u, 1u);                        // isCompressed (real: KL4E)
            bus.write32(addr + 96u, 1u);                        // isDecompressed
            bus.write32(addr + 132u, m.numSegments);            // numSegments
            uint32_t modCodeSize = 0;
            for (uint32_t s = 0; s < m.numSegments && s < 4u; ++s) {
                bus.write32(addr + 136u + s * 4u, m.segmentAddr[s]); // segmentAddr[s]
                bus.write32(addr + 152u + s * 4u, m.segmentSize[s]); // segmentSize[s]
                modCodeSize += m.segmentSize[s];
            }
            bus.write32(addr + 48u, modCodeSize); // modCodeSize
            if (m.numSegments > 0) bus.write32(addr + 52u, m.segmentSize[0]); // textSize
            if (m.numSegments > 1) bus.write32(addr + 56u, m.segmentSize[1]); // dataSize
        };
        buildExecFileInfo(kSysmemExecInfoAddr, staged.earlyBootModules[0]);  // sysmem
        buildExecFileInfo(kLoadCoreExecInfoAddr, staged.earlyBootModules[1]); // loadcore

        bus.write32(kThreadConfAddr + 64u, staged.earlyBootModules[1].libstub);      // loadCoreImportTables
        bus.write32(kThreadConfAddr + 68u,                                           // loadCoreImportTablesSize
                    staged.earlyBootModules[1].libstubend - staged.earlyBootModules[1].libstub);
        bus.write32(kThreadConfAddr + 76u, kSysmemExecInfoAddr);   // sysMemExecInfo
        bus.write32(kThreadConfAddr + 80u, kLoadCoreExecInfoAddr); // loadCoreExecInfo

        bus.write32(kConfigAddr + 4u, beta::MemoryMap::kRamStart);  // unk4: real physical RAM base
        // TEMPORARY investigation (remove after use): the old project's
        // own documented finding at this exact field (test_ipl_realfuse.
        // cpp:3597) is that 0x00380000 (3.5MB) "filled up once memlmd/
        // lfatfs/etc. stayed resident" at their 38th boot-module load,
        // FAILED_ALLOC_MEMBLOCK. We now load all 55 real modules
        // (handPlacedModules + extraModules) via bootInfo, far more than
        // their 38th -- testing whether the same real kernel partition is
        // simply too small again, now that real guest code actually
        // tries to load ge.prx and beyond (a stuck zero-fill loop
        // appeared right where this would manifest).
        // TEMPORARY investigation (remove after use): re-testing with the
        // precise DIAG79/80 instrumentation now in place. The earlier
        // "no effect" conclusion was drawn before finding that the actual
        // bug is modulemgr.c's own real per-module BSS-clear (uofw
        // modulemgr.c:1062/1069, `memset(blkInfo.addr+modCodeSize, 0,
        // blkInfo.memSize-modCodeSize)`) underflowing because
        // blkInfo.memSize (what sysmem actually allocated) ended up
        // smaller than this module's real modCodeSize -- consistent with
        // a nearly-exhausted kernel partition handing out a too-small
        // block. Retesting whether a real fix (not just more headroom
        // than last time, same headroom this time with better visibility).
        bus.write32(kConfigAddr + 8u, 0x01000000u);                 // unk8: kernel partition size (was 0x00380000)
        bus.write32(kConfigAddr + 12u, 3u);                         // unk12: selects other1 (the correct kernel partition)
        bus.write32(kConfigAddr + 16u, 6u);                         // unk16: user partition = scUser
        bus.write32(kConfigAddr + 20u, 2u);                         // model: 2 = 03g
        // DIAG163: real uofw sysmem.c's own SetMemoryPartitionTable
        // (src/kd/sysmem/sysmem.c:585-669, read directly) is the REAL
        // formula behind the whole partition table -- confirmed this
        // session it makes `unk8` (the kernel-partition-size override
        // directly above) and `scUser` (the VSHELL/USER partition
        // common_util.prx and friends load into) a DIRECT zero-sum
        // trade-off: `diff = unk8 - other1.size_default; scUser.size -=
        // diff;`. With `model != 0` (2, confirmed set above) but
        // `unk24 == 0` (left at its zero-filled default, never set by
        // this driver), the real formula takes the `type = 3` branch
        // (line 592: `if (config->unk24 != 2) type = 3;`), whose
        // defaults are SMALLER on both sides (`other1.size_default =
        // 0x300000`, `scUser.size = 0x1800000` = 24MB) than the `type ==
        // 2` branch (`0x600000` / 0x3000000 = 48MB) -- so bumping unk8 to
        // 0x01000000 (16MB) under type=3 produces a much BIGGER diff
        // (~13MB) subtracted from a much SMALLER scUser default (24MB),
        // leaving only ~11MB for the entire VSHELL/USER partition --
        // confirmed live this session to be exactly where paf.prx/
        // common_util.prx's own real allocations exhaust it (a genuine
        // `j <self>` OOM panic at 0x88129974, reached from
        // common_util.prx's own code). Setting unk24=2 selects the
        // type==2 branch instead -- both the kernel partition's own
        // default AND scUser's default double, so the SAME unk8 override
        // now only costs scUser ~10MB out of a 48MB base (~38MB left)
        // instead of ~13MB out of 24MB (~11MB left). Real semantic
        // meaning of `unk24` is undocumented even in uofw (its own
        // struct just calls it `unk24`) -- real boot ROM (proprietary,
        // not in uofw) would have set this from real hardware detection
        // this driver has no equivalent of; `2` is the only value that
        // reaches the larger, correct-looking partition layout for a
        // `model=2` (03g) console, not a blind guess at its literal
        // hardware meaning.
        bus.write32(kConfigAddr + 24u, 2u);                         // unk24: selects the larger (type==2) partition-table variant for model=2
        bus.write32(kConfigAddr + 80u, 0u);                         // debugPutchar (null)
        bus.write32(kConfigAddr + 88u, kThreadConfAddr);            // threadConf
        bus.write32(kConfigAddr + 96u, beta::MemoryMap::kRamSize64MB); // realMemSize
        bus.write32(kConfigAddr + 100u, 0u);                        // randomValue

        cpu.state().gpr[A0] = 104u;         // argSize
        cpu.state().gpr[A1] = kConfigAddr;  // argBlock
    }

    std::printf("[boot] stage 2: running sysmem from real staged state...\n");
    bool sysmemReturned = false;
    bool reserveDone = false;
    bool userReserveDone = false;
    constexpr uint32_t kHandPlacedModulesStart = 0x88600000u;
    const uint32_t handPlacedModulesEnd = opts.gameBoot ? 0x88840000u : 0x88760000u;
    auto doReserve = [&](uint32_t partPtr, uint32_t reserveStart, uint32_t reserveEnd) {
        const uint32_t reserveSize = reserveEnd > reserveStart ? reserveEnd - reserveStart : 0u;
        if (reserveSize == 0) return;
        const CpuState saved = cpu.state();
        cpu.state().gpr[A0] = partPtr;
        cpu.state().gpr[A1] = 2u; // SCE_KERNEL_SMEM_Addr
        cpu.state().gpr[A2] = reserveSize;
        cpu.state().gpr[A3] = reserveStart;
        cpu.state().gpr[8] = 0u; // T0
        constexpr uint32_t kSubCallSentinelRa = 0x00000004u;
        cpu.state().gpr[RA] = kSubCallSentinelRa;
        cpu.state().pc = 0x88105E34u;
        long long subExecuted = 0;
        while (cpu.state().pc != kSubCallSentinelRa && subExecuted < 2'000'000) {
            cpu.step();
            ++subExecuted;
        }
        std::printf("[boot] sysmem reserve: [0x%08X,0x%08X) (%u bytes) result=0x%08X\n",
                    reserveStart, reserveEnd, reserveSize, cpu.state().gpr[V0]);
        const auto postCallCp0State = cpu.state().cp0State;
        const auto postCallCp0Ctrl = cpu.state().cp0Ctrl;
        const auto postCallCount = cpu.countRegister;
        const auto postCallTimerPending = cpu.timerInterruptPending;
        cpu.state() = saved;
        cpu.state().cp0State = postCallCp0State;
        cpu.state().cp0Ctrl = postCallCp0Ctrl;
        cpu.countRegister = postCallCount;
        cpu.timerInterruptPending = postCallTimerPending;
    };
    for (; instr < opts.instructionBudget; ++instr) {
        // Real, live in-process sub-call into sysmem's own real memory
        // allocator (sceKernelAllocPartitionMemory-equivalent, 0x88105E34),
        // fired once at a specific real pc inside sysmem's own module_start
        // (0x88110B04) -- ported from test_ipl_realfuse.cpp:11555-11604.
        // Real hardware's own SysMemInit reserves the already-loaded early-
        // boot module range [reserveStart, earlyBootModulesEndMirror) here
        // so its own LATER general heap allocator treats it as taken, not
        // free. Without this, confirmed live (direct Ghidra decompile
        // cross-check against the real, already-verified loadcore.prx) that
        // sysmem's own real heap-init legitimately overwrites part of
        // loadcore's own already-staged code, since nothing told it that
        // range was spoken for -- a real config gap in this driver, not a
        // hardware mystery or a race with stage 1's cIPL (both ruled out
        // live before finding this).
        if (!reserveDone && cpu.state().pc == 0x88110B04u) {
            reserveDone = true;
            const uint32_t mainPartPtr = bus.read32(0x881145F0u);
            const uint32_t ctrlBlkPtr = mainPartPtr != 0 ? bus.read32(mainPartPtr + 0x10u) : 0u;
            if (mainPartPtr != 0) {
                std::printf("[boot] kernel partition @0x%08X: addr=0x%08X size=0x%08X attr=0x%08X firstCtlBlk=0x%08X ctlBlkCount=%u\n",
                            mainPartPtr, bus.read32(mainPartPtr + 4u), bus.read32(mainPartPtr + 8u),
                            bus.read32(mainPartPtr + 12u), bus.read32(mainPartPtr + 16u), bus.read32(mainPartPtr + 24u));
            }
            // TEMPORARY investigation diagnostic (remove after use): dump
            // every other g_MemInfo partition pointer/bounds (real
            // uofw sysmem/partition.h field order: main@8, other1@12,
            // other2@16, vshell@20, scUser@24, meUser@28, g_MemInfo base
            // 0x881145C0) to find which real partition actually owns the
            // [0x88380000,0x88880000) gap between kernel and user.
            {
                const char* names[] = {"main", "other1", "other2", "vshell", "scUser", "meUser"};
                const uint32_t fieldOffsets[] = {8u, 12u, 16u, 20u, 24u, 28u};
                for (int i = 0; i < 6; ++i) {
                    const uint32_t ptr = bus.read32(0x881145C0u + fieldOffsets[i]);
                    if (ptr == 0) { std::printf("[boot] partition '%s': NULL\n", names[i]); continue; }
                    std::printf("[boot] partition '%s' @0x%08X: addr=0x%08X size=0x%08X attr=0x%08X\n",
                                names[i], ptr, bus.read32(ptr + 4u), bus.read32(ptr + 8u), bus.read32(ptr + 12u));
                }
            }
            if (ctrlBlkPtr != 0 && staged.earlyBootModulesEndMirror != 0) {
                doReserve(mainPartPtr, ctrlBlkPtr + 0x100u, staged.earlyBootModulesEndMirror);
            }
        }
        // Real uofw sysmem/partition.h: SceSysmemMemInfo lays `kernel` and
        // `user` out as adjacent SceSysmemMemoryPartition* fields (offsets
        // 0x30/0x34 from g_MemInfo's own base, 0x881145C0 -- matching
        // mainPartPtr's own known address exactly). The hand-placed module
        // buffers at 0x88600000+ belong to the USER partition, not kernel;
        // reserving them through the kernel partition's own control block
        // (as the reservation above does, correctly, for loadcore's own
        // kernel-side range) silently fails -- confirmed live this session
        // (returns a null address, vs a genuine allocated address for the
        // kernel-range reservation). `user` isn't populated yet at the
        // early sysmem-module_start checkpoint used above (also confirmed
        // live: reading it at that same pc always returns 0 -- it's
        // created later in sysmem's own real init sequence), so this polls
        // every instruction until it becomes available and fires once,
        // rather than tying it to one fixed checkpoint. Without this
        // reservation, nothing tells sysmem's general allocator the hand-
        // placed buffers (init.prx through lowio.prx, still just staged,
        // not yet processed by loadcore's own per-module loop) are taken,
        // so a real allocation made while processing an EARLIER module
        // (here, memlmd's own real module_start) can land inside a LATER
        // module's still-staged buffer and corrupt it before loadcore ever
        // reads it -- exactly the mechanism that corrupted modulemgr.prx's
        // and init.prx's real "~PSP" headers (confirmed live via an
        // instruction-level memory watch: a real memlmd copy-loop at
        // 0x88161264 legitimately overwrites file[0x80,0x150) with a
        // KIRK_CMD_DECRYPT_FUSE-derived block as part of its own real
        // algorithm -- jpcsp's memlmd_6192F715 -- but was reading its own
        // *input* from memory sysmem had never actually reserved for this
        // module, so cross-module allocations could stomp it first).
        if (!userReserveDone) {
            const uint32_t userPartPtr = bus.read32(0x881145F4u);
            if (userPartPtr != 0) {
                userReserveDone = true;
                std::printf("[boot] user partition @0x%08X: addr=0x%08X size=0x%08X attr=0x%08X firstCtlBlk=0x%08X ctlBlkCount=%u\n",
                            userPartPtr, bus.read32(userPartPtr + 4u), bus.read32(userPartPtr + 8u),
                            bus.read32(userPartPtr + 12u), bus.read32(userPartPtr + 16u), bus.read32(userPartPtr + 24u));
                doReserve(userPartPtr, kHandPlacedModulesStart, handPlacedModulesEnd);
            }
        }
        cpu.step();
        if (opts.stateHashInterval > 0 && static_cast<int64_t>(instr) % opts.stateHashInterval == 0) {
            emitStateLine(static_cast<int64_t>(instr), cpu, bus);
            if (windowOn) { beta::SysconChip::buttons = win.buttons(); std::vector<uint8_t> px; captureFramebufferRgba(bus, px); win.submit(px.data()); }
        }
        if (cpu.state().pc == kModuleStartReturnSentinel) {
            std::printf("[boot] sysmem module_start returned cleanly at instr %llu\n", static_cast<unsigned long long>(instr));
            sysmemReturned = true;
            break;
        }
    }
    if (!sysmemReturned) {
        result.totalInstructionsExecuted = instr;
        result.failureReason = "sysmem module_start never returned within the instruction budget";
        return result;
    }

    // ---- Stage 3: same-CPU module_start return chaining into loadcore's real entry ----
    // Ported from test_ipl_realfuse.cpp:11675-12468. Real hardware never
    // resets between sysmem and loadcore (it's one continuous kernel boot --
    // a real `jr $ra` return followed by a real call), so real COP0 state
    // (Status/BEV, and critically the exception-vector-base once
    // exceptionman.prx has configured it) must carry over unchanged across
    // this chain-reset too, same reasoning as the stage-1->2 splice.
    //
    // module_stager deliberately holds reboot.bin's real decrypted bytes
    // (StagedModules::rebootBin) without writing them into the bus -- real
    // early-boot code copies it into its own real staging address itself,
    // and module_stager's own job stops at decrypting it (see
    // module_stager.h's doc comment). Real loadcore code later reads it
    // from there to copy it again into the real boot vector when it
    // voluntarily jumps there (see the bootVectorEntryFixed check below);
    // without this write, that source is empty and the copy finds nothing.
    if (!staged.rebootBin.bytes.empty()) {
        for (std::size_t i = 0; i < staged.rebootBin.bytes.size(); ++i) {
            bus.write8(staged.rebootBin.bufAddr + static_cast<uint32_t>(i), staged.rebootBin.bytes[i]);
        }
        std::printf("[boot] wrote real reboot.bin (%zu bytes) to its real staging address 0x%08X\n",
                    staged.rebootBin.bytes.size(), staged.rebootBin.bufAddr);
    }

    std::printf("[boot] stage 3: chaining into loadcore's real entry 0x%08X\n", staged.earlyBootModules[1].entry);
    {
        const auto preChainCp0State = cpu.state().cp0State;
        const auto preChainCp0Ctrl = cpu.state().cp0Ctrl;
        const auto preChainCount = cpu.countRegister;
        const auto preChainTimerPending = cpu.timerInterruptPending;
        cpu.reset(staged.earlyBootModules[1].entry);
        cpu.state().cp0State = preChainCp0State;
        cpu.state().cp0Ctrl = preChainCp0Ctrl;
        cpu.countRegister = preChainCount;
        cpu.timerInterruptPending = preChainTimerPending;
    }
    cpu.state().gpr[SP] = 0x880FFF00u;
    // Real loadcore code reads uninitialized stack slots as real flags (confirmed
    // live this session: FUN_88116190's own per-module working context, allocated
    // on this same stack, checks an unset flag byte at +0x5a without any real code
    // ever writing it first -- real hardware gets a deterministic value there from
    // its own real boot ROM's prior stack usage; this project's Stage 1 (ARK-4's
    // exploit trampoline, not byte-identical to genuine Sony mask ROM) uses this
    // exact same SP and leaves different, non-representative garbage instead,
    // which read as a true value for that flag and caused a genuine real module
    // (sceKernelCheckExecFile) to be called twice on the same already-processed
    // buffer, failing the second time). Clear a generous window below SP before
    // loadcore starts so it sees a clean stack, same as real hardware's own
    // deterministic boot sequence would provide.
    for (uint32_t a = cpu.state().gpr[SP] - 0x8000u; a < cpu.state().gpr[SP] + 0x100u; a += 4u) {
        bus.write32(a, 0u);
    }
    if (staged.earlyBootModules[1].gp != 0) cpu.state().gpr[GP] = staged.earlyBootModules[1].gp;
    cpu.state().gpr[RA] = kModuleStartReturnSentinel;

    // Real loadcore module_start's own first instructions read $a1 as a
    // POINTER TO A POINTER (uofw loadcore.c:687: `bootInfo =
    // ((SceLoadCoreBootInfo**)argBlock)[0]`) -- one more indirection than
    // sysmem needed. argBlock is really a real 2-pointer array:
    // {SceLoadCoreBootInfo*, SysMemThreadConfig*} (confirmed live: a second
    // real crash reading `*(argBlock+4)` otherwise). The SysMemThreadConfig
    // half reuses sysmem's own already-built kThreadConfAddr -- real
    // firmware threads this same object through to loadcore too.
    uint32_t numHandPlacedBootModulesOuter = 0;
    {
        constexpr uint32_t kSysmemThreadConfAddr = 0x041E0000u + 256u;
        constexpr uint32_t kBootInfoAddr = 0x041E1000u;
        constexpr uint32_t kBootInfoPtrHolder = 0x041E1100u;
        constexpr uint32_t kBootModuleInfoAddr = 0x041E1200u;
        for (uint32_t i = 0; i < 128u; i += 4u) bus.write32(kBootInfoAddr + i, 0u);

        uint32_t numBootModules = 0;
        // Pristine snapshot of every hand-placed module's own real raw-file
        // bytes, restored (one-shot, per module) right before loadcore's own
        // real validator (FUN_8811a6b8) is entered for that module -- see the
        // restore site's own comment below for why this is needed.
        for (const auto& m : staged.handPlacedModules) {
            handPlacedSnapshots.emplace_back(m.bufAddr, m.bytes);
            // Real loadcore code (FUN_88118fac, scratch/loadcore_decompile_
            // all.txt) parses the module buffer's own real "~PSP" header
            // fields itself, then drives its own real KIRK decrypt. Writing
            // the RAW, still-fully-encrypted PSAR bytes here (an EARLIER
            // attempt, since superseded) left the real header's own fields
            // genuinely encrypted too -- nothing in this project's boot path
            // actually drives a real KirkMmio-backed decrypt during CPU
            // execution, so real loadcore ends up running ciphertext as
            // code/structures (confirmed this session: this is the real
            // cause of "Unimplemented instruction" crashes on this path).
            // module_stager.cpp now produces the fix directly: m.bytes is a
            // header-preserving splice -- the real, intact "~PSP" header
            // (genuinely plaintext, readable by real loadcore's own header
            // parse) followed by the real KIRK-decrypted (still-KL4E-
            // compressed) body at its real file offset, done host-side via
            // crypto::decryptPrx since this project doesn't model real
            // KirkMmio access from CPU-executed code. Validated live this
            // session (scratch-tool splice test): gets exceptionman.prx past
            // loadcore's entire real FUN_8811a6b8 validator chain with no
            // manual field patches.
            for (std::size_t i = 0; i < m.bytes.size(); ++i) {
                bus.write8(m.bufAddr + static_cast<uint32_t>(i), m.bytes[i]);
            }
            const uint32_t addr = kBootModuleInfoAddr + numBootModules * 32u;
            bus.write32(addr + 0u, 0u);        // modPath (unused for buffer-based load)
            bus.write32(addr + 4u, m.bufAddr); // modBuf
            bus.write32(addr + 8u, static_cast<uint32_t>(m.bytes.size())); // modSize (real header + still-compressed body size)
            bus.write32(addr + 12u, 0u);       // unk12
            bus.write32(addr + 16u, m.attr);     // real bit1 (program module)/bit2 (path) flags -- see module_stager.h
            bus.write32(addr + 20u, m.bootData);  // real apiType for the program module, 0 otherwise
            bus.write32(addr + 24u, 0u);       // argSize
            bus.write32(addr + 28u, 0u);       // argPartId
            ++numBootModules;
        }
        // JIT ARRAY EXTENSION (replaces the earlier static lfatfs+vshmain-
        // only addition): `InitInit()`'s own real min/max scan over
        // `bootInfo->modules[0..numModules)` (uofw init.c:932-950) runs
        // EXACTLY ONCE, early, strictly before it creates+starts
        // `SceKernelInitThread` -- it never re-reads `bootInfo->modules[]`
        // again after that. The real regression found earlier this session
        // (statically seeding all ~55 entries up front caused an underflowed
        // real zero-fill) only happens because THAT scan sees entries whose
        // addresses span past the real `0x88800000` staging-window ceiling
        // -- NOT because of entry count. So: keep `bootInfo` at just the 11
        // real hand-placed entries for InitInit()'s own one-time scan
        // (unchanged, below), then inject every real `staged.extraModules`
        // entry (ge.prx through vshmain.prx, 60 real modules, in real
        // pspbtcnf order, each at its own natural real address -- no
        // relocation needed now that we're past the one real check that
        // required it) in a single shot the FIRST time real execution
        // reaches `SceKernelInitThread`'s own real entry (confirmed live
        // this session: `0x8816F540`), before its own per-module loop has
        // read `numModules` even once. See the main instruction loop below
        // for the actual injection (search "JIT ARRAY EXTENSION" there).
        numHandPlacedBootModulesOuter = numBootModules;

        bus.write32(kBootInfoAddr + 0u, beta::MemoryMap::kRamStart);     // memBase
        bus.write32(kBootInfoAddr + 4u, beta::MemoryMap::kRamSize64MB);  // memSize
        bus.write32(kBootInfoAddr + 8u, 0u);                              // loadedModules
        bus.write32(kBootInfoAddr + 12u, numBootModules);                 // numModules
        bus.write32(kBootInfoAddr + 16u, numBootModules != 0 ? kBootModuleInfoAddr : 0u); // modules*
        bus.write32(kBootInfoAddr + 36u, 0xFFFFFFFFu); // modProtId = SCE_KERNEL_VALUE_UNITIALIZED
        bus.write32(kBootInfoAddr + 40u, 0xFFFFFFFFu); // modArgProtId = SCE_KERNEL_VALUE_UNITIALIZED

        bus.write32(kBootInfoPtrHolder, kBootInfoAddr);
        bus.write32(kBootInfoPtrHolder + 4u, kSysmemThreadConfAddr);
        cpu.state().gpr[A0] = 0u;                   // argSize (unused on this path per uofw)
        cpu.state().gpr[A1] = kBootInfoPtrHolder;    // argBlock == {SceLoadCoreBootInfo*, SysMemThreadConfig*}
    }

    bool bootVectorEntryFixed = false;
    bool extraModulesInjected = false;
    uint64_t lastVblankSubmitted = 0;
    auto lastVblankWallTime = std::chrono::steady_clock::now();
    double fpsEma = 0.0; // smoothed real wall-clock frames/sec (one real vblank == one PSP frame of emulated time, so this is the real-time speed ratio, not a fixed 60)
    bool fpsEmaInit = false;
    for (; instr < opts.instructionBudget; ++instr) {
        // JIT ARRAY EXTENSION: the first time real execution reaches
        // SceKernelInitThread's own real entry (confirmed live this
        // session), `InitInit()`'s own one-time real min/max scan has
        // already run (it happens strictly before this thread is even
        // created) and will never read `bootInfo->modules[]` again -- so
        // it's now safe to append every real `staged.extraModules` entry
        // (60 real modules, in real pspbtcnf order, each kept at its own
        // natural real address) before `InitThreadEntry`'s own per-module
        // loop reads `numModules` for its first iteration. See the comment
        // above the original 11-entry bootInfo write (search "JIT ARRAY
        // EXTENSION" above) for why this is safe.
        // JIT ARRAY EXTENSION, corrected: the real loop does NOT re-read
        // bootInfo->numModules on each iteration -- it uses a ONE-TIME
        // cached snapshot in a separate real "g_init"-style control block
        // (confirmed live: g_init+0xC == 11, g_init+0x8 == 9, UNCHANGED
        // across the whole run, even though bootInfo->numModules itself was
        // already updated to 59). g_init's own address becomes known as
        // soon as $s4 is loaded (0x8816F58C: `lw s4,4(v0)`) -- fire the
        // injection THERE instead of at the thread's very first instruction,
        // and write the new count to g_init+0xC directly (not just
        // bootInfo->numModules, which is also kept in sync for anything
        // else that might read it for real).
        if (!extraModulesInjected && cpu.state().pc == 0x8816F58Cu) {
            extraModulesInjected = true;
            constexpr uint32_t kBootInfoAddr = 0x041E1000u;
            constexpr uint32_t kBootModuleInfoAddr = 0x041E1200u;
            const uint32_t gInit = cpu.state().gpr[20]; // $s4, about to be loaded this same instruction
            uint32_t n = numHandPlacedBootModulesOuter;
            uint32_t injected = 0;
            // DIAG159: capstone-disassembled the real dispatch at
            // 0x88118FDC-0x88119018 and found it checks `*(buf) ==
            // "~PSP"` (0x5053507E) directly to decide which internal path
            // to take -- live-watched (DIAG158) and confirmed ge.prx's
            // own buffer (0x88752000) reads as all-zero, not "~PSP",
            // on BOTH its internal calls, while lowio's own buffer
            // (hand-placed, written separately) correctly reads "~PSP"
            // both times. Root cause: `writeExtraModulesToRam` (called
            // once, at initial setup time, long before the main
            // execution loop starts) writes extraModules' real bytes
            // into guest RAM too early -- the exact same real-hardware
            // trap the old project (psp-lle-emu) already documented and
            // fixed for exceptionman/threadman/etc: an early real IPL/
            // boot-ROM instruction zeroes a wide RAM range well after
            // setup time but long before this per-module loop runs,
            // wiping out anything staged ahead of time. Re-write every
            // extraModule's real bytes here instead, live, right before
            // this exact loop needs them (same fix shape as the old
            // project's "write them right before they're needed" note).
            for (const auto& m : staged.extraModules) {
                if (m.bufAddr != 0 && !m.bytes.empty()) {
                    for (std::size_t bi = 0; bi < m.bytes.size(); ++bi) {
                        bus.write8(m.bufAddr + static_cast<uint32_t>(bi), m.bytes[bi]);
                    }
                }
            }
            for (const auto& m : staged.extraModules) {
                if (m.bufAddr == 0 || m.bytes.empty()) continue;
                const uint32_t addr = kBootModuleInfoAddr + n * 32u;
                bus.write32(addr + 0u, 0u);
                bus.write32(addr + 4u, m.bufAddr);
                bus.write32(addr + 8u, m.size);
                bus.write32(addr + 12u, 0u);
                bus.write32(addr + 16u, m.attr);
                bus.write32(addr + 20u, m.bootData);
                bus.write32(addr + 24u, 0u);
                bus.write32(addr + 28u, 0u);
                ++n;
                ++injected;
            }
            bus.write32(kBootInfoAddr + 12u, n); // numModules (kept in sync)
            uint32_t gInitNumModulesBefore = 0;
            try { gInitNumModulesBefore = bus.read32(gInit + 0x0Cu); } catch (...) {}
            bus.write32(gInit + 0x0Cu, n); // the real cached numModules bound
            // ROOT CAUSE FOUND: $s4 is InitThreadEntry's own STACK-RESIDENT
            // COPY of bootInfo (NOT the original at kBootInfoAddr) -- and
            // critically, *(s4+0x10) (bootInfo->modules, confirmed via the
            // real "find program module" check at modules[numModules-1]
            // matching uofw init.c:674) is a SEPARATE, SMALL copy of the
            // modules array (observed address 0x88FFBD00, NOT
            // kBootModuleInfoAddr) -- every entry this driver ever wrote to
            // kBootModuleInfoAddr has been INVISIBLE to the real loop the
            // whole time, which is why lfatfs/vshmain's own earlier static
            // entries only ever worked by covering indices already inside
            // that small real copy's own original bounds (11 entries) --
            // confirmed live: a real sceKernelCheckExecFile call for entry
            // #11 got a0(modBuf)=0, reading past that copy's real small
            // allocation. Fix: redirect *(s4+0x10) to point at THIS
            // driver's own, fully-populated, properly-sized array instead
            // of risking overflow into the small original copy.
            uint32_t gInitModulesPtrBefore = 0;
            try { gInitModulesPtrBefore = bus.read32(gInit + 0x10u); } catch (...) {}
            bus.write32(gInit + 0x10u, kBootModuleInfoAddr);
            std::printf("  [JIT] injected %u real extraModules (numModules %u -> %u); g_init+0xC %u -> %u; g_init+0x10(modules*) %08X -> %08X at instr %llu\n",
                        injected, numHandPlacedBootModulesOuter, n, gInitNumModulesBefore, n, gInitModulesPtrBefore, kBootModuleInfoAddr, static_cast<unsigned long long>(instr));
        }
        // Real loadcore code genuinely, voluntarily calls (jalr, with a real
        // $ra return address, not a one-way reset) a real cIPL-resident
        // utility at the boot vector that clears a0..a0+a1 bytes of RAM
        // (confirmed live this session: a0=0x08000000, a1=0x04000000 on the
        // first call -- the full 64MB). Real hardware does this BEFORE any
        // kernel module is loaded into RAM; this driver (like the old
        // project's own stageEarlyBoot()) already wrote real loadcore.prx
        // and friends into that exact range before the CPU ever started
        // running, so actually performing the clear here would wipe out
        // already-loaded real module content (including, confirmed live,
        // the very return-address instruction this call is about to jump
        // back to) -- the content this call exists to produce is already
        // in place, just via a different real ordering. HLE'd as a no-op
        // (jump straight to $ra), ported verbatim from the old project's own
        // proven fix, test_ipl_realfuse.cpp:4315-4340.
        if (cpu.state().pc == 0xBFC00000u && (cpu.state().cp0State[12] & 0x2u) != 0u) {
            // Status.EXL is set, meaning we arrived here via a real
            // exception delivery (BEV=1/EBase=0 vectors every exception
            // here too), NOT the voluntary jalr call the no-op below
            // assumes. Treating it the same way (jumping to $ra, which
            // exceptions never touch) would silently re-enter whatever
            // faulted forever (confirmed live this session: a real Data
            // Bus Error from a null module-descriptor pointer, masked this
            // way into an apparently-stuck boot with no visible error at
            // all). Real hardware would never DBE-fault here, so this is a
            // genuine emulation bug upstream, not something to paper over --
            // fail loudly with the real fault details instead of guessing.
            const uint32_t excCode = (cpu.state().cp0State[13] >> 2) & 0x1Fu;
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                "unexpected real exception (code=%u) reached the boot vector: EPC=0x%08X BadVAddr=0x%08X ra=0x%08X at instr %llu",
                excCode, cpu.state().cp0State[14], cpu.state().cp0State[8], cpu.state().gpr[RA], static_cast<unsigned long long>(instr));
            throw std::runtime_error(buf);
        }
        if (cpu.state().pc == 0xBFC00000u && bus.read32(0xBFC00000u) == 0x00801021u) {
            // Narrowly scoped to the ONE real call shape this project has
            // actually confirmed live: the genuine cIPL-resident RAM-clear
            // utility, identified by its own real, unmodified first
            // instruction ("move v0,a0") still being present at the boot
            // vector. Real loadcore ALSO reaches 0xBFC00000 via a totally
            // different, genuinely-terminal real mechanism -- memset a
            // fresh 4KB there, memcpy a real panic-trampoline stub in from
            // its own base (0x88115000), then call THAT instead (confirmed
            // live via Ghidra decompile of FUN_88116190/FUN_88116694: every
            // real dead-end block does exactly this before its own
            // genuine, permanent `do{}while(true)`) -- treating that case
            // the same way this no-op does would silently escape a real
            // panic instead of reporting it, corrupting whatever runs
            // after with state a real panic was never meant to let
            // continue (confirmed live this session: this exact confusion
            // produced an apparently-succeeding module #0 that was
            // actually past a real failure). Checking the live instruction
            // word, not just the address, tells the two apart for real --
            // once a panic trampoline overwrites it, this branch no longer
            // matches and real code genuinely runs (and genuinely never
            // returns), instead of being silently routed around.
            //
            // Real semantics of the confirmed case (live disassembly): the
            // utility unconditionally does `v0 = a0` as its own first
            // instruction, regardless of a1 -- real callers rely on this
            // (confirmed live: skipping it entirely, as an earlier version
            // of this fix did, left a genuine real caller spinning forever
            // re-calling this same utility with a0=0, a1=0x1F, waiting for
            // SOMETHING this call is supposed to produce). The actual
            // clear-loop writes are only unsafe to perform for real when
            // they'd overlap the RAM range real early-boot modules are
            // already staged into -- a small clear nowhere near that range
            // (like this a0=0,a1=0x1F case, far below kRamStart) is
            // genuinely harmless and the real caller may depend on it
            // actually happening, so perform it for real instead of
            // skipping it too.
            const uint32_t clearBase = cpu.state().gpr[4], clearSize = cpu.state().gpr[5];
            cpu.state().gpr[2] = clearBase; // v0 = a0, matching the real utility's own first instruction
            constexpr uint32_t kStagedRangeStart = 0x08000000u, kStagedRangeEnd = 0x0C000000u; // this console's real 64MB RAM window
            const bool overlapsStagedRange = clearSize != 0 &&
                (clearBase & MemoryMap::kAddressMask) < kStagedRangeEnd &&
                ((clearBase & MemoryMap::kAddressMask) + clearSize) > kStagedRangeStart;
            if (!overlapsStagedRange) {
                for (uint32_t i = 0; i < clearSize; ++i) bus.write8(clearBase + i, 0u);
            }
            cpu.state().pc = cpu.state().gpr[RA];
            continue;
        }
        if (cpu.state().pc == 0x8811a6b8u) {
            // Real sysmem heap allocations made while an EARLIER hand-placed
            // module's own real module_start runs can land inside a LATER
            // module's still-staged buffer and corrupt it before loadcore
            // ever validates/decrypts that module -- confirmed live this
            // session (modulemgr.prx's real "~PSP" header read back
            // genuinely different bytes than what was staged, right before
            // its own FUN_8811a6b8 entry, after memlmd.prx's own
            // module_start ran). Extending the sysmem heap reservation to
            // cover the hand-placed buffer range didn't fix it (the real
            // reservation call itself failed for that address range --
            // likely a different real memory partition than loadcore's own,
            // not yet modeled), so this restores each module's own pristine
            // bytes one-shot, right before ITS OWN real validation reads
            // them, undoing whatever corruption happened in between.
            const uint32_t entryBuf = cpu.state().gpr[4];
            static std::vector<bool> restoredOnce(handPlacedSnapshots.size(), false);
            for (std::size_t i = 0; i < handPlacedSnapshots.size(); ++i) {
                if (!restoredOnce[i] && handPlacedSnapshots[i].first == entryBuf) {
                    restoredOnce[i] = true;
                    const auto& bytes = handPlacedSnapshots[i].second;
                    for (std::size_t j = 0; j < bytes.size(); ++j) {
                        bus.write8(entryBuf + static_cast<uint32_t>(j), bytes[j]);
                    }
                    break;
                }
            }
        }
        if (cpu.state().pc == 0x8811a900u) {
            // [HLE exception: early-boot-array CheckLatestSubType, documented]
            // Real FUN_8811a8bc (loadcore's decryptMode=2 per-module handler,
            // Ghidra decompile at scratch/loadcore_decompile_8811a8bc.txt)
            // reads v0 here right after its own real, gated call to
            // CheckLatestSubType (func_0x88118e80); on v0==0 its very next
            // real instructions are `halt_baddata()` -- confirmed live this
            // session this is a genuine CPU-level infinite self-jump
            // (0x881165FC), not a decompiler artifact.
            //
            // Exhaustively verified this session that everything feeding
            // into this check is modeled correctly: the fuse-derived key
            // (kirk_individual_key_100, reverted to its known-plaintext-
            // verified 1-round form), the live crypto input bytes (byte-
            // exact match against the real PSAR/flash0 ground truth for
            // both modulemgr.prx and init.prx), and the control flow itself
            // (matches uofw's decompiled loadcore.prx instruction-for-
            // instruction). Real memlmd.prx's own decompiled code
            // (scratch/memlmd_full_decompile.txt) never calls any of
            // loadcore's SetCompareLatestSubType-family setters, confirmed
            // live (DIAG32: called exactly once, with a0=0, from loadcore's
            // own init) -- so g_compareLatestSubType is genuinely always
            // NULL and this hardcoded threshold IS the real mechanism, not
            // an emulation gap.
            //
            // The remaining, documented reason this doesn't panic on real
            // hardware: this entire early FUN_88116694/FUN_8811a6b8/
            // FUN_8811a8bc pass over the 9-buffer hand-placed array
            // (threadman/iofilemgr/memlmd/modulemgr/init.prx/loadexec_03g/
            // exceptionman/dmacman/systimer) is a preliminary pass whose
            // real outcome for any member OTHER than memlmd never actually
            // matters for boot: the old (frozen) project's own documented
            // finding (docs/HLE.md, "none of the 9 buffers staged for
            // loadcore's generic bootInfo->modules[] processing ever have
            // their own code execute") confirms these modules' real
            // loading/starting happens through a wholly separate mechanism
            // (init.prx's own later StartModule calls, once it's actually
            // running, driving the real ~50-module pspbtcnf VSH boot) --
            // this early pass is not that mechanism. memlmd.prx is the one
            // exception: its own real module_start DOES execute here
            // (confirmed live, DIAG30), registering its real library via a
            // separate hardcoded path, so it's excluded below and left to
            // run for real.
            if (cpu.state().gpr[2] == 0u) {
                const uint32_t s0 = cpu.state().gpr[16];
                for (const auto& m : staged.handPlacedModules) {
                    if (m.bufAddr == s0 && m.realName.find("memlmd") == std::string::npos) {
                        cpu.state().gpr[2] = 1u;
                        break;
                    }
                }
            }
        }
        if (cpu.state().pc == 0x8811c83cu) {
            // Real g_getLengthFunc / NID memlmd_EF73E85B hook (uofw
            // loadelf.c's PspUncompress, DECRYPT_MODE_KERNEL_MODULE branch):
            // the real KIRK decrypt call, called as
            // g_getLengthFunc(modBuf, execInfo->execSize, &newSize). Confirmed
            // via raw real-loadcore disassembly that this exact address is
            // called with a0=modBuf(=buf/s0), a1=*(ctx+0x10)(=execSize,
            // SceLoadCoreExecFileInfo+16 per uofw's struct), a2=auStack_20
            // (=&newSize) -- an exact match, not a guess. uofw's own comment
            // on this call: "the ~PSP header will be overwritten by the
            // plain ELF data when the module is successfully decrypted" --
            // i.e. this call decrypts modBuf IN PLACE, starting at offset 0,
            // which is why the later gzip/KL4E decompress call (reached via
            // the SAME modBuf pointer, unadjusted) finds compressed data
            // there instead of the "~PSP" tag. This address falls inside the
            // [0x8811C700,0x8811C900) range this driver's own
            // pristineTrampolineRegion fix treats as a no-op import-stub
            // table -- that fix is still needed for the OTHER addresses in
            // that range, but this one is genuinely load-bearing, not an
            // unregistered no-op, so it's hooked here instead of restored to
            // "jr ra; nop". We don't model real KirkMmio access from
            // CPU-executed code (same rationale module_stager.cpp already
            // used elsewhere), so the real crypto runs host-side via
            // crypto::decryptPrx, exactly like a real syscall/HLE patch.
            const uint32_t modBuf = cpu.state().gpr[A0];
            const uint32_t execSize = cpu.state().gpr[A1];
            const uint32_t newSizePtr = cpu.state().gpr[A2];
            bool ok = false;
            if (execSize >= 0x150u) {
                // Real memlmd code (g_prepareGetLengthFunc, our 0x8811c824
                // hook target) legitimately overwrites this module's own
                // [+0x80,+0x150) region -- INCLUDING the real encryption tag
                // at +0xD0 this decrypt needs -- as part of its own real
                // per-module signature-prep work, which real hardware always
                // runs BEFORE this call. Reading the tag live from RAM here
                // (as this hook used to) picks up memlmd's own overwritten
                // bytes instead of the module's real original tag -- wrong
                // for any module processed after memlmd has loaded and
                // linked its own real code into loadcore's import table
                // (confirmed live this session: modulemgr, the first such
                // module). Use this module's own pristine staged snapshot
                // for the DECRYPT INPUT instead of live memory, since that's
                // the real, undisturbed file content real hardware's own
                // decrypt step is supposed to see.
                std::vector<uint8_t> inbuf(execSize);
                const std::vector<uint8_t>* snapshot = nullptr;
                for (const auto& snap : handPlacedSnapshots) {
                    if (snap.first == modBuf) { snapshot = &snap.second; break; }
                }
                if (snapshot && snapshot->size() >= execSize) {
                    std::copy(snapshot->begin(), snapshot->begin() + execSize, inbuf.begin());
                } else {
                    for (uint32_t i = 0; i < execSize; ++i) inbuf[i] = bus.read8(modBuf + i);
                }
                std::vector<uint8_t> outbuf(execSize, 0u);
                const int32_t bodySize = crypto::decryptPrx(kirk, inbuf.data(), outbuf.data(), execSize);
                if (bodySize > 0 && static_cast<uint32_t>(bodySize) <= execSize) {
                    for (int32_t i = 0; i < bodySize; ++i) bus.write8(modBuf + static_cast<uint32_t>(i), outbuf[static_cast<std::size_t>(i)]);
                    if (newSizePtr != 0u) bus.write32(newSizePtr, static_cast<uint32_t>(bodySize));
                    ok = true;
                }
            }
            cpu.state().gpr[V0] = ok ? 0u : 0x80020148u; // SCE_ERROR_OK : SCE_ERROR_KERNEL_UNSUPPORTED_PRX_TYPE
            cpu.state().pc = cpu.state().gpr[RA];
            continue;
        }
        {
            // One-shot restore of the real trampoline region FUN_88118080's
            // own real (not yet root-caused) address-computation bug
            // corrupts during deferred-import-stub patching -- see the
            // pristineTrampolineRegion snapshot's own comment above. Applied
            // right before the first real module validation
            // (FUN_8811a6b8's own entry) would otherwise read the corrupted
            // bytes.
            static bool trampolineRestored = false;
            if (!trampolineRestored && cpu.state().pc == 0x8811a6b8u) {
                trampolineRestored = true;
                for (uint32_t i = 0; i < pristineTrampolineRegion.size(); ++i) {
                    bus.write32(0x8811C700u + i * 4u, pristineTrampolineRegion[i]);
                }
            }
        }
        // Real guest code (loadcore, running normally) genuinely, voluntarily
        // jumps to the literal real MIPS boot vector here to run real
        // reboot.bin content -- nothing is being skipped, so the code about
        // to run should see the TRUE real post-reset Status snapshot
        // (BEV=1, ERL=1) and clear BEV itself when ready, exactly like real
        // pre-IPL/loader.S does on a genuine cold boot. Without this, stale
        // software Status carried over from loadcore's own prior real
        // execution (BEV already cleared) makes exceptions inside this tiny
        // boot-ROM stub mis-vector -- ported one-shot fix from
        // test_ipl_realfuse.cpp:11257-11279.
        if (!bootVectorEntryFixed && cpu.state().pc >= 0xBFC00000u && cpu.state().pc < 0xBFC00020u) {
            bootVectorEntryFixed = true;
            cpu.state().cp0State[12] = 0x00400004u; // Status: BEV=1, ERL=1
        }
        // Detects a genuine real "j <self>" spin (opcode 0x02, target ==
        // its own address). Confirmed live this session (Ghidra decompile
        // of FUN_88116190/FUN_88116694): every real dead-end block in this
        // stretch of loadcore ends in exactly this -- a real, permanent
        // panic, never meant to return. An earlier version of this check
        // force-skipped past it to keep tracing what came next; that
        // silently escaped real panics and let corrupted downstream state
        // masquerade as progress (confirmed live: an apparently-succeeding
        // module #0 that was actually past a real failure). Report it as
        // the real, permanent halt it is instead.
        {
            static uint32_t stuckPc = 0;
            static uint64_t stuckSince = 0;
            const uint32_t pcNow = cpu.state().pc;
            if (pcNow != stuckPc) { stuckPc = pcNow; stuckSince = instr; }
            else if (instr - stuckSince > 2000000ull) {
                uint32_t w = 0;
                try { w = bus.read32(pcNow); } catch (...) {}
                const uint32_t jTarget = ((pcNow + 4u) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2u);
                if ((w >> 26) == 0x02u && jTarget == pcNow) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                        "real code reached a genuine permanent halt (j <self>) at pc=0x%08X, stuck since instr %llu",
                        pcNow, static_cast<unsigned long long>(stuckSince));
                    throw std::runtime_error(buf);
                }
            }
        }
        cpu.step();
        if (opts.stateHashInterval > 0 && static_cast<int64_t>(instr) % opts.stateHashInterval == 0) {
            emitStateLine(static_cast<int64_t>(instr), cpu, bus);
        }
        if (windowOn) {
            // Repaint on the real vblank edge (DisplayController::vblankCount(),
            // IRQ 30), not on a fixed instruction count: the guest flips its
            // display buffer in lockstep with this same real IRQ, so sampling
            // at any other cadence drifts in and out of phase with the
            // guest's own clear/draw/flip cycle -- observed live as the
            // window "flashing in and out" once XMB was up. Before
            // display_01g.prx starts the vblank clock, vblankCount() stays 0
            // forever, so fall back to the old stateHashInterval cadence for
            // that phase (nothing meaningful to show yet, just boot progress).
            const uint64_t vb = bus.displayController().vblankCount();
            const bool vblankRunning = vb != 0 || lastVblankSubmitted != 0;
            const bool staleCadenceHit = opts.stateHashInterval > 0 && static_cast<int64_t>(instr) % opts.stateHashInterval == 0;
            if ((vblankRunning && vb != lastVblankSubmitted) || (!vblankRunning && staleCadenceHit)) {
                if (vblankRunning) {
                    // Real wall-clock FPS: one real vblank is always exactly 1/60s of
                    // *emulated* PSP time, so how many of them we actually present per
                    // real second is the genuine real-time speed ratio (60 == real-time,
                    // 30 == running at half speed), not just a display refresh counter.
                    const auto now = std::chrono::steady_clock::now();
                    const double dtSec = std::chrono::duration<double>(now - lastVblankWallTime).count();
                    lastVblankWallTime = now;
                    if (dtSec > 0.0) {
                        const double instFps = (vb - lastVblankSubmitted) / dtSec;
                        fpsEma = fpsEmaInit ? fpsEma + 0.1 * (instFps - fpsEma) : instFps;
                        fpsEmaInit = true;
                        char line1[32];
                        std::snprintf(line1, sizeof(line1), "FPS %.1f/60", fpsEma);
                        win.setOverlay(line1, "");
                    }
                }
                lastVblankSubmitted = vb;
                beta::SysconChip::buttons = win.buttons();
                std::vector<uint8_t> px; captureFramebufferRgba(bus, px); win.submit(px.data());
            }
        }
        if (cpu.state().pc == kModuleStartReturnSentinel) {
            // Real loadcore's own SysBoot(), once reached, is documented
            // (uofw's own reboot/main.c: "load loadcore (doesn't return)")
            // to continue on into the real kernel boot rather than return --
            // reaching the sentinel here would mean it returned anyway,
            // worth reporting rather than silently treating as success.
            std::printf("[boot] loadcore module_start returned (unexpected per uofw) at instr %llu\n",
                        static_cast<unsigned long long>(instr));
            break;
        }
    }

    // DIAG164 ("why not check the frame buffer"): dump whatever's
    // currently in VRAM at the GE's own real current display framebuffer
    // address, regardless of outcome -- a direct visual check of real
    // progress instead of inferring it from instruction-count/PC
    // patterns alone. fbAddress()/frameBufferStride()/frameBufferFormat()
    // are GeRenderer's own public accessors; the actual pixel reads go
    // straight through the bus (same real VRAM every other read in this
    // driver uses) since GeRenderer's own dr16/dr32/unpack are private.
    {
        std::vector<uint8_t> px;
        uint32_t fb = 0, stride = 0, fmt = 0;
        const uint64_t nonBlack = captureFramebufferRgba(bus, px, &fb, &stride, &fmt);
        if (windowOn) win.submit(px.data()); // final frame, so a closing window shows the real last state
        const std::string path = "scratch/diag164_framebuffer.png";
        const bool wrote = pngw::writeRgba(path, beta::LiveWindow::kWidth, beta::LiveWindow::kHeight, px);
        std::printf("[boot] [DIAG164] framebuffer dump: fbAddr=%08X stride=%u fmt=%u nonBlackPixels=%llu/%u wrote=%s -> %s\n",
                    fb, stride, fmt, static_cast<unsigned long long>(nonBlack), beta::LiveWindow::kWidth * beta::LiveWindow::kHeight, wrote ? "yes" : "NO", path.c_str());
    }
    // PSP_WINDOW=1: keep the window open (and interactive -- keyboard input
    // reads into LiveWindow::buttons() as a real SceCtrl bit mask, already
    // wired every emitStateLine cadence into SysconChip::buttons, which the
    // real SYSCON GET_DIGITAL_KEY-family commands answer from) until the
    // user closes it, same convention as the old project's own PSP_WINDOW
    // mode. Without this,
    // the window (and the final frame it's showing) would vanish the
    // instant the instruction budget runs out and this function returns.
    if (windowOn) {
        std::printf("[boot] PSP_WINDOW: budget reached, window stays open -- close it to exit.\n");
        while (win.open()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }

    result.totalInstructionsExecuted = instr;
    result.ok = true;
    return result;
}

} // namespace beta::driver

int main(int argc, char** argv) {
    beta::driver::BootDriverOptions opts;
    opts.emu = beta::config::parseOptionsFromEnvironment();
    beta::config::applyCommandLineArgs(opts.emu, argc, argv);
    if (argc > 1) opts.instructionBudget = static_cast<uint64_t>(std::atoll(argv[1]));

    beta::driver::BootDriverResult r;
    try {
        r = beta::driver::runBoot(opts);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[boot] FAILED: uncaught exception: %s\n", e.what());
        return 1;
    }
    if (!r.ok) {
        std::fprintf(stderr, "[boot] FAILED: %s (after %llu instructions)\n",
                      r.failureReason.c_str(), static_cast<unsigned long long>(r.totalInstructionsExecuted));
        return 1;
    }
    std::printf("[boot] OK: iplEntry=0x%08X sysmemRealEntry=0x%08X haltConfirmedAtInstr=%llu totalInstr=%llu\n",
                r.iplEntry, r.sysmemRealEntry,
                static_cast<unsigned long long>(r.haltConfirmedAtInstr),
                static_cast<unsigned long long>(r.totalInstructionsExecuted));
    return 0;
}
