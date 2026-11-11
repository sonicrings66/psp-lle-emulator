#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace beta::util {

// Decompresses a standard zlib stream (RFC 1950: 2-byte header + raw
// DEFLATE + Adler-32 trailer) into exactly `expectedSize` bytes, using
// third_party/puff (zlib's own minimal reference inflate decoder). Throws
// on any mismatch -- decoding to the wrong size is as much a failure here
// as an outright decode error, since the caller always knows the real
// uncompressed size in advance (PSAR records it per entry).
std::vector<uint8_t> inflateZlib(const uint8_t* data, std::size_t size, std::size_t expectedSize);

// Decompresses a gzip stream (RFC 1952: variable-length header + raw
// DEFLATE + CRC32/size trailer) into exactly `expectedSize` bytes. Real
// PSP kernel modules that are compressed use this format (distinct from
// PSAR's plain zlib), per their comp_attribute header field. Only the
// fixed and optional-field header parsing needed to find where the
// DEFLATE data starts is implemented; the CRC32/size trailer is not
// verified since the caller already knows and checks the true output size.
std::vector<uint8_t> inflateGzip(const uint8_t* data, std::size_t size, std::size_t expectedSize);

} // namespace beta::util
