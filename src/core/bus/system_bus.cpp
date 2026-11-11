#include "system_bus.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../me/me_core.h"

namespace beta {

namespace {
inline uint32_t maskAddr(uint32_t a) { return a & MemoryMap::kAddressMask; }
} // namespace

SystemBus::SystemBus(uint32_t ramSize, const config::BusOptions& busOpts, const config::AudioOptions& audioOpts,
                      const config::UmdOptions& umdOpts, const config::WlanOptions& wlanOpts,
                      const config::SysconOptions& sysconOpts, const config::GeOptions& geOpts,
                      const config::MeOptions& meOpts)
    : ram_(ramSize),
      scratchpad_(MemoryMap::kScratchpadSize),
      ipl_(MemoryMap::kIplSize),
      ge_(MemoryMap::kGeSize, geOpts),
      wlan_(MemoryMap::kWlanSize, wlanOpts),
      umdDrive_(intc_, gpio_, *this, umdOpts),
      sysconChip_(gpio_, MemoryMap::kSysconChipSize, sysconOpts),
      audio_(MemoryMap::kAudioSize, audioOpts),
      flushPlainMemory_(busOpts.flushPlainMemory),
      tickBatch_(busOpts.tickBatch ? busOpts.tickBatch : 1u),
      kirkMmio_(intc_, nullptr, *this, MemoryMap::kKirkMmioSize),
      meOpts_(meOpts) {
    wlan_.attachInterrupts(&intc_);
    sysconChip_.onWlanPower = [this](bool on) { if (!on) wlan_.powerOff(); };
    auto dmaMem = [this]() {
        Dmac::Memory m;
        m.r8 = [this](uint32_t a) { return read8(a); };
        m.r32 = [this](uint32_t a) { return read32(a); };
        m.w8 = [this](uint32_t a, uint8_t v) { write8(a, v); };
        m.w32 = [this](uint32_t a, uint32_t v) { write32(a, v); };
        return m;
    };
    dmac0_.attach(&intc_, dmaMem());
    dmac1_.attach(&intc_, dmaMem());
    // a DMA from the WLAN data port waits until a read command has been issued (the driver sets the DMA up first)
    const auto wlanGate = [this](uint32_t src) { return (src & ~3u) != WlanDevice::kDataPortPhys || wlan_.dataReady(); };
    dmac0_.sourceReady = wlanGate;
    dmac1_.sourceReady = wlanGate;
    const auto wlanDestGate = [this](uint32_t dst) { return (dst & ~3u) != WlanDevice::kDataPortPhys || wlan_.writeReady(); };
    dmac0_.destReady = wlanDestGate;
    dmac1_.destReady = wlanDestGate;
    wlan_.onWriteTpc = [this] { dmac0_.retryHeld(); dmac1_.retryHeld(); };
    dmacplus_.attach(&intc_, dmaMem());
    audio_.attach(&intc_, &systemTime_);
    for (Dmac* d : {&dmac0_, &dmac1_}) d->pacingHook = [this](uint32_t dst, uint32_t bytes) { return audio_.dmaDue(dst, bytes); };
    for (Dmac* d : {&dmac0_, &dmac1_}) d->startHook = [this](uint32_t dst) { return audio_.dmaStartDue(dst); };
    initFastRam();
    {
        GeMemory geMem;
        geMem.r8 = [this](uint32_t a) { return read8(a); };
        geMem.r16 = [this](uint32_t a) { return read16(a); };
        geMem.r32 = [this](uint32_t a) { return read32(a); };
        geMem.w8 = [this](uint32_t a, uint8_t v) { write8(a, v); };
        geMem.w16 = [this](uint32_t a, uint16_t v) { write16(a, v); };
        geMem.w32 = [this](uint32_t a, uint32_t v) { write32(a, v); };
        ge_.attach(&intc_, [this](uint32_t a) { return read32(a); }, geMem);
    }
    // NOTE: no ME construction here -- enableMe() builds it on demand,
    // same as the old project (see the header's class comment for the
    // one real side effect, RTC ticking, of waiting for that opt-in).
}

void SystemBus::initFastRam() {
    const uint32_t base = maskAddr(MemoryMap::kRamStart), size = static_cast<uint32_t>(ram_.size());
    uint32_t good = 0;
    for (uint32_t o = 0; o < size; o += 0x1000u) {
        const uint32_t end = std::min(o + 0x1000u, size);
        uint32_t off1, off2;
        // Both ends of the page and its middle must decode to RAM (a device window smaller than a page would show up at one of these).
        if (locate(base + o, off1) != &ram_ || locate(base + end - 1u, off2) != &ram_ || locate(base + o + (end - o) / 2u, off1) != &ram_) break;
        good = end;
    }
    ramFastBase_ = base;
    ramFastSize_ = good >= 16u ? good : 0u;
    ramLim8_ = ramFastSize_; ramLim16_ = ramFastSize_ >= 2u ? ramFastSize_ - 1u : 0u; ramLim32_ = ramFastSize_ >= 4u ? ramFastSize_ - 3u : 0u; // valid start offsets
}

SystemBus::~SystemBus() = default;

void SystemBus::enableMe() {
    if (me_) return;
    me_ = std::make_unique<MeCore>(*this, meOpts_);
    if (meOpts_.ratio > 0 && meOpts_.ratio <= 64) meRatio_ = meOpts_.ratio; // ME steps per main-core step
    meHorizonAlways_ = meOpts_.horizonAlways;
    syscon_.meLle = true;
    sysconChip_.nowUsec = [this] { return systemTime_.ticks() / SystemTime::kInstructionsPerMicrosecond; };
    syscon_.onMeReset = [this] { if (me_) me_->reset(); };
    syscon_.onInterruptToMe = [this] { if (me_) me_->interruptFromMain(); };
}

// Device time is advanced in batches of tickBatch_ instructions instead of every instruction (the per-instruction virtual calls and
// counter updates were a large share of the interpreter's cost). Devices see time at most tickBatch_-1 instructions late.
void SystemBus::tick() {
    if (++tickPending_ < tickBatch_) return;
    flushTicks();
}

void SystemBus::flushTicks() {
    const uint32_t n = tickPending_;
    if (n == 0) return;
    tickPending_ = 0;
    for (Timer& t : timers_) {
        t.advance(n);
    }
    nand_.advance(n);
    systemTime_.advance(n);
    displayController_.advance(n);
    if (me_ && me_->running()) me_->run(n * meRatio_);
    audioDivider_ += n;
    if (audioDivider_ >= 1024u) {
        audioDivider_ %= 1024u;
        const uint64_t now = systemTime_.ticks();
        audio_.poll();
        dmac0_.poll(now);
        dmac1_.poll(now);
        wlan_.poll();
        umdDrive_.poll(now);
    }
}

// Device time is applied lazily in batches (tick()/tickN() -> flushTicks()), and a flush does more than advance counters: it
// polls audio + DMA every 1024 ticks. So a block of S steps -- S tick() calls -- is only equivalent to the interpreter if no
// flush inside it (a) reaches a device event or (b) reaches an audio/DMA poll. Flushes that do neither are pure counter
// updates and advanceTicks applies them in one go. The result is in STEPS (1-based: the event lands on, and preempts, that
// step), which is the device horizon rounded up to the next flush boundary, minus the ticks already pending.
uint64_t SystemBus::ticksUntilEvent() const {
    uint64_t dev = nand_.eventHorizon();
    for (const Timer& t : timers_) dev = std::min(dev, t.eventHorizon());
    dev = std::min(dev, systemTime_.eventHorizon());
    dev = std::min(dev, displayController_.eventHorizon());
    const uint64_t batch = tickBatch_, pending = tickPending_;
    auto stepOfFlushReaching = [&](uint64_t deviceTicks) { return (deviceTicks + batch - 1) / batch * batch - pending; };
    uint64_t best = dev == UINT64_MAX ? UINT64_MAX : stepOfFlushReaching(dev);
    if (me_ && me_->running() && (meHorizonAlways_ || !me_->idleParked())) best = std::min(best, batch - pending); // the very next flush runs the ME (unless it is parked with nothing pending: then run() would return at once)
    best = std::min(best, stepOfFlushReaching(1024u - audioDivider_));  // ...and the one that carries audioDivider_ to 1024 polls audio/DMA
    return best;
}

void SystemBus::advanceTicks(uint64_t n) {
    tickPending_ += static_cast<uint32_t>(n);
    const uint32_t flushed = tickPending_ / tickBatch_ * tickBatch_;
    tickPending_ -= flushed;
    if (flushed == 0) return;
    for (Timer& t : timers_) t.fastForward(flushed);
    nand_.fastForward(flushed);
    systemTime_.fastForward(flushed);
    displayController_.fastForward(flushed);
    audioDivider_ += flushed; // ticksUntilEvent guaranteed this stays below 1024 (no audio/DMA poll was skipped)
}

Memory* SystemBus::plainMemory(uint32_t addr, uint32_t& offsetOut) const {
    const uint32_t phys = maskAddr(addr);
    const uint32_t ramBase = maskAddr(MemoryMap::kRamStart);
    if (phys >= ramBase && phys - ramBase < ram_.size()) {
        const std::size_t page = (phys - ramBase) >> 12;
        if (ramPageClass_.empty()) ramPageClass_.assign((ram_.size() >> 12) + 1, 0);
        uint8_t& cls = ramPageClass_[page];
        if (cls == 0) {
            cls = 1;
            const uint32_t pageStart = ramBase + static_cast<uint32_t>(page << 12);
            for (uint32_t o = 0; o < 4096u && cls == 1; o += 4) {
                uint32_t off = 0;
                if (locate(pageStart + o, off) != &ram_) cls = 2;
            }
        }
        if (cls == 1) { offsetOut = phys - ramBase; return const_cast<Memory*>(&ram_); }
    }
    uint32_t off = 0;
    Bus* b = locate(addr, off);
    if (b == &ram_ || b == &scratchpad_ || b == &vram_ || b == &ipl_ || b == &iplBank2_) {
        offsetOut = off;
        return static_cast<Memory*>(b);
    }
    return nullptr;
}

bool SystemBus::hasSideEffects(uint32_t addr) const {
    uint32_t off = 0;
    if (plainMemory(addr, off)) return false;
    return locate(addr, off) != nullptr; // a device register (unmapped -> nullptr -> false: the access itself will fault)
}

bool SystemBus::storeNeedsInterpreter(uint32_t addr) const {
    uint32_t off = 0;
    if (Memory* m = plainMemory(addr, off)) return m->isWatched(off, 4);
    return locate(addr, off) != nullptr;
}

void SystemBus::watchCode(uint32_t addr, uint32_t size) {
    // A translated range is contiguous in one region in practice; walk it per 32-byte line so a range straddling regions is still right.
    for (uint32_t a = addr & ~31u; a < addr + size; a += 32u) {
        uint32_t off = 0;
        if (Memory* m = plainMemory(a, off)) m->watchCode(off & ~31u, 32u);
    }
}

uint64_t SystemBus::codeWriteEpoch() const {
    return ram_.codeWriteEpoch() + scratchpad_.codeWriteEpoch() + vram_.codeWriteEpoch() + ipl_.codeWriteEpoch() + iplBank2_.codeWriteEpoch();
}

Bus* SystemBus::locate(uint32_t addr, uint32_t& offsetOut) const {
    const uint32_t phys = maskAddr(addr);

    // Fixed-size MMIO windows are checked before RAM, not after. RAM is the
    // only region here whose size is caller-configurable -- if it were
    // checked first, a RAM size that numerically reaches as far as a real
    // device's masked base address would silently swallow that device's
    // whole window instead of routing to it. Real hardware decodes fixed
    // physical windows regardless of how much of the address space is
    // actually backed by populated RAM, so the fixed windows must win here
    // too, independent of ram_.size().
    const uint32_t scratchpadBase = maskAddr(MemoryMap::kScratchpadStart);
    if (phys >= scratchpadBase && phys < scratchpadBase + scratchpad_.size()) {
        offsetOut = phys - scratchpadBase;
        return const_cast<Memory*>(&scratchpad_);
    }

    const uint32_t iplBase = maskAddr(MemoryMap::kIplStart);
    if (phys >= iplBase && phys < iplBase + MemoryMap::kIplSize) {
        offsetOut = phys - iplBase;
        return const_cast<Memory*>(&ipl_);
    }

    const uint32_t iplBank2Base = maskAddr(MemoryMap::kIplBank2Start);
    if (phys >= iplBank2Base && phys < iplBank2Base + MemoryMap::kIplBank2Size) {
        offsetOut = phys - iplBank2Base;
        return const_cast<Memory*>(&iplBank2_);
    }

    const uint32_t vramBase = maskAddr(MemoryMap::kVramStart);
    // The console's 2 MB of video memory appears four times, through 0x04000000-0x047FFFFF (the same RAM at +2 MB, +4 MB and +6 MB).
    if (phys >= vramBase && phys < vramBase + MemoryMap::kVramMirrorWindow) {
        offsetOut = (phys - vramBase) & (MemoryMap::kVramSize - 1u);
        return const_cast<Memory*>(&vram_);
    }

    const uint32_t intcBase = maskAddr(MemoryMap::kIntcBase);
    if (phys >= intcBase && phys < intcBase + MemoryMap::kIntcSize) {
        offsetOut = phys - intcBase;
        return const_cast<InterruptController*>(&intc_);
    }

    const uint32_t sysconBase = maskAddr(MemoryMap::kSysconBase);
    if (phys >= sysconBase && phys < sysconBase + MemoryMap::kSysconSize) {
        offsetOut = phys - sysconBase;
        return const_cast<SystemControl*>(&syscon_);
    }

    const uint32_t nandBase = maskAddr(MemoryMap::kNandBase);
    if (phys >= nandBase && phys < nandBase + MemoryMap::kNandSize) {
        offsetOut = phys - nandBase;
        return const_cast<NandController*>(&nand_);
    }

    const uint32_t sysTimeBase = maskAddr(MemoryMap::kSystemTimeBase);
    if (phys >= sysTimeBase && phys < sysTimeBase + MemoryMap::kSystemTimeSize) {
        offsetOut = phys - sysTimeBase;
        return const_cast<SystemTime*>(&systemTime_);
    }

    const uint32_t i2cBase = maskAddr(MemoryMap::kI2cBase);
    if (phys >= i2cBase && phys < i2cBase + MemoryMap::kI2cSize) {
        offsetOut = phys - i2cBase;
        return const_cast<I2c*>(&i2c_);
    }

    const uint32_t geEdramBase = maskAddr(MemoryMap::kGeEdramBase);
    if (phys >= geEdramBase && phys < geEdramBase + MemoryMap::kGeEdramSize) {
        offsetOut = phys - geEdramBase;
        return const_cast<GeEdramStub*>(&geEdram_);
    }
    const uint32_t geBase = maskAddr(MemoryMap::kGeBase);
    if (phys >= geBase && phys < geBase + MemoryMap::kGeSize) {
        offsetOut = phys - geBase;
        return const_cast<GeCoreStub*>(&ge_);
    }
    const uint32_t dispCtlBase = maskAddr(MemoryMap::kDisplayControllerBase);
    if (phys >= dispCtlBase && phys < dispCtlBase + MemoryMap::kDisplayControllerSize) {
        offsetOut = phys - dispCtlBase;
        return const_cast<DisplayController*>(&displayController_);
    }
    const uint32_t gpioBase = maskAddr(MemoryMap::kGpioBase);
    if (phys >= gpioBase && phys < gpioBase + MemoryMap::kGpioSize) {
        offsetOut = phys - gpioBase;
        return const_cast<Gpio*>(&gpio_);
    }
    const uint32_t sysconChipBase = maskAddr(MemoryMap::kSysconChipBase);
    if (phys >= sysconChipBase && phys < sysconChipBase + MemoryMap::kSysconChipSize) {
        offsetOut = phys - sysconChipBase;
        return const_cast<SysconChip*>(&sysconChip_);
    }

    const uint32_t ddrBase = maskAddr(MemoryMap::kDdrBase);
    if (phys >= ddrBase && phys < ddrBase + MemoryMap::kDdrSize) {
        offsetOut = phys - ddrBase;
        return const_cast<DdrController*>(&ddr_);
    }

    if (Bus* stub = locateGenericStub(phys, offsetOut)) {
        return stub;
    }

    for (std::size_t i = 0; i < timers_.size(); ++i) {
        const uint32_t timerBase = maskAddr(MemoryMap::kTimerBase[i]);
        if (phys >= timerBase && phys < timerBase + MemoryMap::kTimerSize) {
            offsetOut = phys - timerBase;
            return const_cast<Timer*>(&timers_[i]);
        }
    }

    // RAM last: it's the catch-all region and the only one whose size
    // varies, so it must not be able to shadow any fixed device window
    // above regardless of how large it's configured.
    const uint32_t ramBase = maskAddr(MemoryMap::kRamStart);
    if (phys >= ramBase && phys < ramBase + ram_.size()) {
        offsetOut = phys - ramBase;
        return const_cast<Memory*>(&ram_);
    }

    return nullptr;
}

Bus* SystemBus::locateGenericStub(uint32_t phys, uint32_t& offsetOut) const {
    struct Range { uint32_t base; uint32_t size; Bus* stub; };
    // NOTE: MemoryMap::kGeBase/kGeEdramBase are handled explicitly in
    // locateDevice() above (real command-stream semantics, not a plain
    // register-file stub), not here.
    const Range ranges[] = {
        {maskAddr(MemoryMap::kMemoryAccessControlBase), MemoryMap::kMemoryAccessControlSize, const_cast<MmioStub*>(&memoryAccessControl_)},
        {maskAddr(MemoryMap::kProfilerBase), MemoryMap::kProfilerSize, const_cast<MmioStub*>(&profiler_)},
        {maskAddr(MemoryMap::kCpuBusFrequencyBase), MemoryMap::kCpuBusFrequencySize, const_cast<MmioStub*>(&cpuBusFrequency_)},
        {maskAddr(MemoryMap::kAudioBase), MemoryMap::kAudioSize, const_cast<AudioStub*>(&audio_)},
        {maskAddr(MemoryMap::kPowerBase), MemoryMap::kPowerSize, const_cast<PowerRegs*>(&power_)},
        {maskAddr(MemoryMap::kTimer0AliasBase), MemoryMap::kTimer0AliasSize, const_cast<TimerStatusAlias*>(&timer0Alias_)},
        {maskAddr(MemoryMap::kDmacplusBase), MemoryMap::kDmacplusSize, const_cast<Dmac*>(&dmacplus_)},
        {maskAddr(MemoryMap::kUart3Base), MemoryMap::kUart3Size, const_cast<MmioStub*>(&uart3_)},
        {maskAddr(MemoryMap::kUart4Base), MemoryMap::kUart4Size, const_cast<MmioStub*>(&uart4_)},
        {maskAddr(MemoryMap::kLcdcBase), MemoryMap::kLcdcSize, const_cast<MmioStub*>(&lcdc_)},
        {maskAddr(MemoryMap::kNandPageBase), MemoryMap::kNandPageSize, const_cast<MmioStub*>(&nandPage_)},
        {maskAddr(MemoryMap::kDmac0Base), MemoryMap::kDmac0Size, const_cast<Dmac*>(&dmac0_)},
        {maskAddr(MemoryMap::kDmac1Base), MemoryMap::kDmac1Size, const_cast<Dmac*>(&dmac1_)},
        {maskAddr(MemoryMap::kMeControllerBase), MemoryMap::kMeControllerSize, const_cast<MeController*>(&meController_)},
        {maskAddr(MemoryMap::kMemoryStickBase), MemoryMap::kMemoryStickSize, const_cast<MsController*>(&memoryStick_)},
        {maskAddr(MemoryMap::kUnknownBe10Base), MemoryMap::kUnknownBe10Size, const_cast<MmioStub*>(&unknownBe10_)},
        {maskAddr(MemoryMap::kWlanBase), MemoryMap::kWlanSize, const_cast<WlanDevice*>(&wlan_)},
        {maskAddr(MemoryMap::kAta2Base), MemoryMap::kAta2Size, const_cast<Ata2Regs*>(&ata2_)},
        {maskAddr(MemoryMap::kUmdAtaBase), MemoryMap::kUmdAtaSize, const_cast<UmdAtaRegs*>(&umdAta_)},
        {maskAddr(MemoryMap::kUsbBase), MemoryMap::kUsbSize, const_cast<MmioStub*>(&usb_)},
        {maskAddr(MemoryMap::kUmdBase), MemoryMap::kUmdSize, const_cast<UmdRegs*>(&umd_)},
        {maskAddr(MemoryMap::kLcdControllerSlimBase), MemoryMap::kLcdControllerSlimSize, const_cast<LcdControllerSlim*>(&lcdControllerSlim_)},
        {maskAddr(MemoryMap::kDisplayControllerSlimBase), MemoryMap::kDisplayControllerSlimSize, const_cast<MmioStub*>(&displayControllerSlim_)},
        {maskAddr(MemoryMap::kA7f00000Base), MemoryMap::kA7f00000Size, const_cast<MmioStub*>(&a7f00000_)},
        {maskAddr(MemoryMap::kA7f80000Base), MemoryMap::kA7f80000Size, const_cast<MmioStub*>(&a7f80000_)},
        {maskAddr(MemoryMap::kKirkMmioBase), MemoryMap::kKirkMmioSize, const_cast<KirkMmio*>(&kirkMmio_)},
    };
    for (const Range& r : ranges) {
        if (phys >= r.base && phys < r.base + r.size) {
            offsetOut = phys - r.base;
            return r.stub;
        }
    }
    return nullptr;
}

void SystemBus::unmapped(uint32_t addr) const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "unmapped bus address: 0x%08X", addr);
    throw std::out_of_range(buf);
}

