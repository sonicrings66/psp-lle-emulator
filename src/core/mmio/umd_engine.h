#pragma once
// The UMD drive: the three register blocks the firmware's ata.prx / umdman.prx drive, modelled as one device.
//
//   0xBDF00000  UMD controller  (IRQ 6)  commands (0x01-0x0B), interrupt/enable registers, 10 DMA-like transfer slots into guest RAM
//   0xBD700000  UMD ATA         (IRQ 5)  an ATAPI "packet" device (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ STRUCTURE, READ(10) ...)
//   0xBD600000  ATA2                     reset line of the ATA block (and a few unknown registers)
//
// Behaviour and register meanings follow jpcsp's LLE model (memory/mmio/umd/MMIOHandlerUmd, MMIOHandlerUmdAta, MMIOHandlerAta2 and the
// ATA/ATAPI base class MMIOHandlerBaseAta, themselves based on the ATA/ATAPI-4 and SFF-8020i specifications); the code here is our own.
// Why it exists (2026-10-15): the firmware's reboot path (starting a homebrew EBOOT, going back to the XMB from a game) asks umdman to stop
// the drive and then waits for the drive's own completion interrupt. With the old plain-register stub the first command umdman issued at
// boot never completed, umdman believed a command was still in flight for ever, and the reboot waited on its "state" event flag for ever.
//
// Simplifications, stated so nobody mistakes them for hardware facts:
//  - every command completes immediately (jpcsp delays some by 1-6 ms); the interrupt is raised from inside the register write.
//  - the transfer slots copy straight into guest RAM instead of going through a real DMA engine.
//  - with no disc inserted the drive reports "medium not present" (sense key NOT READY, ASC 0x3A) like jpcsp's.
// A disc is an ISO file, or a CSO (CISO v1) compressed image, read with 2048-byte logical sectors (PSP_UMD_ISO / ini umd_iso); ZSO/PBP images are
// not supported. CSO (2026-10-19): a plain storage/compression container many PSP backup tools produce (ciso.exe, maxcso, ...), unrelated to any
// firmware or CFW code -- a 24-byte header ("CISO", header size, total uncompressed bytes, block size, version, index shift) followed by one
// uint32 per block plus one extra (the index), each giving that block's byte offset in the file (shifted left by the index shift) with bit 31
// marking the block as stored uncompressed ("NC") rather than raw-DEFLATE; a block's compressed span runs from its own offset to the next block's.
// Decoded with this project's own vendored puff() (third_party/puff, already used for PBP/PSAR's zlib and real kernel modules' gzip, see
// src/core/util/inflate.cpp) since CSO's compressed blocks are the same raw DEFLATE stream puff already decodes, just without a zlib/gzip header.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "puff.h"
}

#include "../bus.h"
#include "generic_stub.h"
#include "gpio.h"
#include "interrupt_controller.h"
#include "system_time.h"
#include "../../config/options.h"

namespace beta {

class UmdDrive {
public:
    static constexpr int kAtaIrq = 5, kUmdIrq = 6;
    static constexpr int kGpioPortUmd = 0x1A, kGpioPortReady = 0x18; // jpcsp MMIOHandlerGpio: the UMD-present line (high while a disc is in the drive)
    static constexpr uint32_t kSector = 2048;

    // ATA status bits, interrupt-reason bits, packet operation codes (SFF-8020i / MMC)
    static constexpr int kStError = 0x01, kStDrq = 0x08, kStReady = 0x40, kStBusy = 0x80;
    static constexpr int kReasonCoD = 0x01, kReasonIO = 0x02;
    static constexpr int kCtlSoftReset = 0x04;
    static constexpr int kCmdPacket = 0xA0;
    static constexpr int kOpTestUnitReady = 0x00, kOpRequestSense = 0x03, kOpInquiry = 0x12, kOpReadBig = 0x28, kOpModeSelectBig = 0x55, kOpModeSenseBig = 0x5A,
                         kOpReadStructure = 0xAD, kOpF0 = 0xF0, kOpF1 = 0xF1, kOpF7 = 0xF7, kOpFC = 0xFC;

    UmdDrive(InterruptController& intc, Gpio& gpio, Bus& mem, const config::UmdOptions& opts = {})
        : intc_(intc), gpio_(gpio), mem_(mem), readyDelayMs_(opts.readyMs) {
        ataReset();
    }
    ~UmdDrive() { closeDisc(); }

