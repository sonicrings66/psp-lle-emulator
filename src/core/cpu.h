#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "cpu_bus.h"
#include "cpu_state.h"
#include "../config/options.h"
#include "me/vme.h"

// Ported from psp-lle-emu/src/core/cpu.h (see docs/PORTING_LOG.md). The
// JIT/block/burst/interpreter-window infrastructure (blockSafeSteps,
// blockSafeWhy, prepareBlock, openBurst, openBurstPreTicked, both
// finishBlock overloads, closeBurstUnrun, syncJitRam, windowAvailable,
// windowOpen, windowStep, windowClose, windowSteps) is DROPPED ENTIRELY --
// beta has no JIT through Phase D (see the plan), and none of this
// project's own code outside those methods ever reads the fields that
// existed solely to support them (confirmed by grep before deleting each
// one; see PORTING_LOG.md for the exact list). The real MIPS-I integer
// ISA, FPU, VFPU, COP0/exception/interrupt delivery, and the direct-RAM
// fast path (genuinely JIT-independent -- the plain interpreter uses it
// too) are ported faithfully, unchanged in behavior.

namespace beta {

// Register name constants (standard MIPS o32 calling-convention names),
// for readability at call sites and in tests.
enum Reg : uint32_t {
    ZERO = 0, AT = 1,
    V0 = 2, V1 = 3,
    A0 = 4, A1 = 5, A2 = 6, A3 = 7,
    T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15,
    S0 = 16, S1 = 17, S2 = 18, S3 = 19, S4 = 20, S5 = 21, S6 = 22, S7 = 23,
    T8 = 24, T9 = 25,
    K0 = 26, K1 = 27,
    GP = 28, SP = 29, FP = 30, RA = 31,
};

// Allegrex (MIPS R4000-ish custom core) integer-pipeline interpreter.
//
// Core MIPS-I integer ISA, the FPU (single-precision only, matching real
// hardware), and correct branch/jump delay-slot semantics (including the
// "branch likely" family, which nullify their delay slot when not taken).
// Also implements real MIPS32 exception/interrupt delivery: Status/Cause/
// EPC (CpuState::cp0), ERET, and a hardware-interrupt check at the top of
// every step() that vectors to the real general-exception address
// (0xBFC00000, or the real EBASE register when software has installed
// one) whenever an enabled line is pending -- see Bus::pendingInterrupts().
// SYSCALL/BREAK report a clear trap message rather than vectoring when
// deliverBusErrorsAsExceptions is off. Unimplemented opcodes throw rather
// than silently no-op, so gaps surface immediately against test binaries.
class Cpu {
private:
    // Declared first (before any reference members below) so they're guaranteed constructed before anything binds to them -- C++
    // initializes members in DECLARATION order, not initializer-list order, regardless of public/private section placement.
    CpuBus& bus_;
    CpuState state_;

public:
    explicit Cpu(CpuBus& bus, const config::CpuOptions& opts = {});

    void reset(uint32_t startPc);

    // Executes exactly one instruction, including a control-flow
    // instruction's delay-slot instruction as part of the same step.
    void step();
    // Device time and interrupt lines are sampled every n instructions (and right after any non-RAM access) instead of every one. 1 (default) is exact.
    void setPollInterval(uint32_t n) { pollInterval_ = n ? n : 1u; pollCountdown_ = 1; }

    CpuState& state() { return state_; }
    const CpuState& state() const { return state_; }

    // 2026-10-24: counts real ERET executions (the only instruction that clears Status.EXL) -- used to directly
    // confirm whether the real exception/syscall-return mechanism ever completes post-reboot, vs. during the
    // successful first-pass boot, instead of inferring it indirectly from Status.EXL alone.
    uint64_t eretCount = 0;
    // 2026-10-24: was a function-local `static int nBreaks` inside cpu.cpp's BreakTrap handler, capped at 40
    // prints for the WHOLE PROCESS -- confirmed live (via the eret-stuck investigation) that the first pass
    // alone burns through far more than 40 real BreakTrap events, so this diagnostic was silently producing
    // zero output for the entire second pass. A member (resettable via resetBreakDiag()) instead of a
    // function-local static lets the reboot site re-arm it.
    int breakDiagCount = 0;
    void resetBreakDiag() { breakDiagCount = 0; }

