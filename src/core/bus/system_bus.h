#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unordered_map>

#include "../cpu_bus.h"
#include "../memory.h"
#include "../memory_map.h"
#include "../mmio/ddr_controller.h"
#include "../mmio/dmac.h"
#include "../mmio/generic_stub.h"
#include "../mmio/ge_edram.h"
#include "../mmio/gpio.h"
#include "../mmio/i2c.h"
#include "../mmio/audio.h"
#include "../mmio/lcd_controller_slim.h"
#include "../mmio/power_regs.h"
#include "../mmio/display_controller.h"
#include "../mmio/syscon_chip.h"
#include "../mmio/wlan_device.h"
#include "../mmio/me_controller.h"
#include "../mmio/ms_controller.h"
#include "../mmio/umd_engine.h"
#include "../mmio/interrupt_controller.h"
#include "../mmio/kirk_mmio.h"
#include "../mmio/nand_controller.h"
#include "../mmio/system_time.h"
#include "../mmio/system_control.h"
#include "../mmio/timer.h"
#include "../util/nand_image.h"
#include "../../config/options.h"

// Ported from psp-lle-emu/src/core/system_bus.{h,cpp} (see
// docs/PORTING_LOG.md for the full list of changes). One real subsystem
// is deliberately NOT wired in yet, matching the OLD project's own
// documented incremental precedent ("...every other real MMIO device not
// listed above are intentionally unmapped here -- later phases add them
// as needed"):
//
//   - The GE register block (real base 0xBD400000, GeCoreStub/GeRenderer)
//     and its embedded-VRAM control twin (0xBD500000, GeEdramStub) ARE now
//     wired in (gpu/ge_renderer.h + gpu/ge_capture.h ported verbatim from
//     the old project, software rasterizer only -- no D3D11 GPU
//     acceleration path, see ge_renderer.h's own BETA_GE_GPU_ACCEL guard).
//     This lets real guest display-list submission actually render into
//     VRAM instead of being a documented no-op.
//   - The Media Engine (MeCore, enableMe()/me()) -- ported 2026-11-08
//     (me/me_core.h), WITHOUT its old project's MeCodec audio-decode HLE
//     shortcut (AT3+/MP3 host decode needs ffmpeg-derived decoders not
//     yet ported -- see me_core.h's own header comment). Off by default:
//     a caller opts in with enableMe(), same as the old project's
//     PSP_ME env-var gate. One real side effect of enabling it: the old
//     project's SysconChip::nowUsec (the RTC's real-time-advance
//     function) is only ever wired up inside enableMe() -- so until a
//     caller calls it, the emulated RTC clock does not advance even with
//     SysconOptions::rtcSeedFlags set (ticking_ stays true but nowUsec
//     stays unset, so readClock() always returns the fixed seeded
//     value). This is the OLD project's own existing coupling, preserved
//     as-is, not a new bug.
//
// Device-registry note (per the plan's §1b): a real-call-site audit
// across the old project's tests/ and launcher/ found ZERO external
// callers of dmac0()/dmac1()/dmacplus()'s accessors (only locate() reaches
// them internally) -- so no DeviceRegistry was built; those three stay
// private members only, same as every other MmioStub. Typed accessors are
// kept only for devices with real confirmed callers: interruptController
// (2x), nandController (5x), umdDrive (6x), memoryStickController (3x),
// sysconChip (1x), systemControl (5x), audio (1x) -- plus attachRealNandImage.

namespace beta {

class MeCore;

class SystemBus : public CpuBus {
public:
    explicit SystemBus(uint32_t ramSize = MemoryMap::kRamSize32MB,
                        const config::BusOptions& busOpts = {},
                        const config::AudioOptions& audioOpts = {},
                        const config::UmdOptions& umdOpts = {},
                        const config::WlanOptions& wlanOpts = {},
                        const config::SysconOptions& sysconOpts = {},
                        const config::GeOptions& geOpts = {},
                        const config::MeOptions& meOpts = {});
    ~SystemBus() override;

    uint8_t  read8(uint32_t addr) const override;
    uint16_t read16(uint32_t addr) const override;
    uint32_t read32(uint32_t addr) const override;
    // Bulk read of n bytes of plain memory (RAM / VRAM / scratchpad / IPL) that lie in one region; false if the range is not plain memory
    // (nothing is copied then). For callers that want one call and a memcpy instead of a bus access per word.
    bool readBlock(uint32_t addr, uint8_t* dst, std::size_t n) const;

    uint64_t writeCounter = 0; // bumped on every bus store (used by a hook recorder, once one exists)
    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void write32(uint32_t addr, uint32_t value) override;

