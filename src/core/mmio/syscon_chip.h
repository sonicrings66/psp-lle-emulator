#pragma once

#include <ctime>
#include <functional>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../bus.h"
#include "../memory.h"
#include "../../config/options.h"
#include "gpio.h"

namespace beta {

// Syscon serial interface, real base 0xBE580000 (jpcsp MMIOHandlerSyscon, its
// built-in default-response path). The Baryon microcontroller itself is not
// emulated; this models the Tachyon-side register protocol plus canned,
// real-console-derived responses:
//   TX: guest writes 16-bit words to 0x08 (big-endian byte pairs) into data[]:
//       data[0]=cmd, data[1]=len, data[2..]=args.  Writing 0x04 with bit1 set
//       starts the command (bit2 resets the data cursor); bit1 clear just
//       drops the END_CMD GPIO line.
//   RX: response = data[]: [0]=Baryon status, [1]=len(=respBytes+2), [2..]=
//       response (first byte 0x82 = OK), then a one's-complement checksum byte
//       at data[len]. Guest reads 16-bit words from 0x08; 0x0C bit2 = more data
//       available, bit0 = no error.
//   Completion: raise GPIO port 4 (GPIO_PORT_SYSCON_END_CMD) -> IRQ 4.
// Baryon 0x00263100 / Pommel 0x00000133 are read off the user's real
// PSP-3000 (TA-090v3, 03g) via PSPident; the rest follow jpcsp's defaults.
class SysconChip : public Bus {
public:
    static constexpr int kMaxData = 16;
    static constexpr uint32_t kBaryonVersion = 0x00263100u;
    static constexpr uint32_t kPommelVersion = 0x00000133u;

    SysconChip(Gpio& gpio, uint32_t size, const config::SysconOptions& opts = {}) : gpio_(gpio), regs_(size) {
        // Static WLAN-switch state (see wlanSwitchOn() below) is read once,
        // on the first SysconChip ever constructed -- matches the old
        // project's "first call wins" lazy-getenv-read semantics in
        // practice, since exactly one SysconChip exists per process and is
        // constructed before any register access can reach wlanSwitchOn().
        if (wlanSwitchState.load() < 0) {
            wlanSwitchState = opts.wlanSwitchOn ? 1 : 0;
            if (opts.wlanSwitchOn) wlanEverOn = true;
        }
        if (opts.rtcSeedFlags != 0) { // start as a console whose clock was already initialised. These are exactly the values the firmware itself writes
            // the first time (zero scratchpad): WRITE_SCRATCHPAD(8, 00 76 48 32), WRITE_SCRATCHPAD(16, 18 93 17 2B 1C 00 00 00) (= tick >> 19 of 2011-01-01), WRITE_ALARM(0xFFFFFFFF), WRITE_CLOCK(0).
            static const uint8_t s8[4] = {0x00, 0x76, 0x48, 0x32}, s16[8] = {0x18, 0x93, 0x17, 0x2B, 0x1C, 0x00, 0x00, 0x00};
            if (opts.rtcSeedFlags & 2) for (int k = 0; k < 4; ++k) scratch_[8 + k] = s8[k]; // flag bit 2: also the [8..11] word
            for (int k = 0; k < 8; ++k) scratch_[16 + k] = s16[k];
            clock_ = 0; alarm_ = 0xFFFFFFFFu; ticking_ = true;
        }
    }

    // The RTC counter (READ_CLOCK / WRITE_CLOCK, whole seconds) counts in emulated time from the moment it was last written, when ticking is on. A real PSP's counter
    // runs inside the battery-backed Syscon; with ticking off (the default, so headless runs stay reproducible) it is a fixed value.
    std::function<uint64_t()> nowUsec; // emulated microseconds, set by SystemBus

