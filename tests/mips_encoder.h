#pragma once

// Minimal MIPS-I instruction encoder used only by tests, to build known
// instruction sequences without hand-computing raw hex machine code.

#include <cstdint>

namespace enc {

constexpr uint32_t rtype(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t shamt = 0) {
    return (0u << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | ((rd & 0x1F) << 11) | ((shamt & 0x1F) << 6) | (funct & 0x3F);
}

constexpr uint32_t itype(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t imm) {
    return ((opcode & 0x3F) << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | imm;
}

constexpr uint32_t jtype(uint32_t opcode, uint32_t addr26) {
    return ((opcode & 0x3F) << 26) | (addr26 & 0x03FFFFFF);
}

// SPECIAL2 (opcode 0x1C) -- same R-type field layout as SPECIAL (rtype above), but under a different major opcode, so it needs its own
// helper rather than reusing rtype (which hardcodes opcode 0).
constexpr uint32_t special2(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t shamt = 0) {
    return (0x1Cu << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | ((rd & 0x1F) << 11) | ((shamt & 0x1F) << 6) | (funct & 0x3F);
}

// SPECIAL (R-type)
constexpr uint32_t SLL(uint32_t rd, uint32_t rt, uint32_t shamt) { return rtype(0x00, 0, rt, rd, shamt); }
constexpr uint32_t SRL(uint32_t rd, uint32_t rt, uint32_t shamt) { return rtype(0x02, 0, rt, rd, shamt); }
constexpr uint32_t SRA(uint32_t rd, uint32_t rt, uint32_t shamt) { return rtype(0x03, 0, rt, rd, shamt); }
// Register-controlled shifts: real assembly form is "SLLV rd, rt, rs" -- rt
// is still the value being shifted (same convention as SLL), rs holds the
// shift AMOUNT register. Confirmed from cpu.cpp's own real implementation
// (setReg(rd, rtVal << (rsVal & 0x1F)), etc.), not assumed from ISA docs.
constexpr uint32_t SLLV(uint32_t rd, uint32_t rt, uint32_t rs) { return rtype(0x04, rs, rt, rd); }
constexpr uint32_t SRLV(uint32_t rd, uint32_t rt, uint32_t rs) { return rtype(0x06, rs, rt, rd); }
constexpr uint32_t SRAV(uint32_t rd, uint32_t rt, uint32_t rs) { return rtype(0x07, rs, rt, rd); }
constexpr uint32_t JR(uint32_t rs) { return rtype(0x08, rs, 0, 0); }
constexpr uint32_t JALR(uint32_t rd, uint32_t rs) { return rtype(0x09, rs, 0, rd); }
constexpr uint32_t MFHI(uint32_t rd) { return rtype(0x10, 0, 0, rd); }
constexpr uint32_t MTHI(uint32_t rs) { return rtype(0x11, rs, 0, 0); }
constexpr uint32_t MFLO(uint32_t rd) { return rtype(0x12, 0, 0, rd); }
constexpr uint32_t MTLO(uint32_t rs) { return rtype(0x13, rs, 0, 0); }
// CLZ/CLO: rt is unused/must be 0 in real encoding (cpu.cpp only ever reads rs and rd for these).
constexpr uint32_t CLZ(uint32_t rd, uint32_t rs) { return rtype(0x16, rs, 0, rd); }
constexpr uint32_t CLO(uint32_t rd, uint32_t rs) { return rtype(0x17, rs, 0, rd); }
constexpr uint32_t MULT(uint32_t rs, uint32_t rt) { return rtype(0x18, rs, rt, 0); }
constexpr uint32_t MULTU(uint32_t rs, uint32_t rt) { return rtype(0x19, rs, rt, 0); }
constexpr uint32_t DIV(uint32_t rs, uint32_t rt) { return rtype(0x1A, rs, rt, 0); }
constexpr uint32_t DIVU(uint32_t rs, uint32_t rt) { return rtype(0x1B, rs, rt, 0); }
// MADD/MADDU/MSUB/MSUBU: rd is unused/must be 0 in real encoding (accumulate straight into hi:lo, no gpr destination).
constexpr uint32_t MADD(uint32_t rs, uint32_t rt) { return rtype(0x1C, rs, rt, 0); }
constexpr uint32_t MADDU(uint32_t rs, uint32_t rt) { return rtype(0x1D, rs, rt, 0); }
constexpr uint32_t ADD(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x20, rs, rt, rd); }
constexpr uint32_t ADDU(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x21, rs, rt, rd); }
constexpr uint32_t SUB(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x22, rs, rt, rd); }
constexpr uint32_t SUBU(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x23, rs, rt, rd); }
constexpr uint32_t AND(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x24, rs, rt, rd); }
constexpr uint32_t OR(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x25, rs, rt, rd); }
constexpr uint32_t XOR(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x26, rs, rt, rd); }
constexpr uint32_t NOR(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x27, rs, rt, rd); }
constexpr uint32_t SLT(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x2A, rs, rt, rd); }
constexpr uint32_t SLTU(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x2B, rs, rt, rd); }
constexpr uint32_t MOVZ(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x0A, rs, rt, rd); }
constexpr uint32_t MOVN(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x0B, rs, rt, rd); }
constexpr uint32_t SYNC() { return rtype(0x0F, 0, 0, 0); } // real form has an unused stype field; cpu.cpp ignores every field but funct
// SPECIAL2 (opcode 0x1C): halt/mfic/mtic. cpu.cpp only ever reads rt for mfic (dest)/mtic (source); rd/shamt are unused/ignored.
constexpr uint32_t HALT() { return special2(0x00, 0, 0, 0); }
constexpr uint32_t MFIC(uint32_t rt) { return special2(0x24, 0, rt, 0); }
constexpr uint32_t MTIC(uint32_t rt) { return special2(0x26, 0, rt, 0); }
// CACHE: real form is itype(op, base, cacheop, offset); cpu.cpp's implementation ignores every field (unconditional no-op).
constexpr uint32_t CACHE(uint32_t rs = 0, uint32_t rt = 0, uint16_t imm = 0) { return itype(0x2F, rs, rt, imm); }

