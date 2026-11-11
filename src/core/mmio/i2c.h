#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

#include "../bus.h"
#include "../memory.h"
#include "interrupt_controller.h"

namespace beta {

// Tachyon I2C master, real base 0xBE200000 (jpcsp MMIOHandlerI2c), IRQ 12
// (PSP_I2C_INTR). sceClockgen/sceAudio drive three slaves through it:
//   0xD2 CY27040 clock generator (revision/clock/spread-spectrum registers)
//   0x34 WM8750  audio codec  (9-bit registers; write-only from the driver)
//   0x94 WM1801  (128 16-bit registers; write-only)
// Protocol: guest sets the data length (0x08), then pushes bytes to 0x0C (the
// first byte is the slave address, the rest the transmit payload), then writes
// a command to 0x04: 0x87 = transmit, 0x85 = "transmit data staged", 0x8A =
// transmit-then-receive (receive address = transmit address ^ 1; response is
// read back from 0x0C). Every command completes at once by raising interrupt
// flag bit0 in register 0x28 (write-1-to-clear), which drives IRQ 12 as a level.
// Live case (2026-09-23): clockgen.prx read 0xBE20001C from an unmapped region.
class I2c : public Bus {
public:
    static constexpr int kInterruptNumber = 12;

    I2c(InterruptController& intc, uint32_t size) : intc_(intc), regs_(size) {}

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x00: case 0x04: case 0x10: case 0x14: case 0x1C: return 0;
            case 0x08: return dataLength_;
            case 0x0C: return const_cast<I2c*>(this)->readData();
            case 0x28: return interrupt_;
            default: return regs_.read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x04: startCommand(value); break;
            case 0x08: dataLength_ = value; dataIndex_ = -1; break;
            case 0x0C: writeData(value); break;
            case 0x10: case 0x14: case 0x1C: case 0x2C: break;
            case 0x28: interrupt_ &= ~value; check(); break;
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
    static constexpr int kCy27040 = 0xD2;
    static constexpr int kWm8750 = 0x34;
    static constexpr int kWm1801 = 0x94;

    void writeData(uint32_t value) {
        if (dataIndex_ < 0) {
            address_ = static_cast<int>(value & 0xFF);
        } else if (dataIndex_ < 16) {
            tx_[dataIndex_] = static_cast<int>(value & 0xFF);
        }
        ++dataIndex_;
    }
    uint32_t readData() {
        ++dataIndex_;
        return (dataIndex_ >= 0 && dataIndex_ < 16) ? static_cast<uint32_t>(rx_[dataIndex_]) : 0u;
    }
    void check() {
        if (interrupt_ & 1u) intc_.triggerInterrupt(kInterruptNumber);
        else intc_.clearInterrupt(kInterruptNumber);
    }

    void cy27040Receive() {
        switch (tx_[0] & 0xFF) {
            case 0x00: rx_[0] = 3; rx_[1] = cyRevision_; rx_[2] = cyClock_; rx_[3] = cySpread_; break;
            case 0x80: rx_[0] = cyRevision_; break;
            case 0x81: rx_[0] = cyClock_; break;
            case 0x82: rx_[0] = cySpread_; break;
            default: warn("CY27040 receive", tx_[0]); break;
        }
    }
    void cy27040Transmit() {
        switch (tx_[0] & 0xFF) {
            case 0x80: cyRevision_ = tx_[1] & 0xFF; break;
            case 0x81: cyClock_ = tx_[1] & 0xFF; break;
            case 0x82: cySpread_ = tx_[1] & 0xFF; break;
            default: warn("CY27040 transmit", tx_[0]); break;
        }
    }
    static void warn(const char* what, int cmd) {
        static int n = 0;
        if (n++ < 16) std::printf("  [i2c] unknown %s command 0x%02X\n", what, cmd & 0xFF);
    }

    void startCommand(uint32_t command) {
        switch (command) {
            case 0x85: break;
            case 0x8A:
                if ((address_ ^ 1) == kCy27040) cy27040Receive();
                else if ((address_ ^ 1) != kWm8750 && (address_ ^ 1) != kWm1801) warn("receive address", address_);
                break;
            case 0x87:
                if (address_ == kCy27040) cy27040Transmit();
                else if (address_ != kWm8750 && address_ != kWm1801) warn("transmit address", address_);
                break;
            default: warn("controller", static_cast<int>(command)); return;
        }
        interrupt_ |= 1u;
        check();
    }

    InterruptController& intc_;
    Memory regs_;
    uint32_t dataLength_ = 0;
    uint32_t interrupt_ = 0;
    int dataIndex_ = -1;
    int address_ = 0;
    std::array<int, 16> tx_{};
    std::array<int, 16> rx_{};
    int cyRevision_ = 0x04; // jpcsp: firmware 1.50 only supports revisions 4 or 8
    int cyClock_ = 0;
    int cySpread_ = 0;
};

} // namespace beta
