#pragma once

#include <cstdint>

#include "../bus.h"
#include "../memory.h"

namespace beta {

// Minimal, real-ish DDR/SDRAM memory-controller behavior (real base
// 0xBD000000, jpcsp's MMIOHandlerDdr/sceDdr). Real reboot.prx code (uofw's
// src/syscon/power.S, sub_0000) writes a channel id to the flush/command
// register (offset 0x04) to trigger a write-buffer flush, then polls that
// same register's low nibble for the pending-flush bits to clear, then
// polls the status register (offset 0x00) bit 0x200 for a busy/PLL-
// changing flag to clear, before proceeding (a real DDR-flush-before-
// power-down sequence). jpcsp's own handler completes this synchronously
// (no modeled latency); matching that here keeps every register a plain
// accept-anything store EXCEPT the command register, whose real
// "operation in progress" bits this project models as completing
// immediately rather than staying set forever (a generic MmioStub would
// otherwise turn a real, normally-instant flush into a permanent poll
// loop, since nothing would ever clear the bits it just stored).
class DdrController : public Bus {
public:
    static constexpr uint32_t kStatusOffset = 0x00;
    static constexpr uint32_t kCommandOffset = 0x04;

    explicit DdrController(uint32_t size) : regs_(size) {}

    uint8_t read8(uint32_t addr) const override { return regs_.read8(addr); }
    uint16_t read16(uint32_t addr) const override { return regs_.read16(addr); }
    uint32_t read32(uint32_t addr) const override { return regs_.read32(addr); }

    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        if (addr == kCommandOffset) {
            // Real code polls this register's own low nibble for the
            // pending-flush bits it just wrote to clear -- modeling the
            // flush as instantaneous means never actually storing the
            // channel-id bits real code expects to see clear moments later.
            regs_.write32(addr, 0);
            return;
        }
        regs_.write32(addr, value);
    }

private:
    Memory regs_;
};

} // namespace beta
