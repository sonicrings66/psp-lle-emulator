// Ported from psp-lle-emu/tests/test_cpu.cpp (see docs/PORTING_LOG.md).
// Loads small hand-encoded MIPS instruction sequences directly into a
// flat Memory (no SystemBus/MMIO involved) and verifies the Allegrex
// interpreter's resulting register/memory state. Every expected value
// is unchanged from the old project -- this pins real, already-verified
// MIPS semantics, not a place to improve anything.

#include <initializer_list>
#include <stdexcept>

#include "cpu.h"
#include "memory.h"
#include "memory_map.h"
#include "mips_encoder.h"
#include "bus/system_bus.h"
#include "test_framework.h"

using namespace beta;
using namespace enc;

namespace {

// Wraps a real SystemBus (needed so the standard exception vector
// correctly aliases down into modeled RAM the same way every real
// module load address in this project does) and adds a manually-settable
// interrupt line, purely for exercising Cpu's interrupt-check logic
// without needing a real timer/device to actually trigger one. Inherits
// CpuBus (not just Bus) so it satisfies Cpu's constructor -- the other
// three interfaces (TimingSource/JitHints/SyscallResolver) keep their
// safe no-op defaults, exactly matching the old project's Bus defaults
// for the same methods (this test never needed the fast path or real
// device timing).
class InjectableInterruptBus : public CpuBus {
public:
    explicit InjectableInterruptBus(SystemBus& inner) : inner_(inner) {}
    uint8_t read8(uint32_t addr) const override { return inner_.read8(addr); }
    uint16_t read16(uint32_t addr) const override { return inner_.read16(addr); }
    uint32_t read32(uint32_t addr) const override { return inner_.read32(addr); }
    void write8(uint32_t addr, uint8_t v) override { inner_.write8(addr, v); }
    void write16(uint32_t addr, uint16_t v) override { inner_.write16(addr, v); }
    void write32(uint32_t addr, uint32_t v) override { inner_.write32(addr, v); }
    uint32_t pendingInterrupts() const override { return asserted; }
    uint32_t asserted = 0;

private:
    SystemBus& inner_;
};

void loadProgram(Bus& mem, uint32_t addr, std::initializer_list<uint32_t> words) {
    uint32_t a = addr;
    for (uint32_t w : words) {
        mem.write32(a, w);
        a += 4;
    }
}

void test_addiu_chain(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        ADDIU(T0, ZERO, 5),
        ADDIU(T1, T0, 10),
        ADDIU(T2, T1, -3),
    });
    cpu.reset(0);
    cpu.step();
    cpu.step();
    cpu.step();
    CHECK_EQ(stats, cpu.reg(T0), 5u);
    CHECK_EQ(stats, cpu.reg(T1), 15u);
    CHECK_EQ(stats, cpu.reg(T2), 12u);
}

void test_rtype_arith_logic(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        ADDIU(T0, ZERO, 6),
        ADDIU(T1, ZERO, 3),
        ADD(T2, T0, T1),  // 9
        SUB(T3, T0, T1),  // 3
        AND(T4, T0, T1),  // 6 & 3 = 2
        OR(T5, T0, T1),   // 6 | 3 = 7
        XOR(T6, T0, T1),  // 6 ^ 3 = 5
        SLT(T7, T1, T0),  // 3 < 6 -> 1
    });
    cpu.reset(0);
    for (int i = 0; i < 8; ++i) cpu.step();
    CHECK_EQ(stats, cpu.reg(T2), 9u);
    CHECK_EQ(stats, cpu.reg(T3), 3u);
    CHECK_EQ(stats, cpu.reg(T4), 2u);
    CHECK_EQ(stats, cpu.reg(T5), 7u);
    CHECK_EQ(stats, cpu.reg(T6), 5u);
    CHECK_EQ(stats, cpu.reg(T7), 1u);
}

void test_lui_ori_32bit_load(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        LUI(T0, 0x1234),
        ORI(T0, T0, 0x5678),
    });
    cpu.reset(0);
    cpu.step();
    cpu.step();
    CHECK_EQ(stats, cpu.reg(T0), 0x12345678u);
}

