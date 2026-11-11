#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>

#include "generic_stub.h"
#include "../net/host_resolver.h"
#include "../net/host_tcp.h"
#include "../net/host_udp.h"
#include "../net/virtual_ap.h"
#include "interrupt_controller.h"
#include "../../config/options.h"

namespace beta {

// The WLAN controller at 0xBD300000: the host side of the Marvell Wi-Fi chip. The host interface is the Memory Stick Pro controller's register block and transfer
// commands (TPCs); what travels over it is the chip's command set (the same as Linux's libertas driver) plus Ethernet frames. Ported from jpcsp (memory/mmio/
// MMIOHandlerBaseMemoryStick.java for the interface, wlan/MMIOHandlerWlan.java for what the chip does); the second half is not here yet.
//
// State of the model (2026-10-11): the register block, the SYS/STATUS/INTERRUPT semantics and the register-file TPCs (SET_RW_REG_ADDRESS, READ_REG, WRITE_REG,
// GET_INT) are modeled. Everything the firmware does beyond that is counted and answered with zeros.
class WlanDevice : public MmioStub {
public:
    static constexpr int kInterruptNumber = 8;          // PSP_WLAN_INTR
    static constexpr int kInterruptBit = 0x0070;        // "found by tracing on a real PSP" (jpcsp)
    static constexpr int kSysCommand = 0x0200, kSysInterrupt = 0x0800, kSysReset = 0x8000;
    static constexpr int kStatusReady = 0x1000, kStatusFifoRw = 0x4000;
    static constexpr int kTpcReadPageData = 0x2, kTpcGetInt = 0x7, kTpcSetRwRegAddress = 0x8, kTpcSetCmd = 0xE, kTpcWriteReg = 0xB, kTpcReadReg = 0x4,
                         kTpcReadIoData = 0x5, kTpcWriteIoData = 0xA, kTpcWritePageData = 0xD, kTpcExSetCmd = 0x9;
    // commands (set with EX_SET_CMD / SET_CMD); the IO ones are what the WLAN driver uses
    static constexpr int kProStop = 0x25;
    static constexpr int kCmdReadIoAtrb = 0xB2, kCmdInIoFifo = 0xB3, kCmdOutIoFifo = 0xB4;
    static constexpr int kStatusUnknown = 0x2000, kIntBreq = 0x20, kCmdStateBusy = 0x0001;
    static constexpr int kRegInt = 0x01, kIntCed = 0x80, kRegType = 0x04, kTypePro = 0x01, kRegSystem = 0x10, kSystemSerial = 0x80;

    explicit WlanDevice(uint32_t size, const config::WlanOptions& opts = {}) : MmioStub(size) {
        // names are looked up with the host resolver; WlanOptions::allowInternet=false keeps the virtual network closed (every lookup fails)
        // WlanOptions::dnsOverride (an IPv4 address) asks that DNS server directly instead of the host resolver
        uint32_t upstream = 0;
        if (!opts.dnsOverride.empty()) { unsigned a = 0, b = 0, c = 0, e = 0; if (std::sscanf(opts.dnsOverride.c_str(), "%u.%u.%u.%u", &a, &b, &c, &e) == 4 && a < 256 && b < 256 && c < 256 && e < 256) upstream = (a << 24) | (b << 16) | (c << 8) | e; }
        if (opts.allowInternet) ap_.resolver = [upstream](const std::string& name, net::VirtualAp::ResolveDone done) { net::resolveHostAsync(name, std::move(done), upstream); };
        if (opts.allowInternet) ap_.tcp().connector = [](uint32_t ip, uint16_t port, net::TcpProxy::HostEvents ev) { return net::hostTcpConnect(ip, port, std::move(ev)); };
        if (opts.allowInternet) ap_.udp().opener = [](uint32_t ip, uint16_t port, net::UdpProxy::OnData cb) { return net::hostUdpOpen(ip, port, std::move(cb)); };
        reset();
    }
    void attachInterrupts(InterruptController* intc) { intc_ = intc; }
    // Called now and then from the emulation loop: frames that arrived from another thread (a finished DNS lookup) are handed to the console.
    void poll() { if (!booting_ && !ap_.empty()) deliverPending(); }
    // The data port has something to read once a read-type TPC has been issued (a DMA that was set up earlier waits for this, see Dmac::sourceReady).
    // The chip lost power: it forgets its firmware, registers and any frames in flight, and boots from scratch when powered on again.
    void powerOff() { event("power off", 0, 0); reset(); }
    // A DMA reading the data port waits until a read command is active and its page still has data. The driver moves a long packet in pages of up to 0x200 bytes (a 0x3F8-byte frame is
    // two TPCs, 0x200 and 0x1F8), setting each DMA up before the TPC that feeds it: a DMA that ran between the pages read zeros from the exhausted page and the second page was lost
    // (the driver waited a second, stopped the transfer and read the same frame again, for ever).
    bool dataReady() const {
        if (tpc_ < 0) return false;
        const int c = (tpc_ >> 12) & 0xF;
        if (c == kTpcReadIoData) return pageDataIndex_ < readSize_;
        return c == kTpcReadReg || c == kTpcGetInt;
    }
    static constexpr uint32_t kDataPortPhys = 0x1D300034u;
    // A DMA writing the data port waits until a write command is active and has room (the console sets the DMA up before it issues the command).
    bool writeReady() const { return tpc_ >= 0 && ((tpc_ >> 12) & 0xF) == kTpcWriteIoData && pageDataIndex_ < writeSize_; }
    std::function<void()> onWriteTpc;   // called when a write or read page starts, so a waiting DMA can run at once

