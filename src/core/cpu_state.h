#pragma once

#include <array>
#include <cstdint>

// Ported from psp-lle-emu/src/core/cpu.h's CpuState (see
// docs/PORTING_LOG.md). PURELY architectural: beta has no JIT at all
// through Phase D (deferred to optional Phase F, designed fresh against
// this final shape, not ported -- see the plan), so this struct simply
// omits the old project's jitRam/jitWatch/jitRamBase/jitLim8/16/32/
// jitRem/jitDone/jitChain fields that existed only because the old x86-64
// JIT's calling convention needed a flat CpuState* pointer. There is
// therefore no "architectural vs. debug-state split" to build here --
// without a JIT, every field below already IS architectural state.

namespace beta {

struct CpuState {
    std::array<uint32_t, 32> gpr{};
    uint32_t pc = 0;
    uint32_t hi = 0;
    uint32_t lo = 0;
    // Coprocessor 0 registers. 2026-09-18 ("what's calling threadman with
    // the info"): real Allegrex hardware has TWO genuinely separate COP0
    // register files at the same numeric indices -- a "state" file
    // (MFC0/MTC0) and a "control" file (CFC0/CTC0) -- confirmed
    // authoritatively via uofw's real include/common/cop0.h, which
    // documents them under two distinct enums (COP0_STATE_*/COP0_CTRL_*)
    // with different real names at the same index for several registers
    // (e.g. index 14 is real EPC in the state file, but real SP_KERNEL --
    // the kernel-mode stack pointer -- in the control file; index 9 is
    // Count in state vs NMI_HANDLER in control; index 25 is a generic
    // state slot vs PROFILER_BASE in control). This was found live: real
    // threadman.prx context-switch code does `ctc0 $a0,$14` (writing a
    // per-thread saved value) as part of a genuine, correct context
    // switch -- which, before this split existed, silently corrupted this
    // project's own EPC (used by the interpreter's own real exception/
    // ERET machinery) with a value that was never meant to be a resume
    // address at all, since real hardware keeps them physically separate.
    // MFC0/MTC0 read/write cp0State; CFC0/CTC0 read/write cp0Ctrl. All of
    // this project's own internal exception/interrupt-delivery logic
    // (Status/Cause/EPC/BadVAddr) uses cp0State exclusively, matching
    // real standard-MIPS convention (uofw's COP0_STATE_* enum doesn't even
    // bother re-listing Status/Cause/EPC/BadVAddr since they're the
    // ordinary, unrenamed state-domain registers) -- EBASE-style dispatch
    // via register 25 uses cp0Ctrl, since real firmware writes it via
    // ctc0 (pspCop0CtrlSet), confirmed via excep.S/interruptman.c.
    std::array<uint32_t, 32> cp0State{};
    std::array<uint32_t, 32> cp0Ctrl{};

    // Coprocessor 1: the real single-precision FPU (the PSP has no
    // double-precision hardware). Each register is reinterpreted as a
    // float or raw 32-bit int depending on the instruction, exactly like
    // real hardware's bit-identical register file -- MFC1/MTC1 move raw
    // bits, everything else operates on the float reinterpretation.
    union FpuReg {
        float f;
        uint32_t u;
        int32_t i;
    };
    std::array<FpuReg, 32> fpr{};
    // FCR31, the FPU control/status register. Bit 23 is the condition
    // flag C.cond.S/BC1 read and write; bits 0-1 are the rounding mode
    // CVT.W.S (but not ROUND/TRUNC/CEIL/FLOOR.W.S, whose rounding is
    // fixed regardless of this field) consults.
    uint32_t fcr31 = 0;

    // VFPU (Allegrex vector unit): the 128-word register file, the 16 control registers, and (see vfpu.h,
    // cpu.cpp's case 0x18/0x19/0x1B/0x34/0x37/0x3C) load/store/move/sync plus the non-transcendental
    // arithmetic/data-movement instructions -- the transcendentals (vsin/vcos/vrcp/vrsq/vsqrt/vlog2/vexp2/
    // vasin/...) still throw, needing real hardware's lookup tables to match bit-for-bit. Register index
    // layout: mtx*16 + col*4 + row (0-7, 0-3, 0-3: 8 "matrices" of 4x4, confirmed against PPSSPP's own real
    // vfpu data-path indexing in MIPSVFPUUtils.cpp's ReadVector/WriteVector/ReadMatrix/WriteMatrix).
    //
    // vfpuCtrl[0..2] (S/T/D prefix) default to 0xE4/0xE4/0 -- "no prefix" -- matching real hardware's reset
    // state and what every VFPU instruction itself resets them to afterward (EatPrefixes); a 0 default for
    // S/T would NOT mean "no prefix" (0 decodes as "every lane swizzles to lane 0"), so the very first VFPU
    // instruction a cold-booted or freshly `reset()` CPU runs -- before any vpfxs/vpfxt has set them for
    // real -- would otherwise silently corrupt every multi-lane operand (caught by test_vfpu.cpp, 2026-10-18).
    std::array<uint32_t, 128> vfpu{};
    std::array<uint32_t, 16> vfpuCtrl{0xE4u, 0xE4u, 0u};

    // 2026-09-18 ("Count timer or some other timer the CPU does"): real MIPS COP0 Count (register 9, accessed via MFC0/MTC0) is a
    // free-running counter completely independent of any peripheral hardware timer (systimer.prx's real timers are a SEPARATE thing).
    // Real Allegrex silicon almost certainly has these as genuinely separate registers: ctc0-addressed "9" and mtc0-addressed "9" are
    // simultaneously used for two completely unrelated real purposes by real firmware. Giving Count its own real, auto-incrementing
    // counter (one tick per step(), a rough approximation matching PPSSPP's own CoreTiming-tied approach) rather than touching cp0[9] --
    // MFC0/MTC0 on register 9 read/write this instead; CFC0/CTC0 on "9" are untouched, still using cp0[9] exactly as before
    // (exceptionman's real, confirmed execNmiHandler storage).
    uint64_t countRegister = 0;

    // 2026-09-18 (found while tracing the real scheduler infinite loop back to interruptman.prx's own real IntrManInit(), which
    // explicitly does `pspCop0StateSet(COP0_STATE_COMPARE, 0x80000000)` and enables a dedicated interrupt line for it): real MIPS32
    // hardware dedicates Cause.IP7 (bit 15) exclusively to the internal Count==Compare timer match (when vectored interrupts aren't in
    // play, which this project doesn't model). The match sets a latched pending request that stays asserted until software next writes
    // Compare -- it is NOT simply "count==compare right now" on every check, since Count immediately counts past that exact value on
    // the following step. See Cpu::step()'s interrupt-check block for where this feeds into Cause, and the MTC0 case for where writing
    // Compare clears it.
    bool timerInterruptPending = false;
};

} // namespace beta