    // Called now and then from the emulation loop with the system time in ticks (instructions).
    void poll(uint64_t now) {
        now_ = now;
        if (readyPending_ && now >= readyDue_) { readyPending_ = false; gpio_.setLevel(kGpioPortReady); }
    }

    // ---- the disc ------------------------------------------------------------------------------------------------------------
    bool insertIso(const std::string& path) {
        closeDisc();
        iso_ = std::fopen(path.c_str(), "rb");
        if (!iso_) { std::printf("  [UMD] cannot open '%s'\n", path.c_str()); updateUmdLine(); return false; }
        currentPath_ = path; // 2026-10-21: tracked so a game-launch reboot (a real process restart) can carry forward whatever disc is
                             // actually mounted right now -- PSP_UMD_ISO alone only reflects the disc chosen at the ORIGINAL cold boot;
                             // a disc swapped in live afterward (state/umd_swap.txt) never touches that env var, so the restarted process
                             // was silently re-mounting no disc at all (or a stale one) after a live swap, found live during a real test.
        if (openCiso()) {
            std::printf("  [UMD] disc inserted (CSO): %s (%llu sectors, block size %u)\n", path.c_str(), static_cast<unsigned long long>(isoSize_ / kSector), cisoBlockSize_);
        } else {
            std::fseek(iso_, 0, SEEK_SET);
            isoSize_ = 0; isCso_ = false;
            std::fseek(iso_, 0, SEEK_END);
            isoSize_ = static_cast<uint64_t>(std::ftell(iso_));
            std::printf("  [UMD] disc inserted: %s (%llu sectors)\n", path.c_str(), static_cast<unsigned long long>(isoSize_ / kSector));
        }
        updateUmdLine();
        return true;
    }
    void removeDisc() { closeDisc(); currentPath_.clear(); updateUmdLine(); }
    bool hasDisc() const { return iso_ != nullptr; }
    const std::string& currentPath() const { return currentPath_; }

    // ---- UMD controller, 0xBDF00000 -------------------------------------------------------------------------------------------
    uint32_t umdRead(uint32_t off, uint32_t stored) const {
        switch (off) {
            case 0x08: return reset_;
            case 0x10: case 0x14: case 0x18: case 0x1C: case 0x24: case 0x2C: case 0x30: case 0x38: return 0; // unknown / not modelled (jpcsp returns 0)
            case 0x20: return interrupt_;
            case 0x28: return interruptEnabled_;
            case 0x90: return totalTransferLength_;
            default: return stored;
        }
    }
    // returns true when the write was consumed (otherwise the caller stores it in the plain register file)
    bool umdWrite(uint32_t off, uint32_t v) {
        switch (off) {
            case 0x08: setReset(v); return true;
            case 0x10: umdCommand(v); return true;
            case 0x24: interrupt_ &= ~v; umdCheckInterrupt(); return true;
            case 0x28: interruptEnabled_ |= v; umdCheckInterrupt(); return true;
            case 0x2C: interruptEnabled_ &= ~v; umdCheckInterrupt(); return true;
            case 0x30: case 0x38: return v == 0x4; // jpcsp ignores a write of 4, stores the others
            case 0x90: totalTransferLength_ = v; return true;
            case 0x94: return v == 1;
            default:
                if (off >= 0x40 && off <= 0x8C && (off & 3u) == 0) { const uint32_t i = (off - 0x40) / 8; if ((off & 4u) == 0) xferAddr_[i] = v; else xferSize_[i] = v; return true; }
                return false;
        }
    }