    // The data port can also be read a byte at a time (the driver does this for the attribute block): the byte at +0x34 fetches the next 16 bits, +0x35 is their upper half.
    uint8_t read8(uint32_t addr) const override {
        WlanDevice& d = self();
        const uint32_t off = addr & 0xFFu;
        if (off == 0x34) { d.byteHi_ = static_cast<uint8_t>(d.readTpcData16() >> 8); return static_cast<uint8_t>(d.lastData16_); }
        if (off == 0x35) return d.byteHi_;
        event("R1", addr, 0); return 0;
    }
    uint16_t read16(uint32_t addr) const override { return self().regRead16(addr & 0xFFu); }
    uint32_t read32(uint32_t addr) const override { return self().regRead32(addr & 0xFFu); }
    // The data port is also written a byte at a time (the frame bytes of a transfer): +0x34 is the low byte of a 16-bit unit, +0x35 the high byte and completes it.
    void write8(uint32_t addr, uint8_t value) override {
        const uint32_t off = addr & 0xFFu;
        if (off == 0x34) { byteLo_ = value; return; }
        if (off == 0x35) { writeTpcData16(static_cast<uint16_t>(byteLo_ | (value << 8))); return; }
        event("W1", addr, value);
    }
    void write16(uint32_t addr, uint16_t value) override { regWrite16(addr & 0xFFu, value); }
    void write32(uint32_t addr, uint32_t value) override { regWrite32(addr & 0xFFu, value); }

private:
    WlanDevice& self() const { return const_cast<WlanDevice&>(*this); }

    void reset() {
        regs_.fill(0);
        interrupt_ = 0; commandState_ = 0; unk08_ = 0; tpc_ = 0; status_ = kStatusReady; sys_ = 0; unk40_ = 0;
        readAddress_ = readSize_ = writeAddress_ = writeSize_ = 0; dmaTpc_ = -1;
        regs_[kRegInt] = kIntCed;
        regs_[kRegType] = kTypePro;       // behaves like a Memory Stick Pro
        regs_[kRegSystem] = kSystemSerial; // in serial mode until the firmware switches it
        regs_[0x22] = 0x24;               // unknown register value (jpcsp)
        cmd_ = 0; tpcExSetCmdIndex_ = 0; pageDataIndex_ = 0; dataIndex_ = 0; writeSize_ = 0;
        cmdPacket_.fill(0); sendData_.fill(0); recvData_.fill(0); ap_.clear(); partialAddress_ = -1;
        booting_ = true; bootCodeIndex_ = 0; firmwareIndex_ = -1; sizeBoot_ = 0; sizeFw_ = 0;
        initAttributes();
        checkInterrupt();
    }

    uint16_t regRead16(uint32_t off) {
        switch (off) {
            case 0x00: return static_cast<uint16_t>(interrupt_);
            case 0x04: return static_cast<uint16_t>(commandState_);
            case 0x08: return static_cast<uint16_t>(unk08_);
            case 0x30: return static_cast<uint16_t>(tpc_);
            case 0x38: return static_cast<uint16_t>(getStatus());
            case 0x3C: return static_cast<uint16_t>(sys_);
            default: event("R2?", off, 0); return 0;
        }
    }
    uint32_t regRead32(uint32_t off) {
        switch (off) {
            case 0x30: return static_cast<uint32_t>(tpc_);
            case 0x34: return readTpcData32();
            case 0x38: return static_cast<uint32_t>(getStatus());
            case 0x3C: return static_cast<uint32_t>(sys_);
            case 0x40: return static_cast<uint32_t>(unk40_);
            default: event("R4?", off, 0); return 0;
        }
    }
    void regWrite16(uint32_t off, uint16_t v) {
        switch (off) {
            case 0x00: clearInterrupt(v); break;
            case 0x04: commandState_ = v; break;
            case 0x30: startTpc(v); break;
            case 0x34: writeTpcData16(v); break;
            case 0x38: status_ = v; break;
            case 0x3C: writeSys(v); break;
            default: event("W2?", off, v); break;
        }
    }
    void regWrite32(uint32_t off, uint32_t v) {
        switch (off) {
            case 0x30: startTpc(static_cast<int>(v)); break;
            case 0x34: writeTpcData(v); break;
            case 0x3C: writeSys(static_cast<int>(v)); break;
            case 0x40: unk40_ = static_cast<int>(v); break;
            default: event("W4?", off, v); break;
        }
    }

    int getStatus() const { return (status_ & ~0xF) | ((regs_[kRegInt] >> 4) & 0xF); }   // the low 4 bits mirror the INT register's top nibble
    void checkInterrupt() { if (!intc_) return; if (interrupt_ != 0) intc_->triggerInterrupt(kInterruptNumber); else intc_->clearInterrupt(kInterruptNumber); }
    void clearInterrupt(int bits) {
        if (bits & 0x1) bits &= ~0x4;   // the firmware clears bit 0x1 with a read-modify-write that also clears 0x4 (jpcsp mirrors it)
        interrupt_ &= ~bits;
        event("interrupt cleared", static_cast<uint32_t>(bits), static_cast<uint32_t>(interrupt_));
        checkInterrupt();
    }
    // SYS: the command (0x0200) and interrupt (0x0800) bits are acknowledge bits that do not stay set; 0x0800 acknowledges the interrupt; 0x8000 resets the interface.
    void writeSys(int v) {
        if (v & kSysInterrupt) clearInterrupt(kInterruptBit);
        v &= ~(kSysCommand | kSysInterrupt);
        sys_ = v;
        if (sys_ & kSysReset) { event("reset", 0x3C, static_cast<uint32_t>(v)); reset(); }
    }