void test_load_store_roundtrip(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        ADDIU(T0, ZERO, 0x1234),
        SW(T0, ZERO, 100),
        LW(T1, ZERO, 100),
        ADDIU(T2, ZERO, -1),  // 0xFFFFFFFF
        SB(T2, ZERO, 200),    // stores 0xFF
        LB(T3, ZERO, 200),    // sign-extended -> 0xFFFFFFFF
        LBU(T4, ZERO, 200),   // zero-extended -> 0x000000FF
    });
    cpu.reset(0);
    for (int i = 0; i < 7; ++i) cpu.step();
    CHECK_EQ(stats, cpu.reg(T1), 0x1234u);
    CHECK_EQ(stats, cpu.reg(T3), 0xFFFFFFFFu);
    CHECK_EQ(stats, cpu.reg(T4), 0x000000FFu);
}

void test_branch_delay_slot_taken(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        ADDIU(T0, ZERO, 1),   // 0
        BEQ(T0, T0, 3),       // 4: taken, target = 8 + 3*4 = 20
        ADDIU(T1, ZERO, 99),  // 8:  delay slot, MUST execute
        ADDIU(T2, ZERO, 11),  // 12: must be skipped
        ADDIU(T2, ZERO, 22),  // 16: must be skipped
        ADDIU(T3, ZERO, 33),  // 20: landing point
    });
    cpu.reset(0);
    cpu.step();                            // ADDIU T0
    cpu.step();                            // BEQ + its delay slot, as one step
    CHECK_EQ(stats, cpu.reg(T1), 99u);     // delay slot executed despite branch taken
    CHECK_EQ(stats, cpu.state().pc, 20u);  // landed on branch target
    cpu.step();                            // ADDIU T3 at addr 20
    CHECK_EQ(stats, cpu.reg(T3), 33u);
    CHECK_EQ(stats, cpu.reg(T2), 0u);      // skipped instructions never ran
}

void test_branch_delay_slot_not_taken(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        ADDIU(T0, ZERO, 1),   // 0
        ADDIU(T1, ZERO, 2),   // 4
        BEQ(T0, T1, 5),       // 8: not taken (1 != 2)
        ADDIU(T2, ZERO, 77),  // 12: delay slot, MUST execute
        ADDIU(T3, ZERO, 44),  // 16: fall-through after delay slot
    });
    cpu.reset(0);
    cpu.step();
    cpu.step();
    cpu.step();                            // BEQ + its delay slot
    CHECK_EQ(stats, cpu.reg(T2), 77u);
    CHECK_EQ(stats, cpu.state().pc, 16u);  // delaySlotPc(12) + 4
    cpu.step();
    CHECK_EQ(stats, cpu.reg(T3), 44u);
}

void test_jal_and_jr_return(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        JAL(100),             // 0: call func at 100, link = 0+4+4 = 8
        ADDIU(A0, ZERO, 5),   // 4: delay slot, MUST run before the jump
    });
    loadProgram(mem, 8, {
        ADDIU(V1, ZERO, 77),  // 8: JR RA lands here
    });
    loadProgram(mem, 100, {
        ADDIU(V0, A0, 1),     // 100: V0 = A0 + 1
        JR(RA),                // 104: return to caller (RA == 8)
        ADDIU(T5, ZERO, 55),  // 108: delay slot, MUST run before the jump
    });

    cpu.reset(0);
    cpu.step();  // JAL + delay slot
    CHECK_EQ(stats, cpu.state().pc, 100u);
    CHECK_EQ(stats, cpu.reg(A0), 5u);
    CHECK_EQ(stats, cpu.reg(RA), 8u);

    cpu.step();  // ADDIU V0, A0, 1
    CHECK_EQ(stats, cpu.reg(V0), 6u);
    CHECK_EQ(stats, cpu.state().pc, 104u);

    cpu.step();  // JR RA + delay slot
    CHECK_EQ(stats, cpu.reg(T5), 55u);
    CHECK_EQ(stats, cpu.state().pc, 8u);

    cpu.step();  // ADDIU V1, ZERO, 77
    CHECK_EQ(stats, cpu.reg(V1), 77u);
}