    // ---- ATA, 0xBD700000 -----------------------------------------------------------------------------------------------------
    uint8_t ataRead8(uint32_t off, uint8_t stored) {
        switch (off) {
            case 0x1: return static_cast<uint8_t>(error_);
            case 0x2: return static_cast<uint8_t>(sectorCount_);
            case 0x3: return static_cast<uint8_t>(sectorNumber_);
            case 0x4: return static_cast<uint8_t>(cylLow_);
            case 0x5: return static_cast<uint8_t>(cylHigh_);
            case 0x6: return static_cast<uint8_t>(drive_);
            case 0x7: intc_.clearInterrupt(kAtaIrq); return static_cast<uint8_t>(status_); // regular status read clears the interrupt
            case 0xE: return static_cast<uint8_t>(status_);                                  // alternate status does not
            default: return stored;
        }
    }
    bool ataWrite8(uint32_t off, uint8_t v) {
        switch (off) {
            case 0x1: features_ = v; return true;
            case 0x2: sectorCount_ = v; return true;
            case 0x3: sectorNumber_ = v; return true;
            case 0x4: cylLow_ = v; return true;
            case 0x5: cylHigh_ = v; return true;
            case 0x6: drive_ = v; return true;
            case 0x7: ataCommand(v); return true;
            case 0x8: return true; // end of data
            case 0xE: control_ = v; if (control_ & kCtlSoftReset) ataReset(); return true;
            default: return false;
        }
    }
    uint16_t ataRead16(uint32_t off, uint16_t stored) {
        if (off != 0) return stored;
        uint16_t d = 0;
        if (dataIndex_ < dataLength_) {
            d = static_cast<uint16_t>(data_[dataIndex_++]);
            if (dataIndex_ < dataLength_) d |= static_cast<uint16_t>(data_[dataIndex_++] << 8);
        }
        if (dataIndex_ >= dataLength_) commandCompleted();
        return d;
    }
    bool ataWrite16(uint32_t off, uint16_t v) {
        if (off != 0) return false;
        if (dataIndex_ < dataLength_) {
            data_[dataIndex_++] = v & 0xFF;
            if (dataIndex_ < dataLength_) data_[dataIndex_++] = (v >> 8) & 0xFF;
        }
        if (dataIndex_ >= dataLength_) {
            totalDataLength_ -= dataIndex_;
            dataIndex_ = 0;
            if (totalDataLength_ > 0) {
                dataLength_ = totalDataLength_ < kAtaBuf ? totalDataLength_ : kAtaBuf;
                executeWithData(false);
            } else {
                status_ &= ~kStDrq;
                status_ |= kStBusy;
                executeWithData(true);
                pendingOp_ = -1;
            }
        }
        return true;
    }

    // ---- ATA2, 0xBD600000 ----------------------------------------------------------------------------------------------------
    uint32_t ata2Read(uint32_t off, uint32_t stored) const {
        switch (off) {
            case 0x00: return 0x00010033;
            case 0x10: case 0x34: case 0x40: return 0;
            default: return stored;
        }
    }
    bool ata2Write(uint32_t off, uint32_t v) {
        switch (off) {
            case 0x04: return v == 0x04028002;
            case 0x10: if (v != 0) ataReset(); return true;
            case 0x14: return true;
            case 0x1C: return v == 0x00020A0C;
            case 0x34: return v == 0;
            case 0x38: return v == 0x00010100;
            case 0x40: return v == 1;
            case 0x44: return true;
            default: return false;
        }
    }

private:
    static constexpr int kAtaBuf = 512;