    uint32_t reg(uint32_t index) const { return state_.gpr[index]; }

    // 2026-09-14: opt-in real MIPS32 trap-exception delivery for two
    // classes of fault this project's interpreter previously treated as
    // unconditionally fatal: Bus Error (IBE for a bad instruction fetch,
    // DBE for a bad data load/store, ExcCode 6/7) for an otherwise-unmapped
    // bus access, and Breakpoint (ExcCode 9) for a real BREAK instruction.
    // Defaults to false so every existing test's "this is a bug, fail
    // loudly" behavior is unchanged -- only a harness that specifically
    // wants to observe real firmware's own exception handler run should
    // set this true.
    bool deliverBusErrorsAsExceptions = false;

    // 2026-09-28 (old project): REFERENCE members aliasing CpuState's own storage (not separate copies),
    // purely so call sites (`cpu.countRegister`, `cpu.timerInterruptPending`) read naturally -- see
    // CpuState::countRegister's own comment for the full real-hardware rationale. Cpu is therefore not
    // copy-assignable (a reference member can't be re-seated); nothing in this codebase copy-assigns a Cpu.
    uint64_t& countRegister = state_.countRegister;
    bool& timerInterruptPending = state_.timerInterruptPending;
    // Idle skip (opt-in): `halt` parks the CPU; while parked every step() fast-forwards device time by up to idleSkipMax instructions
    // (stopping exactly at a Count==Compare match) until an interrupt line is pending and unmasked, then execution resumes after the halt.
    void setIdleSkip(bool on) { idleSkip_ = on; if (!on) halted_ = false; }
    bool halted() const { return halted_; }
    uint32_t idleSkipMax = 1000;
    uint64_t takeSkipped() { const uint64_t s = skipped_; skipped_ = 0; return s; } // extra instructions of time consumed by the last step (beyond its own one)
    // CpuOptions::ringTrace (was PSP_CPU_RING): remember the last 64 instructions the interpreter executed (pc and word) so a fault report can show the exact path.
    bool ringOn = false;
    uint32_t ringPc[64] = {}, ringWord[64] = {}, ringPos = 0;
    bool haltRequested = false; // set by the Allegrex `halt` instruction (0x70000000); consumers clear it

private:
    struct BranchResult {
        bool isControlFlow = false;
        bool taken = false;
        uint32_t target = 0;
        // "Branch likely" (BEQL/BNEL/BLEZL/BGTZL and their REGIMM
        // cousins): when not taken, the delay slot instruction is
        // nullified -- it never executes at all, unlike a regular
        // branch/jump where the delay slot always runs.
        bool likely = false;
        // ERET only: real MIPS ERET has no delay slot at all (unlike
        // every other jump/branch here) -- it transfers control
        // immediately, so step() must not fetch/execute one.
        bool noDelaySlot = false;
        // A SYSCALL whose code resolved to a real linked target (see
        // SyscallResolver::resolveSyscall) -- step() jumps straight to
        // `target` instead of treating this as a genuine branch/jump.
        // Real hardware's own stub convention places the syscall in a
        // `jr $ra` delay slot; since that jr's own $ra was never touched
        // by the stub, redirecting PC to the real resident function here
        // has the same real effect as the full trap/dispatch/ERET round
        // trip (the resident function's own eventual `jr $ra` still
        // returns to the true original caller) without needing to model it.
        bool isSyscallRedirect = false;
    };

    void setReg(uint32_t index, uint32_t value);
    void refreshCauseHardwareLines();

    // Executes one instruction's register/memory effects without advancing
    // pc. If the instruction is a branch/jump, returns its outcome so step()
    // can sequence the delay slot before actually transferring control.
    BranchResult executeAt(uint32_t pc, uint32_t instr);

