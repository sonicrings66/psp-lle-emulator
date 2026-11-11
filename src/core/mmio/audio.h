#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "generic_stub.h"
#include "interrupt_controller.h"
#include "system_time.h"
#include "../../config/options.h"

namespace beta {

// Audio controller, real base 0xBE000000 (jpcsp MMIOHandlerAudio). The guest's audio.prx DMAs 32-bit stereo sample words
// (low 16 bits = left, high 16 bits = right) into 0xBE000060 (line 0) and 0xBE000070 (line 1). Modeled:
//   0x00 busy (R/W), 0x0C in-progress (one bit per line, set by a start write to 0x08 for each line whose bit is CLEAR in the
//   written value, cleared by a stop write to 0x04 whose bit is clear), 0x1C interrupt-pending, 0x24 interrupt-enable (masks
//   pending), 0x28 status = 0x37 (bits 4/5 = ready -- audio.prx spins until (status & 0x30) == 0x30), 0x38/0x3C/0x44 frequency
//   selectors, 0x40 frequency flags, 0x50 volume (reads 0), 0x60/0x70 sample FIFOs.
// Timing: each line drains at its sample rate against EMULATED time (SystemTime ticks, 100 instructions per microsecond). When
// the hardware buffer runs empty and the line's interrupt is enabled, IRQ 10 is raised (jpcsp PSP_AUDIO_INTR) -- this is what
// audio.prx's output thread waits on. A DMA into a line whose FIFO already holds more than ~100 ms of audio completes late
// (Dmac pacing hook), like the real hardware blocking the transfer.
// Output: with AudioOptions::wavDumpDir set (was PSP_AUDIO_OUT), every line is written to <dir>/audio_line<N>.wav (16-bit stereo) as it plays.
class AudioStub : public MmioStub {
public:
    static constexpr int kIrq = 10;
    static uint64_t ticksPerSecond() { return SystemTime::kInstructionsPerMicrosecond * 1000000ull; }

    explicit AudioStub(uint32_t size, const config::AudioOptions& opts = {})
        : MmioStub(size), outDir_(opts.wavDumpDir), dmaMs_(opts.dmaMs) {}
    ~AudioStub() { for (int i = 0; i < 2; ++i) flushWav(i); }