    // ---- the battery-backed part of the Syscon (pure LLE persistence) ---------------------------------------------------------------
    // On a real PSP the Syscon keeps its scratchpad, the alarm and the RTC counter alive on the battery, so the date survives power-off and the firmware
    // only initialises the clock when that memory is blank. This is the state a restart must give back: the 32 scratchpad bytes, the alarm, the RTC counter
    // and the host's wall-clock time when it was captured (a real RTC keeps counting while the console is off, so on restore the counter is advanced by the
    // real time that has passed).
    struct BatteryState { std::array<uint8_t, 32> scratch{}; uint32_t alarm = 0, clock = 0; uint64_t hostTime = 0; };
    BatteryState batteryState(uint64_t hostNowSec) const { BatteryState b; b.scratch = scratch_; b.alarm = alarm_; b.clock = readClock(); b.hostTime = hostNowSec; return b; }
    void restoreBattery(const BatteryState& b, uint64_t hostNowSec) {
        scratch_ = b.scratch; alarm_ = b.alarm;
        clock_ = b.clock + static_cast<uint32_t>(hostNowSec > b.hostTime ? hostNowSec - b.hostTime : 0u);
        clockSetUsec_ = nowUsec ? nowUsec() : 0; ticking_ = true; batteryDirty_ = false;
    }
    void enableClockTicking() { if (!ticking_) { clockSetUsec_ = nowUsec ? nowUsec() : 0; ticking_ = true; } }
    bool batteryDirty() const { return batteryDirty_; }   // the firmware wrote the scratchpad, alarm or clock since the last save
    void clearBatteryDirty() { batteryDirty_ = false; }
    static constexpr char kBatteryMagic[8] = {'P', 'S', 'P', 'B', 'A', 'T', '0', '1'};
    static bool writeBatteryFile(const std::string& path, const BatteryState& b) {
        std::FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return false;
        uint8_t buf[8 + 32 + 4 + 4 + 8]; std::memcpy(buf, kBatteryMagic, 8); std::memcpy(buf + 8, b.scratch.data(), 32);
        std::memcpy(buf + 40, &b.alarm, 4); std::memcpy(buf + 44, &b.clock, 4); std::memcpy(buf + 48, &b.hostTime, 8);
        const bool ok = std::fwrite(buf, 1, sizeof(buf), f) == sizeof(buf); return std::fclose(f) == 0 && ok;
    }
    static bool readBatteryFile(const std::string& path, BatteryState& b) {
        std::FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
        uint8_t buf[8 + 32 + 4 + 4 + 8]; const bool ok = std::fread(buf, 1, sizeof(buf), f) == sizeof(buf); std::fclose(f);
        if (!ok || std::memcmp(buf, kBatteryMagic, 8) != 0) return false;
        std::memcpy(b.scratch.data(), buf + 8, 32); std::memcpy(&b.alarm, buf + 40, 4); std::memcpy(&b.clock, buf + 44, 4); std::memcpy(&b.hostTime, buf + 48, 8);
        return true;
    }

    uint32_t read32(uint32_t addr) const override {
        switch (addr) {
            case 0x04: return 0;
            case 0x08: return const_cast<SysconChip*>(this)->readData16();
            case 0x0C: return flags0C();
            case 0x18: return 0;
            default: return regs_.read32(addr);
        }
    }
    void write32(uint32_t addr, uint32_t value) override {
        switch (addr) {
            case 0x00: if (value != 0xCFu) regs_.write32(addr, value); break;
            case 0x04: setFlags04(value); break;
            case 0x08: writeData16(value); break;
            case 0x14: if (value != 0) regs_.write32(addr, value); break;
            case 0x20: if (value & 3u) error_ = 0; break;
            case 0x24: if (value != 0) regs_.write32(addr, value); break;
            default: regs_.write32(addr, value); break;
        }
    }
    uint16_t read16(uint32_t addr) const override {
        return static_cast<uint16_t>(read32(addr & ~3u) >> ((addr & 2u) * 8));
    }
    uint8_t read8(uint32_t addr) const override {
        return static_cast<uint8_t>(read32(addr & ~3u) >> ((addr & 3u) * 8));
    }
    void write8(uint32_t addr, uint8_t value) override { regs_.write8(addr, value); }
    void write16(uint32_t addr, uint16_t value) override { regs_.write16(addr, value); }

public:
    static inline uint32_t buttons = 0; // SceCtrl bit mask of pressed buttons (harness-driven input)
    // The WLAN slider on the console's edge is read through the key data as SCE_CTRL_WLAN_UP (0x40000, set = switch on; jpcsp Controller.java does the same).
    // Off by default, as before: the WLAN controller is not modeled yet. SysconOptions::wlanSwitchOn turns it on.
    // The slider can be flipped while running (F8 in the window): the driver learns of the switch and of the power-up from changes, so the edge matters.
    static inline std::atomic<int> wlanSwitchState{-1};   // -1 = not read from the environment yet
    static inline std::atomic<bool> wlanEverOn{false};    // once the switch has been on, the WLAN power bit in the status follows the real power state
    // Initialized by the first SysconChip's constructor from SysconOptions
    // (was a lazily-read PSP_WLAN_SWITCH env var); -1 ("not read yet") can
    // only be observed if this is called before any SysconChip exists.
    static bool wlanSwitchOn() { return wlanSwitchState.load() == 1; }
    static bool toggleWlanSwitch() { const bool now = !wlanSwitchOn(); wlanSwitchState = now ? 1 : 0; if (now) wlanEverOn = true; return now; }
private:
    using Resp = std::vector<int>;

