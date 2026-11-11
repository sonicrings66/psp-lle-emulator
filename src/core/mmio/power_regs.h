#pragma once

#include "generic_stub.h"

namespace beta {

// Power / serial-channel register block, real base 0xBE300000, three 0x20-byte channels (jpcsp MMIOHandlerPower's
// PowerObject). Plain storage except one behaviour: reading channel offset +0x0C returns the channel's +0x04 value
// (jpcsp getUnknown0C: unknown0C = unknown04). lowio's transfer routine (start: write cmd/+0x10/+0x14, then set
// +0x08 and +0x04 to 1; result: read +0x0C, +0x10, +0x14) polls +0x0C until nonzero -- on a plain stub it never is, and
// display_03g's init spins forever.
class PowerRegs : public MmioStub {
public:
    explicit PowerRegs(uint32_t size) : MmioStub(size) {}

    uint32_t read32(uint32_t addr) const override {
        if ((addr & 0x1Fu) == 0x0Cu) return MmioStub::read32((addr & ~0x1Fu) | 0x04u);
        return MmioStub::read32(addr);
    }
};

} // namespace beta
