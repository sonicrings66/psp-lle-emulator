#pragma once

#include <cstdint>

#include "../bus.h"
#include "../memory.h"
#include "interrupt_controller.h"

namespace beta {

// Tachyon system-time block, real base 0xBC600000 (jpcsp MMIOHandlerSystemTime).
// This is the kernel's real clock AND its scheduler timer: offset 0x00 reads the
// current system time in microseconds (used by sceKernelGetSystemTime*, and by
// drivers' busy-wait timeouts), and writing offset 0x04 programs an ALARM that
// raises PSP_THREAD0_INTR (IRQ 19) when the time reaches it -- threadman's
// wake-up/time-slice interrupt. Programming a new alarm first drops any pending
// IRQ 19, exactly as jpcsp's setAlarm() does. Previously a plain accept-anything
// stub: time never advanced (every timed busy-wait spun forever) and no alarm
// ever fired (no sleeping thread could ever be woken).
//
// Time base: this interpreter has no wall clock, only instruction count via
// tick(). kInstructionsPerMicrosecond is a project choice (real Allegrex is
// ~222 instr/us at 222MHz); a smaller value makes emulated time run faster than
// real time so long boot delays cost fewer instructions.
class SystemTime : public Bus {
public:
    static constexpr int kInterruptNumber = 19; // PSP_THREAD0_INTR
    static inline uint64_t kInstructionsPerMicrosecond = 100; // settable at startup (PSP_IPUS): fewer instructions per microsecond = guest time closer to real time

    SystemTime(InterruptController& intc, uint32_t size) : intc_(intc), regs_(size) {}

    uint32_t now() const { return static_cast<uint32_t>(ticks_ / kInstructionsPerMicrosecond); }
    uint64_t ticks() const { return ticks_; }

    uint32_t read32(uint32_t addr) const override {
        if (addr == 0x00) return now();
        return regs_.read32(addr);
    }
    uint16_t read16(uint32_t addr) const override {
        if (addr < 4u) return static_cast<uint16_t>(now() >> ((addr & 2u) * 8));
        return regs_.read16(addr);
    }
    uint8_t read8(uint32_t addr) const override {
        if (addr < 4u) return static_cast<uint8_t>(now() >> (addr * 8));
        return regs_.read8(addr);
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        if (addr == 0x00) return; // time register: writes of the boot value 0 are ignored (jpcsp)
        if (addr == 0x04) {
            regs_.write32(addr, value);
            intc_.clearInterrupt(kInterruptNumber);
            const int32_t delta = static_cast<int32_t>(value - now());
            alarmTick_ = ticks_ + (delta > 0 ? static_cast<uint64_t>(delta) * kInstructionsPerMicrosecond : 0u);
            alarmArmed_ = true;
            return;
        }
        regs_.write32(addr, value);
    }

    // Called once per Cpu::step() via SystemBus::tick().
    void tick() { advance(1); }
    void advance(uint32_t n) {
        ticks_ += n;
        if (alarmArmed_ && ticks_ >= alarmTick_) {
            alarmArmed_ = false;
            intc_.triggerInterrupt(kInterruptNumber);
        }
    }

    // See Bus::ticksUntilEvent.
    uint64_t eventHorizon() const {
        if (!alarmArmed_) return UINT64_MAX;
        return alarmTick_ > ticks_ ? alarmTick_ - ticks_ : 1;
    }
    void fastForward(uint64_t n) { ticks_ += n; } // only valid for n < eventHorizon()

private:
    InterruptController& intc_;
    Memory regs_;
    uint64_t ticks_ = 0;
    uint64_t alarmTick_ = 0;
    bool alarmArmed_ = false;
};

} // namespace beta