    // Real interrupt delivery: bit 0 here maps to Cause's IP2 bit in
    // Cpu::step() -- matching real hardware, where all 64 real interrupt
    // sources (including all 4 timers) multiplex onto that one CPU-visible
    // line via the interrupt controller.
    uint32_t pendingInterrupts() const override { return intc_.hasPendingEnabled() ? 0x1u : 0u; }

    // Forwards to SystemControl -- see TimingSource::setTimerInterruptPending's
    // doc comment for why this needs to exist as a separate push from Cpu.
    void setTimerInterruptPending(bool pending) override { syscon_.setTimerPending(pending); }

    // Advances every time-based device (currently: the 4 timers) by one
    // instruction, called once per Cpu::step().
    void tick() override;
    uint64_t* ramWriteCounter() override { return &writeCounter; }
    void tickN(uint32_t n) override { tickPending_ += n; if (tickPending_ >= tickBatch_) flushTicks(); }
    AudioStub& audio() { return audio_; }
    void flushTicks();
    // Diagnostics (not guest accesses) must not move device time: while set, bus accesses skip the pre-access tick flush.
    void setDiag(bool on) { diag_ = on; }
    void setTickBatch(uint32_t n) { flushTicks(); tickBatch_ = n ? n : 1u; }
    const uint8_t* ramWatchMap() const override { return ramFastSize_ ? ram_.watchedMap() : nullptr; }
    const uint8_t* ramWindow(uint32_t& physBase, uint32_t& size) const override { physBase = ramFastBase_; size = ramFastSize_; return ramFastSize_ ? ram_.data() : nullptr; }

    // Exact whole-bus versions of the block-execution hooks: the minimum over every ticking device (the 4 timers, the NAND
    // controller, SystemTime, the display controller -- exactly the set tick() advances), the O(1) equivalent of that many tick()
    // calls, and "is this address a device register rather than plain memory".
    uint64_t ticksUntilEvent() const override;
    void advanceTicks(uint64_t n) override;
    bool hasSideEffects(uint32_t addr) const override;
    bool tracksCodeWrites() const override { return true; }
    bool codeFetchIsTimingNeutral(uint32_t addr) const override {
        if (((addr & MemoryMap::kAddressMask) - ramFastBase_) < ramFastSize_) return true;
        uint32_t off = 0;
        return !needsExactTime(locate(addr, off)); // VRAM / scratchpad / IPL ROM code has no device timing either (unmapped -> locate()==nullptr -> not neutral)
    }
    void watchCode(uint32_t addr, uint32_t size) override;
    uint64_t codeWriteEpoch() const override;
    bool storeNeedsInterpreter(uint32_t addr) const override;

    // Bulk-loads a boot image into the IPL region (the real MIPS reset
    // vector on this hardware, 0xBFC00000).
    void loadIplImage(const uint8_t* data, std::size_t size);

    // Bulk-loads an image into real VRAM (0x04000000, 2MB) -- real hardware's
    // early-boot scratch RAM.
    void loadVramImage(const uint8_t* data, std::size_t size, uint32_t offset = 0);

    // LLE Media Engine (me/me_core.h, now ported -- see MeCore's own
    // header comment for what it does and doesn't model yet). Off by
    // default (me_ stays null) until a caller opts in: a second real
    // Allegrex core is extra per-instruction cost nothing needs unless
    // the boot/guest code actually exercises the ME.
    void enableMe();
    MeCore* me() { return me_.get(); }

    InterruptController& interruptController() { return intc_; }
    SystemControl& systemControl() { return syscon_; }
    SysconChip& sysconChip() { return sysconChip_; } // the Syscon serial chip (clock, scratchpad, battery commands)
    MsController& memoryStickController() { return memoryStick_; }
    UmdDrive& umdDrive() { return umdDrive_; }
    Timer& timer(std::size_t index) { return timers_.at(index); }
    NandController& nandController() { return nand_; }
    GeCoreStub& geCore() { return ge_; }
    DisplayController& displayController() { return displayController_; }

    // Wires a real, raw NAND dump into the NAND controller so command
    // completion deposits genuine page/spare data into the real NAND-page-
    // buffer MMIO region (0xBFF00900) instead of leaving it blank. Pass
    // nullptr to detach (the default, existing behavior).
    void attachRealNandImage(const NandImage* image) {
        nand_.attachRealImage(image, &nandPage_, 0x900);
    }

