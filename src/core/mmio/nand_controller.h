#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <utility>

#include "../bus.h"
#include "../memory.h"
#include "../util/nand_image.h"
#include "generic_stub.h"
#include "interrupt_controller.h"

namespace beta {

// Minimal, real-ish NAND controller behavior (real base 0xBD101000, size
// 0x304 -- MMIOHandlerNand on real hardware, real flash0/flash1 access).
// Every register is a plain accept-anything store by default, same as the
// generic MmioStub this replaces -- the one piece of real behavior modeled
// here is the async command/status/interrupt handshake real reboot.prx code
// actually depends on. Confirmed against jpcsp's own MMIOHandlerNand.java
// (the only known reference that gets real 6.61 firmware LLE-booting): real
// completion doesn't just set status bits for a poll loop to see -- it also
// fires a real hardware interrupt, PSP_NAND_INTR (real PSP interrupt number
// 20, jpcsp's IntrManager), through the same InterruptController every
// other interrupt source in this project already routes through. A poll
// loop testing `(status>>8) & status & 3 != 0` against the real status
// register (0xBD101038) was confirmed first (needs the low bit 0/1 and its
// paired high bit 8/9 set TOGETHER -- real init code clears bits 0/1/8
// before each operation, so a high bit alone can never satisfy it), but a
// separate real code path was later found that clears status and waits
// without ever re-arming either poll mechanism -- consistent with jpcsp's
// own source, which fires a real interrupt on completion in addition to
// setting status, for exactly this kind of interrupt-driven (not
// busy-polling) caller.
//
// Two distinct real trigger paths arm a completion, each modeled as an
// independent per-pair async channel (channel 0: low=bit0/high=bit8,
// channel 1: low=bit1/high=bit9):
//  - writing a command word to the real command register (0xBD101024)
//    arms whichever channel(s) the command word's own low bit(s) select
//    (confirmed real: command 0x301 -> channel 0 completes with 0x301).
//  - writing the status register directly with a high bit (8/9) newly set
//    but its paired low bit (0/1) still clear also arms that channel -- a
//    second, distinct real call site does exactly this, bypassing the
//    command register entirely.
// Once a channel's countdown elapses, its low+high bit pair is OR'd into
// status AND PSP_NAND_INTR fires (gated by the InterruptController's own
// real enable bit, exactly like every other source). Real init/clear
// sequences (read-modify-write on 0x1038, writes to 0x1000/0x1020/etc.)
// all still work exactly as a plain register file otherwise -- only the
// command register's write and the status register's write path have any
// special-cased behavior layered on top.
class NandController : public Bus {
public:
    static constexpr uint32_t kCommandOffset = 0x24;
    static constexpr uint32_t kStatusOffset = 0x38;
    // Real PSP interrupt number for NAND (jpcsp's IntrManager, PSP_NAND_INTR).
    static constexpr int kInterruptNumber = 20;
    // Arbitrary but nonzero/finite -- models "this takes real time". jpcsp's
    // own MMIOHandlerNand completes synchronously (no delay at all -- its
    // own author never modeled real timing either), so this is a project
    // choice, not a sourced real value; a real interrupt now firing on
    // completion is the part actually grounded in jpcsp's source.
    static constexpr uint64_t kLatencyTicks = 64;
    // Real DMA address register (jpcsp: MMIOHandlerNand `dmaAddress`,
    // offset 0x020). jpcsp's own decode is `ppn = dmaAddress >> 10` -- and
    // real reboot.prx code writes exactly `ppn << 10` here before
    // triggering a command, so the two shifts cancel and this register's
    // last-written value, shifted right by 10, is the real requested page
    // number.
    static constexpr uint32_t kAddressOffset = 0x20;
    // Real NAND flash command-latch register (distinct from kCommandOffset
    // above, which drives this project's own async page-read/write
    // completion+interrupt model) and the real data/FIFO register real
    // code reads successive bytes back from after issuing a command --
    // both confirmed live, via test_ipl_realfuse.cpp's own real boot trace
    // 2026-09-12: real code writes the standard JEDEC/ONFI NAND `READ ID`
    // opcode (0x90) here, then reads kReadIdDataOffset repeatedly to fetch
    // the real chip's manufacturer/device ID bytes -- used by real Sony
    // code to classify which of a small set of known real NAND suppliers
    // (Toshiba/Samsung/ST/Hynix, confirmed via a real lookup table's own
    // real manufacturer-ID byte values: 0x98/0xEC/0x20/0xAD) is installed,
    // which in turn determines a real memory-size classification. This
    // project has no way to know which of the four real suppliers this
    // specific physical console actually has -- Samsung (0xEC) is modeled
    // as a documented placeholder choice, not a verified fact.
    static constexpr uint32_t kCommandLatchOffset = 0x8;
    static constexpr uint32_t kReadIdDataOffset = 0x300;
    static constexpr uint8_t kReadIdCommand = 0x90;
    static constexpr uint8_t kReadExtraCommand = 0x50;
    static constexpr uint32_t kPageAddressOffset = 0x0C;
    static constexpr uint32_t kStatOffset = 0x04;
    static constexpr uint32_t kEndCommandOffset = 0x14;

