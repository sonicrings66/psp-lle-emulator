#pragma once

#include <cstdint>

// Minimal read/write/interrupt contract -- split out of the old project's
// bus.h (see docs/PORTING_LOG.md §1a), which had grown to 15+ virtuals
// conflating four unrelated concerns onto one interface. This is the
// ONLY part every addressable thing (plain RAM, the IPL region, every
// MMIO device) needs to implement. A test fixture bus implements only
// this -- it no longer has to inherit or silently default a dozen
// JIT/timing/syscall methods it doesn't care about.
//
// TimingSource, JitHints, and SyscallResolver (separate headers in this
// same directory) are optional mixins: SystemBus composes all four via
// multiple inheritance, but nothing requires a Bus to be any of them.

namespace beta {

class Bus {
public:
    virtual ~Bus() = default;

    virtual uint8_t  read8(uint32_t addr) const = 0;
    virtual uint16_t read16(uint32_t addr) const = 0;
    virtual uint32_t read32(uint32_t addr) const = 0;

    virtual void write8(uint32_t addr, uint8_t value) = 0;
    virtual void write16(uint32_t addr, uint16_t value) = 0;
    virtual void write32(uint32_t addr, uint32_t value) = 0;

    // Bitmask of real MIPS32 hardware interrupt request lines this bus's
    // devices currently assert (bit n = IRQ line n, matching Cause
    // register bits [15:8] once the CPU masks this against Status.IM).
    // A device with no real interrupt-source modeling (the default)
    // never asserts anything; this exists so Cpu::step() can check for a
    // pending interrupt through the same Bus reference it already holds,
    // without depending on any concrete device type.
    virtual uint32_t pendingInterrupts() const { return 0; }
};

} // namespace beta