    static void add16(Resp& r, uint32_t v) { r.push_back(v & 0xFF); r.push_back((v >> 8) & 0xFF); }
    static void add32(Resp& r, uint32_t v) { add16(r, v); add16(r, v >> 16); }
    uint32_t data32(int off) const {
        return (data_[off] & 0xFF) | ((data_[off + 1] & 0xFF) << 8) | ((data_[off + 2] & 0xFF) << 16) | ((data_[off + 3] & 0xFF) << 24);
    }

    // No buttons pressed, analog stick centered (jpcsp addButtonsResponseData /
    // addAnalogResponseData with an idle controller).
    static void addButtons(Resp& r, bool kernel) {
        const uint32_t b = (buttons | (wlanSwitchOn() ? 0x40000u : 0u)) ^ (kernel ? 0x20F7F3F9u : 0x7F3F9u);
        r.push_back(static_cast<int>(((b & 0xF000u) >> 8) | ((b & 0xF0u) >> 4)));
        r.push_back(static_cast<int>(((b & 0xF0000u) >> 12) | ((b & 0x300u) >> 7) | (b & 0x9u)));
        if (kernel) {
            r.push_back(static_cast<int>((b & 0xBF00000u) >> 20));
            r.push_back(static_cast<int>((b & 0x30000000u) >> 28));
        }
    }
    static void addAnalog(Resp& r) { r.push_back(0x80); r.push_back(0x80); }

    void startCmd() {
        gpio_.clearPort(Gpio::kPortSysconEndCmd);
        const int cmd = data_[0];
        Resp r{0x82};
        switch (cmd) {
            case 0x00: break;                                        // NOP
            case 0x01: add32(r, kBaryonVersion); break;              // GET_BARYON
            case 0x02: addButtons(r, false); break;                  // GET_DIGITAL_KEY
            case 0x03: addAnalog(r); break;                          // GET_ANALOG
            case 0x05: add32(r, 0); break;                           // GET_TACHYON_TEMP
            case 0x06: addButtons(r, false); addAnalog(r); break;    // GET_DIGITAL_KEY_ANALOG
            case 0x07: addButtons(r, true); break;                   // GET_KERNEL_DIGITAL_KEY
            case 0x08: addButtons(r, true); addAnalog(r); break;     // GET_KERNEL_DIGITAL_KEY_ANALOG
            case 0x09: add32(r, readClock()); break;                 // READ_CLOCK
            case 0x0A: add32(r, alarm_); break;                      // READ_ALARM
            case 0x0B: add32(r, 0xC2u); break;                       // GET_POWER_SUPPLY_STATUS (battery present)
            case 0x0D: r.push_back(0x00); break;                     // GET_BATT_VOLT
            case 0x0E: add16(r, 0x04C0u & ~0x0080u); break;          // GET_WAKE_UP_FACTOR ("taken from a real PSP")
            case 0x0F: r.push_back(0xFF); break;                     // GET_WAKE_UP_REQ ("taken from a real PSP")
            case 0x10: break;                                        // GET_STATUS2
            case 0x11: for (int i = 0; i < 12; ++i) r.push_back(0); break; // GET_TIMESTAMP
            case 0x20: clock_ = static_cast<uint32_t>(data32(2)); clockSetUsec_ = nowUsec ? nowUsec() : 0; batteryDirty_ = true; break; // WRITE_CLOCK
            case 0x22: alarm_ = data32(2); batteryDirty_ = true; break; // WRITE_ALARM
            case 0x23: {                                             // WRITE_SCRATCHPAD
                const int dst = (data_[2] & 0xFC) >> 2, size = 1 << (data_[2] & 3);
                for (int i = 0; i < size && dst + i < 32; ++i) scratch_[dst + i] = static_cast<uint8_t>(data_[3 + i]);
                batteryDirty_ = true;
                break;
            }
            case 0x24: {                                             // READ_SCRATCHPAD
                const int src = (data_[2] & 0xFC) >> 2, size = 1 << (data_[2] & 3);
                for (int i = 0; i < size; ++i) r.push_back(src + i < 32 ? scratch_[src + i] : 0);
                break;
            }
            case 0x25: break;                                        // SEND_SETPARAM
            case 0x26: add16(r, 72); for (int i = 2; i < 8; ++i) r.push_back(0); break; // RECEIVE_SETPARAM
            case 0x30: {                                             // UNKNOWN_30 (internal registers; Baryon gen>=2)
                const int reg = data_[2] & 0x7F;
                if (reg <= 8 && !(data_[2] & 0x80)) { r.push_back(0); for (int i = 0; i < 8; ++i) r.push_back(0); }
                break;
            }
            case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: // WDT/RESET/XY/HR_POWER/SHUTDOWN/SUSPEND
            case 0x42: case 0x45: case 0x47: case 0x4B: case 0x4C: // VOLTAGE/POWER/LED/LEPTON/MS power
                break;
            case 0x4D: { const bool on = data_[2] != 0; wlanPower_ = on; if (onWlanPower) onWlanPower(on); break; }            // CTRL_WLAN_POWER: the status below reports the new state (with the WLAN switch on)
            case 0x40: add32(r, kPommelVersion); break;              // GET_POMMEL_VERSION
            case 0x46: add32(r, 0); break;                           // GET_POWER_STATUS
            case 0x61: add16(r, 0x5800); add16(r, 0x0006); break;    // BATTERY_GET_STATUS_CAP
            case 0x62: add32(r, 28); break;                          // BATTERY_GET_TEMP
            case 0x63: add32(r, 4135); break;                        // BATTERY_GET_VOLT
            case 0x64: add32(r, 4200); break;                        // BATTERY_GET_ELEC
            case 0x67: add32(r, 1800); break;                        // BATTERY_GET_FULL_CAP
            case 0x69: add32(r, 1025); break;                        // BATTERY_GET_LIMIT_TIME
            case 0x6B: add32(r, 0x000F); break;                      // BATTERY_GET_CYCLE
            case 0x73: break;                                        // BATTERY_WRITE_EEPROM
            case 0x74: r.push_back(0); r.push_back(0); r.push_back(0); break; // BATTERY_READ_EEPROM
            default: {
                static int warned = 0;
                if (warned++ < 16) std::printf("  [syscon] unhandled command 0x%02X (len=%d) -- default empty OK response\n", cmd, data_[1]);
                break;
            }
        }
        setResponse(baryonStatus(), r);
        gpio_.setPort(Gpio::kPortSysconEndCmd);
    }

