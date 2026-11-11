#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "generic_stub.h"
#include "interrupt_controller.h"

namespace beta {

// DMA controller (real bases 0xBC900000 = DMAC0 -> IRQ 22, 0xBCA00000 = DMAC1 -> IRQ 23). Ported from jpcsp's
// MMIOHandlerDmac / DmacProcessor / DmacThread (the register layout, the attribute bit fields and the linked-list
// walk are theirs; jpcsp runs each job on a worker thread, this model runs it immediately when the guest starts it).
//   0x004 completed flags (one bit per channel), 0x008 write: clear completed flags,
//   0x00C error flags, 0x010 write: clear error flags,
//   per channel i (8 of them) at 0x100 + i*0x20: +0 src, +4 dst, +8 next descriptor, +0C attributes, +10 status.
// Starting a transfer = writing the status register with bit 0 (IN_PROGRESS) newly set. Attribute bits: length in
// units 0..11, source/destination step size 12..17, length shifts 18..23, increment flags 26/27, interrupt bit 31.
// Not modeled: the DDR flush handshake (transfers that require it just run). Transfers into an audio FIFO are paced to the playback rate (startHook, pacingHook).
class Dmac : public MmioStub {
public:
    struct Memory {
        std::function<uint8_t(uint32_t)> r8;
        std::function<uint32_t(uint32_t)> r32;
        std::function<void(uint32_t, uint8_t)> w8;
        std::function<void(uint32_t, uint32_t)> w32;
    };

    // chBase/numCh: where the channel register banks start and how many there are (DMAC0/1: 0x100 x8; DMACPLUS:
    // 0x180 x3 = SC->ME, ME->SC, SC128 memcpy). flags[i] is the "completed" bit channel i raises (default 1<<i).
    Dmac(uint32_t size, int irq, uint32_t chBase = 0x100, uint32_t numCh = 8, std::vector<uint32_t> flags = {})
        : MmioStub(size), irq_(irq), chBase_(chBase), numCh_(numCh), flags_(std::move(flags)) {}
    void attach(InterruptController* intc, Memory mem) { intc_ = intc; mem_ = std::move(mem); }

    uint64_t transfers = 0, bytesMoved = 0;

    // Pacing hook for transfers whose destination is a real-time sink (the audio FIFO): returns the emulated tick at which the
    // completion flag/interrupt may be raised (0 = immediately). The data itself is copied at once.
    std::function<uint64_t(uint32_t dstPhys, uint32_t bytes)> pacingHook;
    std::function<uint64_t()> nowFn;
    // Start pacing for the same sinks: returns the emulated tick at which the sink has room for the next descriptor (0 = now). A transfer into the audio FIFO
    // then proceeds at the playback rate, the way the real controller stalls on a full FIFO, so the channel's position (and what the driver reads from it in
    // its completion callback) is where the hardware's would be; the audio driver tells which of its two buffers has finished from that position.
    std::function<uint64_t(uint32_t dstPhys)> startHook;
    // A transfer reading from a device port that is not ready yet waits (the channel stays busy) instead of copying what is not there: on the real bus the DMA
    // engine waits for the device, here the driver sets the transfer up before it issues the device command (the WLAN data port). poll() retries held channels.
    std::function<bool(uint32_t srcPhys)> sourceReady;
    // The same for a destination port that cannot take data yet (the WLAN data port before its write command): retryHeld() runs the held transfers whose port is ready now.
    std::function<bool(uint32_t dstPhys)> destReady;
    void retryHeld() {
        for (uint32_t i = 0; i < 8; ++i) if (held_[i]) { if (!(ch_[i].status & 1u)) held_[i] = false; else if (!blocked(i)) { held_[i] = false; run(i); } }
    }
    // Raises completions whose due tick has passed (called every ~1000 emulated instructions).
    void poll(uint64_t now) {
        retryHeld();
        for (uint32_t i = 0; i < 8; ++i) if (paced_[i].active && paced_[i].due <= now) {
            paced_[i].active = false;
            if (paced_[i].first) runNow(i); else chain(i, paced_[i].nextLocal);
        }
        for (uint32_t i = 0; i < 8; ++i) if (wait_[i].active && mem_.r32 && mem_.r32(wait_[i].desc + 8) != 0) { wait_[i].active = false; chain(i, mem_.r32(wait_[i].desc + 8)); }
        for (size_t i = 0; i < deferred_.size();) {
            if (deferred_[i].due <= now) { completed_ |= deferred_[i].bit; deferred_.erase(deferred_.begin() + static_cast<long>(i)); checkIrq(); }
            else ++i;
        }
    }

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x004: return completed_;
            case 0x00C: return error_;
            default: break;
        }
        if (addr >= chBase_ && addr < chBase_ + numCh_ * 0x20u && ((addr - chBase_) & 0x1Fu) <= 0x10u) {
            const Channel& c = ch_[(addr - chBase_) >> 5];
            switch ((addr - chBase_) & 0x1Fu) {
                case 0x00: return c.src;
                case 0x04: return c.dst;
                case 0x08: return c.next;
                case 0x0C: return c.attr;
                case 0x10: return c.status;
                default: break;
            }
        }
        return MmioStub::read32(addr);
    }

    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x008: completed_ &= ~value; checkIrq(); return;
            case 0x010: error_ &= ~value; return;
            default: break;
        }
        if (addr >= chBase_ && addr < chBase_ + numCh_ * 0x20u && ((addr - chBase_) & 0x1Fu) <= 0x10u) {
            const uint32_t i = (addr - chBase_) >> 5;
            Channel& c = ch_[i];
            switch ((addr - chBase_) & 0x1Fu) {
                case 0x00: c.src = value; return;
                case 0x04: c.dst = value; return;
                case 0x08: c.next = value; return;
                case 0x0C: c.attr = value; return;
                case 0x10: {
                    const uint32_t prev = c.status;
                    c.status = value;
                    if ((prev & 1u) && !(value & 1u)) { wait_[i].active = false; paced_[i].active = false; held_[i] = false; } // driver aborts the (waiting) job
                    if (!(prev & 1u) && (value & 1u)) run(i);
                    return;
                }
                default: break;
            }
        }
        MmioStub::write32(addr, value);
    }

