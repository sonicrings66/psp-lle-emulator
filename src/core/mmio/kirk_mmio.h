#pragma once

#include <cstdint>
#include <vector>

#include "../bus.h"
#include "../memory.h"
#include "../crypto/kirk.h"
#include "interrupt_controller.h"

// Real hardware KIRK crypto-engine MMIO bridge, real base 0xBDE00000
// (jpcsp's own MMIOHandlerKirk.java) -- ported from psp-lle-emu's
// tests/watch_bus.h (WatchBus::read32/write32's KIRK dispatch,
// kirkStartProcessing/kirkStartProcessingImpl, computeKirkSizes), where it
// had lived as test-harness-only logic despite being real, load-bearing
// emulation (see docs/PORTING_LOG.md -- this is exactly the kind of real
// logic the rework's Phase A/B is meant to pull out of the old test file
// into src/core). Real firmware (reboot.prx and others) drives the crypto
// engine through these registers, not a direct function call, unlike every
// other KirkEngine use in this project.
//
// Real two-phase protocol (uofw's own kirk-rom.c; confirmed live against
// this console's own real post-decompression kernel code): write 1 to the
// trigger register (+0x0C) to run a command synchronously via the SAME
// KirkEngine every other real decrypt path uses, reading real input bytes
// from the real source address and writing real output bytes to the real
// dest address, then setting a real completion/error status bit and
// raising PSP_KIRK_INTR (24). A phase-2 trigger (value=2) exists in real
// hardware's own internal state machine; this project only acknowledges it
// (sets the phase-2-complete status bit) rather than modeling real phase-2
// crypto work, matching the old project's own honestly-documented minimal
// fix.
namespace beta {

class KirkMmio : public Bus {
public:
    // `kirk` may be nullptr (no real engine attached yet -- e.g. a plain
    // unit-test fixture that never exercises this device): the trigger
    // register then does nothing at all, matching the old project's own
    // documented fallback ("the plain MmioStub underneath just silently
    // accepts writes and returns zero on reads"). Attach one later via
    // attachKirkEngine() -- mirrors SystemBus::attachRealNandImage's
    // pattern for the same reason (most tests never need a real engine).
    KirkMmio(InterruptController& intc, crypto::KirkEngine* kirk, Bus& mem, uint32_t size)
        : intc_(intc), kirk_(kirk), mem_(mem), regs_(size) {}

    void attachKirkEngine(crypto::KirkEngine* engine) { kirk_ = engine; }

    uint8_t  read8(uint32_t addr) const override { return static_cast<uint8_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8)); }
    uint16_t read16(uint32_t addr) const override { return static_cast<uint16_t>(read32(addr & ~0x3u) >> ((addr & 0x3u) * 8)); }
    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x00: return 0x4B52494Bu; // "KIRK"
            case 0x04: return 0x30313030u; // "0010"
            case 0x08: return error_;
            case 0x10: return command_;
            case 0x14: return result_;
            case 0x1C: return status_;
            case 0x2C: return sourceAddr_;
            case 0x30: return destAddr_;
            default: return regs_.read32(addr);
        }
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x08: error_ = value; return;
            case 0x0C: startProcessing(value); return;
            case 0x28: if (value != 0 && value != 2) intc_.clearInterrupt(kKirkInterrupt); return;
            case 0x10: command_ = value; return;
            case 0x14: result_ = value; return;
            case 0x1C: status_ = value; return;
            case 0x2C: sourceAddr_ = value; return;
            case 0x30: destAddr_ = value; return;
            default: return; // real hardware: every other offset in this block is reserved, writes have no effect
        }
    }

    static constexpr int kKirkInterrupt = 24; // PSP_MEMLMD_INTR

