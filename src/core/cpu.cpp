#include "cpu.h"
#include "vfpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace beta {

namespace {

// 2026-09-14: a real BREAK instruction, distinct from every other
// std::runtime_error thrown elsewhere in executeAt() (which represent
// genuine interpreter gaps -- unimplemented opcodes -- that must stay
// fatal). Step() catches this one specifically so it can optionally
// deliver it as a real MIPS Breakpoint exception (ExcCode 9), the same
// opt-in behavior as the existing Bus Error path.
struct BreakTrap {
    uint32_t code;
    uint32_t pc;
    uint32_t instr;
};

// 2026-09-23 ("I guess we start trying it" -- load real VSH content):
// a SYSCALL with no registered target and no matching host callback was
// previously thrown as a fatal std::runtime_error unconditionally,
// regardless of deliverBusErrorsAsExceptions -- meaning ANY unresolved
// import permanently halted the whole interpreter, even though real
// MIPS hardware would instead take a genuine Syscall exception (ExcCode
// 8) into the real exception vector, letting real exceptionman/kernel
// code decide what happens. Mirrors BreakTrap exactly, same opt-in gate.
struct SyscallTrap {
    uint32_t code;
    uint32_t pc;
    uint32_t instr;
};

// --- instruction field extraction -----------------------------------------

inline uint32_t opcodeOf(uint32_t instr) { return (instr >> 26) & 0x3F; }
inline uint32_t rsOf(uint32_t instr) { return (instr >> 21) & 0x1F; }
inline uint32_t rtOf(uint32_t instr) { return (instr >> 16) & 0x1F; }
inline uint32_t rdOf(uint32_t instr) { return (instr >> 11) & 0x1F; }
inline uint32_t shamtOf(uint32_t instr) { return (instr >> 6) & 0x1F; }
inline uint32_t functOf(uint32_t instr) { return instr & 0x3F; }
inline uint16_t imm16Of(uint32_t instr) { return static_cast<uint16_t>(instr & 0xFFFF); }
inline uint32_t addr26Of(uint32_t instr) { return instr & 0x03FFFFFF; }

inline int32_t signExtend16(uint16_t v) { return static_cast<int16_t>(v); }
inline int32_t signExtend8(uint8_t v) { return static_cast<int8_t>(v); }

[[noreturn]] void unimplemented(uint32_t pc, uint32_t instr, const char* what) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                   "unimplemented instruction (%s): pc=0x%08X instr=0x%08X",
                   what, pc, instr);
    throw std::runtime_error(buf);
}

} // namespace

Cpu::Cpu(CpuBus& bus, const config::CpuOptions& opts) : bus_(bus) {
    ringOn = opts.ringTrace;
    logHalts_ = opts.logHalts;
    logBusErrors_ = opts.logBusErrors;
    traceSyscall21_ = opts.traceSyscall21;

    uint32_t size = 0;
    ramPtr_ = bus.ramWindow(ramBase_, size);
    wc_ = bus.ramWriteCounter();
    ramMut_ = const_cast<uint8_t*>(ramPtr_);
    ramWatch_ = bus.ramWatchMap();
    if (ramPtr_ && wc_ && size >= 16u) { ramLim8_ = size; ramLim16_ = size - 1u; ramLim32_ = size - 3u; } else ramPtr_ = nullptr;
}

void Cpu::reset(uint32_t startPc) {
    tickDone_ = false;
    state_ = CpuState{};
    state_.pc = startPc;
    // 2026-09-18 ("do it see what happens"): real MIPS/Allegrex hardware
    // mandates Status.BEV=1 at reset (bit 22) -- no RAM-resident exception
    // handler exists yet, so the CPU must use the boot-ROM vector
    // (0xBFC00380) until software explicitly clears it (confirmed via
    // uofw's real src/preipl/loader.S, which is exactly the code that runs
    // while BEV=1 and clears it itself, once, right before jumping via
    // EBASE). Previously this defaulted to 0 (zero-initialized along with
    // everything else), routing every exception straight to the
    // RAM-resident vector (0x88000180) from instruction one, regardless of
    // whether real firmware had gotten far enough to install anything
    // there yet.
    // 2026-09-18 ("what about the status stuff"): also set Status.ERL=1
    // (bit 2), the other real MIPS32 reset-mandated bit. Our own dispatch
    // logic doesn't gate on ERL at all (no cache/TLB modeling that cares),
    // so this can't change OUR mechanics the way BEV did -- but real
    // firmware code might read Status as plain data (mfc0 + andi) and
    // branch on this bit expecting real hardware's value; testing whether
    // that's ever load-bearing rather than assuming it isn't.
    state_.cp0State[12] = 0x00400004u;
}

void Cpu::setReg(uint32_t index, uint32_t value) {
    if (index != 0) {
        state_.gpr[index] = value;
    }
}

