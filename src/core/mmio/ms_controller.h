#pragma once
// Real Memory Stick Pro Duo controller (0xBD200000, jpcsp: MMIOHandlerMemoryStick) -- a full protocol engine (TPC commands, sector I/O,
// boot pages, real per-bit STATUS semantics ported from the Linux memstick driver; ~1500 lines in jpcsp) this project does not model.
//
// Traced live (2026-09-27, capstone disassembly of the real driver code that runs once PSP_MS_PRESENT reports a card, see the direction
// memory doc): the driver's media-detect routine busy-polls this controller's real STATUS register (offset 0x38, matching jpcsp's own
// register map exactly) for bits 0x1000 (MS_STATUS_READY) and 0x20, using a real elapsed-time check against the system clock -- not an
// interrupt or a blocking wait -- for up to a 1-second timeout, up to 5 retries. With a plain accept-anything stub (always reads 0) those
// bits never appear, so the driver busy-spins the full timeout on every attempt, starving every other kernel thread; this is what froze
// and then blanked the XMB when presence was first turned on. A second routine (a data-write loop feeding the TPC data register at
// offset 0x34 four bytes at a time) polls the SAME status register for bit 0x4000 (MS_STATUS_FIFO_RW, "FIFO ready for the next word")
// between every chunk -- caught the same way, one real hardware register at a time.
//
// This is a deliberate, minimal, HONEST shortcut, not a port of the real protocol (unlike KIRK, whose real crypto this project actually
// runs): STATUS always reads back with both bits already set, so the driver's poll succeeds on its very first check instead of spinning.
// Every real file access is already fully handled at the sceIo layer (hle/mem_stick.h) before it would ever reach this low-level protocol,
// so nothing above this needs the real TPC/sector semantics -- this stand-in only needs to convince the driver "media detected, proceed."
//
// One further real register access was traced beyond the status polling: the driver programs a register address via TPC command 0x8
// (MS_TPC_SET_RW_REG_ADDRESS, 4 bytes written to the data port: readAddr, readSize, writeAddr, writeSize) then issues TPC command 0x4
// (MS_TPC_READ_REG) and reads the answer back through the same data port. Confirmed live it reads register 0x04 (4 bytes) then register
// 0x00 (8 bytes). jpcsp's own real register map (MMIOHandlerBaseMemoryStick/MMIOHandlerMemoryStick, itself ported from the real Linux
// memstick driver) names register 0x04 as MS_TYPE_ADDRESS, which for a genuine Memory Stick Pro holds MS_TYPE_MEMORY_STICK_PRO (0x01),
// and register 0x10 as MS_SYSTEM_ADDRESS, holding MS_SYSTEM_SERIAL_MODE (0x80) -- these two bytes are what jpcsp's own real code checks
// to answer "is this actually a Memory Stick Pro" and "is it in serial mode". Every other register address stays 0, matching jpcsp's own
// default (a plain zero-initialised array) for anything it doesn't otherwise name.
#include "generic_stub.h"
#include "ms_engine.h"
#include "../util/ms_image.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace beta {

class MsController : public MmioStub {
public:
    static constexpr uint32_t kStatusOffset = 0x38u, kAlwaysReady = 0x5020u; // MS_STATUS_READY (0x1000) | MS_STATUS_FIFO_RW (0x4000) | the second bit the detect poll also waits on (0x20)
    static constexpr uint32_t kCmdOffset = 0x30u, kDataOffset = 0x34u;
    static constexpr uint32_t kCmdSetRwRegAddress = 0x8u, kCmdReadReg = 0x4u;

    explicit MsController(uint32_t size) : MmioStub(size) { regs_.fill(0); }

    // A real card (2026-10-08): with a card image attached every register access goes to the protocol engine (mmio/ms_engine.h), a port of jpcsp's controller, and the
    // firmware's own driver and FAT code run against the image. With none attached everything below behaves exactly as before (the "always ready" stand-in), so a boot
    // without a card is unchanged.
    void attachCard(MsCard* image, InterruptController* intc) { eng_ = std::make_unique<MsEngine>(*image, *intc); }
    bool hasCard() const { return eng_ != nullptr; }
    MsEngine* engine() const { return eng_.get(); }