    // -- disc / ISO9660 --
    void closeDisc() { if (iso_) { std::fclose(iso_); iso_ = nullptr; } isoSize_ = 0; isCso_ = false; cisoIndex_.clear(); }
    // 2026-10-21 (reverted back to edge-triggering): the level-only version above broke real disc detection --
    // a live test showed the UMD controller itself seeing the disc (direct ATA/packet commands work) but the
    // XMB never learning about it (no game appears in the game list), which fits a real console that only ever
    // discovers a disc change by waking on this GPIO's edge and then polling the controller to confirm it --
    // the drive controller's own IRQ6 (kUmdIrq) only fires in response to a command already issued, so nothing
    // proactively tells software to go issue one. The earlier reasoning (IRQ_GPIO and IRQ_UMDMAN are two
    // separate real interrupt numbers, confirmed against ARK-4's iplsdk) is still correct about there being two
    // distinct real interrupts, but doesn't mean this GPIO bit's edge is unused -- the same comment's own live
    // capture already showed real firmware enabling edge detection on it (enabled_ = 0x05000010). The real
    // SYSCON null-pointer crash this edge can trigger (syscon.prx's event-queue dispatcher at 0x8819D910
    // running via a stray GPIO4 wake with nothing queued) is handled independently by the forcedRanges
    // null-read guard in test_ipl_realfuse.cpp, which doesn't depend on this edge being removed -- so restoring
    // it should be safe on its own.
    void updateUmdLine() { if (iso_) gpio_.setPort(kGpioPortUmd); else gpio_.clearPort(kGpioPortUmd); }
    // Reads the CISO header and index table if the just-opened file is one (magic "CISO" at offset 0); sets isCso_/isoSize_ (the logical,
    // uncompressed size -- everything outside readSectorBytes/the command-0x0A transfer already only cares about that) and leaves the file
    // position wherever it ends up (every actual read reseeks). Returns false (plain-ISO fallback) for anything that isn't a valid CISO v1 header.
    bool openCiso() {
        uint8_t hdr[24];
        if (std::fseek(iso_, 0, SEEK_SET) != 0 || std::fread(hdr, 1, sizeof(hdr), iso_) != sizeof(hdr) || std::memcmp(hdr, "CISO", 4) != 0) return false;
        const uint32_t headerSize = le32(hdr + 4);
        const uint64_t totalBytes = le32(hdr + 8) | (static_cast<uint64_t>(le32(hdr + 12)) << 32);
        const uint32_t blockSize = le32(hdr + 16);
        const uint32_t indexShift = hdr[21];
        if (blockSize == 0 || totalBytes == 0 || headerSize < sizeof(hdr)) return false;
        const uint32_t numBlocks = static_cast<uint32_t>((totalBytes + blockSize - 1) / blockSize);
        std::vector<uint32_t> index(numBlocks + 1);
        if (std::fseek(iso_, static_cast<long>(headerSize), SEEK_SET) != 0) return false;
        for (uint32_t i = 0; i <= numBlocks; ++i) {
            uint8_t e[4];
            if (std::fread(e, 1, 4, iso_) != 4) return false;
            index[i] = le32(e);
        }
        cisoBlockSize_ = blockSize; cisoIndexShift_ = indexShift; cisoIndex_ = std::move(index);
        isCso_ = true; isoSize_ = totalBytes;
        return true;
    }
    // One logical byte range, whether the backing file is a plain ISO (a straight seek+read) or a CSO (walked one block at a time, each
    // block either copied straight through ("NC", bit 31 of its index entry) or inflated with puff() -- shared by readSectorBytes below and
    // the command-0x0A bulk transfer, so both paths understand CSO the same way.
    bool readLogical(uint64_t offset, uint8_t* dst, uint32_t n) const {
        if (!iso_) return false;
        if (!isCso_) {
            if (std::fseek(iso_, static_cast<long>(offset), SEEK_SET) != 0) return false;
            return std::fread(dst, 1, n, iso_) == n;
        }
        uint32_t done = 0;
        std::vector<uint8_t> block(cisoBlockSize_);
        while (done < n) {
            const uint64_t absPos = offset + done;
            const uint32_t blockIdx = static_cast<uint32_t>(absPos / cisoBlockSize_);
            const uint32_t blockOff = static_cast<uint32_t>(absPos % cisoBlockSize_);
            if (static_cast<size_t>(blockIdx) + 1 >= cisoIndex_.size()) return false;
            const bool notCompressed = (cisoIndex_[blockIdx] & 0x80000000u) != 0;
            const uint64_t rawStart = static_cast<uint64_t>(cisoIndex_[blockIdx] & 0x7FFFFFFFu) << cisoIndexShift_;
            const uint64_t rawEnd = static_cast<uint64_t>(cisoIndex_[blockIdx + 1] & 0x7FFFFFFFu) << cisoIndexShift_;
            if (rawEnd < rawStart) return false;
            const uint64_t storedSize = rawEnd - rawStart;
            if (std::fseek(iso_, static_cast<long>(rawStart), SEEK_SET) != 0) return false;
            if (notCompressed) {
                if (storedSize < cisoBlockSize_ || std::fread(block.data(), 1, cisoBlockSize_, iso_) != cisoBlockSize_) return false;
            } else {
                std::vector<uint8_t> comp(storedSize);
                if (storedSize && std::fread(comp.data(), 1, static_cast<size_t>(storedSize), iso_) != storedSize) return false;
                unsigned long destLen = cisoBlockSize_, sourceLen = static_cast<unsigned long>(storedSize);
                if (puff(block.data(), &destLen, comp.data(), &sourceLen) != 0 || destLen != cisoBlockSize_) return false;
            }
            const uint32_t take = std::min<uint32_t>(n - done, cisoBlockSize_ - blockOff);
            std::memcpy(dst + done, block.data() + blockOff, take);
            done += take;
        }
        return true;
    }
    bool readSectorBytes(uint64_t offset, uint8_t* dst, uint32_t n) const { return readLogical(offset, dst, n); }
    static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
    // does the ISO hold this path ("PSP_GAME/PARAM.SFO"; case-insensitive, ";1" version suffixes ignored)?
    bool isoHasFile(const char* path) const {
        if (!iso_) return false;
        uint8_t pvd[kSector];
        if (!readSectorBytes(16ull * kSector, pvd, kSector) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
        uint32_t lba = le32(pvd + 156 + 2), len = le32(pvd + 156 + 10);
        std::string rest(path);
        while (!rest.empty()) {
            const size_t slash = rest.find('/');
            std::string part = rest.substr(0, slash);
            rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
            bool found = false, isDir = false;
            std::vector<uint8_t> dir(len);
            if (len == 0 || len > (64u << 20) || !readSectorBytes(static_cast<uint64_t>(lba) * kSector, dir.data(), len)) return false;
            for (uint32_t pos = 0; pos < len;) {
                const uint32_t rl = dir[pos];
                if (rl == 0) { pos = (pos / kSector + 1) * kSector; continue; }
                if (pos + rl > len) break;
                const uint32_t nl = dir[pos + 32];
                std::string name(reinterpret_cast<const char*>(&dir[pos + 33]), nl);
                const size_t semi = name.find(';'); if (semi != std::string::npos) name.resize(semi);
                bool eq = name.size() == part.size();
                for (size_t i = 0; eq && i < name.size(); ++i) eq = std::toupper(static_cast<unsigned char>(name[i])) == std::toupper(static_cast<unsigned char>(part[i]));
                if (eq) { found = true; isDir = (dir[pos + 25] & 2) != 0; lba = le32(&dir[pos + 2]); len = le32(&dir[pos + 10]); break; }
                pos += rl;
            }
            if (!found || (!rest.empty() && !isDir)) return false;
        }
        return true;
    }

    // -- UMD controller --
    void setReset(uint32_t v) {
        reset_ = v;
        // After a reset the drive spins up and then raises GPIO port 0x18 ("drive ready", jpcsp: MMIOHandlerUmd.setReset). umdman polls that line for up to
        // 10 s after its init and, if it never goes high, shuts the drive down again and releases the UMD interrupt, which is what broke the reboot path
        // (found 2026-10-16). Raising it at once (as jpcsp does) panicked our kernel: the edge reached the GPIO dispatcher before umdman had registered its
        // handlers. So it goes high a few milliseconds later, from poll().
        if (v & 1) { updateUmdLine(); readyPending_ = true; readyDue_ = now_ + static_cast<uint64_t>(readyDelayMs_ * 1000.0 * static_cast<double>(SystemTime::kInstructionsPerMicrosecond)); }
    }
    void umdCheckInterrupt() {
        if (interrupt_ & interruptEnabled_) intc_.triggerInterrupt(kUmdIrq); else intc_.clearInterrupt(kUmdIrq);
    }
    void put8(uint32_t a, uint8_t v) { mem_.write8(a, v); }
    void put32(uint32_t a, uint32_t v) { mem_.write32(a, v); }
    void umdCommand(uint32_t cmd) {
        static const uint8_t kQtgp2[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        static const uint8_t kQtgp3[16] = {0x0F, 0xED, 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        switch (cmd & 0xFF) {
            case 0x01: case 0x02: case 0x03: case 0x09: interrupt_ |= 1; break;
            case 0x04: for (uint32_t i = 0; i < 8 && i < xferSize_[0]; ++i) put8(xferAddr_[0] + i, kQtgp2[i]); interrupt_ |= 1; break;
            case 0x05: for (uint32_t i = 0; i < 16 && i < xferSize_[0]; ++i) put8(xferAddr_[0] + i, kQtgp3[i]); interrupt_ |= 1; break;
            case 0x08: {
                // region information: the first region code in the table whose type nibble matches the disc type (game 0x00, video 0x20, audio 0x40).
                // 2026-10-21: this was a direct copy of jpcsp's own generic HLE stub, which hardcodes region "2" (SCE_PSP_REGION_EUROPE_MIDDLE_EAST_AFRICA)
                // for every category -- jpcsp has no real per-console NAND data to draw from, so it just picks a common default. This project is pure LLE
                // and DOES have this console's own real data: a live dump of real IDStorage key 0x0102 (the same leaf sceChkregCheckRegion's real firmware
                // reads) shows region "4" (SCE_PSP_REGION_UK_IRELAND) in every equivalent slot, not "2" -- confirmed consistent with this console's real,
                // documented owner (UK/Ireland). The mismatch between this stub reporting "2" here and real chkreg reporting "4" elsewhere was found live:
                // after confirming chkreg genuinely gates the "wrong region disc" message (a region-check patch test made it go away), a second, different
                // failure ("the disc could not be read") appeared -- the UMD command sequence stops dead right after TEST UNIT READY, never issuing an
                // actual sector read, consistent with real firmware's own cross-check between this table and chkreg's answer coming up inconsistent.
                // If this project ever supports a different real NAND dump from a different console, this would need to come from that console's own real
                // IDStorage key 0x0102 instead of being hardcoded -- for now there is only the one donor console, so a literal, confirmed-real table is fine.
                static const uint32_t kRegions[] = {0xFFFFFFFF, 0x80000001, 0x00000004, 0x80000000, 0x0000000F, 0x80000000, 0x00000014, 0x80000000, 0x0000001F, 0x80000000,
                                                    0x00000024, 0x80000000, 0x0000002F, 0x80000000, 0x00000034, 0x80000000, 0x0000003F, 0x80000000, 0x00000044, 0x80000000,
                                                    0x0000004F, 0x80000000, 0x1000000F, 0x80000000, 0x1000001F, 0x80000000, 0x1000002F, 0x80000000, 0x1000003F, 0x80000000,
                                                    0x1000004F, 0x80000000, 0x2000000F, 0x80000000, 0x00000001, 0x00000000};
                uint32_t type = 0;
                const bool hasGame = isoHasFile("PSP_GAME/PARAM.SFO"), hasVideo = isoHasFile("UMD_VIDEO/PARAM.SFO"), hasAudio = isoHasFile("UMD_AUDIO/PARAM.SFO");
                if (hasGame) type = 0x00; else if (hasVideo) type = 0x20; else if (hasAudio) type = 0x40;
                const uint32_t a = xferAddr_[0];
                put32(a + 0, 0);
                put32(a + 12, 2);
                for (uint32_t i = 0; i < 48; i += 4) put32(a + 40 + i, 0); // two 24-byte region entries, cleared
                for (size_t i = 0; i + 1 < sizeof(kRegions) / sizeof(kRegions[0]); i += 2)
                    if ((kRegions[i] & 0xF0) == type) { put32(a + 40, kRegions[i]); put32(a + 44, kRegions[i + 1]); break; }
                put32(a + 40 + 24, 1); put32(a + 40 + 28, 0); // last entry must be 1, 0
                interrupt_ |= 1;
                commandCompleted();
                break;
            }
            case 0x0A: {
                // the data of the READ(10) just prepared on the ATA side: sectors from the ISO into the transfer slots, in order
                if (iso_) {
                    int64_t remaining = static_cast<int64_t>(totalTransferLength_ / (kSector + 0x10)) * kSector;
                    uint64_t offset = static_cast<uint64_t>(lba_) * kSector;
                    {
                        std::vector<uint8_t> buf;
                        for (int i = 0; remaining > 0 && i < 10; ++i) {
                            const uint32_t n = xferSize_[i];
                            if (n == 0) continue;
                            buf.assign(n, 0);
                            readLogical(offset, buf.data(), n); // past the end of the image (or a CSO decode failure) just leaves the zero fill
                            const uint32_t dst = xferAddr_[i] & ~3u; // the hardware rounds the address down to a word
                            for (uint32_t k = 0; k < n; ++k) put8(dst + k, buf[k]);
                            remaining -= n;
                            offset += n;
                        }
                    }
                }
                interrupt_ |= 1;
                commandCompleted();
                break;
            }
            case 0x0B: break;
            default: std::printf("  [UMD] unknown controller command 0x%X\n", cmd); break;
        }
        umdCheckInterrupt();
    }

    // -- ATA / ATAPI --
    void ataReset() {
        sectorCount_ = 1; sectorNumber_ = 1;
        cylLow_ = 0x14; cylHigh_ = 0xEB; // signature of a device that implements the PACKET command
        drive_ = 0; dataIndex_ = 0; dataLength_ = 0; totalDataLength_ = 0; pendingOp_ = -1; lba_ = 0;
    }
    void setByteCount(int n) { cylLow_ = n & 0xFF; cylHigh_ = (n >> 8) & 0xFF; }
    void setReason(bool cod, bool io) {
        if (cod) sectorCount_ |= kReasonCoD; else sectorCount_ &= ~kReasonCoD;
        if (io) sectorCount_ |= kReasonIO; else sectorCount_ &= ~kReasonIO;
    }
    void raiseAta() { intc_.triggerInterrupt(kAtaIrq); }
    void ataCommand(int cmd) {
        command_ = cmd; dataIndex_ = 0;
        if (cmd == kCmdPacket) {
            status_ |= kStBusy; setReason(true, false); status_ |= kStDrq; status_ &= ~kStBusy; // ready to receive the 12-byte packet
            dataLength_ = 12; totalDataLength_ = 12; pendingOp_ = -1;
        } else {
            std::printf("  [UMD] unsupported ATA command 0x%02X\n", cmd);
        }
    }
    void prepareInit(int alloc) { dataIndex_ = 0; for (int i = 0; i < alloc && i < kAtaBuf; ++i) data_[i] = 0; }
    void p8(int v) { if (dataIndex_ < kAtaBuf) data_[dataIndex_++] = v & 0xFF; }
    void p16(int v) { p8(v >> 8); p8(v); }
    void p24(int v) { p8(v >> 16); p8(v >> 8); p8(v); }
    void p32(int v) { p8(v >> 24); p8(v >> 16); p8(v >> 8); p8(v); }
    void pstr(const char* s) { for (; *s; ++s) p8(*s); }
    void prepareEnd(int alloc, int total) {
        dataLength_ = alloc < dataIndex_ ? alloc : dataIndex_;
        totalDataLength_ = total > dataLength_ ? total : dataLength_;
        dataIndex_ = 0;
        setByteCount(dataLength_);
        setReason(false, true);
        status_ |= kStDrq; status_ &= ~kStBusy;
        raiseAta();
    }
    void commandCompleted() {
        totalDataLength_ -= dataIndex_;
        dataIndex_ = 0;
        if (totalDataLength_ > 0) {
            dataLength_ = totalDataLength_ < kAtaBuf ? totalDataLength_ : kAtaBuf;
            // a multi-sector non-packet transfer: nothing in this device uses it (the UMD only takes PACKET commands)
        } else {
            dataLength_ = 0;
            status_ &= ~kStDrq; status_ &= ~kStBusy; status_ |= kStReady;
            setReason(true, true);
            raiseAta();
        }
    }
    void executeWithData(bool /*last*/) {
        if (command_ != kCmdPacket) return;
        if (pendingOp_ < 0) executePacket(); else executeParameterList();
    }
    void executeParameterList() {
        // MODE SELECT parameter data: accepted and ignored
        commandCompleted();
    }
    void executePacket() {
        const int op = data_[0];
        int alloc = 0;
        switch (op) {
            case kOpInquiry:
                alloc = data_[4];
                prepareInit(alloc);
                p8(0x05); p8(0x80); p8(0x00); p8(0x32); p8(0x5C); p8(0); p8(0); p8(0);
                pstr("SCEI    "); pstr("UMD ROM DRIVE   "); pstr("    "); pstr("1.090 Oct18 ,2004   ");
                prepareEnd(alloc, alloc);
                break;
            case kOpTestUnitReady: commandCompleted(); break;
            case kOpRequestSense: {
                alloc = data_[4];
                prepareInit(alloc);
                const bool present = iso_ != nullptr;
                p8(0x80); p8(0x00); p8(present ? 0x0 : 0x2); p32(0); p8(10); p32(0);
                if (present) { p8(0); p8(0); } else { p8(0x3A); p8(0x02); } // ASC / ASCQ: medium not present
                p8(0); p8(0); p8(0); p8(0);
                prepareEnd(alloc, alloc);
                break;
            }
            case kOpReadStructure: {
                alloc = data_[9] | (data_[8] << 8);
                const int formatCode = data_[7];
                const int ret = alloc + 4;
                prepareInit(ret);
                if (formatCode == 0x00) {
                    const uint32_t sectors = isoSize_ ? static_cast<uint32_t>(isoSize_ / kSector) : 1800u * (1024u * 1024u / kSector);
                    const uint32_t first = 0x030000, last = first + sectors - 1;
                    p16(alloc + 4); p8(0); p8(0); p8(0x80); p8(0x00); p8(0x01); p8(0xE0); p8(0); p24(first); p8(0); p24(last); p8(0); p24(0); p8(0); p8(0x07);
                    while (dataIndex_ < ret && dataIndex_ < kAtaBuf) p8(0);
                } else std::printf("  [UMD] READ STRUCTURE format 0x%X not modelled\n", formatCode);
                prepareEnd(ret, ret);
                break;
            }
            case kOpF0: prepareInit(1); p8(0x08); prepareEnd(1, 1); break; // 0x08, 0x47, 0x48 and 0x50 are accepted by the driver
            case kOpModeSenseBig: {
                alloc = data_[8] | (data_[7] << 8);
                const int page = data_[2] & 0x3F;
                prepareInit(alloc);
                if (page == 0x1A) {
                    p16(26); p8(0); p8(0); p8(0); p8(0); p8(0); p8(0);
                    p8(0x9A); p8(0x12); p8(0); p8(0x02); p8(0); p8(0); p8(0); p8(0x06); p8(0); p8(0); p8(0); p8(0); p8(0); p8(0); p8(0); p8(0x04); p8(0); p8(0); p8(0); p8(0x04);
                } else std::printf("  [UMD] MODE SENSE page 0x%X not modelled\n", page);
                prepareEnd(alloc, alloc);
                break;
            }
            case kOpModeSelectBig: {
                const int len = data_[8] | (data_[7] << 8);
                dataIndex_ = 0; dataLength_ = len; setByteCount(len);
                pendingOp_ = op;
                setReason(false, false);
                status_ &= ~kStBusy; status_ |= kStDrq;
                raiseAta();
                break;
            }
            case kOpF1: prepareInit(0); prepareEnd(0, 0); break;
            case kOpF7: commandCompleted(); break;
            case kOpFC: {
                alloc = data_[8] | (data_[7] << 8);
                prepareInit(alloc);
                for (int i = 0; i < alloc; ++i) p8(0);
                prepareEnd(alloc, alloc);
                break;
            }
            case kOpReadBig:
                lba_ = static_cast<uint32_t>(data_[5] | (data_[4] << 8) | (data_[3] << 16) | (static_cast<uint32_t>(data_[2]) << 24));
                prepareInit(0);
                prepareEnd(0, 0);
                break;
            default: std::printf("  [UMD] unimplemented packet operation 0x%02X\n", op); break;
        }
    }

    InterruptController& intc_;
    Gpio& gpio_;
    Bus& mem_;
    uint64_t now_ = 0, readyDue_ = 0;
    double readyDelayMs_ = 20.0;
    bool readyPending_ = false;
    std::FILE* iso_ = nullptr;
    uint64_t isoSize_ = 0;
    bool isCso_ = false;
    std::string currentPath_;
    uint32_t cisoBlockSize_ = kSector, cisoIndexShift_ = 0;
    std::vector<uint32_t> cisoIndex_;
    // controller
    uint32_t reset_ = 0, interrupt_ = 0, interruptEnabled_ = 0, totalTransferLength_ = 0;
    uint32_t xferAddr_[10] = {}, xferSize_[10] = {};
    // ATA
    int data_[kAtaBuf] = {};
    int dataIndex_ = 0, dataLength_ = 0, totalDataLength_ = 0;
    int error_ = 0, features_ = 0, sectorCount_ = 0, sectorNumber_ = 0, cylLow_ = 0, cylHigh_ = 0, drive_ = 0, status_ = 0, command_ = 0, control_ = 0;
    int pendingOp_ = -1;
    uint32_t lba_ = 0;
};

// The three register blocks as bus devices. Offsets arrive relative to the block base; registers the drive does not model fall back to a plain
// register file, as before.
class UmdRegs : public MmioStub {
public:
    UmdRegs(UmdDrive& d, uint32_t size) : MmioStub(size), d_(d) {}
    uint32_t read32(uint32_t a) const override { return d_.umdRead(a, MmioStub::read32(a)); }
    void write32(uint32_t a, uint32_t v) override { if (!d_.umdWrite(a, v)) MmioStub::write32(a, v); }
private:
    UmdDrive& d_;
};
class UmdAtaRegs : public MmioStub {
public:
    UmdAtaRegs(UmdDrive& d, uint32_t size) : MmioStub(size), d_(d) {}
    uint8_t read8(uint32_t a) const override { return d_.ataRead8(a, MmioStub::read8(a)); }
    uint16_t read16(uint32_t a) const override { return d_.ataRead16(a, MmioStub::read16(a)); }
    void write8(uint32_t a, uint8_t v) override { if (!d_.ataWrite8(a, v)) MmioStub::write8(a, v); }
    void write16(uint32_t a, uint16_t v) override { if (!d_.ataWrite16(a, v)) MmioStub::write16(a, v); }
private:
    UmdDrive& d_;
};
class Ata2Regs : public MmioStub {
public:
    Ata2Regs(UmdDrive& d, uint32_t size) : MmioStub(size), d_(d) {}
    uint32_t read32(uint32_t a) const override { return d_.ata2Read(a, MmioStub::read32(a)); }
    void write32(uint32_t a, uint32_t v) override { if (!d_.ata2Write(a, v)) MmioStub::write32(a, v); }
private:
    UmdDrive& d_;
};

} // namespace beta