// SPECIAL3 (opcode 0x1F): EXT/INS. Same R-type field layout as SPECIAL/SPECIAL2, different major opcode.
constexpr uint32_t special3(uint32_t funct, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t shamt = 0) {
    return (0x1Fu << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | ((rd & 0x1F) << 11) | ((shamt & 0x1F) << 6) | (funct & 0x3F);
}
// Real assembly form EXT rt,rs,pos,size; cpu.cpp's own rd field holds size-1, shamt holds pos.
constexpr uint32_t EXT(uint32_t rt, uint32_t rs, uint32_t pos, uint32_t size) { return special3(0x00, rs, rt, size - 1, pos); }
// Real assembly form INS rt,rs,pos,size; cpu.cpp's own rd field holds msb (pos+size-1) directly, shamt holds pos.
constexpr uint32_t INS(uint32_t rt, uint32_t rs, uint32_t pos, uint32_t msb) { return special3(0x04, rs, rt, msb, pos); }
// Allegrex bit-manipulation extensions ("ALLEGREX0" in PPSSPP's own tables): SPECIAL3 funct 0x20 (cpu.cpp also accepts 0x18, but 0x20 is
// the standard real encoding), sub-op selected by the shamt field. All five read rt, write rd; rs is unused/zero in every real case.
constexpr uint32_t WSBH(uint32_t rd, uint32_t rt) { return special3(0x20, 0, rt, rd, 0x02); }
constexpr uint32_t WSBW(uint32_t rd, uint32_t rt) { return special3(0x20, 0, rt, rd, 0x03); }
constexpr uint32_t SEB(uint32_t rd, uint32_t rt) { return special3(0x20, 0, rt, rd, 0x10); }
constexpr uint32_t BITREV(uint32_t rd, uint32_t rt) { return special3(0x20, 0, rt, rd, 0x14); }
constexpr uint32_t SEH(uint32_t rd, uint32_t rt) { return special3(0x20, 0, rt, rd, 0x18); }
// SYSCALL: the 20-bit code sits in bits 25-6, NOT the ordinary rs/rt/rd/shamt fields at all (confirmed from cpu.cpp:
// `(instr >> 6) & 0xFFFFF`), so this doesn't go through rtype() the way every other funct-0x0C-adjacent op does.
constexpr uint32_t SYSCALL(uint32_t code) { return ((code & 0xFFFFFu) << 6) | 0x0Cu; }
// MAX/MIN: Allegrex-specific, reusing MIPS64 DADD/DADDU's real funct slots (confirmed from cpu.cpp's own comment).
constexpr uint32_t MAX(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x2C, rs, rt, rd); }
constexpr uint32_t MIN(uint32_t rd, uint32_t rs, uint32_t rt) { return rtype(0x2D, rs, rt, rd); }
constexpr uint32_t MSUB(uint32_t rs, uint32_t rt) { return rtype(0x2E, rs, rt, 0); }
constexpr uint32_t MSUBU(uint32_t rs, uint32_t rt) { return rtype(0x2F, rs, rt, 0); }
constexpr uint32_t NOP() { return SLL(0, 0, 0); }