private:
    struct Channel { uint32_t src = 0, dst = 0, next = 0, attr = 0, status = 0; };
    Channel ch_[8];
    uint32_t completed_ = 0, error_ = 0;
    int irq_;
    uint32_t chBase_, numCh_;
    std::vector<uint32_t> flags_;
    InterruptController* intc_ = nullptr;
    Memory mem_;
    struct Deferred { uint64_t due; uint32_t bit; };
    std::vector<Deferred> deferred_;
    struct Wait { bool active = false; uint32_t desc = 0; };
    bool blocked(uint32_t i) const { return (sourceReady && !sourceReady(phys(ch_[i].src))) || (destReady && !destReady(phys(ch_[i].dst))); }
    bool held_[8] = {};
    struct Paced { bool active = false, first = false; uint64_t due = 0; uint32_t nextLocal = 0; };
    Paced paced_[8]; // channels waiting for the audio FIFO to have room for their next descriptor (see startHook)
    Wait wait_[8]; // channels parked on the last descriptor of a list the driver is still extending (jpcsp DmacThread: status bit 0x100 = requires DDR flush)

    void checkIrq() {
        if (!intc_) return;
        if (completed_ != 0) intc_->triggerInterrupt(irq_); else intc_->clearInterrupt(irq_);
    }
    static uint32_t phys(uint32_t a) { return a & 0x1FFFFFFFu; }

    // One descriptor (jpcsp DmacThread.dmacMemcpyStep). Returns false on an unsupported configuration.
    bool step(uint32_t chIndex) {
        Channel& c = ch_[chIndex];
        static const uint32_t stepLen[8] = {2, 16, 8, 4, 0, 0, 0, 0}; // step field: 0=2, 1=16, 2=8, 3=4 bytes
        const uint32_t attr = c.attr;
        uint32_t srcStep = stepLen[(attr >> 12) & 7u], dstStep = stepLen[(attr >> 15) & 7u];
        uint32_t srcShift = (attr >> 18) & 7u, dstShift = (attr >> 21) & 7u;
        bool srcInc = (attr & 0x04000000u) != 0, dstInc = (attr & 0x08000000u) != 0;
        const uint32_t length = attr & 0xFFFu;
        if (attr & 0x02000000u) { srcInc = dstInc = true; srcStep = dstStep = 1; srcShift = dstShift = 0; }
        if (attr & 0x01000000u) { srcInc = dstInc = true; }
        if (srcStep == 0 || dstStep == 0 || (length << srcShift) != (length << dstShift)) { static int nBad = 0; if (nBad++ < 30) std::printf("  [DMAC] unsupported transfer irq=%d ch=%u src=%08X dst=%08X attr=%08X\n", irq_, chIndex, c.src, c.dst, attr); }
        if (srcStep == 0 || dstStep == 0) return false;
        const uint32_t srcLen = length << srcShift, dstLen = length << dstShift;
        if (srcLen != dstLen) return false;
        const uint32_t unit = std::min(srcStep, dstStep);
        uint32_t s = phys(c.src), d = phys(c.dst);
        const uint32_t dst0 = d;
        const bool toFifo = (dst0 & ~0xFu) == 0x1E000060u || (dst0 & ~0xFu) == 0x1E000070u; // the audio FIFO is a single port: every word goes to the same register, whatever the increment bit says
        if (toFifo) dstInc = false;
        uint32_t remaining = std::min(srcLen, dstLen);
        while (remaining >= unit && unit > 0) {
            if (unit == 4 || unit == 8 || unit == 16) { // jpcsp DmacThread.dmacMemcpy: word copies; a non-incrementing side keeps hitting the same address (audio FIFO)
                for (uint32_t k = 0; k < unit; k += 4) mem_.w32(d + (dstInc ? k : 0u), mem_.r32(s + (srcInc ? k : 0u)));
            }
            else if (unit == 4) mem_.w32(d, mem_.r32(s));
            else for (uint32_t b = 0; b < unit; ++b) mem_.w8(d + (dstInc ? b : b), mem_.r8(s + (srcInc ? b : b)));
            remaining -= unit;
            if (srcInc) s += unit;
            if (dstInc) d += unit;
            bytesMoved += unit;
        }
        if (length > 0) { // jpcsp DmacThread: after a step the registers show the transfer as consumed (drivers read them to learn how much is left)
            if (srcInc) c.src += (length - 1) << srcShift;
            if (dstInc) c.dst += (length - 1) << dstShift;
            c.attr = attr & 0xFFFFF000u;
        }
        if (attr & 0x80000000u) {
            const uint32_t bit = chIndex < flags_.size() ? flags_[chIndex] : (1u << chIndex);
            const bool audioDst = pacingHook && (dst0 & ~0xFu) >= 0x1E000060u && (dst0 & ~0xFu) <= 0x1E000070u;
            const uint64_t due = audioDst ? pacingHook(dst0, std::min(srcLen, dstLen)) : 0;
            if (due != 0) deferred_.push_back({due, bit});
            else { completed_ |= bit; checkIrq(); }
        }
        return true;
    }

    static bool isAudioFifo(uint32_t dst) { const uint32_t d = phys(dst) & ~0xFu; return d == 0x1E000060u || d == 0x1E000070u; }

    void run(uint32_t chIndex) {
        if (!mem_.r32) { ch_[chIndex].status &= ~1u; return; }
        Channel& c = ch_[chIndex];
        if (blocked(chIndex)) { held_[chIndex] = true; return; }
        if (startHook && isAudioFifo(c.dst)) {
            const uint64_t due = startHook(phys(c.dst));
            if (due != 0) { paced_[chIndex] = {true, true, due, c.next}; return; }
        }
        runNow(chIndex);
    }

    void runNow(uint32_t chIndex) {
        Channel& c = ch_[chIndex];
        ++transfers;
        const uint32_t nextLocal = c.next; // first the transfer programmed in the registers, then the descriptor list (jpcsp DmacThread)
        if (step(chIndex)) {
            if (nextLocal != 0) c.next = mem_.r32(phys(nextLocal) + 8);
            chain(chIndex, nextLocal);
            return;
        }
        c.status &= ~1u;
    }

    // Walks the descriptor list from `nextLocal`. A list whose last descriptor has next == 0 and whose status requires a DDR flush (the audio
    // driver) is still being extended by the driver: the channel stays busy and poll() resumes it once the next pointer appears.
    void chain(uint32_t chIndex, uint32_t nextLocal) {
        Channel& c = ch_[chIndex];
        while (nextLocal != 0) {
            const uint32_t n = phys(nextLocal);
            if (startHook && isAudioFifo(mem_.r32(n + 4))) {
                const uint64_t due = startHook(phys(mem_.r32(n + 4)));
                if (due != 0) { paced_[chIndex] = {true, false, due, nextLocal}; c.next = nextLocal; return; }
            }
            c.src = mem_.r32(n + 0); c.dst = mem_.r32(n + 4); c.attr = mem_.r32(n + 12);
            if (!step(chIndex)) break;
            nextLocal = mem_.r32(n + 8);
            c.next = nextLocal;
            if (nextLocal == 0 && (c.status & 0x100u)) { wait_[chIndex].active = true; wait_[chIndex].desc = n; return; }
        }
        c.status &= ~1u; // transfer finished
    }
};

} // namespace beta
