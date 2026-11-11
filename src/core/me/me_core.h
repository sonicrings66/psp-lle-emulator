#pragma once
// LLE Media Engine: a second Allegrex core that runs the console's own ME firmware (me_t2img.img, unpacked by the ME's own
// bootstrap code in me_wrapper). Reference: jpcsp mediaengine/* (MEProcessor, MEMemory, MMIOHandlerMe*).
//
// Ported from psp-lle-emu/src/core/me/me_core.h (see docs/PORTING_LOG.md).
// UPDATE (2026-11-08, same day): `MeCodec` (dropped above, originally)
// turned out to be LOAD-BEARING for boot, not just an audio-quality
// nicety -- confirmed live that without it, command 0x60 (AT3+ decode,
// issued by common_util.prx's own real startup) reaches the real ME
// firmware's own VME-dependent decode path, which `vme.h`'s own header
// comment already documents as a real, unresolved hardware gap (the
// request/response channels time out), permanently stalling the whole
// boot. Restored `codec_.handle(cmd)` below with a MINIMAL `MeCodec`
// (this project's own me_codec.h, a silent-output placeholder -- no
// ffmpeg/minimp3 dependency, see its own header comment) instead of the
// old project's real-decode version.
//
// Memory view of the ME (MeBus):
//   - 4 MB ME-local RAM ("AW eDRAM") at 0x00000000, mirrored at 0x40000000 / 0x80000000 / 0xA0000000 (slim models; 2 MB on the fat PSP-1000)
//   - ME-only hardware windows at 0x44000000, 0x44020000, 0x440F8000, 0x440FF000, 0x44100000 (spectra decoder)
//   - its own interrupt controller at 0xBC300000
//   - everything else (shared RAM, IPL region 0xBFC00000, VRAM, the main MMIO map) is the main bus.
// The ME comes out of reset when sysreg reset bit 2 is cleared (SystemControl::onMeReset), starts at 0xBFC00000 where the shared
// exception stub branches on CPUID (mfc0 $22 == 1 for the ME) into the bootstrap that me_wrapper copied to 0xBFC00040. The main CPU
// interrupts it through sysreg 0xBC100044 (ME intc line 31, "MECODEC"); it answers the same way (main intc line 31).
// It is stepped in lock-step with the main core from SystemBus::tick(), and parked on `halt` until an interrupt is pending.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "../cpu.h"
#include "../cpu_bus.h"
#include "../util/kl4e.h"
#include "../mmio/generic_stub.h"
#include "../mmio/interrupt_controller.h"
#include "../bus/system_bus.h"
#include "../../config/options.h"
#include "me_codec.h"

namespace beta {

// ME codec accelerator command block at 0x440FF000 (jpcsp MMIOHandlerMe0FF000). The firmware writes a command number to +0x08
// (0x1D at startup, 0x21 during spectrum decode, 0x5A for output samples, ...); the hardware does the DSP work and raises ME
// interrupt line 5 (ATA_INTR) when it is done. What each command computes is undocumented (jpcsp only logs them), so for now every
// command completes at once without doing any work: enough for the firmware's management paths (boot, malloc). Real decoding is done
// on the host for the decode RPCs (see MeCodec, not yet ported -- see this file's own header comment).
class MeAccelerator : public MmioStub {
public:
    MeAccelerator() : MmioStub(0x30) {}
    void attach(InterruptController* intc) { intc_ = intc; }
    void setLogEnabled(bool v) { log_ = v; }
    uint64_t commands = 0;
    uint32_t lastCommand = 0;

    void write32(uint32_t addr, uint32_t value) override {
        MmioStub::write32(addr, value);
        if (addr == 0x08u) {
            ++commands; lastCommand = value;
            if (log_) { static int n = 0; if (n++ < 200) std::printf("  [ME accel] command 0x%02X (status=%08X a10=%08X a14=%08X a18=%08X)%c", value, MmioStub::read32(0x00), MmioStub::read32(0x10), MmioStub::read32(0x14), MmioStub::read32(0x18), 10); }
            if (intc_) intc_->triggerInterrupt(5); // command complete
        }
    }
private:
    InterruptController* intc_ = nullptr;
    bool log_ = false;
};

class MeBus : public CpuBus {
public:
    static constexpr uint32_t kRamSize = 0x400000;
    static constexpr uint32_t kShadowBase = 0x08300000u, kShadowSize = 0x9F000u;

