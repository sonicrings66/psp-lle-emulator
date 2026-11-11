#include "inflate.h"

#include <stdexcept>
#include <string>

extern "C" {
#include "puff.h"
}

namespace beta::util {

std::vector<uint8_t> inflateZlib(const uint8_t* data, std::size_t size, std::size_t expectedSize) {
    if (size < 6) { // 2-byte zlib header + at least one byte + 4-byte Adler-32 trailer
        throw std::runtime_error("inflateZlib: input too small to be a zlib stream");
    }
    if (data[0] != 0x78) {
        throw std::runtime_error("inflateZlib: missing zlib stream header (expected 0x78 0x9C)");
    }

    std::vector<uint8_t> out(expectedSize);
    unsigned long destLen = static_cast<unsigned long>(expectedSize);
    unsigned long sourceLen = static_cast<unsigned long>(size - 2); // skip the zlib header; puff() wants raw DEFLATE

    const int result = puff(out.data(), &destLen, data + 2, &sourceLen);
    if (result != 0) {
        throw std::runtime_error("inflateZlib: puff() failed with code " + std::to_string(result));
    }
    if (destLen != expectedSize) {
        throw std::runtime_error("inflateZlib: decompressed size did not match the expected size");
    }

    return out;
}

std::vector<uint8_t> inflateGzip(const uint8_t* data, std::size_t size, std::size_t expectedSize) {
    if (size < 10 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 0x08) {
        throw std::runtime_error("inflateGzip: missing gzip header (1F 8B 08)");
    }

    const uint8_t flags = data[3];
    std::size_t offset = 10; // magic(2) + method(1) + flags(1) + mtime(4) + xfl(1) + os(1)

    if (flags & 0x04) { // FEXTRA
        if (offset + 2 > size) {
            throw std::runtime_error("inflateGzip: truncated FEXTRA field");
        }
        const std::size_t xlen = static_cast<std::size_t>(data[offset]) | (static_cast<std::size_t>(data[offset + 1]) << 8);
        offset += 2 + xlen;
    }
    if (flags & 0x08) { // FNAME
        while (offset < size && data[offset] != 0) {
            ++offset;
        }
        ++offset;
    }
    if (flags & 0x10) { // FCOMMENT
        while (offset < size && data[offset] != 0) {
            ++offset;
        }
        ++offset;
    }
    if (flags & 0x02) { // FHCRC
        offset += 2;
    }
    if (offset >= size) {
        throw std::runtime_error("inflateGzip: header fields consumed the whole input");
    }

    std::vector<uint8_t> out(expectedSize);
    unsigned long destLen = static_cast<unsigned long>(expectedSize);
    unsigned long sourceLen = static_cast<unsigned long>(size - offset);

    const int result = puff(out.data(), &destLen, data + offset, &sourceLen);
    if (result != 0) {
        throw std::runtime_error("inflateGzip: puff() failed with code " + std::to_string(result));
    }
    if (destLen != expectedSize) {
        throw std::runtime_error("inflateGzip: decompressed size did not match the expected size");
    }

    return out;
}

} // namespace beta::util
