#pragma once

#include <cstdint>
#include <vector>

#include "kirk_engine.h" // vendored libkirk, third_party/libkirk

// Ported near-verbatim from psp-lle-emu/src/core/crypto/kirk.h (see
// docs/PORTING_LOG.md). No getenv/debug-tap cleanup needed -- this file
// had none to begin with, which is exactly why the Phase-A research
// called it out as reusable as-is.

namespace beta::crypto {

// C++ wrapper around the vendored libkirk reverse-engineering of the real
// PSP KIRK crypto engine (third_party/libkirk). Real Sony firmware talks
// to KIRK through a single syscall, sceUtilsBufferCopyWithRange,
// dispatching on a command number -- command() below exposes that same
// shape directly.
class KirkEngine {
public:
    // deterministicPrngTime: real KIRK command 0xE (the real hardware PRNG)
    // and this engine's own initial PRNG seeding both mix in real wall-clock
    // time(0) -- genuinely non-deterministic on real hardware too, but it
    // means two emulator runs can produce a different RAM state hash if
    // anything (a real KIRK_CMD_PRNG call, or KIRK_CMD_ENCRYPT_IV_USER's
    // session-key generation) ever surfaces PRNG output into guest-visible
    // memory. nullptr (the default) keeps that real non-determinism; pass a
    // fixed value for a test harness that needs exact-repeat runs.
    explicit KirkEngine(const uint32_t* deterministicPrngTime = nullptr);

    // Real per-console fuse ID, for KIRK_CMD_ENCRYPT_IV_FUSE/DECRYPT_IV_FUSE
    // (CMD5/CMD8 -- real firmware's per-console decrypt). The default
    // constructor above always uses libkirk's own placeholder
    // (0xBABEF00D/0xDEADBEEF, see kirk_init()). fuseId90/fuseId94 map
    // directly to the real 64-bit hardware register pair at 0xBC100090
    // (low 32 bits) / 0xBC100094 (high 32 bits): combined = (fuse94 << 32)
    // | fuse90. See deterministicPrngTime above.
    KirkEngine(uint32_t fuseId90, uint32_t fuseId94, const uint32_t* deterministicPrngTime = nullptr);

    // AES-128-CBC using one of the engine's 128 built-in fixed keys
    // (keyId 0-127), IV implicitly zero. Data size must be a multiple of
    // 16 bytes.
    std::vector<uint8_t> encryptCbc(const std::vector<uint8_t>& plain, int keyId);
    std::vector<uint8_t> decryptCbc(const std::vector<uint8_t>& cipher, int keyId);

    std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);

    // Direct pass-through to the real command dispatch
    // (kirk_sceUtilsBufferCopyWithRange), for commands not wrapped above
    // -- this is how real IPL/PRX decryption (CMD1, CMD7, CMD11,
    // CMD_ECDSA_VERIFY) actually runs; see src/core/firmware/ipl_extractor.*
    int command(uint8_t* outbuff, int outsize, uint8_t* inbuff, int insize, int cmd);

private:
    KirkState state_{};
};

} // namespace beta::crypto
