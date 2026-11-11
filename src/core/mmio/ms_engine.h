#pragma once
// The Memory Stick Pro Duo controller's protocol engine (0xBD200000, interrupt 7 = MSCM0): a port of jpcsp's MMIOHandlerBaseMemoryStick and the Pro-card part of
// MMIOHandlerMemoryStick, both themselves modelled on the Linux memstick driver. The firmware's own sceMSstor / FAT code drives it through programmed I/O: a TPC
// ("transfer protocol command") is written to 0x30, its data goes through the FIFO at 0x34, page data through 0x28, and a command-end interrupt is raised. The card behind it
// is a raw image (util/ms_image.h); nothing about the card's contents is interpreted here except the MBR's first partition entry, which the Pro attribute block copies.
//
// Differences from jpcsp, all deliberate: the engine is synchronous (jpcsp polls a flag a DMA thread sets; the bus here has no such thread), every access the engine
// does not implement is counted instead of silently returning 0 (statUnimpl/lastUnimpl below; a caller's own summary can report the counts), and the sectors come from
// an image file with write-through.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "interrupt_controller.h"
#include "../util/ms_image.h"

namespace beta {

class MsEngine {
public:
    static constexpr uint32_t kPage = 512;
    static constexpr int kInterruptNumber = 7;      // PSP_MSCM0_INTR
    static constexpr int kInterruptBit = 0x0004;

    static constexpr int kCmdStateBusy = 0x0001;
    static constexpr int kSysCommand = 0x0200, kSysInterrupt = 0x0800, kSysReset = 0x8000;
    static constexpr int kStatusReady = 0x1000, kStatusUnknown = 0x2000, kStatusFifoRw = 0x4000;
    // TPC codes
    static constexpr int kTpcReadMgStatus = 0x1, kTpcReadPageData = 0x2, kTpcReadShortData = 0x3, kTpcReadReg = 0x4, kTpcReadIoData = 0x5, kTpcGetInt = 0x7,
                         kTpcSetRwRegAddress = 0x8, kTpcExSetCmd = 0x9, kTpcWriteIoData = 0xA, kTpcWriteReg = 0xB, kTpcWriteShortData = 0xC, kTpcWritePageData = 0xD, kTpcSetCmd = 0xE;
    // INT register
    static constexpr int kRegInt = 0x01, kIntCed = 0x80, kIntBreq = 0x20;
    static constexpr int kRegStatus = 0x02, kRegType = 0x04, kTypePro = 0x01, kRegSystem = 0x10, kSystemSerial = 0x80;
    // commands
    static constexpr int kCmdBlockEnd = 0x33, kCmdReset = 0x3C, kCmdBlockWrite = 0x55, kCmdSleep = 0x5A, kCmdLoadId = 0x60, kCmdCmpIcv = 0x7F, kCmdBlockErase = 0x99,
                         kCmdBlockRead = 0xAA, kCmdClearBuf = 0xC3, kCmdFlashStop = 0xCC;
    static constexpr int kProFormat = 0x10, kProSleep = 0x11, kProWakeup = 0x12, kProReadData = 0x20, kProWriteData = 0x21, kProReadAtrb = 0x24, kProStop = 0x25, kProErase = 0x26,
                         kProReadQuad = 0x27, kProWriteQuad = 0x28, kProSetIbd = 0x46, kProGetIbd = 0x47, kProInIoData = 0xB0, kProOutIoData = 0xB1, kProReadIoAtrb = 0xB2,
                         kProInIoFifo = 0xB3, kProOutIoFifo = 0xB4;
    static constexpr int kOvrDefault = 0xF8, kMngDefault = 0xFF; // overwrite area / management flag defaults

    MsEngine(MsCard& img, InterruptController& intc) : img_(img), intc_(intc) { reset(); }