private:
    // Real per-command input/output size computation, ported from jpcsp's
    // own MMIOHandlerKirk.hleUtilsBufferCopyWithRange -- the same real
    // header-field-driven sizing this project already uses for
    // KIRK_CMD_DECRYPT_PRIVATE in crypto/prx_decrypt.cpp and
    // firmware/ipl_extractor.cpp, generalized to every other real command
    // real firmware drives through this register interface. Returns false
    // for a command this project hasn't needed yet (matches jpcsp's own
    // "unimplemented" default -- reports PSP_KIRK_INVALID_OPERATION rather
    // than guessing).
    bool computeSizes(uint32_t command, uint32_t sourceAddr, uint32_t& inSize, uint32_t& outSize) const {
        auto alignUp15 = [](uint32_t v) { return (v + 15u) & ~15u; };
        auto rd32 = [this](uint32_t a) { return mem_.read32(a); };
        switch (command) {
            // Real hardware's documented command range is 1-18 (psdevwiki's error-code
            // table: 0x0D is literally "invalid operation, out of 1-18 range"). Command 0
            // (real KIRK_CMD_DECRYPT_BOOTROM -- devkit kbooti bootrom decrypt, used by
            // tachsm.o; libkirk's own KIRK_CMD_ENCRYPT_PRIVATE name is a legacy misnomer) is
            // bootrom/devkit-only and NOT reachable through this normal MMIO interface on
            // real hardware -- deliberately absent here so it falls to `default: return
            // false`, which already reports PSP_KIRK_INVALID_OPERATION, matching real
            // hardware's own rejection. (Real memlmd.prx's own per-module signature-prep
            // work issues command 8, confirmed via a live KIRK-register read -- never 0.)
            case 1: { // KIRK_CMD_DECRYPT_PRIVATE
                const uint32_t dataSize = rd32(sourceAddr + 0x70);
                const uint32_t dataOffset = rd32(sourceAddr + 0x74);
                inSize = 0x90u + alignUp15(dataSize) + dataOffset;
                outSize = alignUp15(dataSize);
                return true;
            }
            case 3: { // real KIRK command 3 ("decrypts data encrypted by command 2") -- same
                      // metadata-header convention as command 1 (PDF: same 0x90-byte header
                      // shape), AES-CMAC mode only -- see kirk_CMD3's own header comment
                const uint32_t dataSize = rd32(sourceAddr + 0x70);
                const uint32_t dataOffset = rd32(sourceAddr + 0x74);
                inSize = 0x90u + alignUp15(dataSize) + dataOffset;
                outSize = alignUp15(dataSize);
                return true;
            }
            case 0xB: inSize = rd32(sourceAddr) + 4; outSize = 20; return true;              // SHA1_HASH
            // Real size is 0x1C (28) bytes BOTH ways (psdevwiki "Command 0xF: Seed RNG
            // buffer": 64-bit counter + 0x14-byte seed data, in and out) -- this used to
            // be coded as inSize=8/outSize=28 and treated as a pure no-op in
            // startProcessing below; a live KIRK-command-usage audit against this
            // console's real boot (2026-11-10) confirmed real code actually issues this
            // command, so the wrong size was a real, live bug. Now flows through
            // kirk_->command() like every other real command (see kirk_CMD15).
            case 0xF: inSize = 0x1C; outSize = 0x1C; return true;                             // SEED
            case 0xA: {                                                                        // PRIV_SIGN_CHECK -- real libkirk
                                                                                                 // kirk_CMD10 already implements this
                                                                                                 // correctly; it was just unreachable
                                                                                                 // through this MMIO interface. Same real
                                                                                                 // metadata-header sizing as command 1
                                                                                                 // (see that case) -- the PDF documents
                                                                                                 // command 0xA as taking "the same data as
                                                                                                 // commands 1, 2 and 3".
                const uint32_t dataSize = rd32(sourceAddr + 0x70);
                const uint32_t dataOffset = rd32(sourceAddr + 0x74);
                inSize = 0x90u + alignUp15(dataSize) + dataOffset;
                outSize = 0;
                return true;
            }
            case 0xE: inSize = 0; outSize = 0x14; return true;                                // PRNG
            case 0xC: inSize = 0; outSize = 0x3C; return true;                                // ECDSA_GEN_KEYS
            case 0xD: inSize = 0x3C; outSize = 0x28; return true;                             // ECDSA_MULTIPLY_POINT
            case 0x10: inSize = 0x34; outSize = 0x28; return true;                            // ECDSA_SIGN
            case 4: case 5: {                                                                  // ENCRYPT_IV_0 / ENCRYPT_IV_FUSE
                const uint32_t dataSize = rd32(sourceAddr + 0x10);
                inSize = dataSize + 20; outSize = dataSize + 20; return true;
            }
            case 6: {                                                                           // ENCRYPT_IV_USER -- real header is 0x24 bytes in
                                                                                                  // (base 0x14 + a 0x10-byte caller key at 0x14, the
                                                                                                  // PDF's "Commands 6~9" table), 0x34 bytes out (CMD6
                                                                                                  // also writes a 0x10-byte reencrypted random key at
                                                                                                  // output offset 0x24 -- see kirk_CMD6's own comment).
                const uint32_t dataSize = rd32(sourceAddr + 0x10);
                inSize = dataSize + 0x24u; outSize = dataSize + 0x34u; return true;
            }
            case 7: {                                                                          // DECRYPT_IV_0
                const uint32_t dataSize = rd32(sourceAddr + 0x10);
                inSize = alignUp15(dataSize) + 20; outSize = alignUp15(dataSize); return true;
            }
            case 0x12: inSize = 0xB8; outSize = 0; return true;                               // CERT_VERIFY
            case 0x11: inSize = 0x64; outSize = 0; return true;                               // ECDSA_VERIFY
            case 0x8: {                                                                        // DECRYPT_FUSE
                const uint32_t dataSize = rd32(sourceAddr + 0x10);
                inSize = alignUp15(dataSize) + 20; outSize = alignUp15(dataSize); return true;
            }
            case 0x9: {                                                                        // DECRYPT_IV_USER -- real header in is the full 0x34
                                                                                                 // bytes CMD6's output produced (see case 6 above and
                                                                                                 // kirk_CMD9's own comment); output is the raw
                                                                                                 // decrypted data only, same convention as 0x7/0x8.
                const uint32_t dataSize = rd32(sourceAddr + 0x10);
                inSize = alignUp15(dataSize) + 0x34u; outSize = alignUp15(dataSize); return true;
            }
            default: return false;
        }
    }

    void startProcessing(uint32_t value) {
        if (!kirk_) return; // no real engine attached -- see the constructor's own doc comment
        // Real phase-2 trigger: acknowledged (status bit set) without
        // modeling real phase-2 internal crypto work -- see file header.
        if (value == 2) { status_ |= 0x02u; return; }
        if (value != 1) return;
        // Real hardware raises the completion interrupt for every real
        // phase-1 trigger, success or failure alike -- ported as an
        // unconditional action on this path, not gated on what happens
        // below (matches the old project's own kirkStartProcessing, which
        // triggers it in its outer wrapper regardless of kirkStartProcessingImpl's inner outcome).
        intc_.triggerInterrupt(kKirkInterrupt);
        status_ &= ~0x11u; // clear phase-1 completed/error bits
        uint32_t inSize = 0, outSize = 0;
        if (!computeSizes(command_, sourceAddr_, inSize, outSize)) {
            result_ = 0xDu; // PSP_KIRK_INVALID_OPERATION
            status_ |= 0x10u;
            return;
        }
        std::vector<uint8_t> inBuf(inSize), outBuf(outSize);
        for (uint32_t i = 0; i < inSize; ++i) inBuf[i] = mem_.read8(sourceAddr_ + i);
        const int result = kirk_->command(outBuf.empty() ? nullptr : outBuf.data(), static_cast<int>(outSize),
                                           inBuf.empty() ? nullptr : inBuf.data(), static_cast<int>(inSize),
                                           static_cast<int>(command_));
        result_ = static_cast<uint32_t>(result);
        for (uint32_t i = 0; i < outSize; ++i) mem_.write8(destAddr_ + i, outBuf[i]);
        status_ |= (result == 0) ? 0x01u : 0x10u; // phase-1 completed or error
    }

    InterruptController& intc_;
    crypto::KirkEngine* kirk_;
    Bus& mem_;
    Memory regs_; // backing store for every reserved/unlisted offset (real hardware: don't-care, but reads must return something stable)

    uint32_t command_ = 0, sourceAddr_ = 0, destAddr_ = 0;
    uint32_t result_ = 0, error_ = 0, status_ = 0;
};

} // namespace beta