    // Wires a real KirkEngine into the KIRK crypto-engine MMIO bridge
    // (0xBDE00000) so real firmware's register-driven crypto commands
    // (not just this project's other, direct-call KIRK uses) actually run.
    // Pass nullptr to detach (the default -- see kirk_mmio.h's own doc
    // comment on why most tests never need this).
    void attachKirkEngine(crypto::KirkEngine* engine) { kirkMmio_.attachKirkEngine(engine); }

    // Real syscall table: format::linkModules() (once ported) populates this
    // while resolving a real SCE_LIB_SYSCALL_EXPORT import; Cpu::step()
    // consults it whenever it decodes a SYSCALL instruction.
    uint32_t resolveSyscall(uint32_t code) const override {
        const auto it = syscallTable_.find(code);
        return it != syscallTable_.end() ? it->second : 0u;
    }
    void registerSyscall(uint32_t code, uint32_t target) override {
        syscallTable_[code] = target;
    }

    // Real host-callback syscall table, kept separate from syscallTable_
    // above so ordinary guest-to-guest linking and this emulator-side
    // shortcut mechanism can never collide silently in the same map.
    bool tryHostSyscall(uint32_t code, Bus& topBus, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t& outV0) override {
        const auto it = hostSyscallTable_.find(code);
        if (it == hostSyscallTable_.end()) {
            return false;
        }
        outV0 = it->second(topBus, a0, a1, a2, a3);
        return true;
    }
    void registerHostSyscall(uint32_t code, HostSyscallFn fn) override {
        hostSyscallTable_[code] = std::move(fn);
    }

private:
    // Finds among the plain accept-anything stubs (memory_map.h), or
    // returns nullptr if `phys` isn't in any of their ranges.
    Bus* locateGenericStub(uint32_t phys, uint32_t& offsetOut) const;

    // Finds the device backing `addr` and the offset within it, or returns
    // nullptr if nothing is mapped there. Declared const and returning a
    // non-const Bus* (via const_cast on the always-genuinely-mutable member
    // devices) so one implementation serves both the const read* methods
    // and the non-const write* methods.
    Bus* locate(uint32_t addr, uint32_t& offsetOut) const;
    [[noreturn]] void unmapped(uint32_t addr) const;

    // If `addr` is plain memory (RAM/scratchpad/VRAM/IPL) returns that Memory and the offset in it, else nullptr. RAM pages are
    // classified once (every word of the page is probed through locate(), so a device window inside RAM -- legal when RAM is inflated --
    // is still honored) and then answered from a per-page table instead of walking locate()'s long window chain on every access.
    Memory* plainMemory(uint32_t addr, uint32_t& offsetOut) const;
    mutable std::vector<uint8_t> ramPageClass_; // 0 = not yet classified, 1 = whole page is plain RAM, 2 = not (use locate())