    void reset() {
        regs_.fill(0);
        interrupt_ = 0; commandState_ = 0; unk08_ = 0; tpc_ = 0; status_ = kStatusReady; sys_ = 0;
        readAddress_ = readSize_ = writeAddress_ = writeSize_ = 0; cmd_ = 0; numberOfPages_ = 0; pageLba_ = 0; pageIndex_ = 0; pageDataIndex_ = 0; dataIndex_ = 0;
        startBlock_ = 0; oobLength_ = 0; oobIndex_ = 0; commandDataIndex_ = 0; dmaTpcCode_ = -1; tpcExSetCmdIndex_ = 0;
        regs_[kRegInt] = kIntCed;
        // the card (jpcsp: MMIOHandlerMemoryStick::reset with simulateMemoryStickPro): geometry as for a Pro card, media inserted, serial mode
        const uint64_t totalPages = img_.sectorCount();
        for (pagesPerBlock_ = 32; pagesPerBlock_ < 0x8000; pagesPerBlock_ <<= 1) { physBlocks_ = static_cast<uint32_t>(totalPages / pagesPerBlock_); if (physBlocks_ < 0x10000) break; }
        logicalBlocks_ = physBlocks_ - (physBlocks_ / 512 * 16);
        status_ |= kStatusReady | 0x0020;
        regs_[kRegType] = kTypePro; regs_[kRegSystem] = kSystemSerial;
        attrReady_ = false;
        intc_.clearInterrupt(kInterruptNumber);
    }

    // ---- register interface (offsets from 0xBD200000) --------------------------------------------------------------------------------------------------------
    uint16_t read16(uint32_t off) {
        switch (off) {
            case 0x00: return static_cast<uint16_t>(interrupt_);
            case 0x04: return static_cast<uint16_t>(commandState_);
            case 0x08: return static_cast<uint16_t>(unk08_);
            case 0x24: return static_cast<uint16_t>(readOob16());
            case 0x28: return static_cast<uint16_t>(readPageData16());
            case 0x30: return static_cast<uint16_t>(tpc_);
            case 0x34: return static_cast<uint16_t>(readData16());
            case 0x38: return static_cast<uint16_t>(getStatus());
            case 0x3C: return static_cast<uint16_t>(sys_);
            default: unimpl("read16", off, 0); return 0;
        }
    }
    uint32_t read32(uint32_t off) {
        switch (off) {
            case 0x28: return readPageData32();
            case 0x30: return static_cast<uint32_t>(tpc_);
            case 0x34: return readTpcData32();
            case 0x38: return static_cast<uint32_t>(getStatus());
            case 0x3C: return static_cast<uint32_t>(sys_);
            case 0x40: return static_cast<uint32_t>(unk40_);
            default: unimpl("read32", off, 0); return 0;
        }
    }
    uint8_t read8(uint32_t off) { unimpl("read8", off, 0); return 0; }
    void write16(uint32_t off, uint16_t v) {
        switch (off) {
            case 0x00: clearInterrupt(v); break;
            case 0x02: break; // unknown in jpcsp too
            case 0x04: writeCommandState(v); break;
            case 0x10: setNumberOfPages(static_cast<int16_t>(v)); break;
            case 0x12: oobLength_ = static_cast<int16_t>(v); break;
            case 0x14: setStartBlock(static_cast<int16_t>(v)); break;
            case 0x16: cmd_ = kCmdBlockRead; setPageLba(static_cast<int16_t>(v)); break;
            case 0x20: break; // unknown in jpcsp too
            case 0x30: startTpc(v); break;
            case 0x34: writeTpcData16(v); break;
            case 0x38: status_ = v; break;
            case 0x3C: writeSys(v); break;
            default: unimpl("write16", off, v); break;
        }
    }
    void write32(uint32_t off, uint32_t v) {
        switch (off) {
            case 0x28: writePageData32(v); break;
            case 0x30: startTpc(static_cast<int32_t>(v)); break;
            case 0x34: writeTpcData(v); break;
            case 0x3C: writeSys(static_cast<int32_t>(v)); break;
            case 0x40: unk40_ = static_cast<int32_t>(v); break;
            default: unimpl("write32", off, v); break;
        }
    }
    void write8(uint32_t off, uint8_t v) {
        if (off == 0x24) writeCommandData8(v);
        else unimpl("write8", off, v);
    }