void test_interrupt_delivery_and_eret(TestStats& stats) {
    SystemBus sysBus(MemoryMap::kRamSize32MB);
    InjectableInterruptBus bus(sysBus);

    constexpr uint32_t kMainAddr = MemoryMap::kRamStart + 0x100000;
    // Real vector is 0xBFC00000, not the textbook MIPS32 EBASE+0x180
    // (0x88000180) -- see cpu.cpp and the real uofw source
    // (exceptionman/exceptions.c) that proves this.
    constexpr uint32_t kVectorAddr = 0xBFC00000u;
    constexpr uint32_t kStatusImIe = 0x401u; // Status.IM bit for hardware line 0 (bit 10) + IE (bit 0)

    loadProgram(bus, kMainAddr, {
        ADDIU(T0, ZERO, static_cast<int16_t>(kStatusImIe)),
        MTC0(T0, 12),           // Status = IE=1, IM(line0)=1
        ADDIU(T1, ZERO, 0xBAD), // must NOT execute -- the interrupt should
                                 // intercept this pc before it's fetched
        ADDIU(T1, ZERO, 0x600D),// landing point after the handler ERETs back
    });
    loadProgram(bus, kVectorAddr, {
        ADDIU(T2, ZERO, 0x1234), // proof the real handler ran
        ERET(),
    });

    Cpu cpu(bus);
    cpu.reset(kMainAddr);
    cpu.step(); // ADDIU T0, ...
    cpu.step(); // MTC0 -- enables the interrupt
    CHECK_EQ(stats, (cpu.state().cp0State[12] & 0x1u), 1u); // Status.IE set

    bus.asserted = 0x1; // assert hardware interrupt line 0

    const uint32_t pcBeforeInterrupt = cpu.state().pc;
    cpu.step(); // should divert to the exception vector instead of the ADDIU at kMainAddr+8
    CHECK_EQ(stats, cpu.state().pc, kVectorAddr);
    CHECK_EQ(stats, cpu.state().cp0State[14], pcBeforeInterrupt); // EPC captured the interrupted pc
    CHECK_EQ(stats, (cpu.state().cp0State[12] & 0x2u), 0x2u);     // Status.EXL set
    CHECK_EQ(stats, cpu.reg(T1), 0u);                        // the 0xBAD instruction never ran

    bus.asserted = 0; // real hardware would clear the source; simulate that
                       // so the handler's own ERET isn't immediately re-interrupted

    cpu.step(); // ADDIU T2, ZERO, 0x1234 (inside the handler)
    CHECK_EQ(stats, cpu.reg(T2), 0x1234u);

    cpu.step(); // ERET -- no delay slot, returns straight to EPC
    CHECK_EQ(stats, cpu.state().pc, pcBeforeInterrupt);
    CHECK_EQ(stats, (cpu.state().cp0State[12] & 0x2u), 0u); // Status.EXL cleared

    cpu.step(); // the originally-interrupted ADDIU T1, ZERO, 0xBAD, now finally runs
    CHECK_EQ(stats, cpu.reg(T1), 0xBADu);
    cpu.step(); // ADDIU T1, ZERO, 0x600D
    CHECK_EQ(stats, cpu.reg(T1), 0x600Du);
}

// Validates Cpu::tryFastForwardZeroFillLoop() (cpu.cpp) -- the real cIPL
// zero-fill-loop recognizer added to fast-forward past a real, confirmed-
// live early-boot RAM-clear utility that otherwise costs tens of millions of
// interpreted steps per call (psp-lle-beta Milestone A, 2026-11-04). Checks
// the bulk path's result against hand-derived expected values (not a parity
// run against genuine single-stepping, since that would defeat the point of
// a fast path meant to replace millions of steps) -- a mismatch here would
// mean the fast path computes a different final state than real single-
// stepping the same loop would, which is exactly the class of bug this test
// exists to catch.
void test_zerofill_fastforward_completes_whole_loop(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        SW(ZERO, T0, 0),     // pc=0
        BNE(T0, T1, -2),     // pc=4, loops back to pc=0
        ADDIU(T0, T0, 4),    // pc=8 (the bne's delay slot)
    });
    constexpr uint32_t kDataStart = 0x100, kDataEnd = 0x110; // 5 inclusive words: 0x100,0x104,0x108,0x10C,0x110
    for (uint32_t a = kDataStart; a <= kDataEnd; a += 4) mem.write32(a, 0xFFFFFFFFu);

    cpu.reset(0);
    cpu.state().gpr[T0] = kDataStart;
    cpu.state().gpr[T1] = kDataEnd;
    cpu.countRegister = 0;

    cpu.step(); // should recognize the pattern and bulk-complete all 5 iterations in one call

    for (uint32_t a = kDataStart; a <= kDataEnd; a += 4) {
        CHECK_EQ(stats, mem.read32(a), 0u);
    }
    CHECK_EQ(stats, cpu.reg(T0), kDataEnd + 4u);  // the delay slot's own final increment always executes
    CHECK_EQ(stats, cpu.state().pc, 12u);          // falls through past the branch's delay slot
    CHECK_EQ(stats, cpu.countRegister, 10ull);     // 5 iterations * 2 step()-equivalents each
    const uint64_t skipped = cpu.takeSkipped();    // capture first -- takeSkipped() resets its own counter as a side effect
    CHECK_EQ(stats, skipped, 9ull);                 // 10 total, minus the 1 this very step() call already counts as
}

