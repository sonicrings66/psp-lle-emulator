#pragma once

#include "../bus.h"
#include "../memory.h"
#include "interrupt_controller.h"

namespace beta {

// Minimal Phase 1 stub for the real "System Control" hardware block at
// 0xBC100000 (jpcsp: MMIOHandlerSystemControl, registered with size 0x104).
// Real hardware exposes per-subsystem reset lines and bus clock enable bits
// here (KIRK, USB, ATA, audio, ME, etc.) -- real kernel boot code pokes
// these directly to power on/reset hardware blocks before using them. Same
// reasoning as InterruptController: this just needs to accept reads/writes
// without faulting so real boot code doesn't hit an unmapped-address error;
// modeling actual per-bit power/reset semantics is deferred until a phase
// that needs a specific subsystem to actually turn on or off.
//
// 2026-09-19: offset 0 is the ONE exception to "plain accept-anything stub"
// above -- confirmed via direct disassembly of uofw's real exceptionman
// `nmiHandler` (excep.S) that real code reads a live pending-interrupt
// summary there (`lui $v0,0xBC10; lw $s0,0($v0)`) to decide whether ANY
// real interrupt source fired; if it reads back 0, real code falls into
// its own "no real interrupt source found" panic (`break 0x080,0x000`).
// This doesn't attempt to replicate real hardware's full multi-tier
// group/sub-register scheme (the per-group sub-offsets like +0x10/+0x14/
// +0xA0/etc real code reads next) -- that's fine, because real code
// already handles "no specific handler found for this exact source"
// gracefully (skips the call, proceeds to its own clean exit) as long as
// the TOP-LEVEL summary isn't zero. So this only needs to make offset 0
// nonzero whenever something real is actually pending, mirroring the same
// two sources Cpu::step() aggregates onto Cause.IP2: InterruptController's
// own 64 real per-bit sources, and the CPU-internal COP0 Count==Compare
// timer match (pushed in via setTimerPending(), since that's tracked on
// Cpu, not on any Bus device -- see Bus::setTimerInterruptPending's doc
// comment).
class SystemControl : public Bus {
public:
    static constexpr uint32_t kSize = 0x104;

    explicit SystemControl(InterruptController& intc) : intc_(intc), regs_(kSize) {}

    uint8_t  read8(uint32_t addr) const override {
        return static_cast<uint8_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    uint16_t read16(uint32_t addr) const override {
        return static_cast<uint16_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8));
    }
    uint32_t read32(uint32_t addr) const override {
        if (addr == 0x00u) {
            return (intc_.hasPendingEnabled() ? 0x1u : 0u) | (timerPending_ ? 0x2u : 0u);
        }
        return regs_.read32(addr);
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        const uint32_t oldValue = regs_.read32(addr);
        // Sysreg USB/Memory Stick connect-status register (0xBC100080, jpcsp: MMIOHandlerSystemControl.usbAndMemoryStick, real bit layout
        // ported from there): bits 0x1/0x100/0x10000 report USB/MS0/MS1 physically connected and are read-only from software's side; the
        // rest are edge-triggered interrupt bits software acknowledges by writing them back. A raw store here (this class's usual behavior)
        // would let a real "clear interrupts" write also wipe our injected connected bit, so this offset only ever clears interrupt bits.
        if (addr == 0x80u) { regs_.write32(addr, oldValue & ~(value & kUsbMsInterruptMask)); return; }
        regs_.write32(addr, value);
        // Sysreg reset register (0xBC10004C): a falling edge of bit 2 (SYSREG_RESET_ME) releases the Media Engine from reset (jpcsp MMIOHandlerSystemControl).
        if (addr == 0x4Cu && (oldValue & 4u) != 0u && (value & 4u) == 0u && onMeReset) onMeReset();
        // HLE (documented, no LLE path -- the Media Engine is a second Allegrex core we do not run):
        // sceSysregInterruptToOther (0xBC100044, main CPU -> ME) makes the "ME" instantly finish the RPC
        // command placed at 0xBFC00600 and interrupt the main CPU back (PSP_MECODEC_INTR = 31), exactly the
        // shape of jpcsp's MMIOHandlerSystemControl.sysregInterruptToOther / MMIOHandlerMeCore. The result
        // word (0xBFC00628) is left as whatever the caller staged (0 by default). me_wrapper's RPC waits on
        // an event flag set from the IRQ31 handler; with no responder every ME RPC (avcodec start) hangs.
        if (addr == 0x44u && value != 0u) {
            ++meRpcCount;
            if (meLle) { if (onInterruptToMe) onInterruptToMe(); } // real Media Engine core: ring its doorbell, it answers by itself
            else intc_.triggerInterrupt(kMeCodecInterrupt);           // stand-in: complete the RPC instantly
        }
    }

    void setTimerPending(bool pending) { timerPending_ = pending; }
    // Real firmware's Memory Stick driver (mscm/msstor) reads presence from this bit, not from the MS controller block itself (0xBD200000,
    // a full Memory Stick Pro Duo protocol engine this project doesn't model -- see hle/mem_stick.h's doc comment) or from syscon. Called
    // once at setup, gated on whether a host folder is mounted at ms0: (hle/mem_stick.h); real hardware would also raise the connect
    // interrupt (bits 0x1E00) on insertion, which isn't modeled here since nothing currently waits on a live insert/removal event.
    void setMemStickPresent(bool present) {
        const uint32_t cur = regs_.read32(0x80u);
        regs_.write32(0x80u, present ? (cur | kMs0Connected) : (cur & ~kMs0Connected));
    }
    static constexpr uint32_t kMs0Connected = 0x000100u, kUsbMsInterruptMask = 0x001E1E1Eu; // bits 0x1E (USB) | 0x1E00 (MS0) | 0x1E0000 (MS1) -- excludes the three connected bits (0x1, 0x100, 0x10000)
    // Media Engine core hooks (see me/me_core.h).
    bool meLle = false;
    std::function<void()> onMeReset, onInterruptToMe;
    void interruptFromMe() { intc_.triggerInterrupt(kMeCodecInterrupt); } // sceSysregInterruptToOther executed by the ME
    static constexpr int kMeCodecInterrupt = 31;
    uint64_t meRpcCount = 0;

private:
    InterruptController& intc_;
    Memory regs_;
    bool timerPending_ = false;
};

} // namespace beta