    // ---- diagnostics -----------------------------------------------------------------------------------------------------------------------------------------
    uint64_t statTpc[16] = {}, statCmd[256] = {}, statUnimpl = 0, statSectorsRead = 0, statSectorsWritten = 0, statInterrupts = 0;
    std::string lastUnimpl;
    uint32_t pagesPerBlock() const { return pagesPerBlock_; }
    uint32_t physicalBlocks() const { return physBlocks_; }
    uint32_t logicalBlocks() const { return logicalBlocks_; }
    int interruptBits() const { return interrupt_; }

private:
    void unimpl(const char* what, uint32_t off, uint32_t v) {
        ++statUnimpl; char b[96]; std::snprintf(b, sizeof(b), "%s 0x%02X (value 0x%X)", what, off, v); lastUnimpl = b;
    }
    int reg(int r) const { return regs_[static_cast<size_t>(r) & 0xFF]; }
    int regN(int r, int n) const { int v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | reg(r + i); return v; }
    void setReg(int r, int v) { regs_[static_cast<size_t>(r) & 0xFF] = static_cast<uint8_t>(v); }
    int getStatus() const { return (status_ & ~0xF) | ((reg(kRegInt) >> 4) & 0xF); }
    void setBusy() { commandState_ |= kCmdStateBusy; }
    void clearBusy() { commandState_ &= ~kCmdStateBusy; }
    bool isBusy() const { return (commandState_ & kCmdStateBusy) != 0; }
    void checkInterrupt() { if (interrupt_ != 0) intc_.triggerInterrupt(kInterruptNumber); else intc_.clearInterrupt(kInterruptNumber); }
    void setInterrupt() { interrupt_ |= kInterruptBit; ++statInterrupts; checkInterrupt(); }
    void clearInterrupt(int bits) {
        // jpcsp: the PSP clears bit 0x1 with a read-modify-write that sometimes also clears 0x4; mirror that quirk exactly
        if (bits & 0x1) bits &= ~0x4;
        interrupt_ &= ~bits;
        checkInterrupt();
    }
    void writeCommandState(int v) { commandState_ = v; if (isBusy()) clearBusy(); }
    void writeSys(int v) {
        if (v & kSysInterrupt) clearInterrupt(kInterruptBit);
        v &= ~(kSysCommand | kSysInterrupt);
        sys_ = v;
        if (sys_ & kSysReset) reset();
    }
    int tpcCode() const { return tpc_ >> 12; }
    int dataCount() const { return regN(0x11, 2); }      // Pro: 2-byte page count
    int dataAddress() const { return regN(0x13, 4); }    // Pro: 4-byte LBA
    void setNumberOfPages(int n) { numberOfPages_ = n; pageIndex_ = 0; pageDataIndex_ = 0; }
    void setPageLba(int lba) { pageLba_ = lba; pageIndex_ = 0; pageDataIndex_ = 0; }
    void setStartBlock(int b) { startBlock_ = b; pageIndex_ = 0; pageDataIndex_ = 0; oobIndex_ = 0; }

    // The "command completed" tail of a data transfer. jpcsp differs per site, so each caller says exactly what its original does: whether the INT register gets CED,
    // and whether the unk08 error-code bits are touched.
    void commandEnd(bool setIntCed, bool withUnk08) {
        if (setIntCed) setReg(kRegInt, kIntCed);
        clearBusy();
        status_ |= kStatusUnknown;
        sys_ |= 0x4000;
        if (withUnk08) { unk08_ |= 0x0040; unk08_ &= ~0x000F; }
        setInterrupt();
    }

