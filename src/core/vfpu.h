#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "cpu_state.h"

// Allegrex VFPU (the PSP's vector floating-point unit): register addressing, prefix application
// (per-source swizzle/abs/negate/constant, per-destination mask/saturate), and the non-transcendental
// arithmetic/data-movement instructions. This is ported from PPSSPP's Core/MIPS/MIPSVFPUUtils.cpp and
// InterpreterVFPU.cpp (GPL-2.0, a long-established, widely-validated open-source reimplementation of the
// real Allegrex ISA) -- we have never seen Sony's own source for this unit, and this project's "LLE first"
// rule doesn't apply to CPU-instruction *semantics* the way it applies to kernel/firmware code: there is no
// uofw/jpcsp equivalent to check first here, since VFPU is pure processor behavior, not a kernel API.
//
// The flat 128-word register-file index this project already uses elsewhere (lv.s/sv.s/lv.q/sv.q/mfv/mtv
// in cpu.cpp; CpuState::vfpu) is mtx*16 + col*4 + row. That turns out to be EXACTLY what PPSSPP's own real
// data path (ReadVector/WriteVector/ReadMatrix/WriteMatrix in MIPSVFPUUtils.cpp) uses internally too, once
// you read through their "mtx" local variable (already pre-multiplied by 16 via `(reg<<2)&0x70`) -- so no
// index translation was needed, only their separate GetVectorRegs()/GetMatrixRegs() helpers (used by their
// JIT register allocator, irrelevant here) use a different public numbering.
//
// NOT implemented yet: the transcendental ops (vsin/vcos/vrcp/vrsq/vsqrt/vasin/vexp2/vlog2/vnrcp/vnsin/
// vrexp2) and the rarer data-shuffle/pack ops (vbfy*/vsrt*/vcrs/vdet/vhdp/vocp's sibling vsocp/vcst/vwbn/
// vrot/vrnd*/vh2f/vf2h/vmscl/vcrsp/vqmul). Real hardware's transcendentals need ~4.8MB of lookup tables to
// match bit-for-bit (see PPSSPP's assets/vfpu/*.dat) -- a separate, deliberate follow-up. All of these still
// throw cleanly via Cpu::unimplemented(), same as any other real interpreter gap.
namespace beta::vfpu {

enum VSize : int { V_Single = 1, V_Pair = 2, V_Triple = 3, V_Quad = 4 };
enum MSize : int { M_1x1 = 1, M_2x2 = 2, M_3x3 = 3, M_4x4 = 4 };

inline VSize vecSizeOf(uint32_t instr) { const int a = static_cast<int>(instr >> 7) & 1, b = static_cast<int>(instr >> 14) & 2; return static_cast<VSize>(a + b + 1); }
inline MSize mtxSizeOf(uint32_t instr) { const int a = static_cast<int>(instr >> 7) & 1, b = static_cast<int>(instr >> 14) & 2; return static_cast<MSize>(a + b + 1); }
inline int numElem(VSize sz) { return static_cast<int>(sz); }
inline int mtxSide(MSize sz) { return static_cast<int>(sz); }

// The three 7-bit register-code fields of an R-type VFPU instruction (distinct from lv.s/sv.s's I-type
// encoding, which has to steal 2 bits from the immediate field instead -- see cpu.cpp's existing lv.s/sv.s).
inline int vdOf(uint32_t instr) { return static_cast<int>(instr & 0x7Fu); }
inline int vsOf(uint32_t instr) { return static_cast<int>((instr >> 8) & 0x7Fu); }
inline int vtOf(uint32_t instr) { return static_cast<int>((instr >> 16) & 0x7Fu); }

// One vector register's up-to-4 flat array indices.
inline void vecIdx(uint8_t out[4], VSize sz, int reg) {
    const int mtx = (reg >> 2) & 7, col = reg & 3;
    bool transpose = ((reg >> 5) & 1) != 0;
    int row = 0;
    switch (sz) {
        case V_Single: transpose = false; row = (reg >> 5) & 3; break;
        case V_Pair:   row = (reg >> 5) & 2; break;
        case V_Triple: row = (reg >> 6) & 1; break;
        case V_Quad:   row = (reg >> 5) & 2; break;
    }
    const int n = numElem(sz);
    const int base = transpose ? (mtx * 16 + col) : (mtx * 16 + col * 4);
    for (int i = 0; i < n; ++i)
        out[i] = static_cast<uint8_t>(base + (transpose ? (((row + i) & 3) * 4) : ((row + i) & 3)));
}

// One matrix register's up-to-16 flat array indices; out[j*4+i] is column-offset j, row-offset i within
// the side*side block (matches PPSSPP's own [j][i] loop nesting in ReadMatrix/WriteMatrix).
inline void mtxIdx(uint8_t out[16], MSize sz, int reg) {
    const int mtx = (reg >> 2) & 7, col = reg & 3;
    bool transpose = ((reg >> 5) & 1) != 0;
    int row = 0, side = 0;
    switch (sz) {
        case M_1x1: transpose = false; row = (reg >> 5) & 3; side = 1; break;
        case M_2x2: row = (reg >> 5) & 2; side = 2; break;
        case M_3x3: row = (reg >> 6) & 1; side = 3; break;
        case M_4x4: row = (reg >> 5) & 2; side = 4; break;
    }
    const int base = mtx * 16;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i)
            out[j * 4 + i] = static_cast<uint8_t>(base + (transpose ? (((row + i) & 3) + ((col + j) & 3) * 4)
                                                                     : (((col + j) & 3) * 4 + ((row + i) & 3))));
}

