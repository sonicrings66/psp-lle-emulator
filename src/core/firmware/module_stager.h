#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../cpu_bus.h"
#include "../crypto/kirk.h"
#include "../format/flash0_fs.h"
#include "../format/module_link.h"
#include "../format/psar.h"

// Real early-boot module staging -- ported from psp-lle-emu's
// stageEarlyBoot() lambda (tests/test_ipl_realfuse.cpp:1170-2716, ~1546
// lines), then redesigned to be genuinely pspbtcnf-driven (a deliberate
// architectural improvement over the old project, not a port of its
// shortcut -- see docs/PORTING_LOG.md's "pspbtcnf-driven redesign" section
// for the full uofw citations and empirical verification behind this).
//
// What real hardware does (psdevwiki's "Iplloader" article, "Part3 IPL";
// uofw's reboot/main.c and src/kd/init/init.c): the real boot ROM/IPL only
// ever loads /kd/sysmem.prx and /kd/loadcore.prx directly from flash0 --
// pspbtcnf.bin can't be involved yet, since nothing can read a real file
// off the filesystem until sysmem/iofilemgr exist. loadcore's own real
// SysBoot() THEN reads /kd/pspbtcnf.bin -- the real, KIRK-encrypted
// (ordinary "~PSP"-tagged, same as any module) boot-module config -- and
// its real record table (confirmed via uofw's reboot/main.c sub_707C/
// sub_71A8, by direct decompile) is what decides which further modules
// load, in what real order, and with what real run-level inclusion: each
// record's own real marker field is bitwise-ANDed against a value derived
// from the current boot's apiType, and the module loads iff the result is
// nonzero.
//
// This project doesn't yet model SysBoot()/InitInit() running that read-
// and-dispatch loop itself (real kernel code this project hasn't traced/
// verified yet) -- instead, like the old project, this function does the
// real KIRK decrypt + real ELF relocation + real cross-module linking
// SysBoot() would have driven, staging the results for a later boot driver
// to hand to the CPU the way loadcore/init would have. The DIFFERENCE from
// the old project: which modules get staged, in what order, and with what
// VSH/GAME inclusion is now read from the real pspbtcnf.bin record table
// (firmware::parsePspbtcnf) instead of three hardcoded lists (the old
// project's 3-entry earlyBootModules[]'s non-sysmem/loadcore members, 11
// hand-written per-module blocks, and a 49-entry extraModules[] array, plus
// a separately-hardcoded 14-name VSH-only exclusion list duplicated three
// times) -- a confirmed, pre-existing gap between the old emulator's
// shortcut and real hardware behavior, now closed.
//
// Two real tiers:
//   1. sysmem/loadcore/interruptman -- the only modules actually loaded
//      into `bus` here (via format::loadElf) and real-cross-module-linked
//      (via format::linkModules). sysmem and loadcore are NOT driven by
//      pspbtcnf (real hardware can't read it yet either, at this point --
//      this part of the old project's hardcoded approach was already
//      correct, not a shortcut, and is unchanged here). interruptman is
//      loaded directly (not just staged) because loadcore's own early init
//      makes a real, unconditional call into its exported
//      sceKernelRegisterSystemCallTable before SysBoot() would otherwise
//      have loaded it -- a genuine circular real dependency; it ALSO gets
//      a separate staged copy in tier 2 below (pspbtcnf's own record table
//      lists it too) for SysBoot()'s own normal load path.
//   2. Every other real module pspbtcnf.bin's record table names (sysmem/
//      loadcore records skipped -- tier 1 already handles them) --
//      decrypted and held (`StagedModule`), NOT written into `bus` here.
//      Real early-boot code zero-fills the VRAM range these would occupy
//      well after staging but before the kernel reads them (confirmed live
//      in the old project at pc=0x04000528) -- the actual deferred writes
//      belong to the not-yet-built boot driver (Phase B). A small fixed
//      set of real module names (the old project's own "hand-placed 11" --
//      init, exceptionman, interruptman-stage, threadman, dmacman,
//      systimer, iofilemgr, memlmd, modulemgr, loadexec, lowio) keep their
//      own real fixed staging addresses (unchanged from the old project --
//      this is a real memory-placement concern the pspbtcnf redesign
//      doesn't touch); every other real module gets a runtime-allocated,
//      page-aligned address, same as the old project's own "extraModules"
//      approach. reboot.bin (found embedded inside loadexec's own
//      decrypted bytes, not listed in pspbtcnf itself) is staged
//      separately, same as before.
//
// Real console-generation naming note (CORRECTED 2026-11-03, see
// docs/PORTING_LOG.md): the PSAR bundles a real, separately-tagged
// pspbtcnf.bin for EVERY console generation 6.61 shipped on, including
// an exact "pspbtcnf_03g.bin" match for this specific console (tag
// 0x4C9496F0, a key this project already has) -- NOT only a mismatched
// "_11g" copy as earlier assumed. module_stager.cpp's entry-selection
// prefers that exact match directly. This console's own real per-console
// flash0:/kd/pspbtcnf_03g.bin (NAND, per-file tag 0xDF9B3A12) is still
// undecryptable -- but it's no longer needed, since the PSAR's exact-
// generation copy decrypts to byte-identical-looking real boot-config
// data (same cleartext size fields; see the porting log for the exact
// comparison). stripGenerationSuffix() is still real and necessary for a
// separate reason: even the exact-match pspbtcnf_03g.bin's OWN records
// name a handful of modules with a generation suffix (e.g.
// "/kd/loadexec_03g.prx", "/kd/memlmd_03g.prx") that must be normalized
// before matching the PSAR-resolved entries, which carry the same suffix
// via format::psar_names.cpp's generation-aware name table.
namespace beta::firmware {

// One decrypted-and-staged (but not yet bus-written) module.
struct StagedModule {
    std::string name;           // the real pspbtcnf record name, generation-normalized, e.g. "/kd/threadman.prx"
    std::string realName;       // the real PSAR entry path that actually matched, e.g. "flash0:/kd/threadman.prx" (empty if not found/not included)
    std::vector<uint8_t> bytes; // header-preserving splice: the real, intact "~PSP" header
                                 // (raw file bytes [0,0x150)) followed by the real KIRK-decrypted
                                 // but still-KL4E-compressed body at its real file offset -- NOT a
                                 // bare ELF (real loadcore itself validates the header and
                                 // decompresses the body; staging must not pre-empt that). Tier 1
                                 // (sysmem/loadcore/interruptman's directly-loaded copy) is the one
                                 // exception, staying a bare decrypted-and-decompressed ELF, since
                                 // cIPL's own direct load (not loadcore's module-load path) is what
                                 // real hardware uses there. rebootBin is also a bare decrypted ELF
                                 // (a further-decrypted embedded "~PSP" blob, its own separate real
                                 // consumer). Empty if excluded by run-level, not found, or didn't
                                 // fit its reserved buffer.
    uint32_t bufAddr = 0;       // real fixed (the small hand-placed set) or allocated address
    uint32_t size = 0;          // == bytes.size() when staged, 0 otherwise
    uint32_t marker = 0;        // the real pspbtcnf marker this inclusion/exclusion decision was made from (0 if not in pspbtcnf, e.g. reboot.bin)