    void startTpc(int tpc) {
        tpc_ = tpc;
        const int code = tpcCode(), size = tpc & 0x3FF;
        ++statTpc[code & 0xF];
        switch (code) {
            case kTpcSetRwRegAddress: case kTpcReadReg: case kTpcWriteReg: case kTpcGetInt: break;
            case kTpcSetCmd: setReg(kRegInt, reg(kRegInt) & ~kIntCed); break;
            case kTpcExSetCmd: setReg(kRegInt, reg(kRegInt) & ~kIntCed); tpcExSetCmdIndex_ = 0; break;
            case kTpcReadPageData: readSize_ = 0x200; break;
            case kTpcReadIoData: readSize_ = size; pageDataIndex_ = 0; break;
            case kTpcWriteIoData: case kTpcWritePageData: writeSize_ = size; pageDataIndex_ = 0; break;
            default: unimpl("unknown TPC", static_cast<uint32_t>(code), static_cast<uint32_t>(size)); break;
        }
        status_ |= kStatusFifoRw;
        dmaTpcCode_ = code;
    }

    void startCmd(int cmd) {
        cmd_ = cmd; ++statCmd[cmd & 0xFF];
        bool completed = true;
        switch (cmd) {
            case kCmdBlockRead: setBusy(); break; // Pro: nothing more (the non-Pro registers 0x16/0x17 are not used)
            case kCmdBlockErase: clearBusy(); break;
            case kCmdBlockWrite: break;
            case kCmdSleep: case kProSleep: case kProWakeup: case kProStop: break;
            case kProReadData: setNumberOfPages(dataCount()); setStartBlock(0); setPageLba(dataAddress()); completed = false; break;
            case kProWriteData: setNumberOfPages(dataCount()); setStartBlock(0); setPageLba(dataAddress()); completed = false; setInterrupt(); break;
            case kProReadIoAtrb: case kProInIoFifo: case kProOutIoFifo: completed = false; break;
            default: unimpl("unknown command", static_cast<uint32_t>(cmd), 0); break;
        }
        pageIndex_ = 0; pageDataIndex_ = 0; dataIndex_ = 0;
        status_ |= kStatusUnknown;
        dmaTpcCode_ = -1;
        if (completed) { setReg(kRegInt, kIntCed); sys_ |= 0x4000; setInterrupt(); }
        else setReg(kRegInt, kIntBreq);
    }

    // ---- the card: sectors and the Pro attribute block -------------------------------------------------------------------------------------------------------
    void readPageBuffer() {
        if (cmd_ == kCmdBlockRead || cmd_ == kProReadData) {
            const int lba = pageLba_;
            if (startBlock_ != 0xFFFF) pageLba_ += startBlock_ * static_cast<int>(pagesPerBlock_);
            if (pageLba_ == 0x4000) std::memset(pageBuf_, 0, kPage); // an invalid page the firmware reads during its boot sequence (jpcsp does the same)
            else { img_.readSector(static_cast<uint64_t>(static_cast<uint32_t>(lba)), pageBuf_); ++statSectorsRead; }
            ++pageLba_;
        } else unimpl("readPageBuffer cmd", static_cast<uint32_t>(cmd_), 0);
    }
    void writePageBuffer() {
        if (cmd_ == kProWriteData) {
            int lba = pageLba_;
            if (startBlock_ != 0xFFFF) lba += startBlock_ * static_cast<int>(pagesPerBlock_);
            img_.writeSector(static_cast<uint64_t>(static_cast<uint32_t>(lba)), pageBuf_); ++statSectorsWritten;
            ++pageLba_;
        } else unimpl("writePageBuffer cmd", static_cast<uint32_t>(cmd_), 0);
    }