uint8_t SystemBus::read8(uint32_t addr) const {
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim8_) return ram_.data()[o]; }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) const_cast<SystemBus*>(this)->flushTicks(); // device registers must see exact time
    if (!dev) unmapped(addr);
    return dev->read8(offset);
}

uint16_t SystemBus::read16(uint32_t addr) const {
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim16_) { uint16_t v; std::memcpy(&v, ram_.data() + o, 2); return v; } }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) const_cast<SystemBus*>(this)->flushTicks(); // device registers must see exact time
    if (!dev) unmapped(addr);
    return dev->read16(offset);
}

uint32_t SystemBus::read32(uint32_t addr) const {
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim32_) { uint32_t v; std::memcpy(&v, ram_.data() + o, 4); return v; } }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) const_cast<SystemBus*>(this)->flushTicks(); // device registers must see exact time
    if (!dev) unmapped(addr);
    return dev->read32(offset);
}

bool SystemBus::readBlock(uint32_t addr, uint8_t* dst, std::size_t n) const {
    if (n == 0) return true;
    uint32_t off = 0, offLast = 0;
    Memory* m = plainMemory(addr, off);
    if (!m || plainMemory(addr + static_cast<uint32_t>(n) - 1u, offLast) != m || offLast != off + static_cast<uint32_t>(n) - 1u) return false;
    if (static_cast<std::size_t>(off) + n > m->size()) return false;
    std::memcpy(dst, m->data() + off, n);
    return true;
}

