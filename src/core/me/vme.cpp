#include "me/vme.h"

namespace beta {

namespace {
// Handshake status registers: bit 0 = ready. ONLY 0x200 (the command interface). Answering 'ready' on the request/response channels 0x350/0x360/0x370 without real data
// behind them (the entropy-decoded symbols the ME reads back through 0x371/0x362) sent the firmware's symbol loop (ME 0x883661C8..0x88366214) into an endless loop,
// worse than the quick timeouts; see vme.h.
constexpr uint32_t kHandshakeStatus[] = {0x200};
}

uint32_t VmeDevice::read(uint32_t reg) const {
    reg %= kRegisters;
    for (uint32_t r : kHandshakeStatus) if (r == reg) return 1u;
    return regs_[reg];
}

} // namespace beta