    static void be16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
    static void be32(uint8_t* p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xFFFF); }
    int addAttrEntry(int index, int address, int size, int id) {
        uint8_t* e = attr_.data() + 16 + index * 12; be32(e, static_cast<uint32_t>(address)); be32(e + 4, static_cast<uint32_t>(size)); e[8] = static_cast<uint8_t>(id);
        return address + size;
    }
    void initAttribute() {
        if (attrReady_) return;
        attr_.fill(0);
        be16(attr_.data(), 0xA5C3); // signature; version 0
        int count = 0, addr = 0x1A0;
        // SYSINFO (96 bytes, big-endian fields; jpcsp fills only these)
        uint8_t* s = attr_.data() + addr;
        s[0] = 2; /* class: Pro */ be16(s + 2, pagesPerBlock_); be16(s + 4, physBlocks_); be16(s + 6, logicalBlocks_); be16(s + 44, kPage); s[51] = 1; /* interface type */
        addr = addAttrEntry(count++, addr, 96, 0x10);
        // DEVINFO (16 bytes, all zero in jpcsp)
        addr = addAttrEntry(count++, addr, 16, 0x30);
        // MBR entry (16 bytes, zero in jpcsp: the firmware reads the real MBR from sector 0)
        addr = addAttrEntry(count++, addr, 16, 0x20);
        // PBR32: the first 96 bytes of the first partition's boot sector, found through the MBR on the card
        uint8_t* pbr = attr_.data() + addr;
        uint8_t mbr[kPage];
        if (img_.readSector(0, mbr) && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            const uint32_t start = static_cast<uint32_t>(mbr[446 + 8]) | (static_cast<uint32_t>(mbr[446 + 9]) << 8) | (static_cast<uint32_t>(mbr[446 + 10]) << 16) | (static_cast<uint32_t>(mbr[446 + 11]) << 24);
            uint8_t sec[kPage];
            if (start != 0 && img_.readSector(start, sec)) std::memcpy(pbr, sec, 96);
        }
        addr = addAttrEntry(count++, addr, 96, 0x22);
        attr_[4] = static_cast<uint8_t>(count);
        attrReady_ = true;
    }

    // ---- data paths --------------------------------------------------------------------------------------------------------------------------------------------
    uint32_t pageBuf32(uint32_t i) const { uint32_t v; std::memcpy(&v, pageBuf_ + (i & (kPage - 4)), 4); return v; }
    uint32_t pageBuf16(uint32_t i) const { uint16_t v; std::memcpy(&v, pageBuf_ + (i & (kPage - 2)), 2); return v; }