    NandController(InterruptController& intc, uint32_t size) : intc_(intc), regs_(size) {}

    // Attaches a real raw NAND dump so command completion deposits genuine
    // page/spare data into the real NAND-page-buffer MMIO region (real base
    // 0xBFF00000, jpcsp's MMIOHandlerNandPage) instead of leaving it blank.
    // `pageBuffer`/`spareOffset` identify where real reboot.prx code
    // actually reads the fetched spare bytes from (confirmed via decoded
    // disassembly: reads at 0xBFF00900/0x904/0x908, i.e. offset 0x900
    // within this region, landing exactly on real spare bytes 0-11 --
    // bytes 8-11 are exactly where a real dump's IPL FAT marker was found).
    // Both pointers may be null (the default, existing accept-anything
    // behavior with no real backing data).
    void attachRealImage(const NandImage* image, MmioStub* pageBuffer, uint32_t spareOffset) {
        image_ = const_cast<NandImage*>(image); // dump is an in-memory copy; guest writes/erases modify it (never written back to disk)
        pageBuffer_ = pageBuffer;
        pageBufferSpareOffset_ = spareOffset;
    }

    uint8_t read8(uint32_t addr) const override {
        if (addr == kReadIdDataOffset && readExtraMode_) return static_cast<uint8_t>(nextExtraWord());
        if (addr == kReadIdDataOffset) return nextReadIdByte();
        return regs_.read8(addr);
    }
    uint16_t read16(uint32_t addr) const override { return regs_.read16(addr); }
    uint32_t read32(uint32_t addr) const override {
        if (addr == kStatOffset) return regs_.read32(addr) | 0x1u; // READY (uofw sceNandIsReady: stat & 1)
        if (addr == kReadIdDataOffset && readExtraMode_) return nextExtraWord();
        if (addr == kReadIdDataOffset) return nextReadIdByte();
        return regs_.read32(addr);
    }

    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }
    void write32(uint32_t addr, uint32_t value) override {
        regs_.write32(addr, value);
        if (addr == kCommandLatchOffset) {
            ++statCmd[static_cast<uint8_t>(value)];
            { const uint8_t c = static_cast<uint8_t>(value); if (c != kReadIdCommand && c != 0x70 && c != 0x60 && c != 0xD0 && c != 0xFF && c != kReadExtraCommand) noteWatch('C', 0, c, 0, nullptr); }
            readExtraMode_ = false;
            extraNeedsAddress_ = false;
            extraIndex_ = 0;
            if (static_cast<uint8_t>(value) == kReadIdCommand) {
                readIdCursor_ = 0;
            } else if (static_cast<uint8_t>(value) == 0x70) {
                // GET_READ_STATUS: bit7 = write enabled, bit0 = last op failed
                readExtraMode_ = true;
                extraWords_[0] = 0xC0u;
            } else if (static_cast<uint8_t>(value) == 0x60) {
                eraseArmed_ = true; // page address follows on 0x0C
            } else if (static_cast<uint8_t>(value) == kReadExtraCommand) {
                // jpcsp MMIOHandlerNand PSP_NAND_COMMAND_READ_EXTRA: the page
                // address is written next (0x0C); the 16 spare bytes are then
                // read back as four words from the data register (0x300).
                readExtraMode_ = true;
                ++statSpareOnly;
                extraNeedsAddress_ = true;
                for (auto& w : extraWords_) w = 0xFFFFFFFFu;
            }
        } else if (addr == kPageAddressOffset) {
            if (eraseArmed_) {
                eraseArmed_ = false;
                ++statErases;
                if (statErases <= 24) { eraseLog[statErases - 1] = value >> 10; if (image_) { const uint8_t* sp0 = image_->spareData((value >> 10) & ~31u); if (sp0) std::memcpy(eraseSpare[statErases - 1], sp0, 16); } }
                if (image_) {
                    const uint32_t first = (value >> 10) & ~31u;
                    uint32_t held = 0;
                    for (uint32_t i = 0; i < 32; ++i) {
                        const uint8_t* m0 = image_->mainData(first + i); const uint8_t* s0 = image_->spareData(first + i);
                        bool dirty = false;
                        if (m0) for (uint32_t k = 0; k < NandImage::kPageSize && !dirty; ++k) dirty = m0[k] != 0xFF;
                        if (s0) for (uint32_t k = 0; k < NandImage::kSpareSize && !dirty; ++k) dirty = s0[k] != 0xFF;
                        if (dirty) { ++held; image_->markDirty(first + i); } // an erase of an already-erased page changes nothing, so it is not written back
                    }
                    if (held) ++statEraseWithData;
                    noteWatch('E', first, held, 0, nullptr);
                    for (uint32_t i = 0; i < 32; ++i) {
                        if (uint8_t* m = image_->mainDataMut(first + i)) std::memset(m, 0xFF, NandImage::kPageSize);
                        if (uint8_t* sp = image_->spareDataMut(first + i)) std::memset(sp, 0xFF, NandImage::kSpareSize);
                    }
                }
            }
            if (readExtraMode_ && extraNeedsAddress_) {
                extraNeedsAddress_ = false;
                if (image_) {
                    const uint8_t* sp = image_->spareData(value >> 10);
                    if (sp) {
                        for (int i = 0; i < 4; ++i) {
                            extraWords_[i] = static_cast<uint32_t>(sp[i * 4]) | (static_cast<uint32_t>(sp[i * 4 + 1]) << 8) |
                                             (static_cast<uint32_t>(sp[i * 4 + 2]) << 16) | (static_cast<uint32_t>(sp[i * 4 + 3]) << 24);
                        }
                    }
                }
            }
        } else if (addr == kEndCommandOffset) {
            if (value == 1) extraIndex_ = 0;
        } else if (addr == kAddressOffset) {
            addressRegister_ = value;
        } else if (addr == kCommandOffset) {
            if ((value & 0x2u) && image_ && (value & 0x1u)) {
                ++statWrites; commitWrite(); // DMA write (0x303): page buffer -> NAND, completion = bit1 pair only
                arm(1);
            } else {
                if ((value & 0x2u) && (value & 0x1u)) { ++statProgramRejected; noteWatch('X', addressRegister_ >> 10, 1, 0, nullptr); } // a program request with no image to program
                if (value & 0x1u) arm(0);
                if (value & 0x2u) arm(1);
            }
        } else if (addr == kStatusOffset) {
            // A direct status write that sets a channel's high bit without
            // its paired low bit models real code kicking off an operation
            // by writing the "pending" pattern straight into status,
            // bypassing the command register entirely.
            // 2026-09-24: lowio's SceNand interrupt handler acks with `... | 0x300` (both high
            // bits set) -- that is NOT a kick-off. Only a lone high bit counts, otherwise 0x301/0x302
            // re-arm each other forever (interrupt storm, 800k page "reads" of one ppn).
            if ((value & 0x300u) == 0x100u && !(value & 0x1u)) arm(0);
            if ((value & 0x300u) == 0x200u && !(value & 0x2u)) arm(1);
            // 2026-09-23: real lowio's SceNand interrupt handler acks by
            // reading status (0x303) and writing the same value back --
            // write-1-to-clear on the pending bits 0/1. jpcsp's working
            // MMIOHandlerNand models the same thing (`dmaInterrupt &= ~3`
            // on any write to 0x38) and drives PSP_NAND_INTR as a LEVEL:
            // asserted while (status & 3) != 0, deasserted otherwise.
            // Without this the value just re-stored, the line never dropped,
            // and the CPU took ~284k back-to-back interrupts.
            if (value & 0x3u) {
                regs_.write32(kStatusOffset, value & ~0x3u);
            }
            if ((regs_.read32(kStatusOffset) & 0x3u) == 0) {
                intc_.clearInterrupt(kInterruptNumber);
            }
        }
    }