// Same loop, but with COP0 Compare set to land exactly mid-loop -- the fast
// path must stop short (not overshoot past where a real timer interrupt
// would divert control flow), leaving the loop genuinely unfinished so the
// normal interpreter resumes it (and delivers the interrupt) on a later
// step(), exactly as real single-stepping would.
void test_zerofill_fastforward_stops_at_compare_match(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    loadProgram(mem, 0, {
        SW(ZERO, T0, 0),
        BNE(T0, T1, -2),
        ADDIU(T0, T0, 4),
    });
    constexpr uint32_t kDataStart = 0x100, kDataEnd = 0x110; // 5 inclusive words
    for (uint32_t a = kDataStart; a <= kDataEnd; a += 4) mem.write32(a, 0xFFFFFFFFu);

    cpu.reset(0);
    cpu.state().gpr[T0] = kDataStart;
    cpu.state().gpr[T1] = kDataEnd;
    cpu.countRegister = 0;
    cpu.state().cp0State[11] = 4; // Compare: matches after this step()'s own +1 plus 3 more (2 full iterations)

    cpu.step();

    CHECK_EQ(stats, mem.read32(kDataStart), 0u);        // iteration 1 done
    CHECK_EQ(stats, mem.read32(kDataStart + 4u), 0u);    // iteration 2 done
    CHECK_EQ(stats, mem.read32(kDataStart + 8u), 0xFFFFFFFFu); // iteration 3 NOT done -- stopped short
    CHECK_EQ(stats, cpu.reg(T0), kDataStart + 8u);        // resumed from exactly where it stopped
    CHECK_EQ(stats, cpu.state().pc, 0u);                   // loop genuinely unfinished -- resumes at the sw above, not pc+12
    CHECK_EQ(stats, cpu.countRegister, 4ull);
    CHECK_TRUE(stats, cpu.timerInterruptPending);          // the real Count==Compare match fired exactly on schedule
}

void test_unimplemented_instruction_throws(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    mem.write32(0, 0xFC000000u);  // opcode 0x3F: not part of MIPS-I, unimplemented
    cpu.reset(0);
    bool threw = false;
    try {
        cpu.step();
    } catch (const std::exception&) {
        threw = true;
    }
    reportCheck(stats, threw, "unimplemented opcode throws", threw ? 1u : 0u, 1u);
}

} // namespace

void runAllCpuTests(TestStats& stats) {
    runTest(stats, "addiu_chain", test_addiu_chain);
    runTest(stats, "rtype_arith_logic", test_rtype_arith_logic);
    runTest(stats, "lui_ori_32bit_load", test_lui_ori_32bit_load);
    runTest(stats, "load_store_roundtrip", test_load_store_roundtrip);
    runTest(stats, "branch_delay_slot_taken", test_branch_delay_slot_taken);
    runTest(stats, "branch_delay_slot_not_taken", test_branch_delay_slot_not_taken);
    runTest(stats, "jal_and_jr_return", test_jal_and_jr_return);
    runTest(stats, "unimplemented_instruction_throws", test_unimplemented_instruction_throws);
    runTest(stats, "interrupt_delivery_and_eret", test_interrupt_delivery_and_eret);
    runTest(stats, "zerofill_fastforward_completes_whole_loop", test_zerofill_fastforward_completes_whole_loop);
    runTest(stats, "zerofill_fastforward_stops_at_compare_match", test_zerofill_fastforward_stops_at_compare_match);
}
