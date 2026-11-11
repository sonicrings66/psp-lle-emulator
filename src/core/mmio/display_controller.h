#pragma once

#include <cstdint>

#include "../bus.h"
#include "../memory.h"
#include "interrupt_controller.h"
#include "system_time.h"

namespace beta {

// Display controller, real base 0xBE740000 (jpcsp MMIOHandlerDisplayController).
// Source of the 60Hz VBLANK interrupt (PSP_VBLANK_INTR = IRQ 30): the display
// driver (display_01g.prx) writes register 0x04 to start the frame clock and
// then blocks until vblank fires. Register 0x08 reads the frame counter
// (frame * 286 rows), 0x04 the current row counter. Timing uses the same
// instruction-count time base as SystemTime (kInstructionsPerMicrosecond).
// Live case (2026-09-23): boot idled forever right after display_01g created
// "SceDisplayVblank" -- nothing generated IRQ 30.
class DisplayController : public Bus {
public:
    static constexpr int kInterruptNumber = 30;
    static constexpr uint64_t kDisplaySyncMicros = (1000000 + 30) / 60; // 16667
    static constexpr uint64_t kNumberDisplayRows = 286;
    static constexpr uint64_t kRowSyncMicros = (kDisplaySyncMicros + kNumberDisplayRows / 2) / kNumberDisplayRows;
    static uint64_t periodTicks() { return kDisplaySyncMicros * SystemTime::kInstructionsPerMicrosecond; }

    DisplayController(InterruptController& intc, uint32_t size) : intc_(intc), regs_(size) {}

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x04: return static_cast<uint32_t>(displaySync() + ((micros() / kRowSyncMicros + 1) % kNumberDisplayRows));
            case 0x08: return static_cast<uint32_t>(displaySync());
            case 0x20: return 0;
            default: return regs_.read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: case 0x0C: case 0x10: case 0x14: case 0x24: break;
            case 0x04:
                baseTick_ = ticks_;
                nextVblankTick_ = ticks_ + periodTicks();
                running_ = true;
                break;
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

    // Called once per Cpu::step() via SystemBus::tick().
    void tick() { advance(1); }
    void advance(uint32_t n) {
        ticks_ += n;
        if (running_ && ticks_ >= nextVblankTick_) {
            nextVblankTick_ += periodTicks();
            intc_.triggerInterrupt(kInterruptNumber);
            ++vblankCount_;
        }
    }

    // Real vblanks fired so far (0 until display_01g.prx starts the clock via
    // write32(0x04)). A host-side window should repaint on this edge, not on
    // an arbitrary fixed instruction count: the guest flips its display buffer
    // in lockstep with this same real IRQ 30, so sampling at any other cadence
    // drifts in and out of phase with the guest's own clear/draw/flip cycle --
    // observed live as the window "flashing in and out" once XMB is up.
    uint64_t vblankCount() const { return vblankCount_; }

    // See Bus::ticksUntilEvent.
    uint64_t eventHorizon() const {
        if (!running_) return UINT64_MAX;
        return nextVblankTick_ > ticks_ ? nextVblankTick_ - ticks_ : 1;
    }
    void fastForward(uint64_t n) { ticks_ += n; } // only valid for n < eventHorizon()

private:
    uint64_t micros() const { return (ticks_ - baseTick_) / SystemTime::kInstructionsPerMicrosecond; }
    uint64_t displaySync() const { return (micros() / kDisplaySyncMicros) * kNumberDisplayRows; }

    InterruptController& intc_;
    Memory regs_;
    uint64_t ticks_ = 0;
    uint64_t baseTick_ = 0;
    uint64_t nextVblankTick_ = 0;
    uint64_t vblankCount_ = 0;
    bool running_ = false;
};

} // namespace beta