    void startTpc(int tpc) {
        tpc_ = tpc;
        const int code = (tpc >> 12) & 0xF, size = tpc & 0x3FF;
        dmaTpc_ = code;
        event((code == 4 || code == 5 || code == 7 || code == 8 || code == 9 || code == 0xB) ? "TPC common" : "TPC", static_cast<uint32_t>(code), static_cast<uint32_t>(size));
        switch (code) {
            case kTpcSetRwRegAddress: case kTpcReadReg: case kTpcWriteReg: case kTpcGetInt: break;
            case kTpcExSetCmd: tpcExSetCmdIndex_ = 0; regs_[kRegInt] &= static_cast<uint8_t>(~kIntCed); break;   // clears CED (command end) in the INT register
            case kTpcReadIoData: readSize_ = size; pageDataIndex_ = 0; if (onWriteTpc) onWriteTpc(); break;   // a DMA waiting for this page can run now
            case kTpcWriteIoData: writeSize_ = size; pageDataIndex_ = 0; haveHalf_ = false; if (onWriteTpc) { status_ |= kStatusFifoRw; onWriteTpc(); } break;
            default: event("TPC (not modeled)", static_cast<uint32_t>(code), static_cast<uint32_t>(size)); break;
        }
        status_ |= kStatusFifoRw;
    }
    // 16 bits from the data port: only the read-the-data TPCs have any.
    uint16_t readTpcData16() {
        uint16_t v = 0;
        switch ((tpc_ >> 12) & 0xF) {
            case kTpcReadIoData: v = static_cast<uint16_t>(readData16()); break;
            case kTpcGetInt: v = regs_[kRegInt]; break;
            default: event("data read 16 (not modeled)", 0x34, static_cast<uint32_t>((tpc_ >> 12) & 0xF)); break;
        }
        lastData16_ = v;
        return v;
    }
    uint32_t readTpcData32() {
        uint32_t d = 0;
        switch ((tpc_ >> 12) & 0xF) {
            case kTpcGetInt: d = regs_[kRegInt]; break;
            case kTpcReadReg:
                if (readAddress_ == 0x4A && booting_) finishBoot();
                for (int i = 0; i < 32; i += 8) { if (readSize_ <= 0 || readAddress_ >= 256) break; d |= static_cast<uint32_t>(regs_[static_cast<size_t>(readAddress_)]) << i; --readSize_; ++readAddress_; }
                break;
            case kTpcReadIoData: { const uint32_t lo = static_cast<uint32_t>(readData16()), hi = static_cast<uint32_t>(readData16()); d = lo | (hi << 16); break; }
            default: event("data read (not modeled)", 0x34, static_cast<uint32_t>((tpc_ >> 12) & 0xF)); break;
        }
        event("data read", 0x34, d);
        return d;
    }
    // 16 bits written to the data port: two of them make the 32-bit word the transfer logic takes.
    void writeTpcData16(uint16_t h) {
        if (tpc_ < 0) return;
        if (((tpc_ >> 12) & 0xF) != kTpcWriteIoData) { event("data write 16 (not modeled) tpc", static_cast<uint32_t>((tpc_ >> 12) & 0xF), h); return; }
        if (pageDataIndex_ >= writeSize_) { event("data write 16 beyond the TPC size", static_cast<uint32_t>(writeSize_), h); return; }
        if (!haveHalf_) { halfLo_ = h; haveHalf_ = true; return; }
        haveHalf_ = false;
        writeTpcData(static_cast<uint32_t>(halfLo_) | (static_cast<uint32_t>(h) << 16));
    }
    void writeTpcData(uint32_t v) {
        if (tpc_ < 0) return;   // data after a one-word TPC is ignored
        switch ((tpc_ >> 12) & 0xF) {
            case kTpcSetRwRegAddress:
                readAddress_ = static_cast<int>(v & 0xFF); readSize_ = static_cast<int>((v >> 8) & 0xFF); writeAddress_ = static_cast<int>((v >> 16) & 0xFF); writeSize_ = static_cast<int>((v >> 24) & 0xFF);
                event("rw register address", static_cast<uint32_t>(readAddress_ | (readSize_ << 8)), static_cast<uint32_t>(writeAddress_ | (writeSize_ << 8)));
                tpc_ = -1; break;
            case kTpcWriteReg:
                for (int i = 0; i < 4; ++i) { if (writeSize_ <= 0 || writeAddress_ >= 256) break; writeRegister(writeAddress_, static_cast<uint8_t>(v & 0xFF)); event("register write", static_cast<uint32_t>(writeAddress_), v & 0xFF); ++writeAddress_; --writeSize_; v >>= 8; }
                break;
            case kTpcExSetCmd:
                switch (tpcExSetCmdIndex_) {
                    case 0: cmd_ = static_cast<int>(v & 0xFF); regs_[0x11] = static_cast<uint8_t>(v >> 8); regs_[0x12] = static_cast<uint8_t>(v >> 16); regs_[0x13] = static_cast<uint8_t>(v >> 24); break;
                    case 1: regs_[0x14] = static_cast<uint8_t>(v); regs_[0x15] = static_cast<uint8_t>(v >> 8); regs_[0x16] = static_cast<uint8_t>(v >> 16); startCmd(cmd_); break;
                    default: event("too many EX_SET_CMD parameters", v, 0); break;
                }
                ++tpcExSetCmdIndex_; break;
            case kTpcWriteIoData:
                if (pageDataIndex_ < writeSize_) {
                    const bool end = dataIndex_ + 4 >= dataCount();
                    writeData32(dataAddress(), dataIndex_, v, end);
                    pageDataIndex_ += 4; dataIndex_ += 4;
                    if (end) commandEnd();
                }
                break;
            default: event("data write (not modeled)", 0x34, v); break;
        }
    }