    // Direct RAM window (JitHints::ramWindow): skips the virtual bus call and address decode for the overwhelmingly common RAM access.
    // This is a real, JIT-independent perf optimization the plain interpreter uses directly -- anything outside the verified window
    // (MMIO, scratchpad, unmapped -> exceptions) takes the normal bus path.
    const uint8_t* ramPtr_ = nullptr;
    uint32_t ramBase_ = 0, ramLim8_ = 0, ramLim16_ = 0, ramLim32_ = 0;
    uint32_t rd32(uint32_t a) const { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim32_) { uint32_t v; std::memcpy(&v, ramPtr_ + o, 4); return v; } slowAccess(); return bus_.read32(a); }
    uint16_t rd16(uint32_t a) const { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim16_) { uint16_t v; std::memcpy(&v, ramPtr_ + o, 2); return v; } slowAccess(); return bus_.read16(a); }
    uint8_t rd8(uint32_t a) const { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim8_) return ramPtr_[o]; slowAccess(); return bus_.read8(a); }
    void wr32(uint32_t a, uint32_t v) { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim32_) { if (watchedLines(o, 4)) { bus_.write32(a, v); return; } std::memcpy(ramMut_ + o, &v, 4); ++*wc_; return; } slowAccess(); bus_.write32(a, v); }
    void wr16(uint32_t a, uint16_t v) { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim16_) { if (watchedLines(o, 2)) { bus_.write16(a, v); return; } std::memcpy(ramMut_ + o, &v, 2); ++*wc_; return; } slowAccess(); bus_.write16(a, v); }
    void wr8(uint32_t a, uint8_t v) { const uint32_t o = (a & 0x1FFFFFFFu) - ramBase_; if (ramOk_ && o < ramLim8_) { if (watchedLines(o, 1)) { bus_.write8(a, v); return; } ramMut_[o] = v; ++*wc_; return; } slowAccess(); bus_.write8(a, v); }
    // Code-write tracking (JitHints::ramWatchMap): one byte per 32-byte RAM line, nonzero = someone holds a translation of it (unused by
    // the plain interpreter itself, but a direct store to such a line still goes through the bus instead so the epoch is bumped for
    // whoever does care -- a future JIT, per the plan's Phase F).
    const uint8_t* ramWatch_ = nullptr;
    bool watchedLines(uint32_t o, uint32_t n) const { return ramWatch_ && (ramWatch_[o >> 5] | ramWatch_[(o + n - 1) >> 5]); }
    bool ramOk_ = true;              // JitHints::ramDirectOk(), refreshed every pollInterval_ steps
    uint8_t* ramMut_ = nullptr;      // same window, writable
    uint64_t* wc_ = nullptr;         // JitHints::ramWriteCounter()
    bool idleSkip_ = false, halted_ = false;
    uint64_t skipped_ = 0;
    uint32_t pollInterval_ = 1;      // devices tick and interrupt lines are sampled every this many instructions (1 = exact, the default)
    mutable uint32_t pollCountdown_ = 1;
    void slowAccess() const { pollCountdown_ = 1; }
    void execStepBody();
    // Recognizes and bulk-applies one specific, narrow real loop idiom (see
    // cpu.cpp's own comment on the implementation) rather than the normal
    // one-instruction-at-a-time path -- a real, confirmed-live early-boot
    // RAM-clear utility real loadcore calls that would otherwise cost tens
    // of millions of interpreted steps per call. Computes the exact same
    // final memory/register/Count state single-stepping the same number of
    // real iterations would; returns false (no-op) for anything that isn't
    // precisely this shape, falling through to the normal interpreter.
    bool tryFastForwardZeroFillLoop();
    bool tickDone_ = false;          // always false in beta (no burst-mode caller ever sets it true -- see PORTING_LOG.md); kept because step() itself reads it
    uint32_t pendingCache_ = 0;
    // Media Engine only: the VME ("video media engine") register file reached by the ME-specific mtvme (0xB0E00000 | rt << 16 | imm16) and mfvme
    // (0x68E00000 | rt << 16 | imm16). Modelled as plain storage, as jpcsp does. Deliberately outside CpuState, so state hashes are untouched.
    VmeDevice vme_;

    // CpuOptions threaded through at construction (see decision 4 in PORTING_LOG.md) -- replaces the old
    // project's inline getenv() checks for PSP_HALT_LOG/PSP_LOG_BUSERR/PSP_TRACE_SYSCALL21.
    bool logHalts_ = false;
    bool logBusErrors_ = false;
    bool traceSyscall21_ = false;
};

} // namespace beta