    explicit MeBus(SystemBus& sc, const config::MeOptions& opts = {})
        : sc_(sc), ram_(kRamSize / 4u, 0u), shadow_(kShadowSize / 4u, 0u), win0_(0x8000), win1_(0x8000), win2_(0x194), win4_(0x40) {
        win3_.attach(&intc_);
        win3_.setLogEnabled(opts.log);
        logEnabled_ = opts.log;
        shadowEnabled_ = opts.shadow;
    }

    InterruptController& intc() { return intc_; }
    SystemBus& sc() { return sc_; }
    void enableShadow() { shadowEnabled_ = true; }
    void loadShadow(const std::vector<uint8_t>& bytes) { for (size_t i = 0; i < bytes.size() && i < kShadowSize; ++i) { uint32_t& w = shadow_[i >> 2]; const uint32_t sh = static_cast<uint32_t>(i & 3u) * 8u; w = (w & ~(0xFFu << sh)) | (static_cast<uint32_t>(bytes[i]) << sh); } }
    uint32_t* ramData() { return ram_.data(); }

    uint8_t read8(uint32_t a) const override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: return static_cast<uint8_t>(ram_[off >> 2] >> ((off & 3u) * 8u));
            case Target::Shadow: return static_cast<uint8_t>(shadow_[off >> 2] >> ((off & 3u) * 8u));
            case Target::Intc: return static_cast<uint8_t>(const_cast<InterruptController&>(intc_).read8(off));
            case Target::Local: return local(off, a).read8(off & localMask(off));
            default: return sc_.read8(a);
        }
    }
    uint16_t read16(uint32_t a) const override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: return static_cast<uint16_t>(ram_[off >> 2] >> ((off & 2u) * 8u));
            case Target::Shadow: return static_cast<uint16_t>(shadow_[off >> 2] >> ((off & 2u) * 8u));
            case Target::Intc: return static_cast<uint16_t>(intc_.read16(off));
            case Target::Local: return local(off, a).read16(off & localMask(off));
            default: return sc_.read16(a);
        }
    }
    uint32_t read32(uint32_t a) const override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: return ram_[off >> 2];
            case Target::Shadow: return shadow_[off >> 2];
            case Target::Intc: return intc_.read32(off);
            case Target::Local: { const uint32_t v = local(off, a).read32(off & localMask(off)); log("R", a, v); return v; }
            default: return sc_.read32(a);
        }
    }
    void write8(uint32_t a, uint8_t v) override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: { const uint32_t sh = (off & 3u) * 8u; uint32_t& w = ram_[off >> 2]; w = (w & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh); break; }
            case Target::Shadow: { const uint32_t sh = (off & 3u) * 8u; uint32_t& w = shadow_[off >> 2]; w = (w & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh); break; }
            case Target::Intc: intc_.write8(off, v); break;
            case Target::Local: local(off, a).write8(off & localMask(off), v); break;
            default: sc_.write8(a, v); break;
        }
    }
    void write16(uint32_t a, uint16_t v) override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: { const uint32_t sh = (off & 2u) * 8u; uint32_t& w = ram_[off >> 2]; w = (w & ~(0xFFFFu << sh)) | (static_cast<uint32_t>(v) << sh); break; }
            case Target::Shadow: { const uint32_t sh = (off & 2u) * 8u; uint32_t& w = shadow_[off >> 2]; w = (w & ~(0xFFFFu << sh)) | (static_cast<uint32_t>(v) << sh); break; }
            case Target::Intc: intc_.write16(off, v); break;
            case Target::Local: local(off, a).write16(off & localMask(off), v); break;
            default: sc_.write16(a, v); break;
        }
    }
    void write32(uint32_t a, uint32_t v) override {
        uint32_t off; const Target t = decode(a, off);
        switch (t) {
            case Target::Ram: ram_[off >> 2] = v; break;
            case Target::Shadow: shadow_[off >> 2] = v; break;
            case Target::Intc: intc_.write32(off, v); break;
            case Target::Local: log("W", a, v); local(off, a).write32(off & localMask(off), v); break;
            case Target::SysregToMain: sc_.systemControl().interruptFromMe(); break; // sceSysregInterruptToOther on the ME: interrupt the main CPU
            default: if (logEnabled_ && ((a & 0x1FFFFFFFu) >= 0x1FC00600u && (a & 0x1FFFFFFFu) < 0x1FC00740u)) { static int n = 0; if (n++ < 120) std::printf("  [ME->shared] write %08X = %08X\n", a, v); } sc_.write32(a, v); break;
        }
    }

    uint32_t pendingInterrupts() const override { return (intc_.hasPendingEnabled() || doorbell_) ? 0x1u : 0u; }
    // sceSysregInterruptToOther from the main CPU: latches the ME's IP2 request line until the ME takes the exception (jpcsp pendingInterruptIPbitsME).
    void ringDoorbell() { doorbell_ = true; }
    void clearDoorbell() { doorbell_ = false; }
    bool doorbell() const { return doorbell_; }
    // The ME core must not tick the shared devices: they advance with the main core's instruction count.
    void tick() override {}
    uint32_t resolveSyscall(uint32_t) const override { return 0; }