    void attach(InterruptController* intc, const SystemTime* time) { intc_ = intc; time_ = time; }

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x00: return busy_;
            case 0x0C: return inProgress_;
            case 0x1C: return interrupt_;
            case 0x28: return 0x37u;
            case 0x40: return frequencyFlags_;
            case 0x50: return 0;
            default: return MmioStub::read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: busy_ = value; break;
            case 0x04:
                for (int i = 0; i < 3; ++i) {
                    const uint32_t bit = 1u << i;
                    if ((inProgress_ & bit) && !(value & bit)) {
                        inProgress_ &= ~bit;
                        interrupt_ &= ~bit;
                    }
                }
                checkInterrupt();
                break;
            case 0x08:
                for (int i = 0; i < 2; ++i) {
                    if (!(value & (1u << i))) { inProgress_ |= 1u << i; line_[i].started = true; }
                }
                break;
            case 0x24: enabled_ = value; interrupt_ &= value; checkInterrupt(); break;
            case 0x40: frequencyFlags_ = value; break;
            case 0x44: hwFrequency_ = value; break;
            case 0x60: sendSample(0, value); break;
            case 0x70: sendSample(1, value); break;
            default: MmioStub::write32(addr, value); break;
        }
    }

    // Called every ~1000 emulated instructions (SystemBus::tick): drains the lines and raises the buffer-empty interrupt.
    void poll() {
        if (sink_ && (!sinkBuf_[0].empty() || !sinkBuf_[1].empty())) {
            // Both lines reach the speakers (line 0: the XMB's own sounds, line 1: what the music player and games play); the hardware adds them.
            std::vector<int16_t>& a = sinkBuf_[0]; std::vector<int16_t>& b = sinkBuf_[1];
            if (b.empty()) sink_(a.data(), a.size() / 2u, frequency());
            else if (a.empty()) sink_(b.data(), b.size() / 2u, frequency());
            else {
                std::vector<int16_t>& m = a.size() >= b.size() ? a : b; const std::vector<int16_t>& o = a.size() >= b.size() ? b : a;
                for (size_t k = 0; k < o.size(); ++k) { const int v = static_cast<int>(m[k]) + o[k]; m[k] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
                sink_(m.data(), m.size() / 2u, frequency());
            }
            a.clear(); b.clear();
        }
        if (!time_) return;
        const uint64_t now = time_->ticks();
        for (int i = 0; i < 2; ++i) {
            Line& l = line_[i];
            advance(l, now);
            if (l.started && (enabled_ & (1u << i)) && l.queued <= 0.0 && !(interrupt_ & (1u << i))) {
                interrupt_ |= 1u << i;
                l.stalled = true; l.zerosSkipped = 0; // the driver restarts the line with 24 zero samples (jpcsp)
                checkInterrupt();
            }
        }
    }

    // Dmac pacing hook: returns the emulated tick at which a transfer of `bytes` into line `dstPhys` may complete (0 = now).
    uint64_t dmaDue(uint32_t dstPhys, uint32_t bytes) {
        (void)bytes;
        if (!time_) return 0;
        Line& l = line_[(dstPhys & 0xF0u) == 0x70u ? 1 : 0];
        const uint64_t now = time_->ticks();
        advance(l, now);
        // How much audio may wait in the hardware before the DMA completion (and so the driver's buffer release) is held back. A real FIFO holds a few dozen samples, so a
        // block's DMA completes about when it has been played; completing at once made the driver reuse a slot whose data had not been sent (the music player's second
        // block of every pair was lost). PSP_AUDIO_DMA_MS overrides it (default 100 = the old behaviour while this is being tested).
        const double threshold = fifoSamples();
        if (l.queued <= threshold) return 0;
        return now + static_cast<uint64_t>((l.queued - threshold) / frequency() * static_cast<double>(ticksPerSecond()));
    }

    // Dmac start-pacing hook: the tick at which the line's FIFO has room for the next descriptor's data (0 = now). The FIFO holds a few dozen samples.
    uint64_t dmaStartDue(uint32_t dstPhys) {
        if (!time_) return 0;
        Line& l = line_[(dstPhys & 0xF0u) == 0x70u ? 1 : 0];
        const uint64_t now = time_->ticks();
        advance(l, now);
        const double depth = fifoSamples();
        if (l.queued <= depth + 1.0) return 0;
        return now + static_cast<uint64_t>((l.queued - depth) / frequency() * static_cast<double>(ticksPerSecond())) + 1u;
    }

    uint64_t samplesReceived(int line) const { return line_[line].total; }

    // Live playback: both lines' samples (interleaved L,R int16, summed) are handed to `sink` in small batches from poll().
    using Sink = std::function<void(const int16_t* stereo, size_t frames, uint32_t rate)>;
    void setSink(Sink s) { sink_ = std::move(s); }

private:
    struct Line {
        std::vector<int16_t> pcm; // interleaved L,R
        double queued = 0.0;      // samples still "in the hardware FIFO"
        uint64_t lastTick = 0, total = 0;
        bool started = false, stalled = true;
        int zerosSkipped = 0;
        uint64_t flushedAt = 0;
    };

    double fifoSamples() const { return frequency() * dmaMs_ / 1000.0; }
    uint32_t frequency() const {
        switch (hwFrequency_) {
            case 0x01: return 8000; case 0x02: return 11025; case 0x04: return 12000; case 0x08: return 16000;
            case 0x10: return 22050; case 0x20: return 24000; case 0x40: return 32000; case 0x80: return 44100;
            case 0x100: return 48000;
            default: return 44100;
        }
    }
    void advance(Line& l, uint64_t now) {
        if (now > l.lastTick) {
            l.queued -= static_cast<double>(now - l.lastTick) * frequency() / static_cast<double>(ticksPerSecond());
            if (l.queued < 0.0) l.queued = 0.0;
        }
        l.lastTick = now;
    }
    void sendSample(int i, uint32_t value) {
        Line& l = line_[i];
        if (time_) advance(l, time_->ticks());
        if (l.stalled) { // jpcsp: after a stall the driver pushes 24 silent words that are not real audio
            if (value == 0) { if (++l.zerosSkipped >= 24) l.stalled = false; return; }
            l.stalled = false;
        }
        l.queued += 1.0; ++l.total;
        if (sink_) { sinkBuf_[i].push_back(static_cast<int16_t>(value & 0xFFFFu)); sinkBuf_[i].push_back(static_cast<int16_t>(value >> 16)); }
        if (!outDir_.empty()) {
            l.pcm.push_back(static_cast<int16_t>(value & 0xFFFFu));
            l.pcm.push_back(static_cast<int16_t>(value >> 16));
            if (l.total - l.flushedAt >= 88200u) flushWav(i);
        }
    }
    void checkInterrupt() {
        if (!intc_) return;
        if (interrupt_ != 0) intc_->triggerInterrupt(kIrq); else intc_->clearInterrupt(kIrq);
    }
    void flushWav(int i) {
        Line& l = line_[i];
        if (outDir_.empty() || l.pcm.empty()) return;
        l.flushedAt = l.total;
        const std::string path = outDir_ + "/audio_line" + std::to_string(i) + ".wav";
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        const uint32_t rate = frequency(), dataBytes = static_cast<uint32_t>(l.pcm.size() * 2u);
        auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f); w32(36u + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
        w32(16u); w16(1u); w16(2u); w32(rate); w32(rate * 4u); w16(4u); w16(16u);
        std::fwrite("data", 1, 4, f); w32(dataBytes);
        std::fwrite(l.pcm.data(), 2, l.pcm.size(), f);
        std::fclose(f);
    }

    InterruptController* intc_ = nullptr;
    const SystemTime* time_ = nullptr;
    std::string outDir_;
    double dmaMs_ = 2.0;
    Line line_[2];
    Sink sink_;
    std::vector<int16_t> sinkBuf_[2];
    uint32_t busy_ = 0, inProgress_ = 0, interrupt_ = 0, enabled_ = 0, frequencyFlags_ = 0, hwFrequency_ = 0;
};

} // namespace beta