    Memory ram_;
    uint32_t ramFastBase_ = 0, ramFastSize_ = 0, ramLim8_ = 0, ramLim16_ = 0, ramLim32_ = 0; // prefix of RAM verified (initFastRam) to decode to ram_ on every page
    void initFastRam();
    Memory scratchpad_;
    Memory ipl_;
    Memory iplBank2_{MemoryMap::kIplBank2Size};
    Memory vram_{MemoryMap::kVramSize};
    InterruptController intc_;
    // Relies on intc_ already being constructed at this point (declaration
    // order guarantees it), same reasoning as timers_/nand_ below.
    SystemControl syscon_{intc_};
    // Real PSP interrupt numbers for SysTimer0-3. Relies on intc_ already
    // being constructed at this point, which declaration order guarantees.
    std::array<Timer, 4> timers_{{Timer(intc_, 15), Timer(intc_, 16), Timer(intc_, 17), Timer(intc_, 18)}};
    MmioStub memoryAccessControl_{MemoryMap::kMemoryAccessControlSize};
    // Relies on intc_ already being constructed (declaration order).
    I2c i2c_{intc_, MemoryMap::kI2cSize};
    // Real base 0xBD500000 (self-clearing trigger) / 0xBD400000 (list
    // exec/status). ge_.attach() is called in the constructor body, below,
    // once the bus itself (needed for the real-RAM read/write callbacks)
    // is fully constructed.
    GeEdramStub geEdram_{MemoryMap::kGeEdramSize};
    GeCoreStub ge_; // constructed in the .cpp constructor's init list (needs geOpts)
    Dmac dmac0_{MemoryMap::kDmac0Size, 22};
    Dmac dmac1_{MemoryMap::kDmac1Size, 23};
    MeController meController_{MemoryMap::kMeControllerSize};
    MsController memoryStick_{MemoryMap::kMemoryStickSize};
    MmioStub unknownBe10_{MemoryMap::kUnknownBe10Size};
    WlanDevice wlan_; // constructed with WlanOptions in the SystemBus constructor's init list, not here (can't reference a ctor param in a default member initializer)
    MmioStub usb_{MemoryMap::kUsbSize};
    LcdControllerSlim lcdControllerSlim_{MemoryMap::kLcdControllerSlimSize};
    DisplayController displayController_{intc_, MemoryMap::kDisplayControllerSize};
    MmioStub displayControllerSlim_{MemoryMap::kDisplayControllerSlimSize};
    MmioStub a7f00000_{MemoryMap::kA7f00000Size};
    MmioStub a7f80000_{MemoryMap::kA7f80000Size};
    Gpio gpio_{intc_, MemoryMap::kGpioSize};
    // The UMD drive (mmio/umd_engine.h): relies on intc_ and gpio_ being constructed already (declaration order); it writes guest RAM through this bus.
    // Constructed with UmdOptions in the SystemBus constructor's init list (can't reference a ctor param in a default member initializer).
    UmdDrive umdDrive_;
    Ata2Regs ata2_{umdDrive_, MemoryMap::kAta2Size};
    UmdAtaRegs umdAta_{umdDrive_, MemoryMap::kUmdAtaSize};
    UmdRegs umd_{umdDrive_, MemoryMap::kUmdSize};
    SysconChip sysconChip_; // constructed with SysconOptions in the SystemBus constructor's init list
    MmioStub profiler_{MemoryMap::kProfilerSize};
    MmioStub cpuBusFrequency_{MemoryMap::kCpuBusFrequencySize};
    AudioStub audio_; // constructed with AudioOptions in the SystemBus constructor's init list
    uint32_t audioDivider_ = 0;
    bool diag_ = false;
    // Device time is brought up to date (flushTicks) before an access that a DEVICE could observe. Plain memories (RAM, scratchpad, VRAM, the IPL ROM)
    // have no timing, so touching them -- including fetching code from them -- needs no flush, UNLESS BusOptions::flushPlainMemory (was PSP_FLUSH_PLAIN)
    // restores the old, legacy always-flush behavior.
    bool flushPlainMemory_ = false;
    bool needsExactTime(const Bus* dev) const {
        if (flushPlainMemory_) return true;
        return !(dev == &ram_ || dev == &scratchpad_ || dev == &vram_ || dev == &ipl_ || dev == &iplBank2_);
    }
    uint32_t tickPending_ = 0, tickBatch_ = 8; // BusOptions::tickBatch overrides (1 = tick devices every instruction)
    // Relies on intc_ already being constructed (declaration order).
    SystemTime systemTime_{intc_, MemoryMap::kSystemTimeSize};
    PowerRegs power_{MemoryMap::kPowerSize};
    TimerStatusAlias timer0Alias_{timers_}; // read-only status mirrors of the four timers at 0xBC500100..0x13F
    Dmac dmacplus_{MemoryMap::kDmacplusSize, 21, 0x180u, 3u, {0x04u, 0x08u, 0x10u}}; // display frame buffer regs live at +0x100..+0x110 (plain storage)
    MmioStub uart3_{MemoryMap::kUart3Size};
    MmioStub uart4_{MemoryMap::kUart4Size};
    // Relies on intc_ already being constructed at this point (declaration
    // order guarantees it), same as timers_ below.
    NandController nand_{intc_, MemoryMap::kNandSize};
    MmioStub lcdc_{MemoryMap::kLcdcSize};
    MmioStub nandPage_{MemoryMap::kNandPageSize};
    // Real KIRK crypto-engine MMIO registers (0xBDE00000) -- see
    // mmio/kirk_mmio.h. Starts unattached (no real engine), same
    // accept-anything behavior as a plain stub, until attachKirkEngine().
    KirkMmio kirkMmio_;
    DdrController ddr_{MemoryMap::kDdrSize};
    std::unordered_map<uint32_t, uint32_t> syscallTable_;
    std::unordered_map<uint32_t, HostSyscallFn> hostSyscallTable_;
    // LLE Media Engine (me/me_core.h). unique_ptr (not a direct member)
    // because MeCore's own definition isn't visible here -- only
    // enableMe()'s .cpp, which #includes me/me_core.h, needs to know its
    // real size; everything else just forward-declares it.
    std::unique_ptr<MeCore> me_;
    uint32_t meRatio_ = 1; // ME steps per main-core step (PSP_ME_RATIO)
    bool meHorizonAlways_ = false; // was a static-local PSP_ME_HORIZON_ALWAYS read in ticksUntilEvent()
    config::MeOptions meOpts_; // stored at construction; enableMe() is opt-in and happens later, so it needs its own copy
};

} // namespace beta
