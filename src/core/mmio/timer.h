#pragma once

#include <array>
#include <cstdint>

#include "../bus.h"
#include "../timing_source.h"
#include "generic_stub.h"
#include "interrupt_controller.h"

namespace beta {

// One of the PSP's 4 hardware timers (real base addresses
// 0xBC500000/10/20/30). Register offsets and STATUS bit layout are the
// real ones (jpcsp: MMIOHandlerTimer) -- status @0x000 (bits[21:0]
// COUNTER_VALUE, the compare/period value the counter repeatedly counts
// down to zero from; bit22 TRIGGER_INTERRUPT; bit23 COUNTER_ACTIVE),
// counter @0x004 (the live down-counter), prescaler numerator/denominator
// @0x008/0x00C (stored but not applied to the time base here -- this
// interpreter has no real wall-clock to scale against, only instruction
// count via tick(), so exact real-time timing isn't attempted, only
// correct functional behavior: the counter counts down and the configured
// interrupt source fires on each period, same as real hardware). When
// COUNTER_ACTIVE and TRIGGER_INTERRUPT are both set, a real interrupt
// (via InterruptController, at this timer's real PSP interrupt number --
// 15-18 for timers 0-3, per jpcsp's IntrManager) fires once per period
// crossed since the counter was started, exactly matching the source it
// was ported from.
class Timer : public Bus, public TimingSource {
public:
    static constexpr uint32_t kSize = 0x10;
    static constexpr uint32_t kCounterValueMask = 0x003FFFFFu;
    static constexpr uint32_t kTriggerInterruptBit = 0x00400000u;
    static constexpr uint32_t kCounterActiveBit = 0x00800000u;
    static constexpr uint32_t kStatusValueMask = kCounterValueMask | kTriggerInterruptBit | kCounterActiveBit;

    Timer(InterruptController& intc, int interruptNumber) : intc_(intc), interruptNumber_(interruptNumber) {}

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x00: return status_;
            case 0x04: return currentCounter();
            case 0x08: return prescaleNumerator_;
            case 0x0C: return prescaleDenominator_;
            default: return 0;
        }
    }

    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: setStatus(value); break;
            case 0x04: break; // real hardware ignores writes to the counter
            case 0x08: prescaleNumerator_ = value; break;
            case 0x0C: prescaleDenominator_ = value; break;
            default: break;
        }
    }

    // Sub-word accesses to timer registers are not expected in practice;
    // these exist only so an odd-sized access doesn't fault the bus.
    uint8_t read8(uint32_t addr) const override {
        return static_cast<uint8_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    uint16_t read16(uint32_t addr) const override {
        return static_cast<uint16_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}

    // Advances this timer's own instruction-count time base by one and
    // checks whether a period has elapsed since it was started.
    void tick() override { advance(1); }
    void advance(uint32_t n) { // n instructions at once (SystemBus batches its device ticks)
        now_ += n;
        checkInterrupt();
    }

    // See Bus::ticksUntilEvent. checkInterrupt() fires once elapsed >= (firedPeriods_+1)*maxCounter(); if that is already true it fires
    // on the very next tick.
    uint64_t eventHorizon() const {
        if ((status_ & kCounterActiveBit) == 0 || (status_ & kTriggerInterruptBit) == 0 || maxCounter() == 0) return UINT64_MAX;
        const uint64_t elapsed = now_ - startTick_;
        const uint64_t target = (firedPeriods_ + 1) * maxCounter();
        return elapsed >= target ? 1 : target - elapsed;
    }
    void fastForward(uint64_t n) { now_ += n; } // only valid for n < eventHorizon()

private:
    uint32_t maxCounter() const { return status_ & kCounterValueMask; }

    uint32_t currentCounter() const {
        if ((status_ & kCounterActiveBit) == 0 || maxCounter() == 0) {
            return 0;
        }
        const uint64_t elapsed = now_ - startTick_;
        return maxCounter() - static_cast<uint32_t>(elapsed % maxCounter());
    }

    void setStatus(uint32_t value) {
        const bool wasActive = (status_ & kCounterActiveBit) != 0;
        const bool nowActive = (value & kCounterActiveBit) != 0;
        status_ = value & kStatusValueMask;
        if (nowActive && !wasActive) {
            // Starting the counter: real hardware begins counting from
            // this moment, so restart this timer's own elapsed-time base.
            startTick_ = now_;
            firedPeriods_ = 0;
        }
        checkInterrupt();
    }

    void checkInterrupt() {
        if ((status_ & kCounterActiveBit) == 0 || (status_ & kTriggerInterruptBit) == 0 || maxCounter() == 0) {
            return;
        }
        const uint64_t elapsed = now_ - startTick_;
        const uint64_t periods = elapsed / maxCounter();
        if (periods > firedPeriods_) {
            firedPeriods_ = periods;
            intc_.triggerInterrupt(interruptNumber_);
        }
    }

    InterruptController& intc_;
    int interruptNumber_;
    uint32_t status_ = 0;
    uint32_t prescaleNumerator_ = 0;
    uint32_t prescaleDenominator_ = 0;
    uint64_t now_ = 0;
    uint64_t startTick_ = 0;
    uint64_t firedPeriods_ = 0;
};

// The read-only status mirrors at 0xBC500100 + 0x10 * n (jpcsp: MMIOHandlerTimer registers every timer with an extra alias offset 0x100, where a read
// returns the status without the side effects a read of the normal status register has on real hardware). Timer n's alias reads that timer's status;
// writes are ignored. Found 2026-10-16: the reboot path stops the system timers and reads all four, and only the first was mapped (a bus error killed
// the kernel at the second).
class TimerStatusAlias : public MmioStub {
public:
    static constexpr uint32_t kSize = 0x40;
    explicit TimerStatusAlias(const std::array<Timer, 4>& timers) : MmioStub(kSize), timers_(timers) {}
    uint32_t read32(uint32_t addr) const override { return (addr & 0xCu) == 0 ? timers_[(addr >> 4) & 3u].read32(0) : 0u; }
    uint16_t read16(uint32_t addr) const override { return static_cast<uint16_t>(read32(addr & ~3u) >> ((addr & 2u) * 8)); }
    uint8_t read8(uint32_t addr) const override { return static_cast<uint8_t>(read32(addr & ~3u) >> ((addr & 3u) * 8)); }
    void write8(uint32_t, uint8_t) override {}
    void write16(uint32_t, uint16_t) override {}
    void write32(uint32_t, uint32_t) override {}
private:
    const std::array<Timer, 4>& timers_;
};

} // namespace beta