// Unmapped WRITES are silently discarded rather than thrown, matching real
// embedded-MIPS-SoC "open bus" behavior: a stray store past the edge of a
// real device/RAM region typically lands nowhere and has no effect, it
// doesn't trap. Unmapped READS still throw (see read8/16/32 above):
// silently fabricating read data is far more likely to paper over a real
// gap in this emulator than to reflect anything real hardware would do.
void SystemBus::write8(uint32_t addr, uint8_t value) { ++writeCounter;
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim8_) { if (ram_.anyWatched()) ram_.noteStore(o, 1); ram_.data()[o] = value; return; } }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) flushTicks();
    if (!dev) return;
    dev->write8(offset, value);
}

void SystemBus::write16(uint32_t addr, uint16_t value) { ++writeCounter;
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim16_) { if (ram_.anyWatched()) ram_.noteStore(o, 2); std::memcpy(ram_.data() + o, &value, 2); return; } }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) flushTicks();
    if (!dev) return;
    dev->write16(offset, value);
}

void SystemBus::write32(uint32_t addr, uint32_t value) { ++writeCounter;
    { const uint32_t o = (addr & MemoryMap::kAddressMask) - ramFastBase_; if (o < ramLim32_) { if (ram_.anyWatched()) ram_.noteStore(o, 4); std::memcpy(ram_.data() + o, &value, 4); return; } }
    uint32_t offset;
    Bus* dev = locate(addr, offset);
    if (!diag_ && needsExactTime(dev)) flushTicks();
    if (!dev) return;
    dev->write32(offset, value);
}

void SystemBus::loadIplImage(const uint8_t* data, std::size_t size) {
    if (size > ipl_.size()) {
        throw std::out_of_range("IPL image larger than the 1MB IPL region");
    }
    ipl_.loadBinary(0, data, size);
}

void SystemBus::loadVramImage(const uint8_t* data, std::size_t size, uint32_t offset) {
    if (static_cast<std::size_t>(offset) + size > vram_.size()) {
        throw std::out_of_range("VRAM image write exceeds the 2MB VRAM region");
    }
    vram_.loadBinary(offset, data, size);
}

} // namespace beta