    int readOob16() {
        int v;
        if ((oobIndex_ & 3) == 0) v = (kMngDefault << 8) | kOvrDefault;
        else { const int lb = startBlock_ + (oobIndex_ >> 2); v = ((lb & 0xFF) << 8) | ((lb >> 8) & 0xFF); } // endianSwap16
        oobIndex_ += 2;
        if (oobIndex_ >= (oobLength_ << 2)) { oobIndex_ = 0; commandEnd(false, true); }
        return v;
    }
    void increaseReadPageDataIndex(int len) {
        pageDataIndex_ += len;
        if (pageDataIndex_ >= static_cast<int>(kPage)) {
            pageDataIndex_ = 0; ++pageIndex_;
            if (pageIndex_ >= numberOfPages_) { pageIndex_ = 0; commandDataIndex_ = 0; commandEnd(true, true); }
        }
    }
    int readPageData16() {
        int v = 0;
        if (cmd_ == kCmdBlockRead) { if (pageDataIndex_ == 0) readPageBuffer(); v = static_cast<int>(pageBuf16(static_cast<uint32_t>(pageDataIndex_))); }
        else unimpl("readPageData16 cmd", static_cast<uint32_t>(cmd_), 0);
        pageDataIndex_ += 2;
        if (pageDataIndex_ >= static_cast<int>(kPage)) {
            pageDataIndex_ = 0; ++pageIndex_;
            if (pageIndex_ >= numberOfPages_) { pageIndex_ = 0; commandEnd(false, true); }
        }
        return v;
    }
    uint32_t readPageData32() {
        uint32_t v = 0;
        switch (cmd_) {
            case kProReadAtrb: initAttribute(); { const size_t o = static_cast<size_t>(pageLba_) * kPage + static_cast<size_t>(pageDataIndex_); if (o + 4 <= attr_.size()) std::memcpy(&v, attr_.data() + o, 4); } break;
            case kProReadData: if (pageDataIndex_ == 0) readPageBuffer(); v = pageBuf32(static_cast<uint32_t>(pageDataIndex_)); break;
            default: unimpl("readPageData32 cmd", static_cast<uint32_t>(cmd_), 0); break;
        }
        increaseReadPageDataIndex(4);
        return v;
    }
    int readData16() { // 16-bit reads of the data port (TPC READ_IO_DATA / READ_REG / GET_INT); the Pro card has nothing at the IO addresses jpcsp's card serves
        if (dmaTpcCode_ != kTpcReadIoData && dmaTpcCode_ != kTpcReadReg && dmaTpcCode_ != kTpcGetInt) { unimpl("readData16 outside a read TPC", static_cast<uint32_t>(dmaTpcCode_), 0); return 0; }
        int v = 0;
        if (pageDataIndex_ < readSize_) {
            const bool end = dataIndex_ + 2 >= dataCount();
            unimpl("readData16 IO data at address", static_cast<uint32_t>(dataAddress()), static_cast<uint32_t>(dataIndex_));
            pageDataIndex_ += 2; dataIndex_ += 2;
            if (end) commandEnd(true, false);
        }
        return v;
    }
    uint32_t readTpcData32() {
        uint32_t d = 0;
        switch (tpcCode()) {
            case kTpcGetInt: d = static_cast<uint32_t>(reg(kRegInt)); break;
            case kTpcReadReg:
                for (int i = 0; i < 32; i += 8) {
                    if (readSize_ <= 0 || readAddress_ >= 256) break;
                    d |= static_cast<uint32_t>(reg(readAddress_)) << i;
                    --readSize_; ++readAddress_;
                }
                break;
            case kTpcReadIoData: { const uint32_t lo = static_cast<uint32_t>(readData16()), hi = static_cast<uint32_t>(readData16()); d = lo | (hi << 16); break; }
            case kTpcReadPageData: if (pageDataIndex_ == 0) readPageBuffer(); d = pageBuf32(static_cast<uint32_t>(pageDataIndex_)); increaseReadPageDataIndex(4); break;
            default: unimpl("readTpcData32 TPC", static_cast<uint32_t>(tpcCode()), 0); break;
        }
        return d;
    }

