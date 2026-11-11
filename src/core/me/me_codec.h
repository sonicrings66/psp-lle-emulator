#pragma once
// Host-side audio-decode SHORTCUT for Media Engine RPCs -- MINIMAL version.
//
// Ported in spirit (not verbatim) from psp-lle-emu/src/core/me/me_codec.h.
// That version does REAL decoding (ffmpeg-derived ATRAC3/ATRAC3+ decoders,
// a minimp3-based MP3 decoder -- see at3_decoders.h/mp3_standalone.h,
// neither ported to this project yet). This version is a deliberate
// placeholder: it answers the SAME commands with SILENCE instead of real
// decoded audio, using the real work-area protocol (field offsets, the
// real inSize/outSize formulas) so the CALLER's own bookkeeping stays
// consistent, without needing the real decoders.
//
// Why this exists at all (confirmed live this session, not a guess):
// `MeCore::interruptFromMain()` checks `codec_.handle(cmd)` BEFORE waking
// the real ME firmware for a request. Without this class, command 0x60
// (AT3+ decode) reaches the real ME firmware for real, which drives its
// own actual AT3+ decode path through the VME (video media engine)
// register file -- and `vme.h`'s own header comment already documents
// that path's request/response channels as an unresolved real-hardware
// gap ("the request/response channels keep timing out"), a pre-existing
// limitation from before this session. A real boot (common_util.prx's
// own real startup, confirmed live) issues a real AT3+ decode RPC, and
// without this shortcut the whole boot permanently stalls on it. This is
// the exact shape of shortcut the old project's own MeCodec already is --
// real decode quality was never modeled by real hardware emulation either,
// just by better host decoders than this placeholder uses.
//
// Real work-area layout (jpcsp's sceAudiocodec convention, same as the
// old project): +8 error, +24 input buffer, +28 bytes consumed, +32
// output buffer, +36 output size, +48/+56/+64/+72 codec parameters.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "../bus/system_bus.h"
#include "../../config/options.h"

namespace beta {

class MeCodec {
public:
    static constexpr uint32_t kCmdAt3pDecode = 0x60, kCmdAt3pSetupChannel = 0x67, kCmdMp3Decode = 0x8C;
    static constexpr uint32_t kTable = 0xBFC00600u;

    explicit MeCodec(SystemBus& sc, const config::MeOptions& opts = {}) : sc_(sc) { log_ = opts.log; }

    // Called for every request the main CPU posts. Returns true when the
    // request was fully answered here (result written, main CPU notified
    // by the caller) -- matching the old project's own real contract.
    bool handle(uint32_t cmd) {
        if (log_) {
            const uint32_t a2 = sc_.read32(kTable + 8u), a3 = sc_.read32(kTable + 12u);
            static int n = 0; if (n++ < 6000) std::printf("  [ME codec] request 0x%X args %08X %08X%c", cmd, a2, a3, 10);
        }
        if (cmd == kCmdAt3pSetupChannel) {
            // Real ME firmware still performs its own bookkeeping for this
            // one (matches the old project exactly) -- not intercepted.
            return false;
        }
        if (cmd == kCmdAt3pDecode) { result_ = silentAt3p(sc_.read32(kTable + 8u)); return true; }
        if (cmd == kCmdMp3Decode) { result_ = silentMp3(sc_.read32(kTable + 8u)); return true; }
        return false;
    }
    uint32_t result() const { return result_; }

private:
    // Real inSize/outSize formulas copied verbatim from the old project's
    // own real decodeAt3p (so a caller's own stream-position bookkeeping
    // stays consistent) -- only the actual decode call is replaced with a
    // silent fill.
    uint32_t silentAt3p(uint32_t work) {
        auto r32 = [&](uint32_t off) { return sc_.read32(work + off); };
        sc_.write32(work + 8u, 0u); // error field
        uint32_t inBuf = r32(24), outBuf = r32(32);
        uint32_t inSize = r32(48) == 0u ? r32(64) + 2u : 0x100Au;
        if (sc_.read8(inBuf) == 0x0Fu && sc_.read8(inBuf + 1u) == 0xD0u) {
            const uint32_t h = (static_cast<uint32_t>(sc_.read8(inBuf + 2u)) << 8) | sc_.read8(inBuf + 3u);
            inSize = (h & 0x3FFu) << 3;
        }
        const uint32_t outSize = (r32(56) == 1u && r32(72) != r32(56)) ? 0x2000u : (r32(72) << 12);
        sc_.write32(work + 36u, outSize);
        for (uint32_t i = 0; i < outSize; i += 4u) { try { sc_.write32(outBuf + i, 0u); } catch (...) { break; } }
        sc_.write32(work + 28u, inSize); // bytes consumed
        if (log_) { static int n = 0; if (n++ < 12) std::printf("  [ME codec] SILENT AT3+ work=%08X in=%08X(%u) out=%08X(%u)%c", work, inBuf, inSize, outBuf, outSize, 10); }
        return 0u;
    }

    // MP3: consume the whole provided input buffer (no real frame-sync
    // scan needed for a silent placeholder) and report zero decoded
    // samples -- a real decoder would scan for a frame header and decode
    // exactly one frame's worth; this just has to not stall the boot.
    uint32_t silentMp3(uint32_t work) {
        auto r32 = [&](uint32_t off) { return sc_.read32(work + off); };
        sc_.write32(work + 8u, 0u); // error field
        uint32_t inSize = r32(40);
        if (inSize == 0u || inSize > 0x10000u) inSize = 0x10000u;
        sc_.write32(work + 36u, 0u); // output size: zero decoded samples
        sc_.write32(work + 28u, inSize); // bytes consumed: the whole buffer
        if (log_) { static int n = 0; if (n++ < 12) std::printf("  [ME codec] SILENT MP3 work=%08X in=%u%c", work, inSize, 10); }
        return 0u;
    }

    SystemBus& sc_;
    bool log_ = false;
    uint32_t result_ = 0;
};

} // namespace beta
