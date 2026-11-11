#pragma once

#include <algorithm>
#include <cstddef>

#include "../bus.h"
#include "../memory.h"

namespace beta {

// A plain, size-parameterized register file for a real hardware MMIO block
// this project hasn't modeled real semantics for yet (GPIO, Syscon, the
// memory-access-control block, the GE register block, etc.) -- same
// reasoning as InterruptController/SystemControl: real kernel boot code
// pokes these directly, so it just needs to accept reads/writes without
// faulting rather than get stuck on an unmapped-address error. Replace a
// given block with a dedicated class once a later phase needs its actual
// hardware behavior (the GE register block in particular will need real
// semantics for Phase 5).
class MmioStub : public Bus {
public:
    explicit MmioStub(uint32_t size) : regs_(size) {}

    uint8_t  read8(uint32_t addr) const override { return regs_.read8(addr); }
    uint16_t read16(uint32_t addr) const override { return regs_.read16(addr); }
    uint32_t read32(uint32_t addr) const override { return regs_.read32(addr); }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override { regs_.write32(addr, value); }

    // Direct byte injection for a device (NandController, ...) that needs to
    // deposit real fetched data into this stub's backing store on the CPU's
    // behalf, bypassing the ordinary bus write path. Silently clamps to
    // whatever room is actually left in this stub's declared size, rather
    // than faulting -- the real MMIO region a caller models against may be
    // smaller than a full real data structure (confirmed real case: this
    // project's own NAND-page-buffer stub is a few bytes short of a full
    // 16-byte spare area at the offset real code actually reads).
    void writeBytes(uint32_t offset, const uint8_t* data, std::size_t size) {
        if (offset >= regs_.size()) {
            return;
        }
        const std::size_t room = regs_.size() - offset;
        regs_.loadBinary(offset, data, std::min(size, room));
    }

private:
    Memory regs_;
};

} // namespace beta