    // The real controller chip answers this self-identification regardless of whether media is inserted -- confirmed live, it runs on
    // every boot, not just with PSP_MS_PRESENT set. Everything below (register tracking, the read8/16/32 data-port interception) stays
    // fully off, matching this class's previous, proven-safe plain-store behavior, until this is called: offset 0x30 (the TPC command
    // register this tracks) is also reused to carry packed page/type values by other real commands unrelated to register read/write, so
    // watching it at all is only safe once Memory Stick support is genuinely wanted -- confirmed live: enabling it unconditionally broke
    // boots that never asked for Memory Stick support, by misreading one of those unrelated packed writes as a register-read command.
    void setGenuineMemoryStickPro() {
        enabled_ = true;
        regs_[0x04] = 0x01; // MS_TYPE_ADDRESS = MS_TYPE_MEMORY_STICK_PRO
        regs_[0x10] = 0x80; // MS_SYSTEM_ADDRESS = MS_SYSTEM_SERIAL_MODE
    }

    uint8_t read8(uint32_t addr) const override {
        if (eng_) return eng_->read8(addr);
        if (enabled_ && addr == kDataOffset && lastCmd_ == kCmdReadReg) {
            const uint8_t v = regs_[readPos_ & 0xFFu];
            ++readPos_; return v;
        }
        return MmioStub::read8(addr);
    }
    uint16_t read16(uint32_t addr) const override {
        if (eng_) return eng_->read16(addr);
        if (addr == kStatusOffset) return static_cast<uint16_t>(MmioStub::read16(addr) | kAlwaysReady);
        if (enabled_ && addr == kDataOffset && lastCmd_ == kCmdReadReg) {
            uint16_t v; readRegs(2, reinterpret_cast<uint8_t*>(&v));
            return v;
        }
        return MmioStub::read16(addr);
    }
    uint32_t read32(uint32_t addr) const override {
        if (eng_) return eng_->read32(addr);
        if (addr == kStatusOffset) return MmioStub::read32(addr) | kAlwaysReady;
        if (enabled_ && addr == kDataOffset && lastCmd_ == kCmdReadReg) {
            uint32_t v; readRegs(4, reinterpret_cast<uint8_t*>(&v));
            return v;
        }
        return MmioStub::read32(addr);
    }

    void write32(uint32_t addr, uint32_t value) override {
        if (eng_) { eng_->write32(addr, value); return; }
        if (enabled_ && addr == kCmdOffset) {
            lastCmd_ = value & 0xFFFFu;
            if (lastCmd_ == kCmdReadReg) { readPos_ = pendingReadAddr_; }
        }
        else if (enabled_ && addr == kDataOffset && lastCmd_ == kCmdSetRwRegAddress) {
            pendingReadAddr_ = value & 0xFFu; // byte 0 of [readAddr, readSize, writeAddr, writeSize]
        }
        MmioStub::write32(addr, value);
    }
    void write8(uint32_t addr, uint8_t value) override {
        if (eng_) { eng_->write8(addr, value); return; }
        MmioStub::write8(addr, value);
    }
    void write16(uint32_t addr, uint16_t value) override {
        if (eng_) { eng_->write16(addr, value); return; }
        if (enabled_ && addr == kCmdOffset) { lastCmd_ = value; if (lastCmd_ == kCmdReadReg) readPos_ = pendingReadAddr_; }
        MmioStub::write16(addr, value);
    }

private:
    void readRegs(uint32_t n, uint8_t* out) const { for (uint32_t i = 0; i < n; ++i) out[i] = regs_[(readPos_++) & 0xFFu]; }

    std::unique_ptr<MsEngine> eng_;
    std::array<uint8_t, 256> regs_{};
    bool enabled_ = false;
    mutable uint32_t lastCmd_ = 0, readPos_ = 0;
    uint32_t pendingReadAddr_ = 0;
};

} // namespace beta