    // ---- the chip's registers (jpcsp MMIOHandlerWlan.setRegisterValue) ----------------------------------------------------------------------------------------
    static constexpr int kRegResult = 0x54, kResultReadyToSendData = 0x01, kResultPacketReceived = 0x02, kResultReadyToSendCommand = 0x04, kResultEventReceived = 0x08,
                         kResultCommandResponse = 0x10, kResultUnknown80 = 0x80;
    void addResultFlag(int flag) { if (!(regs_[kRegResult] & flag)) { regs_[kRegResult] |= static_cast<uint8_t>(flag); checkResultInterrupt(); } }
    void clearResultFlag(int flag) { if (regs_[kRegResult] & flag) { regs_[kRegResult] &= static_cast<uint8_t>(~flag); checkResultInterrupt(); } }
    // A pending result (something to send, a packet or an event received) raises the interrupt.
    void checkResultInterrupt() { if (regs_[kRegResult] & (kResultReadyToSendData | kResultPacketReceived | kResultReadyToSendCommand | kResultEventReceived)) setInterrupt(); }
    void writeRegister(int reg, uint8_t value) {
        reg &= 0xFF;
        if (reg == kRegResult) { clearResultFlag(static_cast<uint8_t>(~value)); return; }   // writing the result register only clears bits
        regs_[static_cast<size_t>(reg)] = value;
        if (reg == 0x56) addResultFlag(kResultReadyToSendCommand);                          // writing 0x56 also marks the chip ready to take a command
        else if (reg == 0x5E) { if (value & 0x80) addResultFlag(kResultUnknown80); else clearResultFlag(kResultUnknown80); }
    }

    // ---- commands and the data port ------------------------------------------------------------------------------------------------------------------------
    int regN(int r, int n) const { int v = 0; for (int i = 0; i < n; ++i) v = (v << 8) | regs_[static_cast<size_t>(r + i) & 0xFF]; return v; }
    int dataCount() const { return regN(0x11, 2); }      // a Pro card: 2-byte count
    int dataAddress() const { return regN(0x13, 4); }    // and a 4-byte address
    void setInterrupt() { interrupt_ |= kInterruptBit; event("interrupt raised", static_cast<uint32_t>(interrupt_), 0); checkInterrupt(); }
    // The end of a command: only CED in the INT register, not busy, the sys flag the firmware polls, and the interrupt.
    void commandEnd() { regs_[kRegInt] = kIntCed; commandState_ &= ~kCmdStateBusy; status_ |= kStatusUnknown; sys_ |= 0x4000; setInterrupt(); }

    void startCmd(int cmd) {
        // The driver can end a transfer early with the stop command (0x25) when the packet is shorter than the length it announced: the bytes that have arrived are the packet.
        if (haveHalf_) { haveHalf_ = false; if (cmd == kProStop && tpc_ >= 0) writeTpcData(halfLo_); }   // a half-written word at the end of the transfer
        if (partialAddress_ >= 0) {
            const int address = partialAddress_, bytes = partialBytes_;
            partialAddress_ = -1;
            if (cmd == kProStop) {
                swap32Words(sendPacket(address), bytes);
                if (address == kDataAddressCmd) processCommandPacket(bytes); else processTransmitDataPacket(bytes);
            } else event("packet dropped (transfer ended by another command)", static_cast<uint32_t>(address), static_cast<uint32_t>(bytes));
        }
        cmd_ = cmd;
        bool completed = true;
        event("command", static_cast<uint32_t>(cmd), static_cast<uint32_t>(dataCount()));
        switch (cmd) {
            case kCmdReadIoAtrb: case kCmdInIoFifo: case kCmdOutIoFifo: completed = false; break;   // data follows through the data port
            default: event("command (not modeled)", static_cast<uint32_t>(cmd), static_cast<uint32_t>(dataAddress())); break;
        }
        pageDataIndex_ = 0; dataIndex_ = 0;
        status_ |= kStatusUnknown;
        dmaTpc_ = -1;
        if (completed) { regs_[kRegInt] = kIntCed; sys_ |= 0x4000; setInterrupt(); }
        else regs_[kRegInt] = kIntBreq;   // BREQ: the buffer is ready for the data transfer
    }
    // ---- the firmware download (jpcsp MMIOHandlerWlan.writeData32 while "booting") ------------------------------------------------------------------------
    // The driver sends the chip's code with OUT_IO_FIFO: first the boot code (0x85C bytes plus a 0xC header), then the firmware data (0x173FC bytes). The model keeps
    // only the counts: the ARM code is not run (the chip is modeled at command level), but the driver is told the download worked.
    static constexpr int kBootCodeBytes = 0x85C + 0xC, kFirmwareBytes = 0x173FC;
    void writeData32(int address, int index, uint32_t value, bool end) {
        if (!booting_) { writeRunning(address, index, value, end); return; }
        if (outputPacketSize() == 0 && index == 0xC) { bootCodeIndex_ = -1; firmwareIndex_ = 0; setInputPacketSize(0x600); event("boot code received", static_cast<uint32_t>(sizeBoot_), 0); }
        else if (bootCodeIndex_ >= 0) { bootCodeIndex_ += 4; sizeBoot_ += 4; }
        else if (firmwareIndex_ >= 0) { firmwareIndex_ += 4; sizeFw_ += 4; }
    }
    int outputPacketSize() const { return regN(0x44, 2); }
    void setInputPacketSize(int v) { regs_[0x4C] = static_cast<uint8_t>(v >> 8); regs_[0x4D] = static_cast<uint8_t>(v); }
    // After the download the driver reads the 4 bytes at 0x4A and expects the chip's hello value (jpcsp: endianSwap32(0x46554755)).
    void finishBoot() {
        const uint32_t magic = 0x55475546u;
        regs_[0x4A] = static_cast<uint8_t>(magic >> 24); regs_[0x4B] = static_cast<uint8_t>(magic >> 16); regs_[0x4C] = static_cast<uint8_t>(magic >> 8); regs_[0x4D] = static_cast<uint8_t>(magic);
        booting_ = false; firmwareIndex_ = -1;
        event("firmware download finished", static_cast<uint32_t>(sizeBoot_), static_cast<uint32_t>(sizeFw_));
    }