    // Real SceLoadCoreBootModuleInfo::attr/bootData (confirmed live against
    // this project's own real 661.PBP via a Ghidra decompile of real
    // InitThreadEntry, matching uofw's kd/init/init.c): the real pspbtcnf
    // marker is NOT this field directly -- its low byte is a real boot-mode
    // SECTION mask (VSH=1, GAME=2, UPDATER=4, ...), unrelated to attr's own
    // real bit meanings (bit1 "this is the program module", bit2 "modBuf is
    // a path"). Default 0 for every module; init.c's own real loop
    // identifies the program module POSITIONALLY (it's always last --
    // confirmed by `if (mod->attr & 2) break;` ending the loop), not by
    // name, so staging marks whichever real module ends up last with
    // attr|=2 and the real cold-VSH-boot apiType (SCE_EXEC_FILE_APITYPE_
    // KERNEL_1 = 0x200) in bootData, per uofw init.c:673-687.
    uint32_t attr = 0;
    uint32_t bootData = 0;
};

// One of the 3 modules actually loaded + linked (tier 1 above).
struct EarlyBootModuleResult {
    std::string wantedName;
    uint32_t entry = 0;
    uint32_t gp = 0;
    uint32_t base = 0;
    uint32_t moduleInfoAddr = 0;
    uint32_t libstub = 0;
    uint32_t libstubend = 0;
    uint32_t numSegments = 0;
    std::array<uint32_t, 4> segmentAddr{};
    std::array<uint32_t, 4> segmentSize{};
    bool loaded = false;
};

struct StagedModules {
    std::array<EarlyBootModuleResult, 3> earlyBootModules; // ["sysmem","loadcore","interruptman"], in that order
    format::LinkStats linkStats;                            // real cross-module link result across the 3 above

    std::vector<StagedModule> handPlacedModules; // the small fixed-address real module set (see file header), in real pspbtcnf order
    StagedModule rebootBin;                       // name="reboot.bin"; bufAddr is the real kRebootBinBufAddr (0x88600000); not in pspbtcnf