    // Advances every pending channel's countdown by one; called once per
    // Cpu::step() via SystemBus::tick(), same pattern as Timer.
    void tick() { advance(1); }
    void advance(uint32_t n) {
        completeIfDue(0, 0x101u, n);
        completeIfDue(1, 0x202u, n);
    }

    // See Bus::ticksUntilEvent: a pending channel completes on the tick that brings its countdown to <= 0.
    uint64_t eventHorizon() const {
        uint64_t best = UINT64_MAX;
        for (int ch = 0; ch < 2; ++ch) {
            if (!pending_[ch]) continue;
            const uint64_t k = ticksRemaining_[ch] > 1 ? static_cast<uint64_t>(ticksRemaining_[ch]) : 1u;
            if (k < best) best = k;
        }
        return best;
    }
    void fastForward(uint64_t n) { // only valid for n < eventHorizon()
        for (int ch = 0; ch < 2; ++ch) if (pending_[ch]) ticksRemaining_[ch] -= static_cast<decltype(ticksRemaining_[ch])>(n);
    }

private:
    void arm(int channel) {
        pending_[channel] = true;
        ticksRemaining_[channel] = kLatencyTicks;
    }

    void completeIfDue(int channel, uint32_t completionBits, uint32_t n) {
        if (!pending_[channel]) {
            return;
        }
        if (ticksRemaining_[channel] > n) {
            ticksRemaining_[channel] -= n;
            return;
        }
        ticksRemaining_[channel] = 0;
        pending_[channel] = false;
        regs_.write32(kStatusOffset, regs_.read32(kStatusOffset) | completionBits);
        intc_.triggerInterrupt(kInterruptNumber);
        ++statReads;
        lastPpns[statReads & 15] = addressRegister_ >> 10;
        {
            const uint32_t pp = addressRegister_ >> 10;
            if (!runs.empty() && runs.back().first + runs.back().second == pp) ++runs.back().second;
            else if (runs.size() < 4000) runs.emplace_back(pp, 1u);
        }
        deliverRealPageIfAttached();
    }