    // AC supply | WLAN power | power switch. The WLAN power bit follows the last CTRL_WLAN_POWER command when the WLAN slider is on, because the wlan driver learns that the
    // chip is powered from the change of this bit (SYSCON's WLAN power callback sets its power flag, which sceWlanDevAttach checks); with the slider off it stays set, as before.
    int baryonStatus() const { (void)wlanSwitchOn(); return 0x01 | 0x10 | ((!wlanEverOn.load() || wlanPower_) ? 0x02 : 0); }

    void setResponse(int status, const Resp& r) {
        data_.fill(0);
        endData_ = false;
        const int length = static_cast<int>(r.size());
        if (length > kMaxData - 3) return;
        data_[0] = status;
        data_[1] = length + 2;
        for (int i = 0; i < length; ++i) data_[2 + i] = r[i] & 0xFF;
        int hash = 0;
        for (int i = 0; i < data_[1]; ++i) hash = (hash + data_[i]) & 0xFF;
        data_[data_[1]] = (~hash) & 0xFF;
    }

    uint32_t readData16() {
        if (dataIndex_ + 1 >= kMaxData) { dataIndex_ = 0; endData_ = true; return 0; }
        const uint32_t v = ((data_[dataIndex_] & 0xFF) << 8) | (data_[dataIndex_ + 1] & 0xFF);
        dataIndex_ += 2;
        if (dataIndex_ > data_[1]) { dataIndex_ = 0; endData_ = true; }
        return v;
    }
    void writeData16(uint32_t value) {
        if (dataIndex_ + 1 < kMaxData) {
            data_[dataIndex_++] = (value >> 8) & 0xFF;
            data_[dataIndex_++] = value & 0xFF;
        }
        if (dataIndex_ >= kMaxData) { dataIndex_ = 0; endData_ = true; }
    }
    uint32_t flags0C() const { return (endData_ ? 0u : 4u) | (error_ == 0 ? 1u : 0u); }
    void setFlags04(uint32_t flags) {
        if (flags & 4u) { dataIndex_ = 0; endData_ = true; }
        if (flags & 2u) startCmd();
        else gpio_.clearPort(Gpio::kPortSysconEndCmd);
    }

    uint32_t readClock() const { return (ticking_ && nowUsec) ? clock_ + static_cast<uint32_t>((nowUsec() - clockSetUsec_) / 1000000ull) : clock_; }

    bool wlanPower_ = false;
public:
    std::function<void(bool on)> onWlanPower;   // the WLAN rail was switched (CTRL_WLAN_POWER)
private:
    Gpio& gpio_;
    Memory regs_;
    std::array<int, kMaxData> data_{};
    int dataIndex_ = 0;
    bool endData_ = false;
    int error_ = 0;
    uint32_t clock_ = 843609600u, alarm_ = 0; // TEMPORARY: seconds since 2000-01-01 (2026-09-25); real syscon holds a battery-backed RTC
    std::array<uint8_t, 32> scratch_{};
    bool ticking_ = false, batteryDirty_ = false;
    uint64_t clockSetUsec_ = 0;
};

} // namespace beta