    std::vector<StagedModule> extraModules; // every other real pspbtcnf-listed module, in real record order, run-level filtered

    std::map<std::string, std::vector<uint32_t>> moduleAttributes; // every real marker seen per (generation-normalized) module name -- see firmware::groupPspbtcnfByName

    // Convenience duplicates of earlyBootModules[0] (sysmem) -- the old
    // project's own outer-scope sysmemRealEntry/sysmemRealGp, read directly
    // by several later-boot-chain call sites (not yet ported).
    uint32_t sysmemRealEntry = 0;
    uint32_t sysmemRealGp = 0;
    uint32_t earlyBootModulesEndMirror = 0; // nextBase (mirror form) after tiers 1+2 -- the old project's own heap-protection reservation point

    // Two real, PC-keyed loadcore addresses Phase B's (not yet built)
    // HleRegistry needs: sceKernelCheckExecFile (NID 0xD3353EC4, confirmed
    // consumed elsewhere in the old project -- test_ipl_realfuse.cpp:10037,
    // a PC-keyed HLE checkpoint) and sceKernelLinkLibraryEntries (NID
    // 0x8EAE9534, kept for the same historical reason though found to have
    // no other reader anywhere in the old project). Both 0 if loadcore
    // didn't load or doesn't export them.
    uint32_t linkLibraryEntriesAddr = 0;
    uint32_t checkExecFileAddr = 0;
};

// `entries`: the real, already-decrypted PSAR (or unpacked-folder) entries
// (format::resolvePsarEntries / format::loadUnpackedEntries -- the caller's
// choice which source, same as the old project's own fallback logic, not
// re-implemented here). `gameBoot`: selects the real run-level filtering
// (via each pspbtcnf record's own real marker, bitwise-ANDed against
// 0x1 for a cold VSH boot or 0x2 for a game boot -- see module_stager.cpp's
// kWantBitVsh/kWantBitGame and docs/PORTING_LOG.md for how these exact bit
// values were confirmed) and the two real boot-type-dependent fixed
// addresses (loadexec/lowio) plus the extra-modules allocation base.
//
// `nandFs` (optional): this console's own real, already-mounted flash0:
// filesystem (format::Flash0Fs), when the caller has one. The PSAR holds a
// GENERIC 6.61 OFW update package's own copy of every module; this specific
// console's own already-installed/flashed copy (read live from the real NAND
// dump) can carry a genuinely different real per-module `tag`/subtype --
// confirmed live this session that the PSAR's own copy of a real module can
// fail a real anti-downgrade check (memlmd_9D36A439) that the SAME module's
// real NAND-resident copy passes (lfatfs.prx, vshmain.prx). When `nandFs` is
// non-null and valid, every extraModules real file lookup checks the real
// NAND first (by real path, "flash0:" + the record's own normalized name)
// and only falls back to the PSAR's `entries` if the NAND doesn't have it.
// Deliberately NOT applied to handPlacedModules or tier 1
// (sysmem/loadcore/interruptman): both already work from the PSAR, and
// widening this to handPlacedModules was tried and reverted -- at least one
// of those modules' real NAND-resident copy uses a real KIRK tag this
// project's crypto layer (crypto::decryptPrx) has no mapping for yet, and
// that's a real gap of its own, not something to paper over by silently
// keeping those modules on the PSAR without saying so here.
StagedModules stageEarlyBoot(const std::vector<format::ResolvedPsarEntry>& entries, CpuBus& bus,
                              crypto::KirkEngine& kirk, bool gameBoot,
                              const format::Flash0Fs* nandFs = nullptr);

// Writes every one of `extraModules`' real, already-decrypted bytes into
// guest RAM at its own allocated `bufAddr` -- the same real write
// `stageEarlyBoot`'s caller already does for `handPlacedModules`, extended
// to the other ~44 real pspbtcnf-listed modules (module_stager.h's own
// "tier 2" above, which stageEarlyBoot only allocates an address for, by
// design -- see the file header comment). This alone doesn't make real
// guest code discover or start these modules; it only ensures their real
// bytes are actually present in RAM once something does (`init.prx`'s own
// real StartModule loop, once it reaches past the hand-placed set -- see
// docs/PORTING_LOG.md and this session's own live-traced findings).
// Entries with no allocated buffer or no staged bytes (excluded by
// run-level, or not found in the real PSAR) are skipped.
void writeExtraModulesToRam(const std::vector<StagedModule>& extraModules, CpuBus& bus);

} // namespace beta::firmware
