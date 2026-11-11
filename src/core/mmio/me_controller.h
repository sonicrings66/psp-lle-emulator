#pragma once

#include "generic_stub.h"

namespace beta {

// Media Engine controller at 0xBCC00000 (jpcsp MMIOHandlerMeController). Both cores use it: the ME bootstrap (me_wrapper's me_boot_code)
// writes 1 to +0x10 and spins until bit 0 reads back clear, then programs +0x70 = 1, +0x30 = 8, +0x40 = 2. The handshake completes
// instantly: +0x10 always reads 0, and those command writes are accepted without being stored.
class MeController : public MmioStub {
public:
    explicit MeController(uint32_t size) : MmioStub(size) {}

    uint32_t read32(uint32_t addr) const override {
        if (addr == 0x10u) return 0u;
        return MmioStub::read32(addr);
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x10: if (value != 1u) MmioStub::write32(addr, value); return;
            case 0x30: if (value != 8u) MmioStub::write32(addr, value); return;
            case 0x40: if (value != 2u) MmioStub::write32(addr, value); return;
            case 0x70: if (value != 1u) MmioStub::write32(addr, value); return;
            default: MmioStub::write32(addr, value); return;
        }
    }
};

} // namespace beta
