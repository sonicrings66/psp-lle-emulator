#pragma once

#include <cstdint>

namespace beta::MemoryMap {

// Real PSP address layout, cross-checked against jpcsp's MemoryMap.java and
// MMIO.java (the only known emulator to have booted real 6.61 firmware via
// LLE) rather than guessed. MIPS exposes the same physical RAM through
// several segments with different cache policy (cached KUSEG/KSEG0, the PSP
// kernel mirror at 0x88000000, uncached KSEG1); masking an address with
// kAddressMask collapses those mirrors down to one canonical offset before
// SystemBus routes it, the same trick jpcsp's Memory.normalizeAddress()
// uses.
constexpr uint32_t kAddressMask = 0x1FFFFFFF;

constexpr uint32_t kScratchpadStart = 0x00010000;
constexpr uint32_t kScratchpadSize  = 0x00004000; // 16 KB

constexpr uint32_t kRamStart    = 0x08000000;
constexpr uint32_t kRamSize32MB = 0x02000000;
constexpr uint32_t kRamSize64MB = 0x04000000;

// Real PSP VRAM/eDRAM data window (distinct from kGeEdramBase's *control*
// registers): 2MB of real embedded RAM at physical 0x04000000, the same
// well-documented real hardware region the earliest PSP boot stages run out
// of before main RAM has even been verified good. Confirmed load-bearing:
// the genuine, KIRK-CMD1-decrypted NAND-IPL bootstrap block addresses its
// own stack/buffers directly inside this exact range (e.g. 0x040EC000).
constexpr uint32_t kVramStart = 0x04000000;
constexpr uint32_t kVramSize  = 0x00200000; // 2 MB
constexpr uint32_t kVramMirrorWindow = 0x00800000; // the 2 MB repeats four times through 0x04000000-0x047FFFFF

// The real MIPS reset vector on this hardware: 1MB of embedded RAM the CPU
// starts executing from on boot. Real hardware remaps a second bank from
// 0xBFD00000 over this same address after the IPL finishes running; that
// swap is deferred until a phase that actually needs post-IPL behavior.
constexpr uint32_t kIplStart = 0xBFC00000;
constexpr uint32_t kIplSize  = 0x00100000; // 1 MB

// 2026-09-23 ("Bfc bfd and bfe on the pspdev wiki say mips reset vector and
// ram"): confirmed via psdevwiki's real Tachyon SoC memory map -- the real,
// physical "MIPS Reset Vector" region is a full 2MB (0x1FC00000-0x1FDFFFFF
// physical, i.e. 0xBFC00000-0xBFDFFFFF once mapped through the uncached K1
// segment), not just the first 1MB this project already models as `ipl_`.
// This second 1MB bank was already anticipated (see the comment above,
// written before this exact gap was ever reached live) but deferred as
// unneeded until a "post-IPL" phase -- now reached for real: a genuine
// instruction fetch fault at 0xBFD00000 after SceKernelInitThread's first
// real dispatch. Backed the same simple way as the first bank (zero-filled
// Memory, decoding as harmless NOPs) since the real bank-2 content was
// never dumped -- this avoids a hard fault where real hardware has valid,
// mapped memory, without fabricating specific real byte content we don't
// have.
constexpr uint32_t kIplBank2Start = 0xBFD00000;
constexpr uint32_t kIplBank2Size  = 0x00100000; // 1 MB

// jpcsp: MMIOHandlerInterruptMan.BASE_ADDRESS, registered with size 0x30 in
// MMIO.java. Real hardware has 64 interrupt lines with separate
// enable/trigger bit registers — see mmio/interrupt_controller.h for why
// Phase 1 doesn't model that yet.
constexpr uint32_t kIntcBase = 0xBC300000;
constexpr uint32_t kIntcSize = 0x30;

// jpcsp: MMIOHandlerSystemControl.BASE_ADDRESS, registered with size 0x104
// in MMIO.java. Real hardware's per-subsystem reset/bus-clock-enable
// register block — see mmio/system_control.h for why Phase 1 doesn't model
// individual bits yet.
constexpr uint32_t kSysconBase = 0xBC100000;
constexpr uint32_t kSysconSize = 0x104;

// The rest of these are plain accept-anything stubs (mmio/generic_stub.h) —
// real kernel boot code was observed genuinely poking each of them while
// running actual decrypted 6.61 modules, same discovery method as every
// other MMIO block here. Bases/sizes from jpcsp's MMIO.java registrations
// (the addHandler(BASE_ADDRESS, size, ...) call for each handler class).
constexpr uint32_t kMemoryAccessControlBase = 0xBC000000; // MMIOHandlerMemoryAccessControl
constexpr uint32_t kMemoryAccessControlSize = 0x80;
constexpr uint32_t kGpioBase = 0xBE240000; // MMIOHandlerGpio
constexpr uint32_t kGpioSize = 0x4C;
constexpr uint32_t kSysconChipBase = 0xBE580000; // MMIOHandlerSyscon -- the actual
                                                  // power/battery/UMD-tray microcontroller,
                                                  // distinct from kSysconBase's register block
constexpr uint32_t kSysconChipSize = 0x28;
constexpr uint32_t kAudioBase = 0xBE000000; // MMIOHandlerAudio
constexpr uint32_t kAudioSize = 0x80;
constexpr uint32_t kSystemTimeBase = 0xBC600000; // MMIOHandlerSystemTime
constexpr uint32_t kSystemTimeSize = 0x14;
constexpr uint32_t kGeEdramBase = 0xBD500000; // MMIOHandlerGeEdram -- embedded VRAM control, Phase 5 territory like kGeBase
constexpr uint32_t kGeEdramSize = 0x94;
constexpr uint32_t kPowerBase = 0xBE300000; // MMIOHandlerPower -- battery/power management
constexpr uint32_t kPowerSize = 0x60;
// A small register block mirrored 0x100 bytes past kTimerBase[0] (jpcsp's
// MMIO.java registers MMIOHandlerTimer with an extra alias offset { 0x0100
// } alongside its normal 0x10-byte range) -- kept as its own tiny stub
// rather than folding into Timer, since the alias's real relationship to
// the timer (likely an interrupt-status/ack variant) isn't modeled either
// way without real interrupt delivery.
constexpr uint32_t kTimer0AliasBase = 0xBC500100;
constexpr uint32_t kTimer0AliasSize = 0x40; // four timers x 0x10 (see TimerStatusAlias)
constexpr uint32_t kDmacplusBase = 0xBC800000; // MMIOHandlerDmacplus
constexpr uint32_t kDmacplusSize = 0x1D4;
constexpr uint32_t kUart3Base = 0xBE500000; // MMIOHandlerUart3 (MMIOHandlerUartBase.SIZE_OF)
constexpr uint32_t kUart3Size = 0x48;
constexpr uint32_t kUart4Base = 0xBE4C0000; // MMIOHandlerUart4 (MMIOHandlerUartBase.SIZE_OF)
constexpr uint32_t kUart4Size = 0x48;
constexpr uint32_t kNandBase = 0xBD101000; // MMIOHandlerNand -- real flash0/flash1 access goes
constexpr uint32_t kNandSize = 0x304;      // through here on real hardware; a real target for
                                            // actual semantics once file I/O syscalls exist.
constexpr uint32_t kNandPageBase = 0xBFF00000; // MMIOHandlerNandPage -- the actual data buffer a
constexpr uint32_t kNandPageSize = 0x90C;      // completed NAND page read/write transfers through
                                                // (real NAND page + spare-area size), distinct
                                                // from the NAND controller's own status/control
                                                // registers above.
constexpr uint32_t kLcdcBase = 0xBE140000; // MMIOHandlerLcdc -- display controller
constexpr uint32_t kLcdcSize = 0x204;
constexpr uint32_t kKirkMmioBase = 0xBDE00000; // MMIOHandlerKirk -- the real hardware KIRK crypto
constexpr uint32_t kKirkMmioSize = 0x34;       // engine's register interface (signature/version/
                                                // error/command/result/status/statusAsync/
                                                // statusAsyncEnd/sourceAddr/destAddr at their real
                                                // jpcsp-confirmed offsets 0x00/04/08/0C/10/14/1C/
                                                // 20/24/2C/30) -- distinct from this project's own
                                                // already-validated KirkEngine (crypto/kirk.h),
                                                // which models the same real crypto correctly but
                                                // via a direct C++ call, not this real MMIO wire
                                                // protocol real Sony code actually uses.
constexpr uint32_t kGeBase = 0xBD400000; // MMIOHandlerGe -- real semantics needed by Phase 5
constexpr uint32_t kGeSize = 0xE50;
constexpr uint32_t kProfilerBase = 0xBC400000; // MMIOHandlerProfiler -- real on-chip performance-
constexpr uint32_t kProfilerSize = 0x54;       // counter register block; confirmed load-bearing:
                                                // real threadman code bulk-copies a per-thread
                                                // profiler-context save area to/from this exact
                                                // address as part of its own thread-switch
                                                // accounting (FUN_087a4828 in sceThreadManager).
constexpr uint32_t kCpuBusFrequencyBase = 0xBC200000; // MMIOHandlerCpuBusFrequency -- confirmed
constexpr uint32_t kCpuBusFrequencySize = 0x8;        // load-bearing: many real modules' own
                                                       // startup code reads this directly.
constexpr uint32_t kDdrBase = 0xBD000000; // MMIOHandlerDdr -- real DDR/SDRAM memory-controller
constexpr uint32_t kDdrSize = 0x44;       // write-buffer-flush register block (jpcsp's sceDdr;
                                           // confirmed via uofw's src/syscon/power.S sub_0000,
                                           // which writes a channel id to offset 0x04 to trigger
                                           // a flush, then polls offset 0x04's low nibble for the
                                           // pending-flush bits to clear, then offset 0x00 bit
                                           // 0x200 for a busy/PLL-changing flag to clear -- a real
                                           // power-down-path DDR flush, reached for the first time
                                           // 2026-09-12 once the SYSCON checksum gate was solved.

// 2026-09-23: I2C master (jpcsp MMIOHandlerI2c) and the remaining jpcsp-known MMIO
// blocks, mapped as plain register files until a real driver needs their semantics.
constexpr uint32_t kI2cBase = 0xBE200000;
constexpr uint32_t kI2cSize = 0x30;
constexpr uint32_t kDmac0Base = 0xBC900000;
constexpr uint32_t kDmac0Size = 0x1F4;
constexpr uint32_t kDmac1Base = 0xBCA00000;
constexpr uint32_t kDmac1Size = 0x1F4;
constexpr uint32_t kMeControllerBase = 0xBCC00000;
constexpr uint32_t kMeControllerSize = 0x74;
constexpr uint32_t kMemoryStickBase = 0xBD200000;
constexpr uint32_t kMemoryStickSize = 0x44;
// 0xBE100000: an undocumented register block the Memory Stick driver uses once a card is present (found 2026-10-08: it reads +0x84 and tests bit 0, writes 1 to +0x80 and
// read-modify-writes +0x10). jpcsp has no handler there either (it answers 0), and 0 makes the driver take its normal path. Without a mapping the read faults and the kernel spins.
constexpr uint32_t kUnknownBe10Base = 0xBE100000;
constexpr uint32_t kUnknownBe10Size = 0x100;
constexpr uint32_t kWlanBase = 0xBD300000;
constexpr uint32_t kWlanSize = 0x44;
constexpr uint32_t kAta2Base = 0xBD600000;
constexpr uint32_t kAta2Size = 0x50;
constexpr uint32_t kUmdAtaBase = 0xBD700000;
constexpr uint32_t kUmdAtaSize = 0x10;
constexpr uint32_t kUsbBase = 0xBD800000;
constexpr uint32_t kUsbSize = 0x518;
constexpr uint32_t kUmdBase = 0xBDF00000;
constexpr uint32_t kUmdSize = 0x98;
constexpr uint32_t kLcdControllerSlimBase = 0xBE5C0000;
constexpr uint32_t kLcdControllerSlimSize = 0x28;
constexpr uint32_t kDisplayControllerBase = 0xBE740000;
constexpr uint32_t kDisplayControllerSize = 0x28;
constexpr uint32_t kDisplayControllerSlimBase = 0xBE780000;
constexpr uint32_t kDisplayControllerSlimSize = 0x20;
constexpr uint32_t kA7f00000Base = 0xA7F00000;
constexpr uint32_t kA7f00000Size = 0x24;
constexpr uint32_t kA7f80000Base = 0xA7F80000;
constexpr uint32_t kA7f80000Size = 0x2000;

// jpcsp: MMIO.java registers 4 instances of MMIOHandlerTimer, 0x10 bytes
// apart, at these exact addresses.
constexpr uint32_t kTimerBase[4] = {0xBC500000, 0xBC500010, 0xBC500020, 0xBC500030};
constexpr uint32_t kTimerSize = 0x10;

} // namespace beta::MemoryMap