    void commitWrite() {
        const uint32_t ppn = addressRegister_ >> 10;
        uint8_t* m = image_->mainDataMut(ppn);
        uint8_t* sp = image_->spareDataMut(ppn);
        if (!m || !sp) { ++statProgramRejected; noteWatch('X', ppn, 2, 0, nullptr); return; }
        bool changed = false;
        uint8_t first16[16];
        for (uint32_t i = 0; i < NandImage::kPageSize; ++i) { const uint8_t b = pageBuffer_->read8(i); if (b != m[i]) changed = true; m[i] = b; if (i < 16) first16[i] = b; }
        for (uint32_t i = 0; i < 4; ++i) { const uint8_t b = pageBuffer_->read8(0x800 + i); if (b != sp[i]) changed = true; sp[i] = b; }
        for (uint32_t i = 0; i < 12; ++i) { const uint8_t b = pageBuffer_->read8(pageBufferSpareOffset_ + i); if (b != sp[4 + i]) changed = true; sp[4 + i] = b; }
        if (changed) { ++statProgramChanged; image_->markDirty(ppn); }
        noteWatch('P', ppn, changed ? 1u : 0u, static_cast<uint16_t>((sp[6] << 8) | sp[7]), first16);
    }

    void noteWatch(char kind, uint32_t ppn, uint32_t arg, uint16_t lbn, const uint8_t* preview) {
        ++watchSeq;
        if (watchEvents.size() >= kMaxWatchEvents) return;
        WatchEvent e{}; e.seq = watchSeq; e.kind = kind; e.ppn = ppn; e.arg = arg; e.lbn = lbn;
        if (preview) std::memcpy(e.preview, preview, 16);
        watchEvents.push_back(e);
    }

    void deliverRealPageIfAttached() {
        if (!image_ || !pageBuffer_) {
            return;
        }
        const uint32_t ppn = addressRegister_ >> 10;
        const uint8_t* spare = image_->spareData(ppn);
        if (!spare) {
            // Requested page is outside the real dump -- this project's own
            // cursor/seed arithmetic is still an approximation, not a
            // byte-exact real address generator, so this is an expected
            // occurrence rather than an error. Leave the page buffer as-is.
            return;
        }
        if (const uint8_t* m = image_->mainData(ppn)) pageBuffer_->writeBytes(0, m, NandImage::kPageSize);
        // uofw sceNandTransferDataFromNandBuf: spare word0 (user ECC) at 0x800, words 1..3 at 0x900..0x908
        pageBuffer_->writeBytes(0x800, spare, 4);
        pageBuffer_->writeBytes(pageBufferSpareOffset_, spare + 4, NandImage::kSpareSize - 4);
    }