Cpu::BranchResult Cpu::executeAt(uint32_t pc, uint32_t instr) {
    const uint32_t opcode = opcodeOf(instr);
    const uint32_t rs = rsOf(instr);
    const uint32_t rt = rtOf(instr);
    const uint32_t rd = rdOf(instr);
    const uint32_t shamt = shamtOf(instr);
    const uint32_t rsVal = state_.gpr[rs];
    const uint32_t rtVal = state_.gpr[rt];
    const uint32_t nextPc = pc + 4; // address of this instruction's delay slot

    switch (opcode) {
    case 0x00: { // SPECIAL
        switch (functOf(instr)) {
        case 0x00: setReg(rd, rtVal << shamt); return {}; // SLL (NOP when word==0)
        case 0x02: // SRL (rs==0) / ROTR (rs==1) -- real MIPS32R2 shares this
                   // funct code between the two, selected by the rs field
                   // (normally unused/zero for a plain shift). Confirmed
                   // load-bearing 2026-09-11: SHA-256's real compression
                   // function (found via the gate #2 HMAC trace) is built
                   // entirely out of ROTR-family rotates -- treating every
                   // ROTR as a plain SRL silently produces a
                   // plausible-looking but wrong hash, with no crash to
                   // signal the gap.
            if (rs == 0x01u) {
                setReg(rd, shamt == 0 ? rtVal : ((rtVal >> shamt) | (rtVal << (32 - shamt))));
            } else {
                setReg(rd, rtVal >> shamt);
            }
            return {};
        case 0x03: setReg(rd, static_cast<uint32_t>(static_cast<int32_t>(rtVal) >> shamt)); return {}; // SRA
        case 0x04: setReg(rd, rtVal << (rsVal & 0x1F)); return {}; // SLLV
        case 0x06: { // SRLV (shamt==0) / ROTRV (shamt==1) -- same real
                     // sharing as SRL/ROTR above, but selected by the sa
                     // field instead of rs (which here holds the shift-
                     // amount register, as for a normal SRLV).
            const uint32_t sh = rsVal & 0x1Fu;
            if (shamt == 0x01u) {
                setReg(rd, sh == 0 ? rtVal : ((rtVal >> sh) | (rtVal << (32 - sh))));
            } else {
                setReg(rd, rtVal >> sh);
            }
            return {};
        }
        case 0x07: setReg(rd, static_cast<uint32_t>(static_cast<int32_t>(rtVal) >> (rsVal & 0x1F))); return {}; // SRAV
        // 2026-09-23: the PSP ignores the low 2 bits of a jr/jalr target.
        // interruptman.prx keeps mode flags in the low bits of handler pointers
        // and jalr's them without masking (jpcsp BcuState.doJALR: `npc &=
        // 0xFFFFFFFC`). Live case: NAND IRQ handler 0x8818409A -> 0x88184098.
        case 0x08: return {true, true, rsVal & ~3u}; // JR
        case 0x09: setReg(rd, nextPc + 4); return {true, true, rsVal & ~3u}; // JALR
        case 0x10: setReg(rd, state_.hi); return {}; // MFHI
        case 0x11: state_.hi = rsVal; return {}; // MTHI
        case 0x12: setReg(rd, state_.lo); return {}; // MFLO
        case 0x13: state_.lo = rsVal; return {}; // MTLO
        case 0x18: { // MULT
            int64_t result = static_cast<int64_t>(static_cast<int32_t>(rsVal)) *
                              static_cast<int64_t>(static_cast<int32_t>(rtVal));
            state_.lo = static_cast<uint32_t>(result & 0xFFFFFFFF);
            state_.hi = static_cast<uint32_t>((result >> 32) & 0xFFFFFFFF);
            return {};
        }
        case 0x19: { // MULTU
            uint64_t result = static_cast<uint64_t>(rsVal) * static_cast<uint64_t>(rtVal);
            state_.lo = static_cast<uint32_t>(result & 0xFFFFFFFF);
            state_.hi = static_cast<uint32_t>((result >> 32) & 0xFFFFFFFF);
            return {};
        }
        case 0x1C: { // MADD -- hi:lo += (signed) rs*rt
            const int64_t product = static_cast<int64_t>(static_cast<int32_t>(rsVal)) *
                                     static_cast<int64_t>(static_cast<int32_t>(rtVal));
            const int64_t acc = (static_cast<int64_t>(static_cast<uint64_t>(state_.hi) << 32 | state_.lo)) + product;
            state_.lo = static_cast<uint32_t>(static_cast<uint64_t>(acc) & 0xFFFFFFFFu);
            state_.hi = static_cast<uint32_t>((static_cast<uint64_t>(acc) >> 32) & 0xFFFFFFFFu);
            return {};
        }
        case 0x1D: { // MADDU -- hi:lo += (unsigned) rs*rt
            const uint64_t product = static_cast<uint64_t>(rsVal) * static_cast<uint64_t>(rtVal);
            const uint64_t acc = (static_cast<uint64_t>(state_.hi) << 32 | state_.lo) + product;
            state_.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFu);
            state_.hi = static_cast<uint32_t>((acc >> 32) & 0xFFFFFFFFu);
            return {};
        }
        case 0x2E: { // MSUB -- hi:lo -= (signed) rs*rt
            const int64_t product = static_cast<int64_t>(static_cast<int32_t>(rsVal)) *
                                     static_cast<int64_t>(static_cast<int32_t>(rtVal));
            const int64_t acc = (static_cast<int64_t>(static_cast<uint64_t>(state_.hi) << 32 | state_.lo)) - product;
            state_.lo = static_cast<uint32_t>(static_cast<uint64_t>(acc) & 0xFFFFFFFFu);
            state_.hi = static_cast<uint32_t>((static_cast<uint64_t>(acc) >> 32) & 0xFFFFFFFFu);
            return {};
        }
        case 0x2F: { // MSUBU -- hi:lo -= (unsigned) rs*rt
            const uint64_t product = static_cast<uint64_t>(rsVal) * static_cast<uint64_t>(rtVal);
            const uint64_t acc = (static_cast<uint64_t>(state_.hi) << 32 | state_.lo) - product;
            state_.lo = static_cast<uint32_t>(acc & 0xFFFFFFFFu);
            state_.hi = static_cast<uint32_t>((acc >> 32) & 0xFFFFFFFFu);
            return {};
        }
        case 0x1A: // DIV
            // Real MIPS I leaves hi/lo unspecified for divide-by-zero and the
            // INT_MIN/-1 overflow case rather than trapping; guard both here
            // purely to avoid C++ signed-division UB, not to model hardware.
            if (rtVal != 0 && !(static_cast<int32_t>(rsVal) == INT32_MIN && static_cast<int32_t>(rtVal) == -1)) {
                state_.lo = static_cast<uint32_t>(static_cast<int32_t>(rsVal) / static_cast<int32_t>(rtVal));
                state_.hi = static_cast<uint32_t>(static_cast<int32_t>(rsVal) % static_cast<int32_t>(rtVal));
            }
            return {};
        case 0x1B: // DIVU
            if (rtVal != 0) {
                state_.lo = rsVal / rtVal;
                state_.hi = rsVal % rtVal;
            }
            return {};
        case 0x20: setReg(rd, rsVal + rtVal); return {}; // ADD (overflow trap deferred to Phase 1+ exception support)
        case 0x21: setReg(rd, rsVal + rtVal); return {}; // ADDU
        case 0x22: setReg(rd, rsVal - rtVal); return {}; // SUB (overflow trap deferred, see ADD)
        case 0x23: setReg(rd, rsVal - rtVal); return {}; // SUBU
        case 0x24: setReg(rd, rsVal & rtVal); return {}; // AND
        case 0x25: setReg(rd, rsVal | rtVal); return {}; // OR
        case 0x26: setReg(rd, rsVal ^ rtVal); return {}; // XOR
        case 0x27: setReg(rd, ~(rsVal | rtVal)); return {}; // NOR
        case 0x2A: setReg(rd, (static_cast<int32_t>(rsVal) < static_cast<int32_t>(rtVal)) ? 1u : 0u); return {}; // SLT
        case 0x2B: setReg(rd, (rsVal < rtVal) ? 1u : 0u); return {}; // SLTU
        case 0x0A: if (rtVal == 0) setReg(rd, rsVal); return {}; // MOVZ
        case 0x0B: if (rtVal != 0) setReg(rd, rsVal); return {}; // MOVN
        case 0x0C: { // SYSCALL -- real user/kernel privilege-crossing calls
                     // (see SyscallResolver::resolveSyscall's doc comment)
                     // resolve here to whatever real target
                     // format::linkModules() linked this code's syscall
                     // number against. A code with no registered target is
                     // a real trap with nothing to dispatch to yet,
                     // reported clearly rather than as "unimplemented" --
                     // not a decode gap.
            const uint32_t code = (instr >> 6) & 0xFFFFF;
            const uint32_t target = bus_.resolveSyscall(code);
            if (target != 0) {
                BranchResult r;
                r.isControlFlow = true;
                r.isSyscallRedirect = true;
                r.target = target;
                return r;
            }
            // 2026-10-30: code 21 (0x15) is real Sony's own generic "import not yet linked" placeholder --
            // uofw's loadcore.c writes the literal instruction word SYSCALL_NOT_LINKED (0x0000054C, which
            // decodes to exactly this syscall) into every stub whose NID search comes up empty, and its own
            // aLinkLibEntries (loadcore.c:2088-2092) treats that as routine, non-fatal: `stubTable[i].sc.syscall
            // = NOP; returnAddr = SYSCALL_NOT_LINKED; continue;` -- never an error. Letting this fall through to
            // the generic SyscallTrap/exception-vector path (below) does NOT reliably recover (confirmed live in
            // the old project: a full run landed this exact code at a real panic halt instead of gracefully
            // returning, even with deliverBusErrorsAsExceptions on). Handled here instead, generically, since the
            // placeholder can appear at ANY address and this is architecturally the right layer: every real
            // caller of one of our synthetic zero-export libraries' unresolved stubs hits this same code,
            // regardless of which library or which real kernel function was searched for.
            if (code == 21u) {
                // 2026-10-30 (round 2, old project): confirmed LIVE exactly how the graceful skip itself still
                // causes a real cascading failure -- a real per-module validation callback calling straight into
                // one of these unlinked stubs via `jal`, then checking `bltz $v0` on the result two instructions
                // later. Redirecting to $ra without touching v0 leaves it holding whatever STALE value it had
                // before the call -- which can have its high bit set, so the signed bltz reads it as a fatal
                // negative error and the caller takes a real panic path instead of the success path real Sony's
                // own semantics intend ("just didn't run, move on"). Setting v0=0 here is the conservative choice
                // matching most PSP kernel API conventions (0 almost always means success/empty/not-found rather
                // than failure), giving callers that DO check the result the best chance of taking a real
                // continuation path instead of a fatal one.
                setReg(V0, 0u);
                BranchResult r;
                r.isControlFlow = true;
                r.isSyscallRedirect = true;
                r.target = state_.gpr[RA];
                return r;
            }
            // A real host-callback syscall (see SyscallResolver::tryHostSyscall's doc comment) completes
            // synchronously in C++ right here -- there's no guest function to jump into, so this returns a
            // plain, non-control-flow result. When this SYSCALL sits in the delay slot of the real `jr $ra`
            // stub it was patched into (the shape format::linkModules() and this mechanism both use),
            // Cpu::step()'s own delay-slot handling then falls through to that outer jr's own target ($ra,
            // the real original caller) -- exactly as if this were the tail of a function that had already
            // executed its own jr $ra.
            uint32_t hostResult = 0;
            if (bus_.tryHostSyscall(code, bus_, state_.gpr[A0], state_.gpr[A1], state_.gpr[A2], state_.gpr[A3], hostResult)) {
                setReg(V0, hostResult);
                return {};
            }
            throw SyscallTrap{code, pc, instr};
        }
        case 0x0D: // BREAK -- a real trap, distinct from a genuinely
                   // unimplemented opcode (thrown as a dedicated BreakTrap,
                   // not std::runtime_error, precisely so step() can tell
                   // the two apart: an unimplemented opcode is always a
                   // real interpreter gap and must stay fatal, but BREAK is
                   // real, valid code real firmware executes on purpose.
                   // See Cpu::step()'s catch block for the real opt-in
                   // exception-delivery logic (ExcCode 9, Breakpoint),
                   // mirroring the existing Bus Error path.
            throw BreakTrap{(instr >> 6) & 0xFFFFFu, pc, instr};
        case 0x0F: return {}; // SYNC -- memory barrier, a no-op for a single-threaded interpreter
        case 0x16: { // CLZ -- count leading zero bits
            uint32_t v = rsVal;
            uint32_t count = 0;
            while (count < 32 && (v & 0x80000000u) == 0) {
                v <<= 1;
                ++count;
            }
            setReg(rd, count);
            return {};
        }
        case 0x17: { // CLO -- count leading one bits
            uint32_t v = rsVal;
            uint32_t count = 0;
            while (count < 32 && (v & 0x80000000u) != 0) {
                v <<= 1;
                ++count;
            }
            setReg(rd, count);
            return {};
        }
        case 0x2C: // MAX -- Allegrex-specific, reuses MIPS64 DADD's funct slot (no 64-bit support on this core)
            setReg(rd, (static_cast<int32_t>(rsVal) > static_cast<int32_t>(rtVal)) ? rsVal : rtVal);
            return {};
        case 0x2D: // MIN -- Allegrex-specific, reuses MIPS64 DADDU's funct slot
            setReg(rd, (static_cast<int32_t>(rsVal) < static_cast<int32_t>(rtVal)) ? rsVal : rtVal);
            return {};
        default: unimplemented(pc, instr, "SPECIAL");
        }
    }
    case 0x01: { // REGIMM
        switch (rt) {
        case 0x00: { bool t = static_cast<int32_t>(rsVal) < 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BLTZ
        case 0x01: { bool t = static_cast<int32_t>(rsVal) >= 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BGEZ
        case 0x02: { bool t = static_cast<int32_t>(rsVal) < 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BLTZL
        case 0x03: { bool t = static_cast<int32_t>(rsVal) >= 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BGEZL
        case 0x10: { setReg(RA, nextPc + 4); bool t = static_cast<int32_t>(rsVal) < 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BLTZAL
        case 0x11: { setReg(RA, nextPc + 4); bool t = static_cast<int32_t>(rsVal) >= 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BGEZAL
        case 0x12: { setReg(RA, nextPc + 4); bool t = static_cast<int32_t>(rsVal) < 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BLTZALL
        case 0x13: { setReg(RA, nextPc + 4); bool t = static_cast<int32_t>(rsVal) >= 0; return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BGEZALL
        default: unimplemented(pc, instr, "REGIMM");
        }
    }
    case 0x02: // J
        return {true, true, (nextPc & 0xF0000000u) | (addr26Of(instr) << 2)};
    case 0x03: // JAL
        setReg(RA, nextPc + 4);
        return {true, true, (nextPc & 0xF0000000u) | (addr26Of(instr) << 2)};
    case 0x04: { bool t = (rsVal == rtVal); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BEQ
    case 0x05: { bool t = (rsVal != rtVal); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BNE
    case 0x06: { bool t = (static_cast<int32_t>(rsVal) <= 0); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BLEZ
    case 0x07: { bool t = (static_cast<int32_t>(rsVal) > 0); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2))}; } // BGTZ
    case 0x14: { bool t = (rsVal == rtVal); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BEQL
    case 0x15: { bool t = (rsVal != rtVal); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BNEL
    case 0x16: { bool t = (static_cast<int32_t>(rsVal) <= 0); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BLEZL
    case 0x17: { bool t = (static_cast<int32_t>(rsVal) > 0); return {true, t, static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2)), true}; } // BGTZL
    case 0x08: setReg(rt, static_cast<uint32_t>(static_cast<int32_t>(rsVal) + signExtend16(imm16Of(instr)))); return {}; // ADDI (overflow trap deferred, see SPECIAL/ADD)
    case 0x09: setReg(rt, static_cast<uint32_t>(static_cast<int32_t>(rsVal) + signExtend16(imm16Of(instr)))); return {}; // ADDIU
    case 0x0A: setReg(rt, (static_cast<int32_t>(rsVal) < signExtend16(imm16Of(instr))) ? 1u : 0u); return {}; // SLTI
    case 0x0B: setReg(rt, (rsVal < static_cast<uint32_t>(signExtend16(imm16Of(instr)))) ? 1u : 0u); return {}; // SLTIU
    case 0x0C: setReg(rt, rsVal & imm16Of(instr)); return {}; // ANDI
    case 0x0D: setReg(rt, rsVal | imm16Of(instr)); return {}; // ORI
    case 0x0E: setReg(rt, rsVal ^ imm16Of(instr)); return {}; // XORI
    case 0x0F: setReg(rt, static_cast<uint32_t>(imm16Of(instr)) << 16); return {}; // LUI
    case 0x10: { // COP0
        switch (rs) {
        // 2026-09-18: MF/CF (0/2) and MT/CT (4/6) are NOT the same real
        // register file on Allegrex -- see cpu_state.h's cp0State/cp0Ctrl
        // comment. MFC0/MTC0 address cp0State; CFC0/CTC0 address cp0Ctrl.
        case 0x00: // MFC0
            if (rd == 9u) { setReg(rt, static_cast<uint32_t>(countRegister)); return {}; } // real Count
            setReg(rt, state_.cp0State[rd]); return {};
        case 0x02: setReg(rt, state_.cp0Ctrl[rd]); return {}; // CFC0 (register 9 here is exceptionman's own execNmiHandler storage, not Count)
        case 0x04: // MTC0
            if (rd == 9u) { countRegister = rtVal; return {}; } // real Count is software-writable
            if (rd == 11u) { timerInterruptPending = false; bus_.setTimerInterruptPending(false); } // real hardware: writing Compare clears the pending IP7 match
            state_.cp0State[rd] = rtVal; return {};
        case 0x06: state_.cp0Ctrl[rd] = rtVal; return {}; // CTC0
        case 0x10: // "CO" format (rs's MSB set) -- only ERET (funct 0x18) is real here
            switch (functOf(instr)) {
            case 0x18: { // ERET -- return from exception: resume at EPC, clear Status.EXL.
                         // Unlike every other jump/branch, real ERET has no
                         // delay slot at all, so step() must skip that logic.
                const uint32_t epc = state_.cp0State[14];
                state_.cp0State[12] &= ~0x2u; // Status.EXL = 0
                ++eretCount;
                return {true, true, epc, false, true};
            }
            default: unimplemented(pc, instr, "COP0/CO"); // TLB ops (tlbwi/tlbr/...) -- this core's TLB is unmodeled
            }
        default: unimplemented(pc, instr, "COP0");
        }
    }
    case 0x11: { // COP1 -- the real single-precision FPU (PSP has no double-
                 // precision hardware). Field positions here differ from
                 // the integer decode above: FT sits where "rt" is, FS
                 // where "rd" is, and FD where "shamt" is (confirmed
                 // against PPSSPP's _FS/_FT/_FD macros, Interpreter.cpp).
        switch (rs) {
        case 0x00: setReg(rt, state_.fpr[rd].u); return {}; // MFC1 -- raw bits, no conversion
        case 0x02: // CFC1 -- only FCR31 (status/control) and FCR0 (fixed revision) are real
            if (rd == 31) {
                setReg(rt, state_.fcr31);
            } else if (rd == 0) {
                setReg(rt, 0x00003351u); // FCR0_VALUE, the fixed capability/revision real hardware reports
            } else {
                setReg(rt, 0);
            }
            return {};
        case 0x04: state_.fpr[rd].u = rtVal; return {}; // MTC1 -- raw bits, no conversion
        case 0x06: // CTC1 -- only FCR31 is writable
            if (rd == 31) {
                state_.fcr31 = rtVal & 0x0181FFFFu; // real hardware's writable-bits mask
            }
            return {};
        case 0x08: { // BC1 -- branch on the FPU condition flag (FCR31 bit 23)
            const bool cond = (state_.fcr31 & (1u << 23)) != 0;
            const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(nextPc) + (signExtend16(imm16Of(instr)) << 2));
            switch (rt) {
            case 0x00: return {true, !cond, target, false}; // BC1F
            case 0x01: return {true, cond, target, false};  // BC1T
            case 0x02: return {true, !cond, target, true};  // BC1FL
            case 0x03: return {true, cond, target, true};   // BC1TL
            default: unimplemented(pc, instr, "COP1/BC1");
            }
        }
        case 0x10: { // "S" format: single-precision arithmetic/round/convert/compare
            const uint32_t fd = shamt; // FD occupies the shamt bit position in this format
            const uint32_t funct = functOf(instr);
            switch (funct) {
            case 0x00: state_.fpr[fd].f = state_.fpr[rd].f + state_.fpr[rt].f; return {}; // ADD.S
            case 0x01: state_.fpr[fd].f = state_.fpr[rd].f - state_.fpr[rt].f; return {}; // SUB.S
            case 0x02: state_.fpr[fd].f = state_.fpr[rd].f * state_.fpr[rt].f; return {}; // MUL.S
            case 0x03: state_.fpr[fd].f = state_.fpr[rd].f / state_.fpr[rt].f; return {}; // DIV.S
            case 0x04: state_.fpr[fd].f = std::sqrt(state_.fpr[rd].f); return {}; // SQRT.S
            case 0x05: state_.fpr[fd].f = std::fabs(state_.fpr[rd].f); return {}; // ABS.S
            case 0x06: state_.fpr[fd].f = state_.fpr[rd].f; return {}; // MOV.S
            case 0x07: state_.fpr[fd].f = -state_.fpr[rd].f; return {}; // NEG.S
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: { // ROUND/TRUNC/CEIL/FLOOR.W.S --
                                                           // rounding mode is fixed per-opcode here,
                                                           // unlike CVT.W.S below which follows FCR31
                const float src = state_.fpr[rd].f;
                if (std::isnan(src) || std::isinf(src)) {
                    state_.fpr[fd].i = (std::isinf(src) && src < 0.0f) ? INT32_MIN : INT32_MAX;
                    return {};
                }
                switch (funct) {
                case 0x0C: state_.fpr[fd].i = static_cast<int32_t>(std::nearbyint(src)); return {}; // ROUND.W.S (round-half-to-even)
                case 0x0D: // TRUNC.W.S
                    if (src >= 0.0f) {
                        int32_t result = static_cast<int32_t>(std::floor(src));
                        state_.fpr[fd].i = (result == INT32_MIN) ? INT32_MAX : result; // overflow, but source was positive
                    } else {
                        state_.fpr[fd].i = static_cast<int32_t>(std::ceil(src));
                    }
                    return {};
                case 0x0E: state_.fpr[fd].i = static_cast<int32_t>(std::ceil(src)); return {}; // CEIL.W.S
                case 0x0F: state_.fpr[fd].i = static_cast<int32_t>(std::floor(src)); return {}; // FLOOR.W.S
                }
                return {};
            }
            case 0x20: { // CVT.W.S -- like the above but rounding mode comes from FCR31 bits 0-1
                const float src = state_.fpr[rd].f;
                if (std::isnan(src) || std::isinf(src)) {
                    state_.fpr[fd].i = (std::isinf(src) && src < 0.0f) ? INT32_MIN : INT32_MAX;
                    return {};
                }
                switch (state_.fcr31 & 0x3u) {
                case 0: state_.fpr[fd].i = static_cast<int32_t>(std::nearbyint(src)); return {}; // round to nearest
                case 1: state_.fpr[fd].i = static_cast<int32_t>(src); return {}; // truncate toward zero
                case 2: state_.fpr[fd].i = static_cast<int32_t>(std::ceil(src)); return {}; // toward +infinity
                default: state_.fpr[fd].i = static_cast<int32_t>(std::floor(src)); return {}; // toward -infinity
                }
            }
            default:
                if (funct >= 0x30) { // C.cond.S -- the low 4 bits select the actual condition;
                                      // the ordered/unordered pairs (e.g. EQ/SEQ) are
                                      // functionally identical on this core (no FPU
                                      // exception-trap support to tell them apart by)
                    const float a = state_.fpr[rd].f;
                    const float b = state_.fpr[rt].f;
                    bool cond;
                    switch (funct & 0xF) {
                    case 0x0: case 0x8: cond = false; break; // F, SF
                    case 0x1: case 0x9: cond = std::isnan(a) || std::isnan(b); break; // UN, NGLE
                    case 0x2: case 0xA: cond = !std::isnan(a) && !std::isnan(b) && (a == b); break; // EQ, SEQ
                    case 0x3: case 0xB: cond = (a == b) || std::isnan(a) || std::isnan(b); break; // UEQ, NGL
                    case 0x4: case 0xC: cond = a < b; break; // OLT, LT
                    case 0x5: case 0xD: cond = (a < b) || std::isnan(a) || std::isnan(b); break; // ULT, NGE
                    case 0x6: case 0xE: cond = a <= b; break; // OLE, LE
                    default:  cond = (a <= b) || std::isnan(a) || std::isnan(b); break; // ULE, NGT
                    }
                    if (cond) {
                        state_.fcr31 |= (1u << 23);
                    } else {
                        state_.fcr31 &= ~(1u << 23);
                    }
                    return {};
                }
                unimplemented(pc, instr, "COP1/S");
            }
        }
        case 0x14: { // "W" format -- only CVT.S.W is real on this core
            const uint32_t fd = shamt;
            switch (functOf(instr)) {
            case 0x20: state_.fpr[fd].f = static_cast<float>(state_.fpr[rd].i); return {}; // CVT.S.W
            default: unimplemented(pc, instr, "COP1/W");
            }
        }
        default: unimplemented(pc, instr, "COP1");
        }
    }
    case 0x31: state_.fpr[rt].u = rd32(rsVal + signExtend16(imm16Of(instr))); return {}; // LWC1
    case 0x39: wr32(rsVal + signExtend16(imm16Of(instr)), state_.fpr[rt].u); return {}; // SWC1
    case 0x20: setReg(rt, static_cast<uint32_t>(signExtend8(rd8(rsVal + signExtend16(imm16Of(instr)))))); return {}; // LB
    case 0x21: setReg(rt, static_cast<uint32_t>(signExtend16(rd16(rsVal + signExtend16(imm16Of(instr)))))); return {}; // LH
    case 0x22: { // LWL -- unaligned load, left (high-order) part; little-endian formula
                 // ported from PPSSPP's Interpreter.cpp Int_Mem (case 34: lwl)
        const uint32_t addr = rsVal + signExtend16(imm16Of(instr));
        const uint32_t mem = rd32(addr & ~0x3u);
        const uint32_t shift = (addr & 0x3u) * 8;
        setReg(rt, (rtVal & (0x00FFFFFFu >> shift)) | (mem << (24 - shift)));
        return {};
    }
    case 0x23: setReg(rt, rd32(rsVal + signExtend16(imm16Of(instr)))); return {}; // LW
    case 0x24: setReg(rt, rd8(rsVal + signExtend16(imm16Of(instr)))); return {}; // LBU
    case 0x25: setReg(rt, rd16(rsVal + signExtend16(imm16Of(instr)))); return {}; // LHU
    case 0x26: { // LWR -- unaligned load, right (low-order) part; same reference as LWL
        const uint32_t addr = rsVal + signExtend16(imm16Of(instr));
        const uint32_t mem = rd32(addr & ~0x3u);
        const uint32_t shift = (addr & 0x3u) * 8;
        setReg(rt, (rtVal & (0xFFFFFF00u << (24 - shift))) | (mem >> shift));
        return {};
    }
    case 0x1C: { // SPECIAL2 -- on Allegrex this is just halt/mfic/mtic
                 // (interrupt controller read/write via a dedicated CPU
                 // instruction, separate from the MMIO interrupt
                 // controller); every other SPECIAL2 slot is invalid on
                 // this core (PPSSPP's own opcode table, MIPSTables.cpp,
                 // agrees).
                 //
                 // 2026-09-13: mfic/mtic were originally a plain no-op here
                 // (matching PPSSPP's own "not implemented" precedent), but
                 // that broke real loadcore code: uofw's own
                 // kd/loadcore/interruptController.S names these exact two
                 // instructions loadCoreCpuSuspendIntr/loadCoreCpuResumeIntr
                 // -- mfic reads the CPU's real current interrupt-enable
                 // state, mtic writes it (INTR_CTRL_STATE is uofw's own
                 // name for this control "register", always selector 0 in
                 // every real usage this project has found) -- a dedicated,
                 // faster Allegrex-specific alternative to MFC0/MTC0 $12
                 // for this extremely common critical-section pattern.
                 // Modeled directly against the SAME Status.IE bit
                 // (cp0[12] & 0x1) this core's own top-of-step hardware-
                 // interrupt check already reads. halt (real semantics:
                 // stop the CPU clock until the next interrupt) still has
                 // no PPSSPP interpreter function either -- a no-op remains
                 // the correct minimal choice there for a synchronous,
                 // single-step interpreter with no real "wait for async
                 // event" model; it just lets execution fall through to
                 // whatever the loop checks next, same reasoning as SYNC.
        switch (functOf(instr)) {
        case 0x00: haltRequested = true; if (idleSkip_ && (state_.cp0State[12] & 0xFF00u) != 0u) { halted_ = true; static int nHalt = 0; if (nHalt++ < 8 && logHalts_) std::printf("  [HALT] pc=0x%08X Status=0x%08X Cause=0x%08X Compare=0x%08X Count=0x%08X pending=0x%X" "%c", pc, state_.cp0State[12], state_.cp0State[13], state_.cp0State[11], static_cast<uint32_t>(countRegister), pendingCache_, 10); } return {}; // halt (a no-op for the main core; MeCore uses the flag to park the Media Engine until an interrupt)
        case 0x24: setReg(rt, state_.cp0State[12] & 0x1u); return {}; // mfic -- Status.IE
        case 0x26: // mtic -- Status.IE
            if (rtVal != 0) {
                state_.cp0State[12] |= 0x1u;
            } else {
                state_.cp0State[12] &= ~0x1u;
            }
            return {};
        default: unimplemented(pc, instr, "SPECIAL2");
        }
    }
    case 0x1F: { // SPECIAL3
        switch (functOf(instr)) {
        case 0x00: { // EXT rt, rs, pos, size -- pos is the shamt field, size is rd+1
            const uint32_t sizeBits = rd + 1;
            const uint32_t mask = (sizeBits >= 32) ? 0xFFFFFFFFu : ((1u << sizeBits) - 1u);
            setReg(rt, (rsVal >> shamt) & mask);
            return {};
        }
        case 0x04: { // INS rt, rs, pos, msb -- pos is the shamt field, size is msb-pos+1
            const uint32_t pos = shamt;
            const uint32_t sizeBits = rd - pos + 1;
            const uint32_t fieldMask = (sizeBits >= 32) ? 0xFFFFFFFFu : (((1u << sizeBits) - 1u) << pos);
            setReg(rt, (rtVal & ~fieldMask) | ((rsVal << pos) & fieldMask));
            return {};
        }
        case 0x18: case 0x20: { // Allegrex bit-manipulation extensions
                                // ("ALLEGREX0" in PPSSPP's MIPSTables.cpp),
                                // selected by the shamt field rather than
                                // funct alone -- rd is dest, rt is source
                                // for all of these.
            switch (shamt) {
            case 0x02: { // WSBH -- swap bytes within each halfword
                setReg(rd, ((rtVal & 0xFF00FF00u) >> 8) | ((rtVal & 0x00FF00FFu) << 8));
                return {};
            }
            case 0x03: { // WSBW -- full 32-bit byte swap
                setReg(rd, ((rtVal & 0xFF000000u) >> 24) | ((rtVal & 0x00FF0000u) >> 8) |
                           ((rtVal & 0x0000FF00u) << 8) | ((rtVal & 0x000000FFu) << 24));
                return {};
            }
            case 0x10: setReg(rd, static_cast<uint32_t>(signExtend8(static_cast<uint8_t>(rtVal & 0xFF)))); return {}; // SEB
            case 0x14: { // BITREV -- reverse all 32 bits
                uint32_t v = rtVal;
                uint32_t result = 0;
                for (int i = 0; i < 32; ++i) {
                    result = (result << 1) | (v & 1);
                    v >>= 1;
                }
                setReg(rd, result);
                return {};
            }
            case 0x18: setReg(rd, static_cast<uint32_t>(signExtend16(static_cast<uint16_t>(rtVal & 0xFFFF)))); return {}; // SEH
            default: unimplemented(pc, instr, "SPECIAL3/ALLEGREX0");
            }
        }
        default: unimplemented(pc, instr, "SPECIAL3");
        }
    }
    case 0x28: wr8(rsVal + signExtend16(imm16Of(instr)), static_cast<uint8_t>(rtVal & 0xFF)); return {}; // SB
    case 0x29: wr16(rsVal + signExtend16(imm16Of(instr)), static_cast<uint16_t>(rtVal & 0xFFFF)); return {}; // SH
    case 0x2A: { // SWL -- unaligned store, left (high-order) part; same reference as LWL
        const uint32_t addr = rsVal + signExtend16(imm16Of(instr));
        const uint32_t alignedAddr = addr & ~0x3u;
        const uint32_t mem = rd32(alignedAddr);
        const uint32_t shift = (addr & 0x3u) * 8;
        wr32(alignedAddr, (rtVal >> (24 - shift)) | (mem & (0xFFFFFF00u << shift)));
        return {};
    }
    case 0x2B: wr32(rsVal + signExtend16(imm16Of(instr)), rtVal); return {}; // SW
    case 0x2E: { // SWR -- unaligned store, right (low-order) part; same reference as LWL
        const uint32_t addr = rsVal + signExtend16(imm16Of(instr));
        const uint32_t alignedAddr = addr & ~0x3u;
        const uint32_t mem = rd32(alignedAddr);
        const uint32_t shift = (addr & 0x3u) * 8;
        wr32(alignedAddr, (rtVal << shift) | (mem & (0x00FFFFFFu >> (24 - shift))));
        return {};
    }
    case 0x2F: return {}; // CACHE -- cache maintenance op; no-op since this interpreter models no cache
    // LL/SC: user-mode lightweight-mutex/atomic code uses them. Single-core interpreter with interrupts only
    // delivered between instructions, so an SC after an LL can always succeed (link is never broken).
    case 0x30: setReg(rt, rd32(rsVal + signExtend16(imm16Of(instr)))); return {}; // LL
    case 0x38: wr32(rsVal + signExtend16(imm16Of(instr)), rtVal); setReg(rt, 1u); return {}; // SC
    // --- VFPU, minimal (register file + load/store/move + sync). See CpuState::vfpu. ---
    case 0x32: case 0x3A: { // lv.s / sv.s: one VFPU register
        const uint32_t vt = ((instr >> 16) & 0x1Fu) | ((instr & 3u) << 5);
        const uint32_t idx = ((vt >> 2) & 7u) * 16u + (vt & 3u) * 4u + ((vt >> 5) & 3u);
        const uint32_t addr = rsVal + static_cast<uint32_t>(signExtend16(static_cast<uint16_t>(instr & 0xFFFCu)));
        if (opcode == 0x32) state_.vfpu[idx] = rd32(addr); else wr32(addr, state_.vfpu[idx]);
        return {};
    }
    case 0x36: case 0x3E: { // lv.q / sv.q: four VFPU registers (sv.q bit1 = base writeback)
        const uint32_t vt = ((instr >> 16) & 0x1Fu) | ((instr & 1u) << 5);
        const uint32_t mtx = (vt >> 2) & 7u, col = vt & 3u;
        const bool transposed = ((vt >> 5) & 1u) != 0;
        const uint32_t off = static_cast<uint32_t>(signExtend16(static_cast<uint16_t>(instr & 0xFFFCu)));
        const uint32_t addr = rsVal + off;
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t idx = transposed ? (mtx * 16u + col + i * 4u) : (mtx * 16u + col * 4u + i);
            if (opcode == 0x36) state_.vfpu[idx] = rd32(addr + i * 4u); else wr32(addr + i * 4u, state_.vfpu[idx]);
        }
        if (opcode == 0x3E && (instr & 2u)) setReg(rs, rsVal + off);
        return {};
    }
    case 0x12: { // COP2: mfv/mfvc (rs=3), mtv/mtvc (rs=7)
        const uint32_t sub = rsOf(instr), imm = instr & 0xFFu;
        if (sub == 3 || sub == 7) {
            uint32_t* slot;
            if (imm & 0x80u) slot = &state_.vfpuCtrl[imm & 0xFu];
            else slot = &state_.vfpu[((imm >> 2) & 7u) * 16u + (imm & 3u) * 4u + ((imm >> 5) & 3u)];
            if (sub == 3) setReg(rt, *slot); else *slot = rtVal;
            return {};
        }
        unimplemented(pc, instr, "COP2");
    }
    case 0x3F: // VFPU misc: vnop / vsync / vflush are pure pipeline-sync no-ops here
        if (instr == 0xFFFF0000u || instr == 0xFFFF0320u || instr == 0xFFFF040Du) return {};
        unimplemented(pc, instr, "VFPU misc");
    // --- VFPU arithmetic/data-movement (2026-10-18). See vfpu.h for the addressing/prefix machinery and
    // where this is ported from. Every case here ends by resetting the S/T/D prefix control registers
    // (vfpuCtrl[0..2]) to their "no prefix" state, matching real hardware's EatPrefixes behavior -- the
    // prefix a vpfxs/vpfxt/vpfxd instruction sets is consumed by exactly the next VFPU instruction only.
    case 0x18: case 0x19: { // VFPU0 (vadd/vsub/vdiv) / VFPU1 (vmul/vdot/vscl)
        using namespace beta::vfpu;
        const uint32_t group = (instr >> 23) & 7u;
        const VSize sz = vecSizeOf(instr);
        const int n = numElem(sz);
        const int vd = vdOf(instr), vs = vsOf(instr), vt = vtOf(instr);
        auto eat = [&] { state_.vfpuCtrl[kSPrefix] = 0xE4u; state_.vfpuCtrl[kTPrefix] = 0xE4u; state_.vfpuCtrl[kDPrefix] = 0u; };
        if (opcode == 0x18 && group == 0) { // vadd
            float s[4], t[4], d[4];
            readVec(state_, s, sz, vs); readVec(state_, t, sz, vt);
            applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            for (int i = 0; i < n; ++i) d[i] = s[i] + t[i];
            retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (opcode == 0x18 && group == 1) { // vsub
            float s[4], t[4], d[4];
            readVec(state_, s, sz, vs); readVec(state_, t, sz, vt);
            applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            for (int i = 0; i < n; ++i) d[i] = s[i] - t[i];
            retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (opcode == 0x18 && group == 7) { // vdiv -- S/T applied only to the LAST lane (real hardware quirk); D applies mask only
            float s[4]{}, t[4]{}, d[4];
            readVec(state_, s, sz, vs); readVec(state_, t, sz, vt);
            applyPrefixST(&s[n - 1], state_.vfpuCtrl[kSPrefix], 1, -INFINITY);
            applyPrefixST(&t[n - 1], state_.vfpuCtrl[kTPrefix], 1, -INFINITY);
            for (int i = 0; i < n; ++i) d[i] = s[i] / t[i];
            const uint32_t lastmask = (state_.vfpuCtrl[kDPrefix] & (1u << 8)) << (n - 1);
            applyPrefixD(d, lastmask, n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (opcode == 0x19 && group == 0) { // vmul
            float s[4], t[4], d[4];
            readVec(state_, s, sz, vs); readVec(state_, t, sz, vt);
            applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            for (int i = 0; i < n; ++i) d[i] = s[i] * t[i];
            retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (opcode == 0x19 && group == 1) { // vdot -- the S/T prefixes read into a full V_Quad (zero-padded above n)
            float s[4]{}, t[4]{};
            readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], 4);
            readVec(state_, t, sz, vt); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], 4);
            float d = 0.0f; for (int i = 0; i < 4; ++i) d += s[i] * t[i];
            applyPrefixD(&d, state_.vfpuCtrl[kDPrefix], 1); writeVec(state_, &d, V_Single, vd); eat(); return {};
        }
        if (opcode == 0x19 && group == 2) { // vscl -- t is really a single scalar lane of vt, broadcast to all 4 (T prefix forces that swizzle)
            float s[4], t[4]{}; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            const int tlane = (vt >> 5) & 3; float one[1]; readVec(state_, one, V_Single, vt); t[tlane] = one[0];
            const uint32_t tAdd = swizzleBits(tlane, tlane, tlane, tlane);
            applyPrefixST(t, rewritePrefix(state_.vfpuCtrl[kTPrefix], anySwizzle(), tAdd), 4);
            float d[4]; for (int i = 0; i < n; ++i) d[i] = s[i] * t[i];
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        unimplemented(pc, instr, "VFPU0/1");
    }
    case 0x1B: { // VFPU3: vcmp / vmin / vmax / vsge / vslt
        using namespace beta::vfpu;
        const uint32_t group = (instr >> 23) & 7u;
        const VSize sz = vecSizeOf(instr);
        const int n = numElem(sz);
        const int vd = vdOf(instr), vs = vsOf(instr), vt = vtOf(instr);
        auto eat = [&] { state_.vfpuCtrl[kSPrefix] = 0xE4u; state_.vfpuCtrl[kTPrefix] = 0xE4u; state_.vfpuCtrl[kDPrefix] = 0u; };
        if (group == 0) { // vcmp
            float s[4], t[4]; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            readVec(state_, t, sz, vt); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            const int cond = static_cast<int>(instr & 0xFu);
            int cc = 0, orVal = 0, andVal = 1;
            for (int i = 0; i < n; ++i) {
                int c;
                switch (cond) {
                    case 0: c = 0; break;                               // VC_FL
                    case 1: c = s[i] == t[i]; break;                     // VC_EQ
                    case 2: c = s[i] < t[i]; break;                      // VC_LT
                    case 3: c = s[i] <= t[i]; break;                     // VC_LE
                    case 4: c = 1; break;                                // VC_TR
                    case 5: c = s[i] != t[i]; break;                     // VC_NE
                    case 6: c = s[i] >= t[i]; break;                     // VC_GE
                    case 7: c = s[i] > t[i]; break;                      // VC_GT
                    case 8: c = (s[i] == 0.0f); break;                   // VC_EZ (0.0 == -0.0)
                    case 9: c = isnanf(s[i]); break;                     // VC_EN
                    case 10: c = isinff(s[i]); break;                    // VC_EI
                    case 11: c = isnanf(s[i]) || isinff(s[i]); break;    // VC_ES
                    case 12: c = (s[i] != 0.0f); break;                  // VC_NZ
                    case 13: c = !isnanf(s[i]); break;                   // VC_NN
                    case 14: c = !isinff(s[i]); break;                   // VC_NI
                    case 15: c = !(isnanf(s[i]) || isinff(s[i])); break; // VC_NS
                    default: c = 0; break;
                }
                cc |= (c << i); orVal |= c; andVal &= c;
            }
            const uint32_t affected = (1u << 4) | (1u << 5) | ((1u << n) - 1u);
            state_.vfpuCtrl[kCC] = (state_.vfpuCtrl[kCC] & ~affected) |
                                   ((static_cast<uint32_t>(cc) | (static_cast<uint32_t>(orVal) << 4) | (static_cast<uint32_t>(andVal) << 5)) & affected);
            eat(); return {};
        }
        if (group == 2 || group == 3) { // vmin / vmax
            float s[4], t[4], d[4]; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            readVec(state_, t, sz, vt); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            for (int i = 0; i < n; ++i) {
                const bool special = isnanf(s[i]) || isinff(s[i]) || isnanf(t[i]) || isinff(t[i]);
                if (special) {
                    int32_t si, ti; std::memcpy(&si, &s[i], 4); std::memcpy(&ti, &t[i], 4);
                    const bool bothNeg = si < 0 && ti < 0;
                    const bool takeLow = (group == 2) != bothNeg;   // vmin normally takes the smaller int; flips sign-order when both negative
                    int32_t ri = takeLow ? std::min(si, ti) : std::max(si, ti);
                    std::memcpy(&d[i], &ri, 4);
                } else {
                    d[i] = (group == 2) ? std::min(s[i], t[i]) : std::max(s[i], t[i]);
                }
            }
            retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (group == 6 || group == 7) { // vsge / vslt -- NaN always yields 0; D prefix mask only (clamp can't matter: result is always 0 or 1)
            float s[4], t[4], d[4]; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            readVec(state_, t, sz, vt); applyPrefixST(t, state_.vfpuCtrl[kTPrefix], n);
            for (int i = 0; i < n; ++i) {
                if (isnanf(s[i]) || isnanf(t[i])) d[i] = 0.0f;
                else d[i] = (group == 6) ? (s[i] >= t[i] ? 1.0f : 0.0f) : (s[i] < t[i] ? 1.0f : 0.0f);
            }
            retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix] & 0x0F00u, n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        unimplemented(pc, instr, "VFPU3");
    }
    case 0x37: { // VFPU5: vpfxs / vpfxt / vpfxd (prefix SET -- does NOT eat prefixes, that's the whole point) / viim.s / vfim.s
        using namespace beta::vfpu;
        const uint32_t regnum = (instr >> 24) & 3u;
        if (regnum < 3) {
            uint32_t data = instr & 0x000FFFFFu;
            if (regnum == static_cast<uint32_t>(kDPrefix)) data &= 0x00000FFFu;
            state_.vfpuCtrl[kSPrefix + regnum] = data;
            return {};
        }
        // regnum == 3: viim.s (bit23=0, sign-extended 16-bit immediate -> float) / vfim.s (bit23=1, IEEE half-float -> float)
        const int vt = vtOf(instr);
        float f;
        if (((instr >> 23) & 1u) == 0u) {
            f = static_cast<float>(signExtend16(imm16Of(instr)));
        } else {
            const uint32_t h = imm16Of(instr);
            const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
            uint32_t bits;
            if (exp == 0u) bits = mant == 0u ? sign : [&] { uint32_t e = 113u, m = mant; while (!(m & 0x400u)) { m <<= 1; --e; } m &= 0x3FFu; return sign | (e << 23) | (m << 13); }();
            else if (exp == 0x1Fu) bits = sign | 0x7F800000u | (mant << 13);
            else bits = sign | ((exp + 112u) << 23) | (mant << 13);
            std::memcpy(&f, &bits, 4);
        }
        beta::vfpu::applyPrefixD(&f, state_.vfpuCtrl[kDPrefix], 1);
        writeVec(state_, &f, V_Single, vt);
        state_.vfpuCtrl[kSPrefix] = 0xE4u; state_.vfpuCtrl[kTPrefix] = 0xE4u; state_.vfpuCtrl[kDPrefix] = 0u;
        return {};
    }
    case 0x34: { // VFPU4Jump: vmov/vabs/vneg/vidt/vsat0/vsat1/vzero/vone (nested, sub=0) / vocp/vsgn (nested, sub=2) / vf2i*/vi2f/vcmov (direct)
        using namespace beta::vfpu;
        const uint32_t sub = (instr >> 21) & 0x1Fu;
        const VSize sz = vecSizeOf(instr);
        const int n = numElem(sz);
        const int vd = vdOf(instr), vs = vsOf(instr);
        auto eat = [&] { state_.vfpuCtrl[kSPrefix] = 0xE4u; state_.vfpuCtrl[kTPrefix] = 0xE4u; state_.vfpuCtrl[kDPrefix] = 0u; };
        if (sub == 0) { // VFPU4
            const uint32_t optype = (instr >> 16) & 0x1Fu;
            if (optype <= 2) { // vmov(0) / vabs(1, forces abs) / vneg(2, forces negate) -- these are prefix hacks: the "operation" IS the forced prefix
                float s[4];
                readVec(state_, s, sz, vs);
                uint32_t add = 0; if (optype == 1) add = absBits(1, 1, 1, 1); else if (optype == 2) add = negateBits(1, 1, 1, 1);
                applyPrefixST(s, rewritePrefix(state_.vfpuCtrl[kSPrefix], 0, add), n);
                applyPrefixD(s, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, s, sz, vd); eat(); return {};
            }
            if (optype == 4 || optype == 5) { // vsat0 / vsat1
                float s[4], d[4]; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
                for (int i = 0; i < n; ++i) d[i] = (optype == 4) ? vclamp(s[i], 0.0f, 1.0f) : vclamp(s[i], -1.0f, 1.0f);
                if (optype == 5) applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n, true);  // vsat1's own op IS the saturate; D's sat field doesn't apply again
                else applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n);
                writeVec(state_, d, sz, vd); eat(); return {};
            }
            if (optype == 3 || optype == 6 || optype == 7) { // vidt / vzero / vone -- the S prefix generates constants, negate still applies
                float d[4]{};
                if (optype == 3) { // vidt: identity row depends on vd's own column offset
                    const int offmask = (sz == V_Quad || sz == V_Triple) ? 3 : 1;
                    const int off = vd & offmask;
                    const int cX = (off == (0 & offmask)) ? 1 : 0, cY = (off == (1 & offmask)) ? 1 : 0,
                              cZ = (off == (2 & offmask)) ? 1 : 0, cW = (off == (3 & offmask)) ? 1 : 0;
                    const uint32_t add = makeConstants(cX, cY, cZ, cW);
                    applyPrefixST(d, rewritePrefix(state_.vfpuCtrl[kSPrefix], anySwizzle(), add), n);
                } else {
                    const int c = (optype == 7) ? 1 : 0;
                    const uint32_t add = makeConstants(c, c, c, c);
                    applyPrefixST(d, rewritePrefix(state_.vfpuCtrl[kSPrefix], anySwizzle(), add), n);
                }
                applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
            }
            unimplemented(pc, instr, "VFPU4 (vrcp/vrsq/vsin/vcos/vexp2/vlog2/vsqrt/vasin/vnrcp/vnsin/vrexp2 -- transcendentals, not yet implemented)");
        }
        if (sub == 2) { // VFPU9 subset
            const uint32_t optype = (instr >> 16) & 0x1Fu;
            if (optype == 4) { // vocp: one's-complement-ish (1-s), S forces negate, T forces constant 1
                float s[4]{}, t[4]{};
                readVec(state_, s, sz, vs);
                applyPrefixST(s, rewritePrefix(state_.vfpuCtrl[kSPrefix], 0, negateBits(1, 1, 1, 1)), n);
                applyPrefixST(t, rewritePrefix(state_.vfpuCtrl[kTPrefix], anySwizzle(), makeConstants(1, 1, 1, 1)), n);
                float d[4]; for (int i = 0; i < n; ++i) d[i] = isnanf(s[i]) ? std::fabs(s[i]) : t[i] + s[i];
                retainInvalidSwizzleST(d, state_.vfpuCtrl[kSPrefix], state_.vfpuCtrl[kTPrefix], n);
                applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
            }
            if (optype == 10) { // vsgn: sign of (s-t) where t defaults to the zero constant, via integer hackery to handle NaN
                float s[4], t[4]{};
                applyPrefixST(t, rewritePrefix(state_.vfpuCtrl[kTPrefix], anySwizzle(), makeConstants(0, 0, 0, 0)), n);
                readVec(state_, s, sz, vs);
                if (n < 4) std::memcpy(&s[n], &t[n], sizeof(float) * static_cast<size_t>(4 - n));
                applyPrefixST(s, state_.vfpuCtrl[kSPrefix], 4);
                float d[4]; for (int i = 0; i < n; ++i) {
                    const float diff = s[i] - t[i]; uint32_t val; std::memcpy(&val, &diff, 4);
                    d[i] = (val == 0u || val == 0x80000000u) ? 0.0f : ((val >> 31) == 0u ? 1.0f : -1.0f);
                }
                applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
            }
            unimplemented(pc, instr, "VFPU9");
        }
        if (sub >= 16 && sub <= 19) { // vf2in / vf2iz / vf2iu / vf2id -- float-to-fixed, scaled by 2^imm, 4 rounding modes
            const uint32_t imm = (instr >> 16) & 0x1Fu;
            const float mult = static_cast<float>(1ull << imm);
            float s[4]; int32_t d[4];
            readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            for (int i = 0; i < n; ++i) {
                if (isnanf(s[i])) { d[i] = 0x7FFFFFFF; continue; }
                const double sv = static_cast<double>(s[i]) * static_cast<double>(mult);
                if (sv > 2147483647.0) d[i] = 0x7FFFFFFF;
                else if (sv <= -2147483648.0) d[i] = static_cast<int32_t>(0x80000000u);
                else switch (sub) {
                    case 16: d[i] = static_cast<int32_t>(std::lround(sv)); break;              // n: round to nearest
                    case 17: d[i] = static_cast<int32_t>(s[i] >= 0 ? std::floor(sv) : std::ceil(sv)); break; // z: truncate
                    case 18: d[i] = static_cast<int32_t>(std::ceil(sv)); break;                 // u
                    default: d[i] = static_cast<int32_t>(std::floor(sv)); break;                // d (sub==19)
                }
            }
            float df[4]; std::memcpy(df, d, sizeof(df));
            applyPrefixD(df, state_.vfpuCtrl[kDPrefix], n, true); // mask only, not sat
            writeVec(state_, df, sz, vd); eat(); return {};
        }
        if (sub == 20) { // vi2f -- fixed-to-float, scaled by 2^-imm
            const uint32_t imm = (instr >> 16) & 0x1Fu;
            const float mult = 1.0f / static_cast<float>(1ull << imm);
            float sf[4]; int32_t s[4]; float d[4];
            readVec(state_, sf, sz, vs); std::memcpy(s, sf, sizeof(s));
            applyPrefixST(reinterpret_cast<float*>(s), state_.vfpuCtrl[kSPrefix], n);
            for (int i = 0; i < n; ++i) d[i] = static_cast<float>(s[i]) * mult;
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (sub == 21) { // vcmov -- D is also read as a source (T-prefixed); copies from S when the CC bit matches
            const uint32_t tf = (instr >> 19) & 1u, imm3 = (instr >> 16) & 7u;
            float s[4], d[4]; readVec(state_, s, sz, vs); applyPrefixST(s, state_.vfpuCtrl[kSPrefix], n);
            readVec(state_, d, sz, vd); applyPrefixST(d, state_.vfpuCtrl[kTPrefix], n);
            const uint32_t cc = state_.vfpuCtrl[kCC];
            if (imm3 < 6) { if (((cc >> imm3) & 1u) == (1u - tf)) for (int i = 0; i < n; ++i) d[i] = s[i]; }
            else if (imm3 == 6) { for (int i = 0; i < n; ++i) if (((cc >> i) & 1u) == (1u - tf)) d[i] = s[i]; }
            applyPrefixD(d, state_.vfpuCtrl[kDPrefix], n); writeVec(state_, d, sz, vd); eat(); return {};
        }
        unimplemented(pc, instr, "VFPU4Jump");
    }
    case 0x3C: { // VFPU6: vmmul / v(h)tfm2/3/4 / vmscl / VFPUMatrixSet1 (vmmov/vmidt/vmzero/vmone)
        using namespace beta::vfpu;
        const uint32_t sub = (instr >> 21) & 0x1Fu;
        const int vd = vdOf(instr), vs = vsOf(instr), vt = vtOf(instr);
        auto eat = [&] { state_.vfpuCtrl[kSPrefix] = 0xE4u; state_.vfpuCtrl[kTPrefix] = 0xE4u; state_.vfpuCtrl[kDPrefix] = 0u; };
        if (sub < 4) { // vmmul
            const MSize msz = mtxSizeOf(instr); const int side = mtxSide(msz);
            float s[16]{}, t[16]{}, d[16];
            readMtx(state_, s, msz, vs); readMtx(state_, t, msz, vt);
            for (int a = 0; a < side; ++a) for (int b = 0; b < side; ++b) {
                if (a == side - 1 && b == side - 1) { applyPrefixST(&s[b * 4], state_.vfpuCtrl[kSPrefix], 4); applyPrefixST(&t[a * 4], state_.vfpuCtrl[kTPrefix], 4); }
                float sum = 0.0f;
                const int terms = (a == side - 1 && b == side - 1) ? 4 : side;
                for (int c = 0; c < terms; ++c) sum += s[b * 4 + c] * t[a * 4 + c];
                d[a * 4 + b] = sum;
            }
            const uint32_t lastmask = (state_.vfpuCtrl[kDPrefix] & (1u << 8)) << (side - 1);
            const uint32_t lastsat = (state_.vfpuCtrl[kDPrefix] & 3u) << (side + side - 2);
            applyPrefixD(&d[4 * (side - 1)], lastmask | lastsat, 4);
            writeMtx(state_, d, msz, vd); eat(); return {};
        }
        if (sub < 16) { // v(h)tfm2 (4-7) / v(h)tfm3 (8-11) / v(h)tfm4 (12-15) -- matrix * vector, homogeneous row optionally forced to 1/0
            const int ins = static_cast<int>(instr >> 23) & 3;
            const VSize sz = static_cast<VSize>(ins + 1); const MSize msz = static_cast<MSize>(ins + 1);
            const int n = numElem(vecSizeOf(instr)); const int tn = std::min(n, ins + 1);
            float s[16]{}, t[4]{}; readMtx(state_, s, msz, vs); readVec(state_, t, sz, vt);
            float d[4];
            for (int i = 0; i < ins; ++i) {
                d[i] = s[i * 4] * t[0];
                for (int k = 1; k < tn; ++k) d[i] += s[i * 4 + k] * t[k];
                if (ins >= n) d[i] += s[i * 4 + ins];
            }
            applyPrefixST(&s[ins * 4], state_.vfpuCtrl[kSPrefix], 4);
            int cX = -1, cY = (n < 2) ? 0 : -1, cZ = (n < 3) ? 0 : -1, cW = (n < 4) ? 0 : -1;
            if (ins >= n) { if (ins == 1) cY = 1; else if (ins == 2) cZ = 1; else if (ins == 3) cW = 1; }
            const uint32_t tRemove = swizzleBits(0, n < 2 ? 3 : 0, n < 3 ? 3 : 0, n < 4 ? 3 : 0);
            applyPrefixST(t, rewritePrefix(state_.vfpuCtrl[kTPrefix], tRemove, makeConstants(cX, cY, cZ, cW)), 4);
            d[ins] = s[ins * 4] * t[0]; for (int k = 1; k < 4; ++k) d[ins] += s[ins * 4 + k] * t[k];
            const uint32_t lastmask = (state_.vfpuCtrl[kDPrefix] & (1u << 8)) << ins;
            const uint32_t lastsat = (state_.vfpuCtrl[kDPrefix] & 3u) << (ins + ins);
            applyPrefixD(d, lastmask | lastsat, sz == V_Quad ? 4 : numElem(sz));
            writeVec(state_, d, sz, vd); eat(); return {};
        }
        if (sub < 20) { // vmscl: matrix * scalar (the scalar is a single vt lane, V_Single)
            const MSize msz = mtxSizeOf(instr); const int side = mtxSide(msz);
            float s[16]{}, d[16]; readMtx(state_, s, msz, vs);
            float t[1]; readVec(state_, t, V_Single, vt);
            for (int a = 0; a < side; ++a) for (int b = 0; b < side; ++b) d[a * 4 + b] = s[a * 4 + b] * t[0];
            writeMtx(state_, d, msz, vd); eat(); return {};
        }
        if (sub == 28) { // VFPUMatrixSet1: vmmov / vmidt / vmzero / vmone
            const uint32_t optype = (instr >> 16) & 0xFu;
            const MSize msz = mtxSizeOf(instr); const int side = mtxSide(msz);
            if (optype == 0) { // vmmov
                float s[16]; readMtx(state_, s, msz, vs); writeMtx(state_, s, msz, vd); eat(); return {};
            }
            static const float kIdt[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            static const float kZero[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
            static const float kOne[16] = {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
            const float* m = (optype == 3) ? kIdt : (optype == 6) ? kZero : (optype == 7) ? kOne : nullptr;
            if (!m) unimplemented(pc, instr, "VFPUMatrixSet1");
            (void)side;
            writeMtx(state_, m, msz, vd); eat(); return {};
        }
        unimplemented(pc, instr, "VFPU6");
    }
    case 0x2C: // MTVME (Media Engine only): vme[imm16] = rt
        if (rsOf(instr) == 7u) { vme_.write(imm16Of(instr), rtVal); return {}; }
        unimplemented(pc, instr, "opcode");
    case 0x1A: // MFVME (Media Engine only): rt = vme[imm16]
        if (rsOf(instr) == 7u) { const uint32_t v = vme_.read(imm16Of(instr)); setReg(rt, v); return {}; }
        unimplemented(pc, instr, "opcode");
    default: unimplemented(pc, instr, "opcode");
    }
}

// Cause's hardware-line bits (IP2-7) always mirror the live interrupt lines, whatever Status.IE/EXL say -- see the long comment in
// step(). Here: the interrupt controller's aggregated line (as last sampled into pendingCache_) on IP2, and the Count==Compare latch on
// IP7 (uofw interruptman start.S: Cause&0x9300 -> intr 52+15 = 67).
void Cpu::refreshCauseHardwareLines() {
    const uint32_t hardwareLines = ((pendingCache_ << 10) & 0xFC00u) | (timerInterruptPending ? 0x8000u : 0u);
    state_.cp0State[13] = (state_.cp0State[13] & ~0xFC00u) | hardwareLines;
}

void Cpu::step() {
    if (halted_) { // waiting for an interrupt: fast-forward device time instead of executing idle-loop instructions
        uint32_t n = idleSkipMax ? idleSkipMax : 1u;
        const uint32_t d = state_.cp0State[11] - static_cast<uint32_t>(countRegister); // instructions until Count == Compare
        if (!timerInterruptPending && d != 0 && d <= n) n = d;
        bus_.tickN(n);
        countRegister += n;
        skipped_ += n - 1u;
        pendingCache_ = bus_.pendingInterrupts();
        if (!timerInterruptPending && static_cast<uint32_t>(countRegister) == state_.cp0State[11]) {
            timerInterruptPending = true;
            bus_.setTimerInterruptPending(true);
        }
        state_.cp0State[13] = (state_.cp0State[13] & ~0xFC00u) | ((pendingCache_ << 10) & 0xFC00u) | (timerInterruptPending ? 0x8000u : 0u);
        if ((state_.cp0State[13] & state_.cp0State[12] & 0xFF00u) != 0) halted_ = false; // an unmasked interrupt is pending: wake
        return;
    }
    // Give any time-based device (a hardware timer counting toward a
    // compare match) a chance to advance, using instruction count as this
    // interpreter's time base -- must happen before the interrupt check
    // just below so a timer that just reached its period can actually be
    // observed this same step.
    if (tickDone_) tickDone_ = false; // (always false in beta -- see cpu.h's comment on tickDone_)
    else if (--pollCountdown_ == 0) { // devices tick and interrupt lines are sampled every pollInterval_ instructions, not every one
        pollCountdown_ = pollInterval_;
        bus_.tickN(pollInterval_);
        pendingCache_ = bus_.pendingInterrupts();
        ramOk_ = bus_.ramDirectOk();
    }
    ++countRegister; // real COP0 Count free-runs every step; see cpu_state.h comment
    if (!timerInterruptPending && static_cast<uint32_t>(countRegister) == state_.cp0State[11]) {
        timerInterruptPending = true; // real Count==Compare match; see cpu_state.h comment
        bus_.setTimerInterruptPending(true);
    }

    // Real MIPS32 hardware-interrupt check, done once here rather than
    // inside executeAt: a branch and its delay slot execute atomically
    // within one step() call (see below), so checking only at the very
    // top means an interrupt is never taken "mid-delay-slot" -- Cause.BD
    // (branch-delay bit) is therefore always 0 for an interrupt taken
    // here, which is why it's never set below.
    //
    // 2026-09-19 ("well we can continue with the other modules"): CORRECTED
    // a real bug found via a 2.3B-instruction run where Count genuinely
    // crossed Compare (timerInterruptPending latched true, confirmed via a
    // direct trace) but Cause.IP2 never budged from its stale value. Root
    // cause: the Cause.IP2-7 (hardware-line) bits were only ever refreshed
    // INSIDE the `interruptsEnabled && !alreadyInException` gate. On real
    // silicon, Cause always reflects the live state of the hardware
    // interrupt lines regardless of Status.EXL/IE -- those two bits only
    // gate whether a pending line is actually allowed to fire a trap right
    // now, never whether Cause is allowed to show it. Cause is now
    // refreshed unconditionally every step; only the trap-taking decision
    // below stays gated on IE/EXL, matching real hardware.
    refreshCauseHardwareLines();
    {
        const uint32_t status = state_.cp0State[12];
        const bool interruptsEnabled = (status & 0x1u) != 0;  // Status.IE
        const bool alreadyInException = (status & 0x2u) != 0; // Status.EXL
        if (interruptsEnabled && !alreadyInException) {
            // Cause.IP2-IP7 (bits 15:10, the real hardware lines -- IP0/IP1
            // at bits 9:8 are software-triggered and left alone here)
            // gated by the matching Status.IM bits -- a line only actually
            // interrupts the CPU if both the device asserts it and
            // software has unmasked it, exactly like real hardware.
            // 2026-09-18: CORRECTED. Real Allegrex hardware does NOT use
            // textbook MIPS32 IP7 (bit 15) for the internal Count==Compare
            // match. Confirmed via uofw's real interruptman.c IntrManInit():
            // `pspCop0StateSet(COP0_STATE_STATUS, (Status & 0xFFFF00FF) |
            // 0x0400)` -- this clears ALL of Status's interrupt-mask bits
            // (8-15) and unmasks ONLY bit 10 (IP2), right before arming
            // Compare and calling `sceKernelEnableIntr(67)`. Real hardware
            // aggregates all 68 software-visible interrupt lines (including
            // the CPU-internal timer match, "line 67") through a single
            // interrupt controller onto this one physical CPU line (IP2) --
            // interruptman.prx's own registered exception handler does the
            // real software demultiplexing, not the CPU itself.
            const uint32_t cause = state_.cp0State[13];
            if ((cause & status & 0xFF00u) != 0) {
                state_.cp0State[14] = state_.pc; // EPC
                state_.cp0State[13] &= ~0x3Fu; // ExcCode = 0 (Interrupt), Cause.BD stays 0
                state_.cp0State[12] |= 0x2u; // Status.EXL = 1
                // 2026-09-18 ("Have a look then") -- CORRECTED. The
                // textbook MIPS32r2 "KSEG0 base + 0x180 for BEV=0" address
                // (0x88000180 in this project's RAM-based addressing) was
                // wrong for real Allegrex hardware. Real uofw source
                // (src/kd/exceptionman/exceptions.c, ExcepManInit()) proves
                // the real general-exception dispatcher is installed
                // UNCONDITIONALLY at 0xBFC00000 (not gated on Status.BEV at
                // all). The BEV=1 textbook fallback (0xBFC00380) is
                // therefore never used -- nothing real this project has
                // traced ever installs code there.
                // 2026-09-23 FIX: EBase is COP0 *state* register 25 (mtc0/mfc0),
                // NOT the ctc0/cfc0 control register 25. jpcsp
                // (Cp0State.getEbase == getDataRegister(25)) and uofw common.S
                // (COP0_STATE_EBASE $25 -- virtual address of exception vector)
                // agree; uofw calls ctrl 25 COP0_CTRL_PROFILER_BASE.
                if ((status & 0x00400000u) == 0 && state_.cp0State[25] != 0u) {
                    state_.pc = state_.cp0State[25];
                } else {
                    state_.pc = 0xBFC00000u;
                }
                return;
            }
        }
    }

    if (tryFastForwardZeroFillLoop()) return;
    execStepBody();
}

// Recognizes and bulk-applies real cIPL-resident zero-fill loops (confirmed
// live, psp-lle-beta Milestone A tracing 2026-11-04): real loadcore calls a
// ROM-resident utility at the boot vector to clear large RAM ranges word by
// word --
//   sw   $zero, 0(addrReg)      <- pc
//   bne  addrReg, boundReg, pc  <- pc+4  (loops back to the sw above)
//   addiu addrReg, addrReg, 4   <- pc+8  (the bne's own delay slot)
// -- real arguments seen live span the entire 64MB of RAM, costing on the
// order of tens of millions of real interpreted steps per call. This is not
// a shortcut around real behavior: it computes the exact same final memory
// content, addrReg value, and Count/device-time advance that single-
// stepping the same number of real iterations would, including the same
// Count==Compare interrupt-timing cap the halted_ fast-forward above already
// uses -- anything that doesn't match this exact three-instruction shape
// (byte for byte, including the +4 delay-slot step matching the store's own
// word width) falls straight through to the normal interpreter untouched.
bool Cpu::tryFastForwardZeroFillLoop() {
    const uint32_t pc = state_.pc;
    uint32_t wSw, wBr, wInc;
    try {
        wSw = rd32(pc);
        wBr = rd32(pc + 4u);
        wInc = rd32(pc + 8u);
    } catch (const std::out_of_range&) {
        return false; // let the normal fetch path raise this the usual way
    }

    if (opcodeOf(wSw) != 0x2Bu) return false;              // SW
    if (rtOf(wSw) != 0u) return false;                      // must store the literal $zero
    if (signExtend16(imm16Of(wSw)) != 0) return false;      // no base offset
    const uint32_t addrReg = rsOf(wSw);
    if (addrReg == 0u) return false;                        // a real loop never re-zeroes through $zero itself

    if (opcodeOf(wBr) != 0x05u) return false;                // BNE
    const uint32_t brRs = rsOf(wBr), brRt = rtOf(wBr);
    uint32_t boundReg;
    if (brRs == addrReg && brRt != addrReg) boundReg = brRt;
    else if (brRt == addrReg && brRs != addrReg) boundReg = brRs;
    else return false;
    const uint32_t branchTarget = static_cast<uint32_t>(static_cast<int32_t>(pc + 8u) + (signExtend16(imm16Of(wBr)) << 2));
    if (branchTarget != pc) return false;                    // must loop back exactly to the sw above

    if (opcodeOf(wInc) != 0x09u) return false;                // ADDIU (the bne's delay slot)
    if (rtOf(wInc) != addrReg || rsOf(wInc) != addrReg) return false;
    if (signExtend16(imm16Of(wInc)) != 4) return false;       // must match the sw's own word width

    const uint32_t addrStart = state_.gpr[addrReg];
    const uint32_t boundVal = state_.gpr[boundReg];
    const uint32_t totalBytes = boundVal - addrStart; // real loop direction: forward, inclusive of boundVal
    if ((totalBytes & 0x3u) != 0u) return false;       // must be word-aligned (also excludes the degenerate all-ones wrap)
    uint64_t n = (static_cast<uint64_t>(totalBytes) >> 2) + 1u; // iterations, inclusive of the final word at boundVal

    // Same Count==Compare interrupt-timing safety the halted_ fast-forward
    // above already relies on: never bulk past the exact instruction where a
    // real pending timer interrupt would actually divert control flow, since
    // that would silently skip real exception delivery mid-loop.
    if (timerInterruptPending) return false; // a real interrupt is already due to fire on this very step -- let the normal path take it
    // This very call to step() already advanced countRegister by 1 for the
    // sw instruction at pc (the normal top-of-function bookkeeping, which
    // ran before this function was reached, below). n iterations cost n*2
    // step()-equivalents in total (one per sw, one per bne+delay-slot pair)
    // -- 1 of those is already done, so everything here adds n*2-1 more, or
    // Count would double-count that first step.
    const uint64_t nFull = n; // the full iteration count needed to actually finish the loop (reach boundVal)
    const uint32_t stepsUntilCompare = state_.cp0State[11] - static_cast<uint32_t>(countRegister);
    uint64_t extraSteps = n * 2u - 1u;
    if (stepsUntilCompare != 0u && extraSteps > stepsUntilCompare) {
        n = (static_cast<uint64_t>(stepsUntilCompare) + 1u) / 2u; // largest n' with n'*2-1 <= stepsUntilCompare
        if (n == 0u) return false; // can't even safely bulk one whole iteration -- let the normal interpreter step up to the match
        extraSteps = n * 2u - 1u;
    }

    for (uint64_t i = 0; i < n; ++i) {
        wr32(addrStart + static_cast<uint32_t>(i * 4u), 0u);
    }
    setReg(addrReg, addrStart + static_cast<uint32_t>(n * 4u)); // the delay slot's own increment always executes, even on the exiting iteration
    // Only a FULL n == nFull means the loop has actually reached boundVal and
    // genuinely falls through (real bne not taken). A Count==Compare cap
    // above may have shrunk n to less than that -- the loop isn't done, so
    // pc must resume at the sw above (pc), not fall through, exactly as if
    // a real not-yet-finished iteration had just run.
    state_.pc = (n == nFull) ? (pc + 12u) : pc;

    uint64_t remaining = extraSteps;
    while (remaining > 0) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(remaining, 0xFFFFFFFFull));
        bus_.tickN(chunk);
        remaining -= chunk;
    }
    countRegister += extraSteps;
    if (!timerInterruptPending && static_cast<uint32_t>(countRegister) == state_.cp0State[11]) {
        timerInterruptPending = true;
        bus_.setTimerInterruptPending(true);
    }
    pendingCache_ = bus_.pendingInterrupts();
    skipped_ += extraSteps;
    return true;
}

// Everything step() does after the per-step device tick, Count update and interrupt check: fetch, execute
// (with its delay slot), and the exception delivery.
void Cpu::execStepBody() {
    const uint32_t pc = state_.pc;
    bool faultIsFetch = false;
    try {
        uint32_t instr;
        try {
            instr = rd32(pc);
        } catch (const std::out_of_range&) {
            faultIsFetch = true;
            throw;
        }
        if (ringOn) { ringPc[ringPos & 63u] = pc; ringWord[ringPos & 63u] = instr; ++ringPos; }
        const BranchResult branch = executeAt(pc, instr);

        if (branch.noDelaySlot || branch.isSyscallRedirect) {
            // ERET, or a standalone (non-delay-slot) resolved SYSCALL --
            // neither has a delay slot to execute.
            state_.pc = branch.target;
            return;
        }

        if (!branch.isControlFlow) {
            state_.pc = pc + 4;
            return;
        }

        const uint32_t delaySlotPc = pc + 4;

        if (branch.likely && !branch.taken) {
            // A not-taken "branch likely" nullifies its delay slot entirely --
            // unlike every other branch/jump, it never executes at all.
            state_.pc = delaySlotPc + 4;
            return;
        }

        // The delay slot instruction always executes before control transfers,
        // per MIPS/Allegrex pipeline semantics.
        uint32_t delaySlotInstr;
        try {
            delaySlotInstr = rd32(delaySlotPc);
        } catch (const std::out_of_range&) {
            faultIsFetch = true;
            throw;
        }
        const BranchResult nested = executeAt(delaySlotPc, delaySlotInstr);
        if (nested.isSyscallRedirect) {
            // Real convention (uofw's SceStub union): a real kernel/user
            // stub is exactly `jr $ra` (this outer branch) with `syscall N`
            // in its delay slot. $ra was never touched by the stub, so
            // jumping straight to the real resolved target here -- instead
            // of completing the outer jr's own branch.target -- has the
            // same real effect as the full trap/dispatch/ERET round trip.
            state_.pc = nested.target;
            return;
        }
        if (nested.isControlFlow) {
            throw std::runtime_error(
                "branch/jump in delay slot at pc=0x" + std::to_string(delaySlotPc) +
                " is unsupported (undefined on real hardware)");
        }

        state_.pc = branch.taken ? branch.target : delaySlotPc + 4;
    } catch (const std::out_of_range& e) {
        if (deliverBusErrorsAsExceptions) {
            // Real MIPS32 Bus Error exception delivery -- mirrors the
            // hardware-interrupt vectoring above (same Status.EXL/BEV-gated
            // vector convention), except for ExcCode (6=IBE instruction
            // fetch, 7=DBE data load/store, per faultIsFetch) and BadVAddr,
            // which real hardware also latches on this exception class.
            // system_bus.cpp's unmapped() only carries the faulting address
            // as text ("unmapped bus address: 0x%08X") -- parsed back out
            // here rather than plumbing a new structured exception type
            // through every Bus::read/write call site for one consumer.
            uint32_t badVAddr = pc;
            std::sscanf(e.what(), "unmapped bus address: 0x%x", &badVAddr);
            const uint32_t excCode = faultIsFetch ? 6u : 7u; // IBE : DBE
            if (logBusErrors_) { // CpuOptions::logBusErrors: every delivered bus error (first 200)
                static int nLogged = 0;
                if (nLogged < 200) { ++nLogged; std::printf("  [BUSERR] %s at 0x%08X pc=0x%08X ra=0x%08X Status=0x%08X\n", faultIsFetch ? "IBE" : "DBE", badVAddr, pc, state_.gpr[31], state_.cp0State[12]);
                  uint32_t w0 = 0, w1 = 0; try { w0 = bus_.read32(pc); w1 = bus_.read32(pc + 4u); } catch (...) {}
                  std::printf("    [BUSERR detail] what='%s' insn@pc=%08X insn@pc+4=%08X t0=%08X t1=%08X t6=%08X a0=%08X\n", e.what(), w0, w1, state_.gpr[8], state_.gpr[9], state_.gpr[14], state_.gpr[4]); }
            }
            state_.cp0State[8] = badVAddr;   // BadVAddr
            state_.cp0State[14] = pc;        // EPC -- the whole step() restarts on ERET, matching the interrupt path above
            state_.cp0State[13] = (state_.cp0State[13] & ~0x3Fu) | ((excCode << 2) & 0x3Fu); // Cause.ExcCode
            state_.cp0State[12] |= 0x2u;     // Status.EXL = 1
            if ((state_.cp0State[12] & 0x00400000u) == 0u && state_.cp0State[25] != 0u) {
                state_.pc = state_.cp0State[25];
            } else {
                state_.pc = 0xBFC00000u;
            }
            return;
        }
        // Bus errors don't carry pc context on their own (system_bus.cpp
        // only knows the faulting address) -- tag it here so diagnostics
        // can tell a genuine bad-address bug from an expected gap.
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s (step starting at pc=0x%08X)", e.what(), pc);
        throw std::runtime_error(buf);
    } catch (const BreakTrap& bt) {
        { // diagnostic (always on, a BREAK is rare): what the registers held, and what the preceding instruction was (a divide-by-zero guard is `div; beql rt,zero,+1; break 7`)
            if (breakDiagCount++ < 40) {
                uint32_t prev = 0, prev2 = 0; try { prev = bus_.read32(bt.pc - 4u); prev2 = bus_.read32(bt.pc - 8u); } catch (...) {}
                std::printf("  [BREAK] pc=%08X code=%u ra=%08X sp=%08X a0=%08X a1=%08X a2=%08X a3=%08X s0=%08X t0=%08X t1=%08X t2=%08X insn@pc-4=%08X insn@pc-8=%08X f0=%08X f1=%08X f2=%08X f3=%08X Status=%08X Cause=%08X%c",
                            bt.pc, bt.code, state_.gpr[31], state_.gpr[29], state_.gpr[4], state_.gpr[5], state_.gpr[6], state_.gpr[7], state_.gpr[16], state_.gpr[8], state_.gpr[9], state_.gpr[10], prev, prev2,
                            state_.fpr[0].u, state_.fpr[1].u, state_.fpr[2].u, state_.fpr[3].u, state_.cp0State[12], state_.cp0State[13], 10);
                std::fflush(stdout);
            }
        }
        if (deliverBusErrorsAsExceptions) {
            // Real MIPS32 Breakpoint exception (ExcCode 9) -- same
            // Status.EXL/BEV-gated vector convention as the Bus Error and
            // hardware-interrupt paths above. BadVAddr is left untouched
            // (real hardware doesn't latch it for this exception class).
            state_.cp0State[14] = bt.pc; // EPC
            state_.cp0State[13] = (state_.cp0State[13] & ~0x3Fu) | ((9u << 2) & 0x3Fu); // Cause.ExcCode = 9 (Bp)
            state_.cp0State[12] |= 0x2u; // Status.EXL = 1
            if ((state_.cp0State[12] & 0x00400000u) == 0u && state_.cp0State[25] != 0u) {
                state_.pc = state_.cp0State[25];
            } else {
                state_.pc = 0xBFC00000u;
            }
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "BREAK trap (code=%u): pc=0x%08X instr=0x%08X", bt.code, bt.pc, bt.instr);
        throw std::runtime_error(buf);
    } catch (const SyscallTrap& st) {
        // 2026-10-30 (old project): real Sony's "unlinked import" placeholder (SYSCALL_NOT_LINKED, code 21 --
        // uofw loadcore.c writes this literal instruction into every stub whose NID isn't found in the
        // library it DID find by name, e.g. a synthetic zero-export library's unresolved stub). Finding
        // which real call site actually executes one of these -- as opposed to guessing -- is the direct way
        // to identify which real function a game needs that a synthetic library doesn't actually provide yet.
        if (st.code == 21u && traceSyscall21_) { // CpuOptions::traceSyscall21
            static long long hits = 0;
            ++hits;
            if (hits <= 100 || hits % 500000 == 0) {
                std::fprintf(stderr, "  [SYSCALL21] #%lld pc=0x%08X ra=0x%08X a0=0x%08X a1=0x%08X\n",
                             hits, st.pc, state_.gpr[31], state_.gpr[4], state_.gpr[5]);
            }
        }
        if (deliverBusErrorsAsExceptions) {
            // Real MIPS32 Syscall exception (ExcCode 8) -- identical
            // Status.EXL/BEV-gated vector convention as BreakTrap above.
            // BadVAddr is left untouched (real hardware doesn't latch it
            // for this exception class either).
            // 2026-09-23: Allegrex latches the syscall code in COP0 state register 21 (SCCODE;
            // jpcsp Common.COP0_STATE_SCCODE) -- interruptman's syscall handler reads it to index
            // the syscall table, so without it every real user syscall dispatched as code 0.
            // Stored pre-shifted (code << 2): the handler subtracts it from each table's byte-scaled
            // seed (SceSyscallTable.seed = firstCode << 2). jpcsp RuntimeContextLLE:
            // processor.cp0.setSyscallCode(syscallCode << 2).
            state_.cp0State[21] = st.code << 2;
            // A syscall in a branch delay slot (every PSP import stub: `jr $ra` + `syscall N`)
            // reports EPC = the BRANCH and Cause.BD=1; interruptman's handler then returns to $ra
            // (`movn v0, ra, v1` with v1 = Cause>>31) instead of EPC+4.
            const bool inDelaySlot = (st.pc != pc);
            state_.cp0State[14] = inDelaySlot ? pc : st.pc; // EPC
            state_.cp0State[13] = (state_.cp0State[13] & ~0x8000003Fu) | ((8u << 2) & 0x3Fu) | (inDelaySlot ? 0x80000000u : 0u); // Cause.BD, ExcCode = 8 (Sys)
            state_.cp0State[12] |= 0x2u; // Status.EXL = 1
            if ((state_.cp0State[12] & 0x00400000u) == 0u && state_.cp0State[25] != 0u) {
                state_.pc = state_.cp0State[25];
            } else {
                state_.pc = 0xBFC00000u;
            }
            return;
        }
        char buf2[96];
        std::snprintf(buf2, sizeof(buf2), "SYSCALL trap (code=%u): pc=0x%08X instr=0x%08X", st.code, st.pc, st.instr);
        throw std::runtime_error(buf2);
    }
}

} // namespace beta
