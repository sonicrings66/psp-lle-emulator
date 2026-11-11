#pragma once

#include <cstdint>

#include "../bus.h"
#include "../memory.h"

namespace beta {

// The real interrupt controller at 0xBC300000 (jpcsp: MMIOHandlerInterruptMan).
// Real hardware multiplexes 64 distinct interrupt *sources* (timers, DMA,
// USB, ...) onto a single CPU-visible line -- confirmed against jpcsp's own
// source: every one of them routes through Cause's IP2 bit (`checkException()`
// calls `triggerInterruptException(..., IP2)` unconditionally). Real
// register layout for the low 32 sources (confirmed against jpcsp's
// read32/write32): offset 0x00 = triggered&enabled (the "should the CPU
// see this" view), 0x04 = triggered (raw), 0x08 = enabled (read/write).
// Writing offset 0x00 acknowledges (clears) the bits it sets -- real
// hardware's ack convention. Only sources 0-31 are modeled with real
// per-bit semantics (everything this project currently drives -- the 4
// timers -- lives at interrupt numbers 15-18); offsets 0x10 and up
// (sources 32-63) remain a plain accept-anything register file, matching
// this class's original Phase 1 stub behavior, since nothing here needs
// them yet.
class InterruptController : public Bus {
public:
    static constexpr uint32_t kSize = 0x30;

    InterruptController() : regs_(kSize) {}

    uint8_t read8(uint32_t addr) const override {
        return static_cast<uint8_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    uint16_t read16(uint32_t addr) const override {
        return static_cast<uint16_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
        case 0x00: return static_cast<uint32_t>(triggered_ & enabled_ & 0xFFFFFFFFull);
        case 0x04: return static_cast<uint32_t>(triggered_ & 0xFFFFFFFFull);
        case 0x08: return static_cast<uint32_t>(enabled_ & 0xFFFFFFFFull);
        default: return regs_.read32(addr);
        }
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
        case 0x00: // acknowledge: writing a 1 bit clears that source's trigger
            triggered_ &= ~static_cast<uint64_t>(value);
            return;
        case 0x04: return; // raw triggered state is read-only on real hardware
        case 0x08:
            enabled_ = (enabled_ & ~0xFFFFFFFFull) | static_cast<uint64_t>(value);
            return;
        default:
            regs_.write32(addr, value);
            return;
        }
    }

    // For devices (Timer, ...) to signal a real hardware event. `number`
    // is the real PSP interrupt number (e.g. 15-18 for SysTimer0-3, per
    // jpcsp's IntrManager) -- must be < 32, the only range this class
    // gives real per-bit semantics.
    void triggerInterrupt(int number) {
        triggered_ |= (uint64_t{1} << number);
    }
    void clearInterrupt(int number) { triggered_ &= ~(uint64_t{1} << number); }
    bool hasPendingEnabled() const { return (triggered_ & enabled_) != 0; }

private:
    Memory regs_; // backing store for offsets >= 0x10 only (sources 32-63, unmodeled)
    uint64_t triggered_ = 0;
    uint64_t enabled_ = 0;
};

} // namespace beta