    void writeTpcData16(uint16_t) { if (tpc_ >= 0) unimpl("writeTpcData16 TPC", static_cast<uint32_t>(tpcCode()), 0); }
    void increaseWritePageDataIndex() {
        pageDataIndex_ += 4;
        if (pageDataIndex_ >= static_cast<int>(kPage)) {
            pageDataIndex_ = 0;
            writePageBuffer();
            ++pageIndex_;
            if (pageIndex_ >= numberOfPages_) { pageIndex_ = 0; commandDataIndex_ = 0; commandEnd(true, true); }
        }
    }
    void writePageData32(uint32_t v) {
        if (cmd_ == kProWriteData) std::memcpy(pageBuf_ + (static_cast<uint32_t>(pageDataIndex_) & (kPage - 4)), &v, 4);
        else unimpl("writePageData32 cmd", static_cast<uint32_t>(cmd_), 0);
        increaseWritePageDataIndex();
    }
    void writeTpcData(uint32_t v) {
        if (tpc_ < 0) return; // data after a one-word TPC is ignored
        switch (tpcCode()) {
            case kTpcSetRwRegAddress:
                readAddress_ = static_cast<int>(v & 0xFF); readSize_ = static_cast<int>((v >> 8) & 0xFF); writeAddress_ = static_cast<int>((v >> 16) & 0xFF); writeSize_ = static_cast<int>((v >> 24) & 0xFF);
                tpc_ = -1; break;
            case kTpcWriteReg:
                for (int i = 0; i < 4; ++i) { if (writeSize_ <= 0 || writeAddress_ >= 256) break; setReg(writeAddress_, static_cast<int>(v & 0xFF)); ++writeAddress_; --writeSize_; v >>= 8; }
                break;
            case kTpcSetCmd: startCmd(static_cast<int>(v & 0xFF)); tpc_ = -1; break;
            case kTpcExSetCmd:
                switch (tpcExSetCmdIndex_) {
                    case 0: cmd_ = static_cast<int>(v & 0xFF); setReg(0x11, static_cast<int>((v >> 8) & 0xFF)); setReg(0x12, static_cast<int>((v >> 16) & 0xFF)); setReg(0x13, static_cast<int>((v >> 24) & 0xFF)); break;
                    case 1: setReg(0x14, static_cast<int>(v & 0xFF)); setReg(0x15, static_cast<int>((v >> 8) & 0xFF)); setReg(0x16, static_cast<int>((v >> 16) & 0xFF)); startCmd(cmd_); break;
                    default: unimpl("too many EX_SET_CMD parameters", v, 0); break;
                }
                ++tpcExSetCmdIndex_; break;
            case kTpcWriteIoData:
                unimpl("write IO data at address", static_cast<uint32_t>(dataAddress()), v);
                break;
            case kTpcWritePageData: std::memcpy(pageBuf_ + (static_cast<uint32_t>(pageDataIndex_) & (kPage - 4)), &v, 4); increaseWritePageDataIndex(); break;
            default: unimpl("writeTpcData TPC", static_cast<uint32_t>(tpcCode()), v); break;
        }
    }
    void writeCommandData8(int v) { // the byte port at 0x24: command, page count (2 bytes), start LBA (4 bytes), used by READ_ATRB / READ_DATA / WRITE_DATA
        switch (commandDataIndex_) {
            case 0: cmd_ = v; tpc_ = kTpcSetCmd; ++statCmd[v & 0xFF];
                    if (v != kProReadAtrb && v != kProReadData && v != kProWriteData) unimpl("byte-port command", static_cast<uint32_t>(v), 0); break;
            case 1: numberOfPages_ = (numberOfPages_ & 0x00FF) | (v << 8); break;
            case 2: setNumberOfPages((numberOfPages_ & 0xFF00) | v); break;
            case 3: pageLba_ = static_cast<int>((static_cast<uint32_t>(pageLba_) & 0x00FFFFFFu) | (static_cast<uint32_t>(v) << 24)); break;
            case 4: pageLba_ = static_cast<int>((static_cast<uint32_t>(pageLba_) & 0xFF00FFFFu) | (static_cast<uint32_t>(v) << 16)); break;
            case 5: pageLba_ = static_cast<int>((static_cast<uint32_t>(pageLba_) & 0xFFFF00FFu) | (static_cast<uint32_t>(v) << 8)); break;
            case 6: setStartBlock(0); setPageLba(static_cast<int>((static_cast<uint32_t>(pageLba_) & 0xFFFFFF00u) | static_cast<uint32_t>(v))); break;
            default: unimpl("byte-port data beyond index 6", static_cast<uint32_t>(v), static_cast<uint32_t>(commandDataIndex_)); break;
        }
        ++commandDataIndex_;
    }

    MsCard& img_;
    InterruptController& intc_;
    int interrupt_ = 0, commandState_ = 0, unk08_ = 0, tpc_ = 0, status_ = 0, sys_ = 0, unk40_ = 0;
    std::array<uint8_t, 256> regs_{};
    int readAddress_ = 0, readSize_ = 0, writeAddress_ = 0, writeSize_ = 0, tpcExSetCmdIndex_ = 0, cmd_ = 0, oobLength_ = 0, startBlock_ = 0, oobIndex_ = 0;
    int pageLba_ = 0, numberOfPages_ = 0, pageDataIndex_ = 0, pageIndex_ = 0, dataIndex_ = 0, commandDataIndex_ = 0, dmaTpcCode_ = -1;
    uint8_t pageBuf_[kPage] = {};
    std::array<uint8_t, 2 * kPage> attr_{};
    bool attrReady_ = false;
    uint32_t pagesPerBlock_ = 32, physBlocks_ = 0, logicalBlocks_ = 0;
};

} // namespace beta