// VFPU control registers (CpuState::vfpuCtrl index), matching the imm&0xF convention cpu.cpp's mtv/mfv
// (COP2 rs=3/7) already uses.
enum Ctrl : int { kSPrefix = 0, kTPrefix = 1, kDPrefix = 2, kCC = 3 };

inline void readVec(const CpuState& st, float out[4], VSize sz, int reg) {
    uint8_t idx[4]; vecIdx(idx, sz, reg);
    for (int i = 0; i < numElem(sz); ++i) { const uint32_t u = st.vfpu[idx[i]]; std::memcpy(&out[i], &u, 4); }
}
inline void writeVec(CpuState& st, const float in[4], VSize sz, int reg) {
    uint8_t idx[4]; vecIdx(idx, sz, reg);
    const uint32_t dmask = (st.vfpuCtrl[kDPrefix] >> 8) & 0xFu;
    for (int i = 0; i < numElem(sz); ++i) {
        if ((dmask >> i) & 1u) continue;  // masked lane: destination keeps its old value
        uint32_t u; std::memcpy(&u, &in[i], 4); st.vfpu[idx[i]] = u;
    }
}
inline void readMtx(const CpuState& st, float out[16], MSize sz, int reg) {
    uint8_t idx[16]; mtxIdx(idx, sz, reg);
    const int side = mtxSide(sz);
    for (int j = 0; j < side; ++j) for (int i = 0; i < side; ++i) { const uint32_t u = st.vfpu[idx[j * 4 + i]]; std::memcpy(&out[j * 4 + i], &u, 4); }
}
inline void writeMtx(CpuState& st, const float in[16], MSize sz, int reg) {
    uint8_t idx[16]; mtxIdx(idx, sz, reg);
    const int side = mtxSide(sz);
    // Real hardware: the write mask, if set at all, only applies to the LAST column; every other column
    // writes unconditionally (confirmed via PPSSPP's WriteMatrix: `j != side-1 || !VfpuWriteMask(i)`).
    const uint32_t dmask = (st.vfpuCtrl[kDPrefix] >> 8) & 0xFu;
    for (int j = 0; j < side; ++j)
        for (int i = 0; i < side; ++i) {
            if (j == side - 1 && ((dmask >> i) & 1u)) continue;
            uint32_t u; std::memcpy(&u, &in[j * 4 + i], 4); st.vfpu[idx[j * 4 + i]] = u;
        }
}

inline float vclamp(float v, float lo, float hi) {
    // NaN is preserved (both comparisons are false for NaN, so it falls through to the last branch).
    return v >= hi ? hi : (v <= lo ? lo : v);
}
inline bool isnanf(float f) { return f != f; }
inline bool isinff(float f) { return std::isinf(f); }

