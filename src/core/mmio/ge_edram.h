#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <vector>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#include "../gpu/ge_capture.h"
#include "../gpu/ge_renderer.h"
#include "generic_stub.h"
#include "interrupt_controller.h"
#include "../../config/options.h"

namespace beta {

// GE embedded-VRAM (eDRAM) control block, real base 0xBD500000. Modeled on
// jpcsp's MMIOHandlerGeEdram (the only known reference that LLE-boots real
// firmware): register 0x10 is a self-clearing trigger -- the GE driver
// (ge.prx) writes 1 and then spins until bit 0 clears, and jpcsp's handler
// makes it always read back 0 (writes of 0/1/2 are commands, not stored).
// Register 0x00 resets to 0x00012223. Everything else stays a plain register
// file, same as the generic stub. Live case (2026-09-23): boot stalled
// forever at 0x8818D560 polling this register against a plain stub.
class GeEdramStub : public MmioStub {
public:
    explicit GeEdramStub(uint32_t size) : MmioStub(size) { MmioStub::write32(0x00, 0x00012223u); }
    uint32_t read32(uint32_t addr) const override {
        if (addr == 0x10) return 0u;
        return MmioStub::read32(addr);
    }
    void write32(uint32_t addr, uint32_t value) override {
        if (addr == 0x10 && value <= 2u) return;
        MmioStub::write32(addr, value);
    }
};

// GE core register block, real base 0xBD400000. Modeled on jpcsp's MMIOHandlerGe (register layout)
// and uofw's ge.c (interrupt protocol). MINIMAL: display lists are walked (JUMP/CALL/RET/BASE,
// stall address) but nothing is rendered; SIGNAL/FINISH/END raise the same status bits and IRQ 25
// the real engine would, so the real ge.prx driver's queue/sync logic completes. Documented HLE
// stand-in for the rendering half of the GE; a real command interpreter/renderer replaces it later.
//   0x100 exec/ctrl (bit0 = running), 0x108 list address, 0x10C stall address,
//   0x304 interrupt status (SIG=1, END=2, FIN=4, ERR=8; the handler reads it),
//   0x308 write: clear interrupt bits, 0x30C write: toggle interrupt bits,
//   0x310 write: toggle status bits (also drops the matching interrupt bits).
// The GE interrupt (IRQ 25) is asserted while an END interrupt bit is pending.
class GeCoreStub : public MmioStub {
public:
    using ReadFn = std::function<uint32_t(uint32_t)>;
    explicit GeCoreStub(uint32_t size, const config::GeOptions& opts = {}) : MmioStub(size), renderer_(opts) {}
    void attach(InterruptController* intc, ReadFn readMem, GeMemory mem) { intc_ = intc; readMem_ = std::move(readMem); renderer_.attach(std::move(mem)); }
    GeRenderer& renderer() { return renderer_; }
    // Bus hooks for the GPU-resident frame buffer (see GeRenderer::vramAccess).
    bool gpuHookActive() const { return renderer_.gpuHookActive(); }
    void onVramAccess(uint32_t phys, uint32_t n, bool write) { renderer_.vramAccess(phys, n, write); }

    // ---- GE capture (see gpu/ge_capture.h): record the command stream and the memory it touches, to replay later without the guest CPU ----
    bool captureActive() const { return cap_ != nullptr; }
    // listLimit: stop and save automatically after that many lists (0 = run until endCapture()).
    bool beginCapture(const std::string& path, uint64_t listLimit = 0) {
        if (cap_) return false;
        renderer_.syncToMemory(); // a GPU-resident frame buffer must be in memory before the recorder starts observing it
        capPath_ = path; capLimit_ = listLimit;
        const GeRenderer::WinInfo wins[2] = {renderer_.windowInfo(0), renderer_.windowInfo(1)};
        cap_ = std::make_unique<GeCapture>(wins, 2, renderer_.memory());
        std::vector<uint8_t> st; renderer_.exportState(st);
        cap_->begin(st);
        renderer_.beginObservedMemory(cap_->wrap());
        return true;
    }
    // Stops recording and writes the file; a one-line summary is returned through `info` (may be null).
    bool endCapture(std::string* info = nullptr) {
        if (!cap_) return false;
        renderer_.endObservedMemory();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%llu commands, %llu lists, %llu pages (%llu change patches), %.1f MB",
                      static_cast<unsigned long long>(cap_->commands()), static_cast<unsigned long long>(cap_->lists()), static_cast<unsigned long long>(cap_->pagesLogged()),
                      static_cast<unsigned long long>(cap_->patches()), static_cast<double>(cap_->bytes()) / (1024.0 * 1024.0));
        if (info) *info = buf;
        const bool ok = cap_->save(capPath_);
        cap_.reset();
        lastCaptureOk = ok; lastCaptureInfo = buf; lastCapturePath = capPath_;
        return ok;
    }
    bool lastCaptureOk = false; std::string lastCaptureInfo, lastCapturePath; // result of the most recent capture (also set by the automatic stop)

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x100: return exec_;
            case 0x108: return list_;
            case 0x10C: return stall_;
            case 0x304: return status_;
            case 0x308: return interrupt_;
            default: return MmioStub::read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x100: {
                const bool starting = (value & 1u) != 0 && (exec_ & 1u) == 0;
                exec_ = value;
                if (starting) { depth_ = 0; base_ = 0; run(); }
                return;
            }
            case 0x108: list_ = value; return;
            case 0x10C: stall_ = value; if (exec_ & 1u) run(); return;
            case 0x304: status_ = value; interrupt_ &= status_; checkIrq(); return;
            case 0x308: interrupt_ &= ~value; checkIrq(); return;
            case 0x30C: interrupt_ ^= value; checkIrq(); return;
            case 0x310: status_ ^= value; interrupt_ &= status_; checkIrq(); return;
            default: MmioStub::write32(addr, value); return;
        }
    }
    uint64_t listsCompleted = 0, cmdsWalked = 0, geRuns = 0;
    double geSeconds = 0.0; // wall time spent walking display lists (includes rasterisation)
    std::vector<uint32_t> lastList; // words of the most recently completed display list (tooling)