private:
    enum class Target { Ram, Shadow, Intc, Local, SysregToMain, Main };

    Target decode(uint32_t a, uint32_t& off) const {
        const uint32_t m29 = a & 0x1FFFFFFFu; // 0x00000000 / 0x40000000 / 0x80000000 / 0xA0000000 all reach ME RAM
        if (m29 < kRamSize) { off = m29; return Target::Ram; }
        const uint32_t p = a & 0x5FFFFFFFu; // bit 30 distinguishes the ME-local window from VRAM at 0x04xxxxxx
        if (p >= 0x44000000u && p < 0x44008000u) { off = p - 0x44000000u; return Target::Local; }
        if (p >= 0x44020000u && p < 0x44028000u) { off = p - 0x44020000u + 0x100000u; return Target::Local; }
        if (p >= 0x440F8000u && p < 0x440F8194u) { off = p - 0x440F8000u + 0x200000u; return Target::Local; }
        if (p >= 0x440FF000u && p < 0x440FF030u) { off = p - 0x440FF000u + 0x300000u; return Target::Local; }
        if (p >= 0x44100000u && p < 0x44100040u) { off = p - 0x44100000u + 0x400000u; return Target::Local; }
        const uint32_t phys = a & 0x1FFFFFFFu;
        // The block 0x88300000.. is the ME's own on real hardware (me_wrapper reserves it as the "old ME partition"). Our kernel partition is larger than the
        // real one and holds live kernel objects there, so the ME gets a private copy of the range its first firmware stage is unpacked into.
        if (shadowEnabled_ && phys >= kShadowBase && phys < kShadowBase + kShadowSize) { off = phys - kShadowBase; return Target::Shadow; }
        if (phys >= 0x1C300000u && phys < 0x1C300030u) { off = phys - 0x1C300000u; return Target::Intc; }
        if (phys == 0x1C100044u) { off = 0; return Target::SysregToMain; }
        off = 0; return Target::Main;
    }
    // `off` encodes which local window: 0x000000.. win0, 0x100000.. win1, 0x200000.. win2, 0x300000.. win3, 0x400000.. win4
    MmioStub& local(uint32_t off, uint32_t) const {
        MeBus* self = const_cast<MeBus*>(this);
        switch (off >> 20) { case 0: return self->win0_; case 1: return self->win1_; case 2: return self->win2_; case 3: return self->win3_; default: return self->win4_; }
    }
    static uint32_t localMask(uint32_t off) { return off & 0xFFFFFu; }
    void log(const char* rw, uint32_t a, uint32_t v) const {
        if (!logEnabled_) return;
        static int n = 0; if (n++ < 400) std::printf("  [ME local] %s %08X = %08X\n", rw, a, v);
    }

    SystemBus& sc_;
    std::vector<uint32_t> ram_;
    std::vector<uint32_t> shadow_;
    InterruptController intc_;
    MmioStub win0_, win1_, win2_, win4_;
    MeAccelerator win3_;
    bool logEnabled_ = false;
    bool doorbell_ = false;
    bool shadowEnabled_ = false; // experiment only: does not work while me_wrapper itself sits inside the range -- set from MeOptions::shadow in the constructor
};

class MeCore {
public:
    explicit MeCore(SystemBus& sc, const config::MeOptions& opts = {}) : bus_(sc, opts), cpu_(bus_), codec_(sc, opts) {
        logEnabled_ = opts.log;
        hleBoot_ = !opts.realBoot;
    }

