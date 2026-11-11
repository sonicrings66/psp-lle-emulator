// VFPU (Allegrex vector unit) tests, 2026-10-18: hand-encoded instruction words (see mips_encoder.h) run
// through the real interpreter, checking the resulting CpuState::vfpu register-file contents. Only the
// non-transcendental subset cpu.cpp/vfpu.h implement is covered here -- see vfpu.h's own comment for what's
// still missing (sin/cos/rcp/rsq/sqrt/log2/exp2/asin and the rarer shuffle/pack ops).
//
// All expected values are chosen to be exact in IEEE-754 single precision (small integers, halves, quarters)
// so CHECK_EQ's bit-exact uint32_t comparison (via floatBits below) is meaningful without a tolerance macro.

#include <cstring>

#include "cpu.h"
#include "memory.h"
#include "mips_encoder.h"
#include "test_framework.h"

using namespace beta;
using namespace enc;

namespace {

uint32_t floatBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float asFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t swizzleBitsForTest(int x, int y, int z, int w) {
    return static_cast<uint32_t>((x & 3) | ((y & 3) << 2) | ((z & 3) << 4) | ((w & 3) << 6));
}
int vfpuFlatIdx(uint32_t reg, int lane) {
    const int mtx = static_cast<int>(reg >> 2) & 7, col = static_cast<int>(reg) & 3;
    return mtx * 16 + col * 4 + lane;
}

void loadProgram(Bus& mem, uint32_t addr, std::initializer_list<uint32_t> words) {
    uint32_t a = addr;
    for (uint32_t w : words) { mem.write32(a, w); a += 4; }
}

// Sets register `reg`'s up to 4 flat-array words directly (bypassing instruction decode), for seeding
// operands without depending on lv.q's own correctness.
void pokeVfpuQuad(Cpu& cpu, uint32_t reg, float a, float b, float c, float d) {
    // reg is assumed column-0-of-a-matrix, non-transposed, quad-aligned (the only layout these tests use):
    // flat index = mtx*16 + col*4 + row, mtx=(reg>>2)&7, col=reg&3 -- matches vfpu.h's vecIdx exactly.
    const int mtx = static_cast<int>(reg >> 2) & 7, col = static_cast<int>(reg) & 3;
    const int base = mtx * 16 + col * 4;
    cpu.state().vfpu[base + 0] = floatBits(a);
    cpu.state().vfpu[base + 1] = floatBits(b);
    cpu.state().vfpu[base + 2] = floatBits(c);
    cpu.state().vfpu[base + 3] = floatBits(d);
}
float readVfpuLane(Cpu& cpu, uint32_t reg, int lane) {
    const int mtx = static_cast<int>(reg >> 2) & 7, col = static_cast<int>(reg) & 3;
    return asFloat(cpu.state().vfpu[mtx * 16 + col * 4 + lane]);
}

void test_vadd_vsub_vmul_vdiv_quad(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 1.0f, 2.0f, 3.0f, 4.0f);   // vs = reg 0
    pokeVfpuQuad(cpu, 4, 10.0f, 20.0f, 0.5f, 2.0f); // vt = reg 4
    loadProgram(mem, 0, {
        VADD(8, 0, 4, 4),  // vd = reg 8 = vs + vt
        VSUB(12, 0, 4, 4), // vd = reg 12 = vs - vt
        VMUL(16, 0, 4, 4), // vd = reg 16 = vs * vt
        VDIV(20, 0, 4, 4), // vd = reg 20 = vs / vt
    });
    cpu.step(); cpu.step(); cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(11.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 3)), floatBits(6.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 0)), floatBits(-9.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 16, 1)), floatBits(40.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 20, 2)), floatBits(6.0f)); // 3.0 / 0.5
}

void test_vdot_and_vscl(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 1.0f, 2.0f, 3.0f, 4.0f);
    pokeVfpuQuad(cpu, 4, 1.0f, 1.0f, 1.0f, 1.0f);
    loadProgram(mem, 0, {
        VDOT(8, 0, 4, 4),   // reg 8 lane0 = 1+2+3+4 = 10
        VSCL(12, 0, 4, 4),  // vt's lane (reg 4, lane 0 since vt&0x60==0 -> tlane=0) broadcast: vd = vs * vt[0](=1.0) elementwise
    });
    cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(10.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 2)), floatBits(3.0f));
}

void test_vmov_vabs_vneg(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, -1.0f, 2.0f, -3.0f, 4.0f);
    loadProgram(mem, 0, {
        VMOV(4, 0, 4),
        VABS(8, 0, 4),
        VNEG(12, 0, 4),
    });
    cpu.step(); cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 0)), floatBits(-1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 2)), floatBits(3.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 0)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 1)), floatBits(-2.0f));
}