// Applies an S or T source prefix (raw 20-bit control-register value) to an n-element vector, in place.
// invalidFill is what an out-of-range (non-constant) swizzle lane reads as -- 0 normally, but several real
// instructions force a different sentinel (checked via RetainInvalidSwizzleST-equivalent logic at the call
// site) to detect that case afterward.
inline void applyPrefixST(float v[4], uint32_t data, int n, float invalidFill = 0.0f) {
    if (data == 0xE4u) return;  // identity (xyzw, no abs/negate/const): "no prefix", the overwhelmingly common case
    static const float kConst[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};
    float orig[4] = {invalidFill, invalidFill, invalidFill, invalidFill};
    for (int i = 0; i < n; ++i) orig[i] = v[i];
    for (int i = 0; i < n; ++i) {
        const int regnum = static_cast<int>(data >> (i * 2)) & 3;
        const bool absBit = ((data >> (8 + i)) & 1u) != 0;
        const bool negBit = ((data >> (16 + i)) & 1u) != 0;
        const bool isConst = ((data >> (12 + i)) & 1u) != 0;
        float r;
        if (isConst) {
            r = kConst[regnum + (absBit ? 4 : 0)];
        } else {
            r = orig[regnum];
            if (absBit) { uint32_t u; std::memcpy(&u, &r, 4); u &= 0x7FFFFFFFu; std::memcpy(&r, &u, 4); }
        }
        if (negBit) { uint32_t u; std::memcpy(&u, &r, 4); u ^= 0x80000000u; std::memcpy(&r, &u, 4); }
        v[i] = r;
    }
}
// Applies the destination prefix's per-lane saturate (mask is handled separately, by writeVec/writeMtx
// themselves, since it has to skip the actual register write, not just clamp a local buffer).
inline void applyPrefixD(float v[4], uint32_t dprefix, int n, bool onlyMask = false) {
    if (!dprefix || onlyMask) return;
    for (int i = 0; i < n; ++i) {
        const int sat = static_cast<int>(dprefix >> (i * 2)) & 3;
        if (sat == 1) v[i] = vclamp(v[i], 0.0f, 1.0f);
        else if (sat == 3) v[i] = vclamp(v[i], -1.0f, 1.0f);
    }
}
// A swizzle lane that reads past the real operand size (e.g. .z on a V_Pair) is a genuine hardware quirk:
// several instructions force that lane's RESULT to 0 afterward rather than using whatever invalidFill
// value leaked through the arithmetic. Mirrors PPSSPP's RetainInvalidSwizzleST.
inline void retainInvalidSwizzleST(float d[4], uint32_t sPrefix, uint32_t tPrefix, int n) {
    for (int i = 0; i < n; ++i) {
        const int swS = static_cast<int>(sPrefix >> (i * 2)) & 3, swT = static_cast<int>(tPrefix >> (i * 2)) & 3;
        const bool constS = ((sPrefix >> (12 + i)) & 1u) != 0, constT = ((tPrefix >> (12 + i)) & 1u) != 0;
        if ((swS >= n && !constS) || (swT >= n && !constT)) d[i] = 0.0f;
    }
}
inline uint32_t rewritePrefix(uint32_t prefix, uint32_t remove, uint32_t add) { return (prefix & ~remove) | add; }
inline uint32_t swizzleBits(int x, int y, int z, int w) { return static_cast<uint32_t>((x & 3) | ((y & 3) << 2) | ((z & 3) << 4) | ((w & 3) << 6)); }
inline uint32_t negateBits(int x, int y, int z, int w) { return static_cast<uint32_t>(((x & 1) | ((y & 1) << 1) | ((z & 1) << 2) | ((w & 1) << 3)) << 16); }
inline uint32_t absBits(int x, int y, int z, int w) { return static_cast<uint32_t>(((x & 1) | ((y & 1) << 1) | ((z & 1) << 2) | ((w & 1) << 3)) << 8); }
inline uint32_t anySwizzle() { return 0xFFu; }
// VFPUConst lane values: -1=leave alone (NONE), 0=0,1=1,2=2,3=0.5,4=3,5=1/3,6=0.25,7=1/6.
inline uint32_t makeConstants(int x, int y, int z, int w) {
    uint32_t r = 0; const int c[4] = {x, y, z, w};
    for (int i = 0; i < 4; ++i) if (c[i] >= 0) r |= static_cast<uint32_t>(((c[i] & 3) << (i * 2)) | ((c[i] & 4) << (6 + i)) | (1 << (12 + i)));
    return r;
}

} // namespace beta::vfpu