// I-type / branches
constexpr uint32_t BEQ(uint32_t rs, uint32_t rt, int16_t offsetWords) { return itype(0x04, rs, rt, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BNE(uint32_t rs, uint32_t rt, int16_t offsetWords) { return itype(0x05, rs, rt, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BLEZ(uint32_t rs, int16_t offsetWords) { return itype(0x06, rs, 0, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGTZ(uint32_t rs, int16_t offsetWords) { return itype(0x07, rs, 0, static_cast<uint16_t>(offsetWords)); }
// REGIMM (opcode 0x01): rt selects the sub-op (0x00 BLTZ / 0x01 BGEZ) rather than naming a real second register.
constexpr uint32_t BLTZ(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x00, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGEZ(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x01, static_cast<uint16_t>(offsetWords)); }
// "Likely" variants: a not-taken one nullifies its own delay slot entirely (real semantics in x64_jit.h's own comment).
constexpr uint32_t BLTZL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x02, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGEZL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x03, static_cast<uint16_t>(offsetWords)); }
// "-AL" link-and-branch variants: write $ra = nextPc+4 UNCONDITIONALLY (real quirk: even when the branch itself is NOT taken --
// confirmed in x64_jit.h's own comment), on top of BLTZ/BGEZ's (or BLTZL/BGEZL's) own comparison.
constexpr uint32_t BLTZAL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x10, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGEZAL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x11, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BLTZALL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x12, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGEZALL(uint32_t rs, int16_t offsetWords) { return itype(0x01, rs, 0x13, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BEQL(uint32_t rs, uint32_t rt, int16_t offsetWords) { return itype(0x14, rs, rt, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BNEL(uint32_t rs, uint32_t rt, int16_t offsetWords) { return itype(0x15, rs, rt, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BLEZL(uint32_t rs, int16_t offsetWords) { return itype(0x16, rs, 0, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BGTZL(uint32_t rs, int16_t offsetWords) { return itype(0x17, rs, 0, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t ADDI(uint32_t rt, uint32_t rs, int16_t imm) { return itype(0x08, rs, rt, static_cast<uint16_t>(imm)); }
constexpr uint32_t ADDIU(uint32_t rt, uint32_t rs, int16_t imm) { return itype(0x09, rs, rt, static_cast<uint16_t>(imm)); }
constexpr uint32_t SLTI(uint32_t rt, uint32_t rs, int16_t imm) { return itype(0x0A, rs, rt, static_cast<uint16_t>(imm)); }
constexpr uint32_t SLTIU(uint32_t rt, uint32_t rs, int16_t imm) { return itype(0x0B, rs, rt, static_cast<uint16_t>(imm)); }
constexpr uint32_t ANDI(uint32_t rt, uint32_t rs, uint16_t imm) { return itype(0x0C, rs, rt, imm); }
constexpr uint32_t ORI(uint32_t rt, uint32_t rs, uint16_t imm) { return itype(0x0D, rs, rt, imm); }
constexpr uint32_t XORI(uint32_t rt, uint32_t rs, uint16_t imm) { return itype(0x0E, rs, rt, imm); }
constexpr uint32_t LUI(uint32_t rt, uint16_t imm) { return itype(0x0F, 0, rt, imm); }
constexpr uint32_t LB(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x20, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t LH(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x21, rs, rt, static_cast<uint16_t>(offset)); }
// Unaligned load/store, left (high-order, LWL/SWL) and right (low-order, LWR/SWR) parts -- real formulas in x64_jit.h's own comment.
constexpr uint32_t LWL(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x22, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t LW(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x23, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t LBU(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x24, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t LHU(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x25, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t LWR(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x26, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t SB(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x28, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t SH(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x29, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t SWL(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x2A, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t SW(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x2B, rs, rt, static_cast<uint16_t>(offset)); }
// LL/SC: real semantics are load-linked/store-conditional, but on this single-core interpreter (interrupts only delivered between
// instructions, never mid-instruction) a link is never broken, so LL is bit-for-bit identical to LW, and SC always succeeds -- see
// cpu.cpp's own comment.
constexpr uint32_t LL(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x30, rs, rt, static_cast<uint16_t>(offset)); }
constexpr uint32_t SC(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x38, rs, rt, static_cast<uint16_t>(offset)); }
// lv.s/sv.s/lv.q/sv.q: the real VFPU register number (vt) is split across the ordinary rt field and the low bits of what would otherwise
// be the offset field -- confirmed from cpu.cpp's own decode (see x64_jit.h's matching comment). offset itself is always word-aligned
// (low 2 bits cleared) since those bits are repurposed here.
constexpr uint32_t LV_S(uint32_t vt, uint32_t rs, int16_t offset) {
    return (0x32u << 26) | ((rs & 0x1Fu) << 21) | ((vt & 0x1Fu) << 16) | (static_cast<uint16_t>(offset) & 0xFFFCu) | ((vt >> 5) & 3u);
}
constexpr uint32_t SV_S(uint32_t vt, uint32_t rs, int16_t offset) {
    return (0x3Au << 26) | ((rs & 0x1Fu) << 21) | ((vt & 0x1Fu) << 16) | (static_cast<uint16_t>(offset) & 0xFFFCu) | ((vt >> 5) & 3u);
}
// lv.q/sv.q steal only bit0 of the offset field for vt's own top bit; sv.q additionally uses bit1 for optional base writeback.
constexpr uint32_t LV_Q(uint32_t vt, uint32_t rs, int16_t offset) {
    return (0x36u << 26) | ((rs & 0x1Fu) << 21) | ((vt & 0x1Fu) << 16) | (static_cast<uint16_t>(offset) & 0xFFFCu) | ((vt >> 5) & 1u);
}
constexpr uint32_t SV_Q(uint32_t vt, uint32_t rs, int16_t offset, bool writeback = false) {
    return (0x3Eu << 26) | ((rs & 0x1Fu) << 21) | ((vt & 0x1Fu) << 16) | (static_cast<uint16_t>(offset) & 0xFFFCu) | ((vt >> 5) & 1u) | (writeback ? 2u : 0u);
}
constexpr uint32_t SWR(uint32_t rt, uint32_t rs, int16_t offset) { return itype(0x2E, rs, rt, static_cast<uint16_t>(offset)); }
// LWC1/SWC1: raw 32-bit load/store into/from an FPU register (fpr[ft].u -- MFC1/MTC1's own move-raw-bits convention).
constexpr uint32_t LWC1(uint32_t ft, uint32_t rs, int16_t offset) { return itype(0x31, rs, ft, static_cast<uint16_t>(offset)); }
constexpr uint32_t SWC1(uint32_t ft, uint32_t rs, int16_t offset) { return itype(0x39, rs, ft, static_cast<uint16_t>(offset)); }

// J-type. addr is an absolute byte address (must share the current 256MB
// region with the delay slot, per real MIPS semantics).
constexpr uint32_t J(uint32_t addr) { return jtype(0x02, addr >> 2); }
constexpr uint32_t JAL(uint32_t addr) { return jtype(0x03, addr >> 2); }

// COP0. rtype()'s own opcode field is hardcoded to SPECIAL (0), so COP0's
// real opcode (0x10) is ORed in separately here rather than going through
// rtype() for that part; rtype()'s "rs" parameter carries COP0's actual
// sub-op selector (0x00 MFC0 / 0x04 MTC0), not a register.
constexpr uint32_t MFC0(uint32_t rt, uint32_t cop0reg) { return (0x10u << 26) | rtype(0x00, 0x00, rt, cop0reg); }
constexpr uint32_t MTC0(uint32_t rt, uint32_t cop0reg) { return (0x10u << 26) | rtype(0x00, 0x04, rt, cop0reg); }
// CFC0/CTC0 address the SEPARATE cp0Ctrl register file (rs=0x02/0x06), not cp0State -- confirmed in cpu.h's own real comment about the
// two genuinely distinct COP0 register files at the same numeric indices.
constexpr uint32_t CFC0(uint32_t rt, uint32_t cop0reg) { return (0x10u << 26) | rtype(0x00, 0x02, rt, cop0reg); }
constexpr uint32_t CTC0(uint32_t rt, uint32_t cop0reg) { return (0x10u << 26) | rtype(0x00, 0x06, rt, cop0reg); }
constexpr uint32_t ERET() { return (0x10u << 26) | (0x10u << 21) | 0x18u; }

// COP1 "S" (single-precision) format. Real opcode is 0x11, ORed in the same way COP0's 0x10 is above. rtype()'s own "rs" parameter
// carries the S-format selector (0x10, fixed); its real field-position quirk (confirmed in x64_jit.h's own comment): FT sits where
// rtype()'s "rt" parameter goes, FS where "rd" goes, and FD where "shamt" goes -- NOT the same rd/rt/shamt meaning as an integer R-type.
constexpr uint32_t ADD_S(uint32_t fd, uint32_t fs, uint32_t ft) { return (0x11u << 26) | rtype(0x00, 0x10, ft, fs, fd); }
constexpr uint32_t SUB_S(uint32_t fd, uint32_t fs, uint32_t ft) { return (0x11u << 26) | rtype(0x01, 0x10, ft, fs, fd); }
constexpr uint32_t MUL_S(uint32_t fd, uint32_t fs, uint32_t ft) { return (0x11u << 26) | rtype(0x02, 0x10, ft, fs, fd); }
constexpr uint32_t DIV_S(uint32_t fd, uint32_t fs, uint32_t ft) { return (0x11u << 26) | rtype(0x03, 0x10, ft, fs, fd); }
constexpr uint32_t SQRT_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x04, 0x10, 0, fs, fd); }
constexpr uint32_t ABS_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x05, 0x10, 0, fs, fd); }
constexpr uint32_t MOV_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x06, 0x10, 0, fs, fd); }
constexpr uint32_t NEG_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x07, 0x10, 0, fs, fd); }

// MFC1/CFC1/MTC1/CTC1: the raw-bits gpr<->FPU bridge. rtype()'s own "rs" parameter here carries the sub-op selector (0x00/0x02/0x04/0x06,
// same convention as MFC0/MTC0's 0x00/0x04), NOT a register; "rd" carries FS (or the FCR selector for CFC1/CTC1), per the same COP1
// field-position quirk as the S-format ops above.
constexpr uint32_t MFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | rtype(0x00, 0x00, rt, fs); }
constexpr uint32_t CFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | rtype(0x00, 0x02, rt, fs); }
constexpr uint32_t MTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | rtype(0x00, 0x04, rt, fs); }
constexpr uint32_t CTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | rtype(0x00, 0x06, rt, fs); }

// Float<->int conversions. ROUND/TRUNC/CEIL/FLOOR/CVT.W.S are still "S"-format (rs=0x10), same FS/FD field mapping as ADD_S etc above,
// rt unused (no second float operand). CVT.S.W is real MIPS "W" format instead (rs=0x14) -- a genuinely different sub-opcode, not just a
// different funct within "S".
constexpr uint32_t ROUND_W_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x0C, 0x10, 0, fs, fd); }
constexpr uint32_t TRUNC_W_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x0D, 0x10, 0, fs, fd); }
constexpr uint32_t CEIL_W_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x0E, 0x10, 0, fs, fd); }
constexpr uint32_t FLOOR_W_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x0F, 0x10, 0, fs, fd); }
constexpr uint32_t CVT_W_S(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x20, 0x10, 0, fs, fd); }
constexpr uint32_t CVT_S_W(uint32_t fd, uint32_t fs) { return (0x11u << 26) | rtype(0x20, 0x14, 0, fs, fd); }