void test_vzero_vone_vidt(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 9.0f, 9.0f, 9.0f, 9.0f); // reg 0: will become identity row 0 (vidt uses vd's own column offset)
    loadProgram(mem, 0, {
        VZERO(4, 4),
        VONE(8, 4),
        VIDT(0, 4), // vd = reg 0 (col 0) -> identity row: [1,0,0,0]
    });
    cpu.step(); cpu.step(); cpu.step();
    for (int i = 0; i < 4; ++i) CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, i)), floatBits(0.0f));
    for (int i = 0; i < 4; ++i) CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, i)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 0, 0)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 0, 1)), floatBits(0.0f));
}

void test_vsat0_vsat1(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, -2.0f, 0.5f, 2.0f, -0.25f);
    loadProgram(mem, 0, {
        VSAT0(4, 0, 4),
        VSAT1(8, 0, 4),
    });
    cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 0)), floatBits(0.0f));   // vsat0: clamp [0,1]
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 1)), floatBits(0.5f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 2)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(-1.0f));  // vsat1: clamp [-1,1]
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 3)), floatBits(-0.25f));
}

void test_vmin_vmax_vcmp_vcmov(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 1.0f, 5.0f, 3.0f, 2.0f);
    pokeVfpuQuad(cpu, 4, 4.0f, 2.0f, 3.0f, 9.0f);
    loadProgram(mem, 0, {
        VMIN(8, 0, 4, 4),
        VMAX(12, 0, 4, 4),
        VCMP(6 /*VC_GE*/, 0, 4, 4),               // CC bit i = (s[i] >= t[i]): [0,1,1,0]
        VCMOV(16, 0, 6 /*per-element: compare against the matching CC bit*/, true, 4),
    });
    pokeVfpuQuad(cpu, 16, 100.0f, 100.0f, 100.0f, 100.0f); // vd for vcmov, read as "T" (destination-as-source)
    cpu.step(); cpu.step(); cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 1)), floatBits(2.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 0)), floatBits(4.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 12, 3)), floatBits(9.0f));
    CHECK_EQ(stats, cpu.state().vfpuCtrl[3] & 0xFu, 0x6u); // CC bits 0-3 = 0b0110 (lanes 1,2 true)
    // vcmov's real condition (ported as-is in cpu.cpp): copies vs into vd when (CCbit == !tf), i.e. with
    // tf=true, lanes where CC is CLEAR get copied from vs; lanes where CC is SET keep vd's original value.
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 16, 0)), floatBits(1.0f));   // lane0: CC clear -> copied from vs (1.0)
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 16, 1)), floatBits(100.0f)); // lane1: CC set -> kept (100.0)
}

void test_vf2i_vi2f_roundtrip(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 1.5f, -1.5f, 2.25f, -2.25f);
    loadProgram(mem, 0, {
        VF2IN(4, 0, 0 /*scale 2^0*/, 4), // round-to-nearest, no scale
        VI2F(8, 4, 0 /*scale 2^0*/, 4),
    });
    cpu.step(); cpu.step();
    // 1.5 rounds to 2 (round-half-away via lround), -1.5 -> -2, 2.25 -> 2, -2.25 -> -2
    CHECK_EQ(stats, cpu.state().vfpu[vfpuFlatIdx(4, 0)], 2u);
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(2.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 2)), floatBits(2.0f));
}

void test_vpfxs_swizzle_and_negate(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 1.0f, 2.0f, 3.0f, 4.0f);
    // S prefix: [Z, Y, X, W] with X negated -> swizzle regnum per lane (2,1,0,3), negate bit0 set.
    const uint32_t data = swizzleBitsForTest(2, 1, 0, 3) | (1u << 16);
    loadProgram(mem, 0, {
        VPFXS(data),
        VMOV(4, 0, 4),
        VMOV(8, 0, 4), // the prefix must NOT survive past the instruction right after vpfxs (EatPrefixes)
    });
    cpu.step(); cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 0)), floatBits(-3.0f)); // Z, negated
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 1)), floatBits(2.0f));  // Y
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 2)), floatBits(1.0f));  // X
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 3)), floatBits(4.0f));  // W
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 0)), floatBits(1.0f));  // unswizzled: passes through as X
}

void test_vpfxd_mask_and_saturate(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 5.0f, 5.0f, 5.0f, 5.0f);
    pokeVfpuQuad(cpu, 4, 1.0f, 2.0f, 3.0f, 4.0f); // pre-existing vd contents; lane0 should survive the write mask
    // D prefix: lane0 masked (bit8), lane1 sat[0,1] (bits2-3 = 01).
    const uint32_t data = (1u << 8) | (1u << 2);
    loadProgram(mem, 0, {
        VPFXD(data),
        VMOV(4, 0, 4),
    });
    cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 0)), floatBits(1.0f)); // masked: untouched
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 1)), floatBits(1.0f)); // saturated to [0,1]: 5.0 -> 1.0
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 2)), floatBits(5.0f)); // no sat requested: passes through
}