private:
    static constexpr int kGeInterrupt = 25;
    static constexpr uint32_t kSig = 1, kEnd = 2, kFin = 4;
    InterruptController* intc_ = nullptr;
    ReadFn readMem_;
    GeRenderer renderer_;
    uint32_t exec_ = 0, list_ = 0, stall_ = 0, status_ = 0, interrupt_ = 0;
    uint32_t base_ = 0, depth_ = 0;
    uint32_t retStack_[4] = {};
    bool sigPending_ = false, finPending_ = false;
    std::vector<uint32_t> curList_;
    std::unique_ptr<GeCapture> cap_;
    std::string capPath_; uint64_t capLimit_ = 0;
    // Executes a state/draw command and, while capturing, records it (after the command's own memory snapshots, see GeCapture).
    void exec(uint32_t cmd, uint32_t arg) { renderer_.command(cmd, arg); if (cap_) cap_->afterCommand((cmd << 24) | arg); }

    void checkIrq() {
        if (!intc_) return;
        if (interrupt_ & kEnd) intc_->triggerInterrupt(kGeInterrupt); else intc_->clearInterrupt(kGeInterrupt);
    }
    void run() { // times the display-list walk (software rasterisation happens inside)
        const auto t0 = std::chrono::steady_clock::now();
        try { runImpl(); }
        catch (const std::exception& e) { std::printf("  [GE] exception during list run (capture %s): %s\n", cap_ ? "active" : "off", e.what()); throw; }
        geSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++geRuns;
    }
    void runImpl() {
        if (!readMem_) { exec_ &= ~1u; return; }
        if (cap_) cap_->onRunBegin();
        uint32_t pc = list_;
        for (uint32_t guard = 0; guard < 4000000u; ++guard) {
            if (stall_ != 0 && pc >= stall_) { list_ = pc; return; } // stalled: stays running until the stall address moves
            uint32_t w = 0;
            try { w = readMem_(pc); } catch (...) { list_ = pc; exec_ &= ~1u; return; } // unreadable list: treat as ended
            const uint32_t cmd = w >> 24, arg = w & 0x00FFFFFFu;
            pc += 4; ++cmdsWalked;
            if (curList_.size() < 30000u) curList_.push_back(w);
            switch (cmd) {
                case 0x10: base_ = (arg << 8) & 0x0F000000u; exec(cmd, arg); break; // BASE
                case 0x08: pc = (base_ | arg) & ~3u; break;                         // JUMP
                case 0x0A: if (depth_ < 4) retStack_[depth_++] = pc; pc = (base_ | arg) & ~3u; break; // CALL
                case 0x0B: if (depth_ > 0) pc = retStack_[--depth_]; break;         // RET
                case 0x0E: sigPending_ = true; break;                               // SIGNAL
                case 0x0F: finPending_ = true; break;                               // FINISH
                case 0x0C: {                                                        // END
                    const uint32_t bits = kEnd | (finPending_ ? kFin : 0u) | (sigPending_ ? kSig : 0u);
                    finPending_ = sigPending_ = false;
                    list_ = pc; exec_ &= ~1u; ++listsCompleted;
                    if (cap_) { cap_->onListEnd(renderer_.frameBufferAddress() & 0x1FFFFFFFu, std::max<uint32_t>(renderer_.frameBufferStride(), 1u) * 272u * (renderer_.frameBufferFormat() == 3u ? 4u : 2u));
                        if (capLimit_ && cap_->lists() >= capLimit_) { endCapture(); std::printf("  [GE CAPTURE] stopped after %llu lists: %s -> %s\n", static_cast<unsigned long long>(capLimit_), lastCaptureInfo.c_str(), lastCapturePath.c_str()); } }
                    lastList.swap(curList_); curList_.clear();
                    status_ |= bits; interrupt_ |= bits;
                    checkIrq();
                    return;
                }
                default: exec(cmd, arg); break;                                      // state / drawing commands
            }
        }
        list_ = pc; exec_ &= ~1u; // runaway list: give up rather than hang the host
    }
};

} // namespace beta