    // ---- the command channel (jpcsp MMIOHandlerWlan.processCommandPacket) ---------------------------------------------------------------------------------
    // The driver writes a command packet to data address 0x100 (data frames go to 0x102), the chip answers in the same buffer: the command with 0x8000 added, the result code
    // at +6, the length of the answer in register 0x4A, and the "response available" flag in the result register, which the driver polls. Packets are stored byte-swapped per
    // 32-bit word while on the host interface.
    static constexpr int kDataAddressCmd = 0x100, kDataAddressPacket = 0x102;
    static constexpr size_t kPacketBytes = 0xC00;
    // the Marvell command numbers (Linux libertas host.h, as in jpcsp)
    static constexpr int kCmdDeauthenticate = 0x0024, kCmdBeaconStop = 0x0049;
    static constexpr int kCmdAuthenticate = 0x0011, kCmdUnknown0012 = 0x0012, kCmdMacRegAccess = 0x0019, kCmdUnknown0026 = 0x0026;
    static constexpr int kCmdScan = 0x0006, kBssInfrastructure = 1, kBssAdhoc = 2;
    static constexpr int kCmdGetHwSpec = 0x0003, kCmdSnmpMib = 0x0016, kCmdMacMulticastAdr = 0x0010, kCmdSetWep = 0x0013, kCmdBbpRegAccess = 0x001A, kCmdRadioControl = 0x001C,
                         kCmdRfChannel = 0x001D, kCmdRfTxPower = 0x001E, kCmdRssi = 0x001F, kCmdRfAntenna = 0x0020, kCmdDataRate = 0x0022, kCmdMacControl = 0x0028,
                         kCmdUnknown007A = 0x007A, kCmdUnknown007D = 0x007D;
    std::array<uint8_t, kPacketBytes>& sendPacket(int address) { return address == kDataAddressPacket ? sendData_ : cmdPacket_; }
    std::array<uint8_t, kPacketBytes>& receivePacket(int address) { return address == kDataAddressPacket ? recvData_ : cmdPacket_; }
    static void swap32Words(std::array<uint8_t, kPacketBytes>& b, int bytes) {
        // every 32-bit word that holds any of the `bytes`, the last one whole even if it is only partly used (jpcsp endianSwap32 does the same; the buffer has room)
        for (int o = 0; o < bytes && o + 3 < static_cast<int>(b.size()); o += 4) { std::swap(b[static_cast<size_t>(o)], b[static_cast<size_t>(o) + 3]); std::swap(b[static_cast<size_t>(o) + 1], b[static_cast<size_t>(o) + 2]); }
    }
    static int rd16(const std::array<uint8_t, kPacketBytes>& b, int o) { return b[static_cast<size_t>(o)] | (b[static_cast<size_t>(o) + 1] << 8); }
    static void wr16(std::array<uint8_t, kPacketBytes>& b, int o, int v) { b[static_cast<size_t>(o)] = static_cast<uint8_t>(v); b[static_cast<size_t>(o) + 1] = static_cast<uint8_t>(v >> 8); }
    static void wr32(std::array<uint8_t, kPacketBytes>& b, int o, uint32_t v) { for (int i = 0; i < 4; ++i) b[static_cast<size_t>(o + i)] = static_cast<uint8_t>(v >> (8 * i)); }
    // A word of a packet the driver is sending.
    void writeRunning(int address, int index, uint32_t value, bool end) {
        std::array<uint8_t, kPacketBytes>& b = sendPacket(address);
        if (index >= 0 && index + 3 < static_cast<int>(b.size())) wr32(b, index, value);
        if (!end) { partialAddress_ = address; partialBytes_ = index + 4; return; }
        partialAddress_ = -1;
        const int size = index + 4;
        swap32Words(b, size);
        if (address == kDataAddressCmd) processCommandPacket(size);
        else processTransmitDataPacket(size);
    }
    // ---- Ethernet frames between the console and the virtual access point (jpcsp MMIOHandlerWlan.processTransmitDataPacket / receiveMessage) ---------------------------------
    // A frame the console sends: a 24-byte header (status, control, where the frame starts, its length, destination, priority, flags) and the frame itself.
    void processTransmitDataPacket(int size) {
        const int loc = rd16(sendData_, 8) | (rd16(sendData_, 10) << 16), len = rd16(sendData_, 12);
        if (loc < 0 || len < 0 || loc + len > static_cast<int>(sendData_.size())) { event("bad data packet", static_cast<uint32_t>(loc), static_cast<uint32_t>(len)); return; }
        (void)size;
        if (len == 0) { addResultFlag(kResultReadyToSendData); return; }   // nothing was written
        ap_.fromConsole(&sendData_[static_cast<size_t>(loc)], static_cast<size_t>(len));
        addResultFlag(kResultReadyToSendData);
        deliverPending();
    }
    // Hands the next frame from the access point to the console, if the receive buffer is free.
    void deliverPending() {
        if (regs_[kRegResult] & kResultPacketReceived) return;
        net::VirtualAp::Frame f;
        if (!ap_.pop(f)) return;
        const int len = static_cast<int>(f.size());
        if (24 + len > static_cast<int>(recvData_.size())) return;   // does not fit: dropped
        recvData_.fill(0);
        wr16(recvData_, 0, 0);                 // rx status
        recvData_[2] = 0; recvData_[3] = 0;    // SNR, control
        wr16(recvData_, 4, len);               // length of the frame
        recvData_[6] = 0; recvData_[7] = 3;    // noise floor, rate (11 Mbit/s)
        wr32(recvData_, 8, 24);                // the frame starts at 24
        std::memcpy(&recvData_[24], f.data(), f.size());
        // the console moves 12 bytes from offset 16 to 24, which overwrites the destination and source addresses: put them at 16 as well so the frame comes out right
        std::memcpy(&recvData_[16], f.data(), f.size() < 12 ? f.size() : 12);
        const int total = 24 + len;
        regs_[0x40] = static_cast<uint8_t>(total >> 8); regs_[0x41] = static_cast<uint8_t>(total);   // received packet length, 2 bytes
        swap32Words(recvData_, total);
        addResultFlag(kResultPacketReceived);
    }
    void sendCommandResponse(int size) {
        wr16(cmdPacket_, 0, rd16(cmdPacket_, 0) | 0x8000);
        const int len = (size + 7) & ~7;
        regs_[0x4A] = static_cast<uint8_t>(len >> 8); regs_[0x4B] = static_cast<uint8_t>(len);   // the answer's length, 2 bytes
        swap32Words(cmdPacket_, size);
        addResultFlag(kResultCommandResponse);
    }
    void processCommandPacket(int size) {
        const int cmd = rd16(cmdPacket_, 0), bodySize = rd16(cmdPacket_, 2), seq = rd16(cmdPacket_, 4);
        event("command packet", static_cast<uint32_t>(cmd), static_cast<uint32_t>(bodySize | (seq << 16)));
        int resultCode = 0, responseSize = size < bodySize ? size : bodySize;
        switch (cmd) {
            case kCmdGetHwSpec:
                wr16(cmdPacket_, 8, 0);     // hardware interface version
                wr16(cmdPacket_, 10, 0);    // hardware version
                wr16(cmdPacket_, 12, 0);    // number of WCB
                wr16(cmdPacket_, 14, 1);    // maximum number of multicast addresses
                for (int i = 0; i < 6; ++i) cmdPacket_[static_cast<size_t>(16 + i)] = kMacAddress[i];
                wr16(cmdPacket_, 22, 0);    // region code
                wr16(cmdPacket_, 24, 0);    // number of antennas
                wr32(cmdPacket_, 26, 0x01000000u);   // firmware release number
                wr32(cmdPacket_, 42, 0);    // firmware capability information
                responseSize = size < bodySize + 2 ? size : bodySize + 2;
                break;
            case kCmdRfAntenna: {   // set (1) / get (2) the antenna: accepted for antenna 1
                const int action = rd16(cmdPacket_, 8), antenna = rd16(cmdPacket_, 10);
                if (!((action == 1 || action == 2) && antenna == 1)) { event("RF_ANTENNA (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(antenna)); resultCode = -1; }
                break;
            }
            case kCmdRadioControl: {
                const int action = rd16(cmdPacket_, 8), control = rd16(cmdPacket_, 10);
                if (!(action == 1 && (control == 1 || control == 3))) { event("RADIO_CONTROL (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(control)); resultCode = -1; }
                break;
            }
            case kCmdRfTxPower: {
                const int action = rd16(cmdPacket_, 8), level = rd16(cmdPacket_, 10);
                if (action == 1 && level == 10) { cmdPacket_[12] = 20; cmdPacket_[13] = 0; }   // maximum and minimum valid power level
                else { event("RF_TX_POWER (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(level)); resultCode = -1; }
                break;
            }
            case kCmdDataRate: {
                const int action = rd16(cmdPacket_, 8);
                if (action == 0) { cmdPacket_[12] = 0; }   // set TX auto (jpcsp writes the rate and then the end marker)
                else { event("DATA_RATE (not modeled)", static_cast<uint32_t>(action), 0); resultCode = -1; }
                break;
            }
            case kCmdSnmpMib: {
                const int action = rd16(cmdPacket_, 8), oid = rd16(cmdPacket_, 10), vsize = rd16(cmdPacket_, 12);
                const bool known = action == 1 && ((oid == 5 && vsize == 2) || (oid == 6 && vsize == 2) || (oid == 8 && vsize == 2) || (oid == 0 && vsize == 1));   // RTS, short retry, fragment threshold, BSS type
                if (!known) { event("SNMP_MIB (not modeled)", static_cast<uint32_t>(action | (oid << 16)), static_cast<uint32_t>(vsize)); resultCode = -1; }
                break;
            }
            case kCmdRfChannel: {
                const int action = rd16(cmdPacket_, 8), channel = rd16(cmdPacket_, 10);
                if (!(action == 1 && (channel == 1 || channel == 11))) { event("RF_CHANNEL (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(channel)); resultCode = -1; }
                break;
            }
            case kCmdSetWep: {
                const int action = rd16(cmdPacket_, 8), key = rd16(cmdPacket_, 10);
                if (!(action == 4 && key == 0)) { event("SET_WEP (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(key)); resultCode = -1; }   // 4 = remove
                break;
            }
            case kCmdRssi:
                if (rd16(cmdPacket_, 8) == 0) { wr16(cmdPacket_, 8, 40); wr16(cmdPacket_, 10, 0); wr16(cmdPacket_, 12, 0); wr16(cmdPacket_, 14, 0); }   // SNR 40 = full signal
                else event("RSSI (not modeled)", static_cast<uint32_t>(rd16(cmdPacket_, 8)), 0);
                break;
            case kCmdBbpRegAccess: {
                const int action = rd16(cmdPacket_, 8), reg = rd16(cmdPacket_, 10);
                if (!(action == 1 && (reg == 0x2C || reg == 0x2D))) event("BBP_REG_ACCESS (not modeled)", static_cast<uint32_t>(action), static_cast<uint32_t>(reg));
                break;
            }
            case kCmdMacMulticastAdr:   // set the multicast list: nothing to do with a virtual network
                if (rd16(cmdPacket_, 8) != 1) event("MAC_MULTICAST_ADR (not modeled)", static_cast<uint32_t>(rd16(cmdPacket_, 8)), 0);
                break;
            case kCmdMacControl: case kCmdUnknown007A: case kCmdUnknown007D:   // accepted as they are
                break;
            case kCmdAuthenticate: case kCmdUnknown0026: case kCmdMacRegAccess:   // joining the virtual network: always accepted
            case kCmdDeauthenticate: case kCmdBeaconStop:                          // leaving it: accepted too
                break;
            case kCmdUnknown0012:
                wr16(cmdPacket_, 10, 0);
                break;
            case kCmdScan: {
                // Answers a scan with the virtual access point (jpcsp builds a fixed list the same way): per network its signal, BSSID, beacon interval, capabilities and
                // the information elements the firmware reads (channel, SSID, rates), then the TSF timestamps.
                const int bssType = cmdPacket_[10];
                const int count = bssType == kBssInfrastructure ? 1 : 0;   // ad-hoc networks are not modeled
                event("scan", static_cast<uint32_t>(bssType), static_cast<uint32_t>(count));
                for (int i = 11; i < 32; ++i) cmdPacket_[static_cast<size_t>(i)] = 0;   // clears the request's MAC filter and SSID
                cmdPacket_[32] = static_cast<uint8_t>(count);
                int offset = 33;
                for (int n = 0; n < count; ++n) {
                    cmdPacket_[static_cast<size_t>(14 + n)] = 40;   // signal strength 0..40 = 100%
                    for (int i = 0; i < 6; ++i) cmdPacket_[static_cast<size_t>(offset + 2 + i)] = kApBssid[i];
                    for (int i = 0; i < 8; ++i) cmdPacket_[static_cast<size_t>(offset + 8 + i)] = 0;   // timestamp
                    wr16(cmdPacket_, offset + 16, 100);           // beacon interval (must not be 0)
                    wr16(cmdPacket_, offset + 18, 0x0020 | 0x0001);   // short preamble | BSS (infrastructure); no privacy bit = an open network
                    int q = offset + 20;
                    auto ie = [&](int id, const uint8_t* data, int len) { cmdPacket_[static_cast<size_t>(q)] = static_cast<uint8_t>(id); cmdPacket_[static_cast<size_t>(q) + 1] = static_cast<uint8_t>(len); for (int i = 0; i < len; ++i) cmdPacket_[static_cast<size_t>(q + 2 + i)] = data[i]; q += 2 + len; };
                    { const uint8_t fh[5] = {0, 0, 0, 0, 0}; ie(2, fh, 5); }                                   // FH parameters
                    { const uint8_t ch = kApChannel; ie(3, &ch, 1); }                                        // DS parameters: the channel
                    ie(0, reinterpret_cast<const uint8_t*>(kApSsid), static_cast<int>(sizeof(kApSsid) - 1)); // SSID
                    { const uint8_t rates[4] = {0x82, 0x84, 0x0B, 0x16}; ie(1, rates, 4); }                  // supported rates 1, 2, 5.5, 11 Mbit/s
                    { const uint8_t ext[2] = {0x0C, 0x12}; ie(50, ext, 2); }                                 // extended rates 6, 9 Mbit/s
                    { const uint8_t ibss[2] = {0, 0}; ie(6, ibss, 2); }                                      // IBSS parameters (ATIM window)
                    wr16(cmdPacket_, offset, q - offset - 2);     // total length of the elements
                    offset = q;
                }
                for (int n = 0; n < count; ++n) { for (int i = 0; i < 8; ++i) cmdPacket_[static_cast<size_t>(offset + i)] = 0; offset += 8; }   // TSF timestamps
                wr16(cmdPacket_, 30, offset - 33);   // size of the scan response buffer
                responseSize = offset;
                break;
            }
            default:
                event("command not modeled", static_cast<uint32_t>(cmd), static_cast<uint32_t>(size));
                resultCode = -1;
                break;
        }
        wr16(cmdPacket_, 6, resultCode);
        if (responseSize >= 0) sendCommandResponse(responseSize);
    }
    // The console's MAC address: a Sony OUI (00:01:4A) with a fixed tail. (The real one is in the console's ID storage and is not read.)
    static constexpr uint8_t kMacAddress[6] = {0x00, 0x01, 0x4A, 0x5E, 0x5E, 0x01};
    // The virtual network: an open infrastructure network on channel 6 (locally administered BSSID).
    static constexpr uint8_t kApBssid[6] = {0x02, 0x50, 0x53, 0x50, 0x4C, 0x01};
    static constexpr char kApSsid[] = "PSP-LLE";
    static constexpr uint8_t kApChannel = 6;

    // 16 bits from the data port of the current command (jpcsp MMIOHandlerWlan.readData16).
    int readData16() {
        int value = 0;
        if (pageDataIndex_ < readSize_) {
            const bool end = dataIndex_ + 2 >= dataCount();
            switch (cmd_) {
                case kCmdReadIoAtrb:
                    if (dataAddress() == 0 && dataIndex_ + 1 < static_cast<int>(attributes_.size())) value = attributes_[static_cast<size_t>(dataIndex_)] | (attributes_[static_cast<size_t>(dataIndex_) + 1] << 8);
                    else if (dataAddress() == kDummyAttributeEntry) value = 0;
                    else event("attribute read (not modeled)", static_cast<uint32_t>(dataAddress()), static_cast<uint32_t>(dataIndex_));
                    break;
                case kCmdInIoFifo: {
                    const std::array<uint8_t, kPacketBytes>& pk = receivePacket(dataAddress());
                    if (dataIndex_ + 1 < static_cast<int>(pk.size())) value = pk[static_cast<size_t>(dataIndex_)] | (pk[static_cast<size_t>(dataIndex_) + 1] << 8);
                    if (end) {
                        if (dataAddress() == kDataAddressCmd) { clearResultFlag(kResultCommandResponse); addResultFlag(kResultReadyToSendCommand); }
                        else if (dataAddress() == kDataAddressPacket) { clearResultFlag(kResultPacketReceived); deliverPending(); }
                    }
                    break;
                }
                default: event("data port read (not modeled) cmd", static_cast<uint32_t>(cmd_), static_cast<uint32_t>(dataIndex_)); break;
            }
            pageDataIndex_ += 2; dataIndex_ += 2;
            if (end) commandEnd();
        }
        return value;
    }
    // The 0x40-byte attribute block the driver reads with READ_IO_ATRB: a card version and 7 dummy entries (jpcsp), as little-endian 32-bit words byte-swapped.
    static constexpr int kDummyAttributeEntry = 0x1234;
    void initAttributes() {
        attributes_.fill(0);
        // The driver reads each field as swap16(lhu(buffer)) from a buffer the DMA filled low byte first, so the block holds big-endian fields (checked against the driver: its table of
        // accepted cards is {0x0011,0x0001,0x0001}, {0x0011,0x0001,0x1B18}, {0x0011,0x0002,0x1B11}, {0x0011,0x0002,0x0B11}, the same four jpcsp lists).
        auto w16 = [&](int o, int v) { attributes_[static_cast<size_t>(o)] = static_cast<uint8_t>(v >> 8); attributes_[static_cast<size_t>(o) + 1] = static_cast<uint8_t>(v); };
        w16(0, 0x0011); w16(2, 0x0001); w16(4, 0x0001);
        for (int i = 1; i < 8; ++i) { const int o = i * 8; w16(o, i); w16(o + 2, kDummyAttributeEntry); w16(o + 4, 0x0040); }
        // What reaches the driver's buffer is this block with each 4-byte group reversed (measured from the guest buffer: the DMA takes the port 32 bits at a time), so store it reversed.
        for (size_t o = 0; o < attributes_.size(); o += 4) { std::swap(attributes_[o], attributes_[o + 3]); std::swap(attributes_[o + 1], attributes_[o + 2]); }
    }

    // Diagnostic event trace, dropped in beta (was gated behind PSP_WLAN_LOG/
    // PSP_WLAN_VERBOSE with no behavioral effect of its own); call sites
    // throughout this file are left as-is since this is now a no-op.
    void event(const char*, uint32_t, uint32_t) const {}

    InterruptController* intc_ = nullptr;
    int interrupt_ = 0, commandState_ = 0, unk08_ = 0, tpc_ = 0, status_ = 0, sys_ = 0, unk40_ = 0;
    int readAddress_ = 0, readSize_ = 0, writeAddress_ = 0, writeSize_ = 0, dmaTpc_ = -1;
    std::array<uint8_t, 256> regs_{};
    std::array<uint8_t, 0x40> attributes_{};
    uint8_t byteHi_ = 0, byteLo_ = 0; uint16_t lastData16_ = 0, halfLo_ = 0; bool haveHalf_ = false;
    std::array<uint8_t, kPacketBytes> cmdPacket_{}, sendData_{}, recvData_{};
    net::VirtualAp ap_;
    int partialAddress_ = -1, partialBytes_ = 0;
    bool booting_ = true; int bootCodeIndex_ = 0, firmwareIndex_ = -1, sizeBoot_ = 0, sizeFw_ = 0;
    int cmd_ = 0, tpcExSetCmdIndex_ = 0, pageDataIndex_ = 0, dataIndex_ = 0;
};

} // namespace beta