    bool running() const { return running_; }
    // Parked on `halt` with no interrupt pending: run() returns at once and nothing can change that until the main CPU rings the doorbell
    // (a device access, which already ends every JIT burst and flushes device time first) -- so a flush before then has no ME work to do.
    bool idleParked() const { return running_ && parked_ && bus_.pendingInterrupts() == 0u; }
    uint64_t steps() const { return steps_; }
    MeBus& bus() { return bus_; }
    Cpu& cpu() { return cpu_; }

    // Reset released (sysreg reset bit 2 falling edge): start executing at the shared reset/exception stub.
    void reset() {
        if (hleBoot_ && bootFirstStageOnHost()) return;
        if (hleBoot_) { running_ = false; dead_ = true; return; }
        cpu_.reset(0xBFC00000u);
        cpu_.state().cp0State[22] = 1u; // CPUID = 1 for the Media Engine (the stub at 0xBFC00000 branches on it)
        bus_.intc().write32(0x08, 0u);
        running_ = true; parked_ = false; dead_ = false; ++resets_;
        std::printf("  [ME] reset released (#%u) at main instr-tick %llu\n", resets_, static_cast<unsigned long long>(sc_ticks()));
    }
    // Main CPU rang the doorbell (sysreg 0xBC100044): ME interrupt line 31
    // (MECODEC). Requests the (undocumented) codec accelerators are
    // needed for are answered on the host (see me_codec.h); everything
    // else goes to the real ME firmware.
    void interruptFromMain() {
        {
            SystemBus& sc = bus_.sc();
            const uint32_t cmd = sc.read32(0xBFC00600u);
            if (codec_.handle(cmd)) { sc.write32(0xBFC00628u, codec_.result()); sc.systemControl().interruptFromMe(); return; }
        }
        bus_.intc().triggerInterrupt(31);
        bus_.ringDoorbell();
        parked_ = false;
        if (logEnabled_) std::printf("  [ME] doorbell from main CPU (running=%d)\n", running_ ? 1 : 0);
    }
    // Steps the core `steps` times. A parked core with nothing pending stays parked for the rest of the batch (only the main CPU, between
    // flushes, or its own interrupt controller can wake it), so the batch ends early -- identical to calling step() that many times.
    void run(uint32_t steps) {
        for (uint32_t i = 0; i < steps; ++i) {
            if (!running_) return;
            if (parked_ && bus_.pendingInterrupts() == 0u) return;
            step();
        }
    }
    void step() {
        if (!running_) return;
        if (parked_) {
            if (bus_.pendingInterrupts() == 0u) return;
            parked_ = false;
        }
        ring_[ringPos_++ & 31u] = cpu_.state().pc;
        cpu_.haltRequested = false;
        const bool exlBefore = (cpu_.state().cp0State[12] & 2u) != 0u;
        try {
            cpu_.step();
            if (!exlBefore && (cpu_.state().cp0State[12] & 2u) != 0u) { // an exception was taken
                const uint32_t excCode = (cpu_.state().cp0State[13] >> 2) & 0x1Fu;
                if (logEnabled_) { static int n = 0; if (n++ < 60) std::printf("  [ME] exception code=%u epc=%08X -> pc=%08X cause=%08X status=%08X doorbell=%d step=%llu\n", excCode, cpu_.state().cp0State[14], cpu_.state().pc, cpu_.state().cp0State[13], cpu_.state().cp0State[12], bus_.doorbell() ? 1 : 0, static_cast<unsigned long long>(steps_)); }
                if (excCode == 0u) bus_.clearDoorbell(); // an interrupt was taken: the request line is consumed (jpcsp pendingInterruptIPbitsME)
            }
            ++steps_;
        } catch (const std::exception& e) {
            running_ = false; dead_ = true;
            std::printf("  [ME] STOPPED after %llu steps at pc=%08X: %s\n", static_cast<unsigned long long>(steps_), cpu_.state().pc, e.what());
            std::printf("  [ME] recent pcs:"); for (uint32_t k = 0; k < 32u; ++k) std::printf(" %08X", ring_[(ringPos_ + k) & 31u]); std::printf("\n");
            std::printf("  [ME] regs: ra=%08X sp=%08X a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X t9=%08X gp=%08X\n", cpu_.state().gpr[31], cpu_.state().gpr[29], cpu_.state().gpr[4], cpu_.state().gpr[5], cpu_.state().gpr[6], cpu_.state().gpr[7], cpu_.state().gpr[2], cpu_.state().gpr[3], cpu_.state().gpr[25], cpu_.state().gpr[28]);
            return;
        }
        if (logEnabled_ && (steps_ % 250000u) == 0u) std::printf("  [ME] step %llu pc=%08X ra=%08X sp=%08X status=%08X parked=%d\n", static_cast<unsigned long long>(steps_), cpu_.state().pc, cpu_.state().gpr[31], cpu_.state().gpr[29], cpu_.state().cp0State[12], parked_ ? 1 : 0);
        if (cpu_.haltRequested) { parked_ = true; cpu_.haltRequested = false; if (logEnabled_) { static int n = 0; if (n++ < 40) std::printf("  [ME] halt at pc=%08X (step %llu) intc masked=%08X raw=%08X enabled=%08X status=%08X cause=%08X\n", cpu_.state().pc, static_cast<unsigned long long>(steps_), bus_.intc().read32(0x00), bus_.intc().read32(0x04), bus_.intc().read32(0x08), cpu_.state().cp0State[12], cpu_.state().cp0State[13]); } }
    }
    bool dead() const { return dead_; }

