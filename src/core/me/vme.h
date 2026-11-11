#pragma once
// The Media Engine's "video media engine" (VME) register file, reached only by the ME-specific mtvme / mfvme instructions
// (mtvme rt, imm16 = 0xB0E00000 | rt << 16 | imm16, mfvme = 0x68E00000 | ...). jpcsp keeps these as plain storage; the hardware behind them is a video (H.264)
// decoder. What is known from the ME firmware's own driver (mediaengine.prx, ME code 0x88367000-0x8836E200) and from PSP_VME_LOG traces:
//   0x200..0x203  command interface: the firmware polls bit 0 of 0x200 ("ready", up to 4096 times) and then writes the command (0x200), two operands (0x201, 0x202)
//                 and 0 (0x203). Seen at boot: command 0x9400, 0x201 = an offset (multiples of 8 up to 0x1B8), 0x202 = 1, 1532 times.
//   0x300-0x33F per-block parameters, 0x350 / 0x360-0x364 trigger + busy polls, 0x400-0x4FF tables, 0x500-0x58F a per-frame/slice bank (not yet traced).
//   0x350 -> 0x370/0x371 and 0x360 -> 0x361/0x362 look like request/response channels: the firmware waits for bit 0 of the request register ("ready", up to 4096 polls,
//                 error code 6/7 on timeout), writes it, waits for bit 0 of the response register ("valid", error 9/8), then reads the data register (0x371 / 0x362).
// What those channels are (found 2026-10-09 when 'always ready' made the ME loop for 3.4 billion steps): the ME's own code at 0x883661C8..0x88366214 sends a
// command through the 0x200 interface, reads the answer through 0x371 (low 5 bits) and jumps through a 9-entry table on it, until an end marker arrives. That is the
// entropy decoder (the VLD unit): the ME pulls decoded symbols out of the hardware. Answering with 0 forever never ends the loop, while a timeout (the ready bit
// never set) ends it through the firmware's error path, so for now ONLY 0x200 reads ready and the request/response channels keep timing out.
// Model so far: plain storage, except that 0x200 (the command interface status) reads 1 (idle/ready).
#include <array>
#include <cstdint>

namespace beta {

class VmeDevice {
public:
    static constexpr uint32_t kRegisters = 0x590;   // jpcsp has seen register numbers up to 0x58F
    static constexpr uint32_t kCommandStatus = 0x200;

    uint32_t read(uint32_t reg) const;              // vme.cpp: the handshake status registers read 1, everything else its stored value
    void write(uint32_t reg, uint32_t value) { regs_[reg % kRegisters] = value; }

private:
    std::array<uint32_t, kRegisters> regs_{};
};

} // namespace beta
