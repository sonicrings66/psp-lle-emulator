#pragma once

#include "generic_stub.h"

namespace beta {

// Slim/Brite LCD panel serial controller, real base 0xBE5C0000 (jpcsp MMIOHandlerLcdControllerSlim). display_0[349]g.prx
// drives the panel over this small command interface during init: write a command word to +0x08, poll +0x0C bit 2 (0x4)
// for "response ready", read the response back from +0x08 (which clears the ready bit); writing 2 to +0x04 sets +0x0C
// bit 1. Only the command/response pairs jpcsp documents are modeled; 0xC200 returns 0xD7 for the 03g (generation 3;
// 0x07 for the 07g). Without this the 03g display module spins forever waiting for the ready bit.
class LcdControllerSlim : public MmioStub {
public:
    explicit LcdControllerSlim(uint32_t size, int generation = 3) : MmioStub(size), generation_(generation) {}

    uint32_t read32(uint32_t addr) const override {
        auto* self = const_cast<LcdControllerSlim*>(this);
        switch (addr) {
            case 0x08: self->status0C_ &= ~0x4u; return response08_;
            case 0x0C: return status0C_;
            default: return MmioStub::read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: case 0x10: case 0x14: case 0x24: break;
            case 0x04: if (value == 0x2u) status0C_ |= 0x2u; break;
            case 0x08: command(value); break;
            default: MmioStub::write32(addr, value); break;
        }
    }

private:
    void command(uint32_t value) {
        response08_ = value;
        switch (value) {
            case 0x0030: case 0x1E00: case 0x2000: case 0xC100: case 0xC101: case 0xC120: case 0xC122: case 0xC123:
            case 0xC124: case 0xC12C: case 0xC13E: case 0xC140: case 0xC167: case 0xC168: case 0xC16B: case 0xC1CC:
            case 0xC1D0: case 0xCF01: case 0x30F0: case 0x3100: case 0xEF00:
                status0C_ |= 0x4u;
                break;
            case 0x3E00:
                status0C_ |= 0x4u;
                response08_ |= 0x00A0u;
                break;
            case 0xC200:
                status0C_ |= 0x4u;
                response08_ |= (generation_ == 7) ? 0x0007u : 0x00D7u;
                break;
            default: break;
        }
    }

    int generation_;
    uint32_t response08_ = 0;
    uint32_t status0C_ = 0;
};

} // namespace beta