    // Real Samsung NAND READ ID bytes (manufacturer 0xEC, device 0x75).
    // Confirmed live 2026-09-12, via full disassembly of the real
    // classification function at 0x0400B4E8: it gates on struct+0xF
    // (mirrors the table row's byte2 -- must be 3, not 1, checked at
    // 0x0400B520) and then branches on struct+8 (the row's word8, checked
    // at 0x0400B52C/0x0400B53C) three ways --
    //   word8==0x800:  direct success (`beq ...,0x0400B55C`, a0=0), no
    //                  further checks.
    //   word8==0x1000: falls through to a real subsequent call at
    //                  0x0400D374, which just does `lw v0,0(a0)` on real
    //                  MMIO register 0xBC100098 (jpcsp: SystemControl
    //                  offset 0x098, sceSysregGetFuseConfig()) and
    //                  requires bit0 of that register set to succeed.
    //   anything else: unconditional SCE_ERROR_NOT_SUPPORTED via a
    //                  delay-slot `ori $a0,$t2,4` with $t2 pre-loaded to
    //                  0x80000000.
    // The word8==0x1000 path is NOT usable here: this project's own
    // SystemControl (system_control.h) is an unmodeled accept-anything
    // register file that returns 0 for offset 0x098, and jpcsp's own real
    // captured fuseConfig value (0x2400, sceSysreg.java) ALSO has bit0
    // clear -- so real hardware doesn't rely on that bit either. Row 7
    // (byte0=0xEC byte1=0x75 byte2=0x03 word8=0x800) is the Samsung row
    // hitting the direct, dependency-free success path instead.
    uint32_t nextReadIdByte() const {
        // jpcsp MMIOHandlerNand: 0x75 (K9F5608U0C) = 32MB, 0x36 (K9F1208R0B) = 64MB.
        // This console's dump is 64MB, and lfatfs derives its partition layout from the chip size.
        const uint8_t kReadIdBytes[] = {0xECu, static_cast<uint8_t>((image_ && image_->pageCount() > 65536) ? 0x36u : 0x75u), 0x00u, 0x00u};
        const uint8_t b = kReadIdBytes[readIdCursor_ % (sizeof(kReadIdBytes) / sizeof(kReadIdBytes[0]))];
        ++readIdCursor_;
        return b;
    }

    uint32_t nextExtraWord() const {
        const uint32_t w = extraWords_[extraIndex_ & 3u];
        ++extraIndex_;
        return w;
    }

    InterruptController& intc_;
    Memory regs_;
    bool eraseArmed_ = false;
public:
    uint32_t lastPpns[16] = {};
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    uint32_t eraseLog[24] = {};
    uint8_t eraseSpare[24][16] = {};
    uint64_t statReads = 0, statWrites = 0, statErases = 0, statSpareOnly = 0;
    // NAND write watch (PSP_NAND_WATCH, harness side): everything the guest does that could change the flash, seen at the register level so a
    // write that the model does not understand is still visible. kind: 'E' block erase (arg = pages that held data before), 'P' page program
    // (arg = 1 if the page content changed), 'C' an unusual command-latch byte (arg = the byte), 'X' a program request the model could not
    // perform (arg = reason: 1 no image attached, 2 page outside the dump). Only the first kMaxWatchEvents are kept; watchSeq keeps counting.
    struct WatchEvent { uint64_t seq; char kind; uint32_t ppn; uint32_t arg; uint16_t lbn; uint8_t preview[16]; };
    static constexpr size_t kMaxWatchEvents = 4096;
    std::vector<WatchEvent> watchEvents;
    uint64_t watchSeq = 0;
    uint64_t statCmd[256] = {};          // histogram of command-latch bytes (0x8)
    uint64_t statProgramChanged = 0;     // programs that changed the page content
    uint64_t statEraseWithData = 0;      // erases of a block that held something other than 0xFF
    uint64_t statProgramRejected = 0;    // program requests that did nothing (no image / page outside the dump)
private:
    bool readExtraMode_ = false;
    bool extraNeedsAddress_ = false;
    mutable uint32_t extraIndex_ = 0;
    uint32_t extraWords_[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    bool pending_[2] = {false, false};
    uint64_t ticksRemaining_[2] = {0, 0};
    uint32_t addressRegister_ = 0;
    NandImage* image_ = nullptr;
    MmioStub* pageBuffer_ = nullptr;
    uint32_t pageBufferSpareOffset_ = 0;
    mutable uint32_t readIdCursor_ = 0;
};

} // namespace beta