// C.cond.S: still "S"-format, funct = 0x30 | cond (the low 4 bits select the real condition, confirmed in x64_jit.h's own comment).
// Result goes to FCR31, not fpr[], so the shamt/FD field is unused (0).
constexpr uint32_t C_COND_S(uint32_t cond, uint32_t fs, uint32_t ft) { return (0x11u << 26) | rtype(0x30 | (cond & 0xFu), 0x10, ft, fs, 0); }

// BC1F/BC1T/BC1FL/BC1TL: real opcode 0x11 (COP1) with rs=0x08 (the BC1 selector, confirmed from cpu.cpp), rt selects F/T/FL/TL.
constexpr uint32_t BC1F(int16_t offsetWords) { return itype(0x11, 0x08, 0x00, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BC1T(int16_t offsetWords) { return itype(0x11, 0x08, 0x01, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BC1FL(int16_t offsetWords) { return itype(0x11, 0x08, 0x02, static_cast<uint16_t>(offsetWords)); }
constexpr uint32_t BC1TL(int16_t offsetWords) { return itype(0x11, 0x08, 0x03, static_cast<uint16_t>(offsetWords)); }

// COP2 (Allegrex VFPU), the minimal real subset cpu.cpp itself implements: MFV/MFVC (rs=3) and MTV/MTVC (rs=7) move raw bits between a
// gpr and one VFPU register/control-register slot, selected by `imm` -- the LOW 8 BITS of the whole instruction word directly, not a
// normal rd/shamt/funct decomposition (confirmed from cpu.cpp's own `instr & 0xFFu` decode), so this doesn't go through rtype() at all.
constexpr uint32_t MFV(uint32_t rt, uint32_t imm) { return (0x12u << 26) | (0x03u << 21) | ((rt & 0x1Fu) << 16) | (imm & 0xFFu); }
constexpr uint32_t MTV(uint32_t rt, uint32_t imm) { return (0x12u << 26) | (0x07u << 21) | ((rt & 0x1Fu) << 16) | (imm & 0xFFu); }

// VFPU arithmetic/data-movement (2026-10-18; see cpu.cpp's own case 0x18/0x19/0x1B/0x34/0x37/0x3C and
// vfpu.h). vd/vs/vt are the real 7-bit register codes (mtx<<2 | col, plus bit5 transpose and, for triple,
// bit6 -- NOT the same split-encoding lv.s/sv.s above use). `sz` is 1/2/3/4 (Single/Pair/Triple/Quad); the
// size bits (bit7, bit15) are shared across this whole family, confirmed against cpu.cpp's vecSizeOf.
constexpr uint32_t vfpuSizeBits(int sizeMinus1) {
    return (static_cast<uint32_t>(sizeMinus1 & 1) << 7) | (static_cast<uint32_t>((sizeMinus1 >> 1) & 1) << 15);
}
constexpr uint32_t vfpu3(uint32_t opcode6, uint32_t group3, int sz, uint32_t vd, uint32_t vs, uint32_t vt) {
    return (opcode6 << 26) | ((group3 & 7u) << 23) | vfpuSizeBits(sz - 1) | ((vt & 0x7Fu) << 16) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
// VFPU0 (0x18) / VFPU1 (0x19): elementwise vd = vs OP vt.
constexpr uint32_t VADD(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x18, 0, sz, vd, vs, vt); }
constexpr uint32_t VSUB(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x18, 1, sz, vd, vs, vt); }
constexpr uint32_t VDIV(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x18, 7, sz, vd, vs, vt); }
constexpr uint32_t VMUL(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x19, 0, sz, vd, vs, vt); }
constexpr uint32_t VDOT(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x19, 1, sz, vd, vs, vt); }
constexpr uint32_t VSCL(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x19, 2, sz, vd, vs, vt); }
// VFPU3 (0x1B): vcmp writes the CC flags, not a vd register, so its low 4 bits are the condition code instead.
constexpr uint32_t VCMP(uint32_t cond, uint32_t vs, uint32_t vt, int sz) { return (0x1Bu << 26) | vfpuSizeBits(sz - 1) | ((vt & 0x7Fu) << 16) | ((vs & 0x7Fu) << 8) | (cond & 0xFu); }
constexpr uint32_t VMIN(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x1B, 2, sz, vd, vs, vt); }
constexpr uint32_t VMAX(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x1B, 3, sz, vd, vs, vt); }
constexpr uint32_t VSGE(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x1B, 6, sz, vd, vs, vt); }
constexpr uint32_t VSLT(uint32_t vd, uint32_t vs, uint32_t vt, int sz) { return vfpu3(0x1B, 7, sz, vd, vs, vt); }
// VFPU5 (0x37): vpfxs/vpfxt/vpfxd SET the next instruction's prefix (regnum 0/1/2 at bits24-25); regnum 3
// is viim.s (bit23=0, signed 16-bit immediate) / vfim.s (bit23=1, IEEE half-float).
constexpr uint32_t VPFXS(uint32_t data20) { return (0x37u << 26) | (0u << 24) | (data20 & 0xFFFFFu); }
constexpr uint32_t VPFXT(uint32_t data20) { return (0x37u << 26) | (1u << 24) | (data20 & 0xFFFFFu); }
constexpr uint32_t VPFXD(uint32_t data12) { return (0x37u << 26) | (2u << 24) | (data12 & 0xFFFu); }
constexpr uint32_t VIIM_S(uint32_t vt, int16_t imm) { return (0x37u << 26) | (3u << 24) | ((vt & 0x7Fu) << 16) | static_cast<uint16_t>(imm); }
constexpr uint32_t VFIM_S(uint32_t vt, uint16_t half) { return (0x37u << 26) | (3u << 24) | (1u << 23) | ((vt & 0x7Fu) << 16) | half; }
// VFPU4Jump (0x34): sub=0 nests VFPU4 (mov/abs/neg/idt/sat0/sat1/zero/one, by optype at bits16-20); sub=2
// nests the VFPU9 subset this project implements (vocp/vsgn); sub=16-21 are direct (vf2i*/vi2f/vcmov).
constexpr uint32_t vfpu34sub0(uint32_t optype5, int sz, uint32_t vd, uint32_t vs) {
    return (0x34u << 26) | (0u << 21) | ((optype5 & 0x1Fu) << 16) | vfpuSizeBits(sz - 1) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t VMOV(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub0(0, sz, vd, vs); }
constexpr uint32_t VABS(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub0(1, sz, vd, vs); }
constexpr uint32_t VNEG(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub0(2, sz, vd, vs); }
constexpr uint32_t VIDT(uint32_t vd, int sz) { return vfpu34sub0(3, sz, vd, 0); }
constexpr uint32_t VSAT0(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub0(4, sz, vd, vs); }
constexpr uint32_t VSAT1(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub0(5, sz, vd, vs); }
constexpr uint32_t VZERO(uint32_t vd, int sz) { return vfpu34sub0(6, sz, vd, 0); }
constexpr uint32_t VONE(uint32_t vd, int sz) { return vfpu34sub0(7, sz, vd, 0); }
constexpr uint32_t vfpu34sub2(uint32_t optype5, int sz, uint32_t vd, uint32_t vs) {
    return (0x34u << 26) | (2u << 21) | ((optype5 & 0x1Fu) << 16) | vfpuSizeBits(sz - 1) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t VOCP(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub2(4, sz, vd, vs); }
constexpr uint32_t VSGN(uint32_t vd, uint32_t vs, int sz) { return vfpu34sub2(10, sz, vd, vs); }
constexpr uint32_t vfpu34direct(uint32_t sub5, uint32_t scale5, int sz, uint32_t vd, uint32_t vs) {
    return (0x34u << 26) | ((sub5 & 0x1Fu) << 21) | ((scale5 & 0x1Fu) << 16) | vfpuSizeBits(sz - 1) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t VF2IN(uint32_t vd, uint32_t vs, uint32_t scale, int sz) { return vfpu34direct(16, scale, sz, vd, vs); }
constexpr uint32_t VF2IZ(uint32_t vd, uint32_t vs, uint32_t scale, int sz) { return vfpu34direct(17, scale, sz, vd, vs); }
constexpr uint32_t VF2IU(uint32_t vd, uint32_t vs, uint32_t scale, int sz) { return vfpu34direct(18, scale, sz, vd, vs); }
constexpr uint32_t VF2ID(uint32_t vd, uint32_t vs, uint32_t scale, int sz) { return vfpu34direct(19, scale, sz, vd, vs); }
constexpr uint32_t VI2F(uint32_t vd, uint32_t vs, uint32_t scale, int sz) { return vfpu34direct(20, scale, sz, vd, vs); }
constexpr uint32_t VCMOV(uint32_t vd, uint32_t vs, uint32_t imm3, bool tf, int sz) {
    return (0x34u << 26) | (21u << 21) | ((tf ? 1u : 0u) << 19) | ((imm3 & 7u) << 16) | vfpuSizeBits(sz - 1) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
// VFPU6 (0x3C): vmmul/vtfm2/3/4/vmscl (by sub at bits21-25) and, nested at sub=28, vmmov/vmidt/vmzero/vmone.
constexpr uint32_t vfpu6(uint32_t sub5, int msz, uint32_t vd, uint32_t vs, uint32_t vt) {
    return (0x3Cu << 26) | ((sub5 & 0x1Fu) << 21) | vfpuSizeBits(msz - 1) | ((vt & 0x7Fu) << 16) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t VMMUL(uint32_t vd, uint32_t vs, uint32_t vt, int msz) { return vfpu6(0, msz, vd, vs, vt); }
constexpr uint32_t VMSCL(uint32_t vd, uint32_t vs, uint32_t vt, int msz) { return vfpu6(16, msz, vd, vs, vt); }
// ins: 1=vtfm2 (2x2), 2=vtfm3 (3x3), 3=vtfm4 (4x4) -- real hardware reads `ins` straight from bits23-24,
// which overlap the sub field's own bits, so any sub in the matching [4,8)/[8,12)/[12,16) range works.
constexpr uint32_t VTFM(uint32_t vd, uint32_t vs, uint32_t vt, int ins) {
    return (0x3Cu << 26) | (static_cast<uint32_t>(ins) << 23) | (1u << 21) | vfpuSizeBits(ins) | ((vt & 0x7Fu) << 16) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t vfpu6set1(uint32_t optype4, int msz, uint32_t vd, uint32_t vs) {
    return (0x3Cu << 26) | (28u << 21) | ((optype4 & 0xFu) << 16) | vfpuSizeBits(msz - 1) | ((vs & 0x7Fu) << 8) | (vd & 0x7Fu);
}
constexpr uint32_t VMMOV(uint32_t vd, uint32_t vs, int msz) { return vfpu6set1(0, msz, vd, vs); }
constexpr uint32_t VMIDT(uint32_t vd, int msz) { return vfpu6set1(3, msz, vd, 0); }
constexpr uint32_t VMZERO(uint32_t vd, int msz) { return vfpu6set1(6, msz, vd, 0); }
constexpr uint32_t VMONE(uint32_t vd, int msz) { return vfpu6set1(7, msz, vd, 0); }

} // namespace enc
