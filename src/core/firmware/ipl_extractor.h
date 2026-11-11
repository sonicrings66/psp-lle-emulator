#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../crypto/kirk.h"
#include "../util/nand_image.h"

// Real NAND-based IPL (second-stage bootloader) extraction -- ported from
// psp-lle-emu's extractAndLoadRealIpl/extractAndLoadRealIplCached
// (tests/test_ipl_realfuse.cpp:235-517, verbatim-shared with
// test_pbp.cpp). This is real, load-bearing emulation logic, not test
// scaffolding -- it belongs in src/core, which is why it was the first
// thing pulled out of that file (see docs/PORTING_LOG.md and the user's
// explicit ask that firmware/kirk do the real decrypting of boot modules).
//
// What this does, on real hardware: after cIPL/pre-IPL hands off, the
// real boot ROM (or, in this project's case, the ARK-4 cIPL exploit path)
// reads the IPL block table from NAND (physical block 4 region) and
// decrypts the real IPL in 4KB chunks via genuine KIRK commands --
// generic decrypt (cmd 0-3, selected per chunk), KIRK_CMD_DECRYPT_IV_0
// (per-chunk hash check), KIRK_CMD_SHA1_HASH, and a final
// KIRK_CMD_ECDSA_VERIFY against Sony's real embedded public key over the
// XOR of every chunk's hash. No shortcuts: every one of those is a real
// `kirk.command()` call into the vendored libkirk engine, not a stand-in.
//
// The one documented exception is the real, publicly-known ARK-4/DC-M33
// "reset block" exploit: a chunk whose header literally asks to write to
// the real CPU reset register (0xBC10004C) is recognized and its own
// embedded jump target decoded directly, bypassing the ECDSA check the
// same way real hardware's CFW exploit does -- this is real documented
// behavior of this specific console's software, not emulator-side
// invention.
namespace beta::firmware {

// Writes one 32-bit word to guest address `addr`. The caller supplies
// this instead of IplExtractor taking a Bus& directly, so this module
// stays usable before Bus/SystemBus exist (Phase A) and composes cleanly
// with whatever addressing scheme the eventual caller uses.
using WriteWordFn = std::function<void(uint32_t addr, uint32_t value)>;

// Runs the real extraction once. Returns the IPL's entry point, or 0 if
// extraction failed (see the thrown-away printf diagnostics' old
// messages, now routed through whatever logger the caller wires in --
// Phase A does not yet thread a Logger through this free function; it
// throws std::runtime_error on a hard failure instead, which is a
// trade-off a later pass can revisit once src/config::Logger is
// threaded through src/core broadly).
uint32_t extractAndLoadRealIpl(beta::crypto::KirkEngine& kirk, const beta::NandImage& nand,
                                const WriteWordFn& writeWord);

// Cache key: a pure function of (the NAND's IPL-relevant region, the fuse
// key, this code's own format version) -- ported from iplCacheKey.
uint64_t iplCacheKey(const beta::NandImage& nand, uint64_t fuseKey);

// Same real extraction, but memoized to a file keyed by iplCacheKey() so
// a repeat run with the same NAND/fuse replays the ~8s KIRK/ECDSA chain
// instead of re-running it. `cacheDir` is the directory to read/write the
// cache file in (caller's choice -- Phase A has no pspOutPath() yet);
// pass an empty string to disable caching entirely.
uint32_t extractAndLoadRealIplCached(beta::crypto::KirkEngine& kirk, const beta::NandImage& nand,
                                      const WriteWordFn& writeWord, uint64_t fuseKey,
                                      const std::string& cacheDir);

} // namespace beta::firmware