    // Host-side stand-in for the ME bootstrap ("me_boot_code" + me_wrapper's decompressAndRunMeImage, which unpack the first firmware
    // stage to 0x88300000 and jump there). On real hardware that block is the ME's own; in our kernel layout me_wrapper itself sits in it
    // (the kernel partition is larger than the real one), so the ME core gets a private copy of the range and starts directly at the unpacked stage.
    // Everything after that first jump (the stage unpacking the second segment, the RPC loop) is the real ME firmware running on the second core.
    bool bootFirstStageOnHost() {
        SystemBus& sc = bus_.sc();
        uint32_t data = 0;
        try { data = ((sc.read32(0xBFC00064u) & 0xFFFFu) << 16) | (sc.read32(0xBFC00068u) & 0xFFFFu); } catch (const std::exception&) { return false; }
        std::vector<uint8_t> in(0x64000u, 0);
        try { for (uint32_t i = 0; i < in.size(); ++i) in[i] = sc.read8(data + i); } catch (const std::exception&) { }
        if (in[0] != 'K' || in[1] != 'L' || in[2] != '4' || in[3] != 'E') { std::printf("  [ME] no KL4E image at 0x%08X\n", data); return false; }
        std::vector<uint8_t> stage;
        std::size_t used = 0;
        try {
            std::vector<uint8_t> compressed(in.begin() + 4, in.end());
            stage = util::decompressKl4e(compressed, MeBus::kRamSize, /*isKl4e=*/true, &used);
        } catch (const std::exception& e) {
            std::printf("  [ME] first-stage decompression failed: %s\n", e.what());
            return false;
        }
        bus_.enableShadow();
        bus_.loadShadow(stage);
        sc.write32(0xBFC00700u, 0xFFFFFFFCu); // decompressAndRunMeImage publishes -4 ("image decompression in progress") before it starts
        cpu_.reset(0x88300000u);
        cpu_.state().cp0State[22] = 1u;
        cpu_.state().cp0State[12] = 0x20000000u;
        cpu_.state().gpr[4] = data + 4u + static_cast<uint32_t>(used); // second compressed segment, handed to the first stage
        cpu_.state().gpr[29] = 0x88400000u;
        running_ = true; parked_ = false; dead_ = false; ++resets_;
        std::printf("  [ME] host bootstrap: image at 0x%08X, first stage %zu bytes -> 0x88300000, second segment at 0x%08X\n", data, stage.size(), data + 4u + static_cast<uint32_t>(used));
        return true;
    }

private:
    uint64_t sc_ticks() const { return steps_; }
    MeBus bus_;
    Cpu cpu_;
    MeCodec codec_;
    bool running_ = false, parked_ = false, dead_ = false, logEnabled_ = false;
    bool hleBoot_ = true; // default: host-side first stage (see bootFirstStageOnHost) -- set from !MeOptions::realBoot in the constructor
    uint64_t steps_ = 0;
    uint32_t resets_ = 0;
    uint32_t ring_[32] = {}; uint32_t ringPos_ = 0;
};

} // namespace beta