void test_vmmul_identity(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    // reg 0 (mtx0): a 4x4 matrix with rows [1,2,3,4],[5,6,7,8],[9,10,11,12],[13,14,15,16] (column-major storage,
    // i.e. vfpu[0..3]=col0, vfpu[4..7]=col1, ... -- pokeVfpuQuad on reg 0/1/2/3 sets one column each).
    pokeVfpuQuad(cpu, 0, 1, 5, 9, 13);
    pokeVfpuQuad(cpu, 1, 2, 6, 10, 14);
    pokeVfpuQuad(cpu, 2, 3, 7, 11, 15);
    pokeVfpuQuad(cpu, 3, 4, 8, 12, 16);
    // reg 8 (mtx2, distinct from mtx0's source and mtx4's destination below): identity.
    pokeVfpuQuad(cpu, 8, 1, 0, 0, 0);
    pokeVfpuQuad(cpu, 9, 0, 1, 0, 0);
    pokeVfpuQuad(cpu, 10, 0, 0, 1, 0);
    pokeVfpuQuad(cpu, 11, 0, 0, 0, 1);
    // Real vmmul computes d[a*4+b] = sum_c vs[b*4+c] * vt[a*4+c] -- the identity has to be the FIRST operand
    // (vs) for the result to pass the second (vt) through unchanged; the other way around transposes it.
    loadProgram(mem, 0, { VMMUL(16, 8, 0, 4) }); // reg 16 (mtx4) = identity * mtx0 = mtx0
    cpu.step();
    for (uint32_t col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK_EQ(stats, cpu.state().vfpu[4 * 16 + col * 4 + row], cpu.state().vfpu[0 * 16 + col * 4 + row]);
}

void test_vmidt_vmzero_vmone(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    loadProgram(mem, 0, {
        VMIDT(0, 4),  // reg 0 -> mtx0 (flat base 0)
        VMZERO(4, 4), // reg 4 -> mtx1 (flat base 16)
        VMONE(8, 4),  // reg 8 -> mtx2 (flat base 32)
    });
    cpu.step(); cpu.step(); cpu.step();
    CHECK_EQ(stats, cpu.state().vfpu[0], floatBits(1.0f));  // mtx0 col0 row0
    CHECK_EQ(stats, cpu.state().vfpu[1], floatBits(0.0f));  // mtx0 col0 row1
    CHECK_EQ(stats, cpu.state().vfpu[4 + 1], floatBits(1.0f)); // mtx0 col1 row1
    for (int i = 0; i < 16; ++i) CHECK_EQ(stats, cpu.state().vfpu[16 + i], floatBits(0.0f)); // mtx1 all zero
    for (int i = 0; i < 16; ++i) CHECK_EQ(stats, cpu.state().vfpu[32 + i], floatBits(1.0f)); // mtx2 all one
}

void test_vocp_vsgn(TestStats& stats) {
    Memory mem(4096);
    Cpu cpu(mem);
    cpu.reset(0);
    pokeVfpuQuad(cpu, 0, 0.25f, 1.0f, -3.0f, 0.0f);
    loadProgram(mem, 0, {
        VOCP(4, 0, 4),  // 1 - s
        VSGN(8, 0, 4),  // sign(s)
    });
    cpu.step(); cpu.step();
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 0)), floatBits(0.75f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 4, 2)), floatBits(4.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 1)), floatBits(1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 2)), floatBits(-1.0f));
    CHECK_EQ(stats, floatBits(readVfpuLane(cpu, 8, 3)), floatBits(0.0f));
}

} // namespace

void runAllVfpuTests(TestStats& stats) {
    runTest(stats, "vfpu_vadd_vsub_vmul_vdiv_quad", test_vadd_vsub_vmul_vdiv_quad);
    runTest(stats, "vfpu_vdot_and_vscl", test_vdot_and_vscl);
    runTest(stats, "vfpu_vmov_vabs_vneg", test_vmov_vabs_vneg);
    runTest(stats, "vfpu_vzero_vone_vidt", test_vzero_vone_vidt);
    runTest(stats, "vfpu_vsat0_vsat1", test_vsat0_vsat1);
    runTest(stats, "vfpu_vmin_vmax_vcmp_vcmov", test_vmin_vmax_vcmp_vcmov);
    runTest(stats, "vfpu_vf2i_vi2f_roundtrip", test_vf2i_vi2f_roundtrip);
    runTest(stats, "vfpu_vpfxs_swizzle_and_negate", test_vpfxs_swizzle_and_negate);
    runTest(stats, "vfpu_vpfxd_mask_and_saturate", test_vpfxd_mask_and_saturate);
    runTest(stats, "vfpu_vmmul_identity", test_vmmul_identity);
    runTest(stats, "vfpu_vmidt_vmzero_vmone", test_vmidt_vmzero_vmone);
    runTest(stats, "vfpu_vocp_vsgn", test_vocp_vsgn);
}
