#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "../bus.h"
#include "../memory.h"
#include "interrupt_controller.h"

namespace beta {

// Tachyon GPIO block, real base 0xBE240000 (jpcsp MMIOHandlerGpio). Modeled
// registers: 0x00 direction, 0x04 port levels, 0x08 set-ports, 0x0C clear-
// ports, 0x10/0x14/0x18 edge/falling/rising config, 0x1C interrupt enable,
// 0x20 interrupt-triggered, 0x24 acknowledge, 0x30/0x34 capture, 0x40 input-on.
// PSP_GPIO_INTR (IRQ 4) is a LEVEL: asserted while (triggered & enabled) != 0.
// Devices drive individual port lines with setPort()/clearPort(); a rising or
// falling edge on a port whose edge-config bit is set latches its triggered
// bit. Live case (2026-09-23): sceSyscon signals "command finished" by raising
// port 4 (GPIO_PORT_SYSCON_END_CMD) and waits for the resulting IRQ 4; the old
// accept-anything stub never produced it, so syscon.prx's start blocked.
class Gpio : public Bus {
public:
    static constexpr int kInterruptNumber = 4;
    static constexpr int kPortSysconEndCmd = 0x04;

    Gpio(InterruptController& intc, uint32_t size) : intc_(intc), regs_(size) {}

    void setPort(int port) {
        const uint32_t bit = 1u << port;
        if (!(ports_ & bit)) {
            if (rising_ & bit) trigger(bit);
            ports_ |= bit;
        }
    }
    // Changes a port level without the edge detection (no interrupt latch): for a line a device holds in a state before the firmware has armed its edge interrupt.
    void setLevel(int port) { ports_ |= 1u << port; }
    void clearLevel(int port) { ports_ &= ~(1u << port); }
    void clearPort(int port) {
        const uint32_t bit = 1u << port;
        if (ports_ & bit) {
            if (falling_ & bit) trigger(bit);
            ports_ &= ~bit;
        }
    }

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x00: return output_;
            case 0x04: return ports_;
            case 0x10: return edge_;
            case 0x14: return falling_;
            case 0x18: return rising_;
            case 0x1C: return enabled_;
            case 0x20: return triggered_;
            case 0x30: return capture_;
            case 0x34: return timerCapture_;
            case 0x40: return inputOn_;
            case 0x48: return 0;
            default: return regs_.read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: output_ = value; break;
            case 0x08: for (int i = 0; i < 32; ++i) if (value & (1u << i)) setPort(i); break;
            case 0x0C: for (int i = 0; i < 32; ++i) if (value & (1u << i)) clearPort(i); break;
            case 0x10: edge_ = value; break;
            case 0x14: falling_ = value; break;
            case 0x18: rising_ = value; break;
            case 0x1C: if (enabled_ != value) { enabled_ = value; check(); } break;
            case 0x24: if (value != 0 && triggered_ != 0) { triggered_ &= ~value; check(); } break;
            case 0x30: capture_ = value; break;
            case 0x34: timerCapture_ = value; break;
            case 0x40: inputOn_ = value; break;
            default: regs_.write32(addr, value); break;
        }
    }
    uint16_t read16(uint32_t addr) const override {
        return static_cast<uint16_t>(read32(addr & ~3u) >> ((addr & 2u) * 8));
    }
    uint8_t read8(uint32_t addr) const override {
        return static_cast<uint8_t>(read32(addr & ~3u) >> ((addr & 3u) * 8));
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }

private:
    void trigger(uint32_t bit) {
        if (!(triggered_ & bit)) {
            triggered_ |= bit;
            check();
        }
    }
    void check() {
        if (triggered_ & enabled_) intc_.triggerInterrupt(kInterruptNumber);
        else intc_.clearInterrupt(kInterruptNumber);
    }

    InterruptController& intc_;
    Memory regs_;
    uint32_t output_ = 0, ports_ = 0, inputOn_ = 0, enabled_ = 0, triggered_ = 0;
    uint32_t edge_ = 0, rising_ = 0, falling_ = 0, capture_ = 0, timerCapture_ = 0;
};

} // namespace beta
