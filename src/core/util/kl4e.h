#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Merged KL4E decoder (see docs/PORTING_LOG.md "one behavior, one home"):
// the old project had two independent transcriptions of the same jpcsp
// KL4E.decompress_kle port -- format/kl4e.h (header-only, bool-returning,
// reports consumed input bytes, used by the ME bootstrap's two
// back-to-back streams) and util/kl4e.h/.cpp (throwing, .at()-bounds-
// checked, used by crypto/prx_decrypt.cpp). Beta keeps the latter's
// safer style as canonical and adds the former's `consumedOut` capability
// so both call sites are served by one implementation.

namespace beta::util {

// Decompresses Sony's KL4E/KL3E format -- an undocumented, proprietary
// LZ77-plus-arithmetic-coding scheme real PSP firmware modules (sysmem,
// loadcore, loadexec, the Media Engine bootstrap image, etc.) are
// compressed with, distinct from PSAR's plain zlib. No official
// documentation exists; this is a faithful port of the community's
// reverse-engineered reference implementation (github.com/John-K/
// pspdecrypt's kl4e.c, via its Java port jpcsp/src/jpcsp/format/KL4E.java
// `decompress_kle`), not a fresh reverse-engineering effort.
//
// `data` must start right after the 4-byte "KL4E"/"KL3E" magic (the
// caller already used that magic to choose `isKl4e`). `expectedSize` is
// only a safe upper bound on the output, not a required exact size -- the
// format is self-terminating (an explicit end-of-stream marker), and the
// returned vector is trimmed to the real produced size. If `consumedOut`
// is non-null, it receives the number of input bytes consumed through
// that end marker -- needed by a caller with more than one stream back to
// back (e.g. the ME bootstrap image's two segments). Throws
// std::runtime_error on a malformed stream or an expectedSize too small
// to hold the real output.
std::vector<uint8_t> decompressKl4e(const std::vector<uint8_t>& data, std::size_t expectedSize,
                                     bool isKl4e, std::size_t* consumedOut = nullptr);

} // namespace beta::util
