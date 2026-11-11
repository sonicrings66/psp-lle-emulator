#include "prx_decrypt.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "prx_tags.h"
#include "../util/inflate.h"
#include "../util/kl4e.h"

namespace beta::crypto {

namespace {

uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readI32LE(const uint8_t* p) {
    return static_cast<int32_t>(readU32LE(p));
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

// --- PPSSPP: PRXType0/1/2/5/6/9 (Core/ELF/PrxDecrypter.cpp) -----------------
// Reassemble a module's key/header material into one contiguous block in a
// fixed shape each decode type expects. Struct member order matters: some
// `decrypt()` methods deliberately walk past one member's declared size
// into the next (e.g. PrxType1), relying on plain uint8_t arrays having no
// inter-member padding -- exactly mirroring the reference's own reliance
// on C struct layout.

struct PrxType0 {
    explicit PrxType0(const uint8_t* prx) {
        std::memcpy(sha1, prx + 0xD4, sizeof(sha1));
        std::memcpy(unused, prx + 0xE8, sizeof(unused));
        std::memcpy(kirkBlock, prx + 0x110, 0x40);
        std::memcpy(kirkBlock + 0x40, prx + 0x80, sizeof(kirkBlock) - 0x40);
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }

    uint8_t sha1[0x14];
    uint8_t unused[0x28];
    uint8_t kirkBlock[0x90];
    uint8_t prxHeader[0x80];
};

struct PrxType1 {
    explicit PrxType1(const uint8_t* prx) {
        std::memcpy(sha1, prx + 0xD4, sizeof(sha1));
        std::memcpy(unused, prx + 0xE8, sizeof(unused));
        std::memcpy(kirkBlock, prx + 0x110, 0x40);
        std::memcpy(kirkBlock + 0x40, prx + 0x80, sizeof(kirkBlock) - 0x40);
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }

    // Walks past sha1's declared end into unused/kirkBlock -- relies on
    // the three being laid out contiguously, exactly as PPSSPP's original
    // does through the same kind of pointer arithmetic.
    void decrypt(int key) { kirk7(sha1 + 0xC, sha1 + 0xC, 0xA0, key); }

    uint8_t sha1[0x14];
    uint8_t unused[0x28];
    uint8_t kirkBlock[0x90];
    uint8_t prxHeader[0x80];
};

struct PrxType2 {
    explicit PrxType2(const uint8_t* prx) {
        std::memcpy(tag, prx + 0xD0, sizeof(tag));
        std::memset(empty, 0, sizeof(empty));
        std::memcpy(id, prx + 0x140, sizeof(id));
        std::memcpy(sha1, prx + 0x12C, sizeof(sha1));
        std::memcpy(kirkHeader, prx + 0x80, sizeof(kirkHeader) - 0x10);
        std::memcpy(kirkHeader + 0x30, prx + 0xC0, 0x10);
        std::memcpy(kirkMetadata, prx + 0xB0, sizeof(kirkMetadata));
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }
    void decrypt(int key) { kirk7(id, id, 0x60, key); }

    uint8_t tag[4];
    uint8_t empty[0x58];
    uint8_t id[0x10];
    uint8_t sha1[0x14];
    uint8_t kirkHeader[0x40];
    uint8_t kirkMetadata[0x10];
    uint8_t prxHeader[0x80];
};

struct PrxType5 {
    explicit PrxType5(const uint8_t* prx) {
        std::memcpy(tag, prx + 0xD0, sizeof(tag));
        std::memset(empty, 0, sizeof(empty));
        std::memcpy(id, prx + 0x140, sizeof(id));
        std::memcpy(sha1, prx + 0x12C, sizeof(sha1));
        std::memcpy(kirkHeader, prx + 0x80, sizeof(kirkHeader) - 0x10);
        std::memcpy(kirkHeader + 0x30, prx + 0xC0, 0x10);
        std::memcpy(kirkMetadata, prx + 0xB0, sizeof(kirkMetadata));
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }

    void decrypt(int key, const uint8_t* xor1, const uint8_t* xor2) {
        uint8_t data[0x50];
        std::memcpy(data, kirkHeader, sizeof(kirkHeader));
        std::memcpy(data + sizeof(kirkHeader), sha1, 0x10);

        for (int i = 0; i < 0x50; ++i) {
            if (xor1) data[i] = static_cast<uint8_t>(data[i] ^ xor1[i % 0x10]);
            if (xor2) data[i] = static_cast<uint8_t>(data[i] ^ xor2[i % 0x10]);
        }

        kirk7(data, data, 0x50, key);

        std::memcpy(kirkHeader, data, sizeof(kirkHeader));
        std::memcpy(sha1, data + sizeof(kirkHeader), 0x10);

        if (xor1) {
            // Deliberately walks past id[0x10]'s declared end into sha1/
            // kirkHeader, which are contiguous right after it -- go through
            // a raw pointer first so the compiler's array-bounds-aware loop
            // optimizer doesn't treat that as unreachable, matching how
            // PPSSPP's own code (`u8 *p = id;`) avoids the same issue.
            uint8_t* p = id;
            for (int i = 0; i < 0x60; ++i) {
                p[i] = static_cast<uint8_t>(p[i] ^ xor1[i % 0x10]);
            }
        }
        kirk7(id, id, 0x60, key);
    }

    uint8_t tag[4];
    uint8_t empty[0x58];
    uint8_t id[0x10];
    uint8_t sha1[0x14];
    uint8_t kirkHeader[0x40];
    uint8_t kirkMetadata[0x10];
    uint8_t prxHeader[0x80];
};

struct PrxType6 {
    explicit PrxType6(const uint8_t* prx) {
        std::memcpy(tag, prx + 0xD0, sizeof(tag));
        std::memset(empty, 0, sizeof(empty));
        std::memcpy(ecdsaSignatureTail, prx + 0x10C, sizeof(ecdsaSignatureTail));
        std::memcpy(id, prx + 0x140, sizeof(id));
        std::memcpy(sha1, prx + 0x12C, sizeof(sha1));
        std::memcpy(kirkHeader, prx + 0x80, sizeof(kirkHeader) - 0x10);
        std::memcpy(kirkHeader + 0x30, prx + 0xC0, 0x10);
        std::memcpy(kirkMetadata, prx + 0xB0, sizeof(kirkMetadata));
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }
    void decrypt(int key) { kirk7(id, id, 0x60, key); }

    uint8_t tag[4];
    uint8_t empty[0x38];
    uint8_t ecdsaSignatureTail[0x20];
    uint8_t id[0x10];
    uint8_t sha1[0x14];
    uint8_t kirkHeader[0x40];
    uint8_t kirkMetadata[0x10];
    uint8_t prxHeader[0x80];
};

// Same layout as type 6, except the ECDSA signature at 0x104..0x12C is
// present (excluded from the hash, left zero here) rather than empty.
struct PrxType9 {
    explicit PrxType9(const uint8_t* prx) {
        std::memcpy(tag, prx + 0xD0, sizeof(tag));
        std::memset(empty, 0, sizeof(empty));
        std::memset(ecdsaSignatureTail, 0, sizeof(ecdsaSignatureTail));
        std::memcpy(id, prx + 0x140, sizeof(id));
        std::memcpy(sha1, prx + 0x12C, sizeof(sha1));
        std::memcpy(kirkHeader, prx + 0x80, sizeof(kirkHeader) - 0x10);
        std::memcpy(kirkHeader + 0x30, prx + 0xC0, 0x10);
        std::memcpy(kirkMetadata, prx + 0xB0, sizeof(kirkMetadata));
        std::memcpy(prxHeader, prx, sizeof(prxHeader));
    }
    void decrypt(int key) { kirk7(id, id, 0x60, key); }

    uint8_t tag[4];
    uint8_t empty[0x38];
    uint8_t ecdsaSignatureTail[0x20];
    uint8_t id[0x10];
    uint8_t sha1[0x14];
    uint8_t kirkHeader[0x40];
    uint8_t kirkMetadata[0x10];
    uint8_t prxHeader[0x80];
};

std::array<uint8_t, 0x90> expandSeed(const uint8_t* seed, int key, const uint8_t* bonusSeed = nullptr) {
    std::array<uint8_t, 0x90> expandedSeed{};
    for (std::size_t i = 0; i < expandedSeed.size(); i += 0x10) {
        std::memcpy(expandedSeed.data() + i, seed, 0x10);
        expandedSeed[i] = static_cast<uint8_t>(i / 0x10);
    }
    kirk7(expandedSeed.data(), expandedSeed.data(), expandedSeed.size(), key);
    if (bonusSeed) {
        for (std::size_t i = 0; i < expandedSeed.size(); ++i) {
            expandedSeed[i] = static_cast<uint8_t>(expandedSeed[i] ^ bonusSeed[i % 0x10]);
        }
    }
    return expandedSeed;
}

// PPSSPP: decryptKirkHeaderType0. An XOR/kirk7/XOR sandwich that turns the
// scrambled key material in `kirkBlock` into the real AES/CMAC key CMD1
// expects, using `xorbuf` (the tag's, possibly kirk7-unscrambled, key) as
// the XOR pad on both sides. Used by types 0 and 1.
void decryptKirkHeaderType0(uint8_t* outbuf, const uint8_t* kirkBlock, const std::array<uint8_t, 0x90>& xorbuf, int key) {
    uint8_t tmp[0x70];
    for (int i = 0; i < 0x70; ++i) {
        tmp[i] = static_cast<uint8_t>(kirkBlock[i] ^ xorbuf[static_cast<std::size_t>(i) + 0x14]);
    }
    kirk7(tmp, tmp, sizeof(tmp), key);
    for (int i = 0; i < 0x70; ++i) {
        outbuf[i] = static_cast<uint8_t>(tmp[i] ^ xorbuf[static_cast<std::size_t>(i) + 0x20]);
    }
}

// PPSSPP: decryptKirkHeader (generic). Same shape as decryptKirkHeaderType0
// but over a 0x40-byte window and taking an iterator so it can start partway
// through xorbuf. Used by types 2, 5, 6, 9.
template <typename It>
void decryptKirkHeader(uint8_t* outbuf, const uint8_t* inbuf, It xorbuf, int key) {
    for (int i = 0; i < 0x40; ++i) {
        outbuf[i] = static_cast<uint8_t>(inbuf[i] ^ *xorbuf++);
    }
    kirk7(outbuf, outbuf, 0x40, key);
    for (int i = 0; i < 0x40; ++i) {
        outbuf[i] = static_cast<uint8_t>(outbuf[i] ^ *xorbuf++);
    }
}

std::vector<uint8_t> sha1Check(KirkEngine& kirk, std::initializer_list<std::pair<const uint8_t*, std::size_t>> pieces) {
    std::vector<uint8_t> input;
    for (const auto& [data, size] : pieces) {
        input.insert(input.end(), data, data + size);
    }
    return kirk.sha1(input);
}

void requireZero(const uint8_t* data, std::size_t count, const char* what) {
    for (std::size_t i = 0; i < count; ++i) {
        if (data[i] != 0) {
            throw std::runtime_error(std::string("decryptPrx: ") + what + " not empty");
        }
    }
}

// kirk_CMD1 (the vendored C engine) writes exactly header->data_size bytes
// into outbuf starting at offset 0, with no bounds checking of its own
// against how large outbuf actually is -- confirmed by a real,
// intermittently-reproducing heap-corruption crash when this went
// unchecked. `dataSize` comes out of the just-decrypted header, which is
// still essentially data-controlled at this point (nothing upstream
// guarantees it matches the buffer we actually allocated), so it must be
// validated here, before ever handing control to that call.
void checkWriteFits(uint32_t dataSize, uint32_t bufferCapacity, const char* who) {
    if (dataSize > bufferCapacity) {
        throw std::runtime_error(std::string(who) + ": decrypted data_size (" + std::to_string(dataSize) +
                                  ") exceeds the output buffer (" + std::to_string(bufferCapacity) + " bytes)");
    }
}

// kirk_CMD1's own final AES-CBC decrypt (the vendored engine) reads its
// SOURCE starting at inbuff+sizeof(KIRK_CMD1_HEADER)+data_offset, not from
// offset 0 -- data_offset is a second, independent field out of the same
// decrypted header, with no bounds checking of its own against how much
// real ciphertext is actually there to read. When data_offset+data_size
// (rounded up to a whole AES block, same rounding the engine applies to
// the write side) reaches past the real ciphertext this call actually
// has, CMD1 doesn't fail -- it silently decrypts whatever bytes happen to
// follow (padding, or unrelated heap contents) as if they were genuine
// data, handing back a wrong-but-plausible-looking payload whose garbage
// tail corrupts whatever parses it next (ELF loading, gzip/KL4E
// decompression) -- often nowhere near this call and long after it
// returns. This is the actual mechanism behind a real, confirmed
// intermittent, crash-site-varying segfault across this project's full
// real-module test corpus (initially misread as a write-side overflow;
// live guard-byte tracing across several buffer-sizing attempts showed
// the crash depends on the *content* of the extra bytes, not just how
// many there are, which only makes sense if they're being decrypted and
// used, not just read speculatively).
void checkReadFits(uint32_t dataOffset, uint32_t dataSize, uint32_t insize, const char* who) {
    const uint64_t roundedSize = (static_cast<uint64_t>(dataSize) + 15) & ~static_cast<uint64_t>(15);
    const uint64_t readEnd = static_cast<uint64_t>(sizeof(KIRK_CMD1_HEADER)) + dataOffset + roundedSize;
    if (readEnd > insize) {
        throw std::runtime_error(std::string(who) + ": decrypted data_offset (" + std::to_string(dataOffset) +
                                  ") + data_size reads past the real ciphertext (" + std::to_string(insize) + " bytes available)");
    }
}

} // namespace

int32_t decryptPrxType0(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;    // sizeof PSP_Header (the module header)
    constexpr uint32_t kCmd1HeaderSize = 0x90; // sizeof KIRK_CMD1_HEADER
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kCmd1HeaderSize - kPrxHeaderSize; // 0x40

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType0: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo* pti = getTagInfo(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType0: no key for tag " + hex8(tag));
    }

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);

    std::array<uint8_t, 0x90> xorbuf{};
    std::memcpy(xorbuf.data(), pti->key, xorbuf.size());
    if (pti->scrambleKey) {
        kirk7(xorbuf.data(), xorbuf.data(), xorbuf.size(), pti->code);
    }

    const PrxType0 type0(inbuf);
    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {xorbuf.data(), 0x14},
        {type0.unused, sizeof(type0.unused)},
        {type0.kirkBlock, sizeof(type0.kirkBlock)},
        {type0.prxHeader, sizeof(type0.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type0.sha1)) {
        throw std::runtime_error("decryptPrxType0: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    uint8_t* header = outbuf + kOffset;
    std::memcpy(header, type0.kirkBlock, kCmd1HeaderSize);
    std::memcpy(header + kCmd1HeaderSize, type0.prxHeader, kPrxHeaderSize);
    decryptKirkHeaderType0(header, type0.kirkBlock, xorbuf, pti->code);

    checkWriteFits(readU32LE(header + 0x70), size, "decryptPrxType0"); // data_size field, KIRK_CMD1_HEADER+0x70
    checkReadFits(readU32LE(header + 0x74), readU32LE(header + 0x70), size - kOffset, "decryptPrxType0"); // data_offset field, KIRK_CMD1_HEADER+0x74
    const int result = kirk.command(outbuf, static_cast<int>(size), header, static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType0: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrxType1(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;
    constexpr uint32_t kCmd1HeaderSize = 0x90;
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kCmd1HeaderSize - kPrxHeaderSize;

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType1: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo* pti = getTagInfo(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType1: no key for tag " + hex8(tag));
    }

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);

    std::array<uint8_t, 0x90> xorbuf{};
    std::memcpy(xorbuf.data(), pti->key, xorbuf.size());

    PrxType1 type1(inbuf);
    type1.decrypt(pti->code);

    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {xorbuf.data(), 0x14},
        {type1.unused, sizeof(type1.unused)},
        {type1.kirkBlock, sizeof(type1.kirkBlock)},
        {type1.prxHeader, sizeof(type1.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type1.sha1)) {
        throw std::runtime_error("decryptPrxType1: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    uint8_t* header = outbuf + kOffset;
    std::memcpy(header, type1.kirkBlock, kCmd1HeaderSize);
    std::memcpy(header + kCmd1HeaderSize, type1.prxHeader, kPrxHeaderSize);
    decryptKirkHeaderType0(header, type1.kirkBlock, xorbuf, pti->code);

    checkWriteFits(readU32LE(header + 0x70), size, "decryptPrxType1");
    checkReadFits(readU32LE(header + 0x74), readU32LE(header + 0x70), size - kOffset, "decryptPrxType1");
    const int result = kirk.command(outbuf, static_cast<int>(size), header, static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType1: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrxType2(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;
    constexpr uint32_t kCmd1HeaderSize = 0x90;
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kCmd1HeaderSize - kPrxHeaderSize;

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType2: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo2* pti = getTagInfo2(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType2: no key for tag " + hex8(tag));
    }

    requireZero(inbuf + 0xD4, 0x58, "signature-check region");

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);
    const std::array<uint8_t, 0x90> xorbuf = expandSeed(pti->key, pti->code);

    PrxType2 type2(inbuf);
    type2.decrypt(pti->code);

    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {type2.tag, sizeof(type2.tag)},
        {xorbuf.data(), 0x10},
        {type2.empty, sizeof(type2.empty)},
        {type2.id, sizeof(type2.id)},
        {type2.kirkHeader, sizeof(type2.kirkHeader)},
        {type2.kirkMetadata, sizeof(type2.kirkMetadata)},
        {type2.prxHeader, sizeof(type2.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type2.sha1)) {
        throw std::runtime_error("decryptPrxType2: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    auto* header = reinterpret_cast<KIRK_CMD1_HEADER*>(outbuf + kOffset);
    std::memset(header, 0, sizeof(KIRK_CMD1_HEADER));
    std::memcpy(&header->data_size, type2.kirkMetadata, sizeof(type2.kirkMetadata));
    std::memcpy(reinterpret_cast<uint8_t*>(header) + sizeof(KIRK_CMD1_HEADER), type2.prxHeader, sizeof(type2.prxHeader));
    decryptKirkHeader(reinterpret_cast<uint8_t*>(header), type2.kirkHeader, xorbuf.cbegin() + 0x10, pti->code);
    header->mode = 1;

    checkWriteFits(header->data_size, size, "decryptPrxType2");
    checkReadFits(header->data_offset, header->data_size, size - kOffset, "decryptPrxType2");
    const int result = kirk.command(outbuf, static_cast<int>(size), reinterpret_cast<uint8_t*>(header), static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType2: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrxType5(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;
    constexpr uint32_t kCmd1HeaderSize = 0x90;
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kCmd1HeaderSize - kPrxHeaderSize;

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType5: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo2* pti = getTagInfo2(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType5: no key for tag " + hex8(tag));
    }

    requireZero(inbuf + 0xD4 + 1, 0x57, "signature-check region");

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);
    const std::array<uint8_t, 0x90> xorbuf = expandSeed(pti->key, pti->code, nullptr);

    PrxType5 type5(inbuf);
    type5.decrypt(pti->code, pti->seed, nullptr);

    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {type5.tag, sizeof(type5.tag)},
        {xorbuf.data(), 0x10},
        {type5.empty, sizeof(type5.empty)},
        {type5.id, sizeof(type5.id)},
        {type5.kirkHeader, sizeof(type5.kirkHeader)},
        {type5.kirkMetadata, sizeof(type5.kirkMetadata)},
        {type5.prxHeader, sizeof(type5.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type5.sha1)) {
        throw std::runtime_error("decryptPrxType5: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    auto* header = reinterpret_cast<KIRK_CMD1_HEADER*>(outbuf + kOffset);
    std::memset(header, 0, sizeof(KIRK_CMD1_HEADER));
    std::memcpy(&header->data_size, type5.kirkMetadata, sizeof(type5.kirkMetadata));
    std::memcpy(reinterpret_cast<uint8_t*>(header) + sizeof(KIRK_CMD1_HEADER), type5.prxHeader, sizeof(type5.prxHeader));
    decryptKirkHeader(reinterpret_cast<uint8_t*>(header), type5.kirkHeader, xorbuf.cbegin() + 0x10, pti->code);
    header->mode = 1;

    checkWriteFits(header->data_size, size, "decryptPrxType5");
    checkReadFits(header->data_offset, header->data_size, size - kOffset, "decryptPrxType5");
    const int result = kirk.command(outbuf, static_cast<int>(size), reinterpret_cast<uint8_t*>(header), static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType5: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrxType6(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;
    constexpr uint32_t kEcdsaHeaderSize = 0x90; // sizeof KIRK_CMD1_ECDSA_HEADER
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kEcdsaHeaderSize - kPrxHeaderSize;

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType6: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo2* pti = getTagInfo2(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType6: no key for tag " + hex8(tag));
    }

    requireZero(inbuf + 0xD4, 0x38, "signature-check region");

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);
    const std::array<uint8_t, 0x90> xorbuf = expandSeed(pti->key, pti->code);

    PrxType6 type6(inbuf);
    type6.decrypt(pti->code);

    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {type6.tag, sizeof(type6.tag)},
        {xorbuf.data(), 0x10},
        {type6.empty, sizeof(type6.empty)},
        {type6.ecdsaSignatureTail, sizeof(type6.ecdsaSignatureTail)},
        {type6.id, sizeof(type6.id)},
        {type6.kirkHeader, sizeof(type6.kirkHeader)},
        {type6.kirkMetadata, sizeof(type6.kirkMetadata)},
        {type6.prxHeader, sizeof(type6.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type6.sha1)) {
        throw std::runtime_error("decryptPrxType6: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    auto* header = reinterpret_cast<KIRK_CMD1_ECDSA_HEADER*>(outbuf + kOffset);
    std::memset(header, 0, sizeof(KIRK_CMD1_ECDSA_HEADER));
    std::memcpy(outbuf + kOffset + 0x40, type6.ecdsaSignatureTail, sizeof(type6.ecdsaSignatureTail));
    std::memcpy(&header->data_size, type6.kirkMetadata, sizeof(type6.kirkMetadata));
    std::memcpy(reinterpret_cast<uint8_t*>(header) + sizeof(KIRK_CMD1_ECDSA_HEADER), type6.prxHeader, sizeof(type6.prxHeader));
    decryptKirkHeader(reinterpret_cast<uint8_t*>(header), type6.kirkHeader, xorbuf.cbegin() + 0x10, pti->code);
    header->mode = 1;
    header->ecdsa_hash = 1;

    checkWriteFits(header->data_size, size, "decryptPrxType6");
    checkReadFits(header->data_offset, header->data_size, size - kOffset, "decryptPrxType6");
    const int result = kirk.command(outbuf, static_cast<int>(size), reinterpret_cast<uint8_t*>(header), static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType6: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrxType9(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    constexpr uint32_t kHeaderSize = 0x150;
    constexpr uint32_t kEcdsaHeaderSize = 0x90;
    constexpr uint32_t kPrxHeaderSize = 0x80;
    constexpr uint32_t kOffset = kHeaderSize - kEcdsaHeaderSize - kPrxHeaderSize;

    if (size < kHeaderSize) {
        throw std::runtime_error("decryptPrxType9: block smaller than the 0x150-byte header");
    }

    const uint32_t tag = readU32LE(inbuf + 0xD0);
    const TagInfo2* pti = getTagInfo2(tag);
    if (!pti) {
        throw std::runtime_error("decryptPrxType9: no key for tag " + hex8(tag));
    }

    // Only up to 0x104 -- past that is a real ECDSA signature, not empty.
    requireZero(inbuf + 0xD4, 0x104 - 0xD4, "signature-check region");

    const int32_t decryptSize = readI32LE(inbuf + 0xB0);
    const std::array<uint8_t, 0x90> xorbuf = expandSeed(pti->key, pti->code);

    PrxType9 type9(inbuf);
    type9.decrypt(pti->code);

    const std::vector<uint8_t> computedSha1 = sha1Check(kirk, {
        {type9.tag, sizeof(type9.tag)},
        {xorbuf.data(), 0x10},
        {type9.empty, sizeof(type9.empty)},
        {type9.ecdsaSignatureTail, sizeof(type9.ecdsaSignatureTail)},
        {type9.id, sizeof(type9.id)},
        {type9.kirkHeader, sizeof(type9.kirkHeader)},
        {type9.kirkMetadata, sizeof(type9.kirkMetadata)},
        {type9.prxHeader, sizeof(type9.prxHeader)},
    });
    if (!std::equal(computedSha1.begin(), computedSha1.end(), type9.sha1)) {
        throw std::runtime_error("decryptPrxType9: SHA1 self-check failed");
    }

    if (outbuf != inbuf) {
        std::memcpy(outbuf, inbuf, size);
    }

    auto* header = reinterpret_cast<KIRK_CMD1_ECDSA_HEADER*>(outbuf + kOffset);
    std::memset(header, 0, sizeof(KIRK_CMD1_ECDSA_HEADER));
    std::memcpy(&header->data_size, type9.kirkMetadata, sizeof(type9.kirkMetadata));
    std::memcpy(reinterpret_cast<uint8_t*>(header) + sizeof(KIRK_CMD1_ECDSA_HEADER), type9.prxHeader, sizeof(type9.prxHeader));
    decryptKirkHeader(reinterpret_cast<uint8_t*>(header), type9.kirkHeader, xorbuf.cbegin() + 0x10, pti->code);
    header->mode = 1;
    // Left at 0, unlike type 6: type 9 carries a real ECDSA signature that
    // was verified separately, so KIRK is not asked to hash-check this block.
    header->ecdsa_hash = 0;

    checkWriteFits(header->data_size, size, "decryptPrxType9");
    checkReadFits(header->data_offset, header->data_size, size - kOffset, "decryptPrxType9");
    const int result = kirk.command(outbuf, static_cast<int>(size), reinterpret_cast<uint8_t*>(header), static_cast<int>(size - kOffset), KIRK_CMD_DECRYPT_PRIVATE);
    if (result != 0) {
        throw std::runtime_error("decryptPrxType9: KIRK CMD1 (DECRYPT_PRIVATE) failed, code " + std::to_string(result));
    }

    return decryptSize;
}

int32_t decryptPrx(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size) {
    if (size < 0x150) {
        throw std::runtime_error("decryptPrx: block smaller than the 0x150-byte header");
    }

    using Fn = int32_t (*)(KirkEngine&, const uint8_t*, uint8_t*, uint32_t);
    static constexpr Fn attempts[] = {
        decryptPrxType0, decryptPrxType1, decryptPrxType2,
        decryptPrxType5, decryptPrxType6, decryptPrxType9,
    };

    for (Fn attempt : attempts) {
        try {
            return attempt(kirk, inbuf, outbuf, size);
        } catch (const std::exception&) {
            // Wrong type (or tag not in our table) for this block; try the next.
        }
    }

    throw std::runtime_error("decryptPrx: no known type/tag combination matched tag " + hex8(readU32LE(inbuf + 0xD0)));
}

std::vector<uint8_t> decryptModule(KirkEngine& kirk, const std::vector<uint8_t>& rawModule) {
    if (rawModule.size() < 0x30) {
        throw std::runtime_error("decryptModule: block too small for a module header");
    }

    // comp_attribute (u16 @ 0x6) and elf_size (u32 @ 0x28) live in the
    // module's own cleartext header, which decryptPrx overwrites in place
    // with the decrypted payload -- so both must be captured before that
    // call, not after.
    const uint16_t compAttr = static_cast<uint16_t>(rawModule[0x6] | (static_cast<uint16_t>(rawModule[0x7]) << 8));
    const uint32_t elfSize = readU32LE(rawModule.data() + 0x28);
    constexpr uint16_t kCompressedFlag = 0x0001;

    // decryptPrx's callee (decryptPrxTypeN, all six) validates CMD1's own
    // data_offset+data_size against this exact size via checkReadFits
    // before ever handing control to the vendored engine -- see that
    // function's comment for why this specific field matters. This buffer
    // is a standalone extracted module with no real bytes beyond its own
    // end to lend as slack (unlike decodePsarBlock's PSAR-section slice,
    // which borrows real neighboring archive bytes for the same purpose),
    // so there is nothing to pad here: a module whose data_offset/data_size
    // would read past its own real content is genuinely invalid, and
    // checkReadFits rejects it rather than silently decrypting whatever
    // (heap-allocator-dependent) bytes happen to follow this vector.
    std::vector<uint8_t> buffer = rawModule;
    const int32_t decryptedSize = decryptPrx(kirk, buffer.data(), buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (decryptedSize <= 0 || static_cast<std::size_t>(decryptedSize) > buffer.size()) {
        throw std::runtime_error("decryptModule: decrypted size out of range");
    }
    buffer.resize(static_cast<std::size_t>(decryptedSize));

    // Sniff the decrypted payload's own magic rather than trusting
    // comp_attribute alone: an embedded module (e.g. reboot.prx, found
    // packed inside loadexec.prx rather than as its own top-level PSAR
    // entry) has been observed with a genuine "KL3E" payload but a
    // comp_attribute that doesn't set kCompressedFlag -- the header flag
    // isn't reliable for every module shape, but the magic always is.
    if (buffer.size() >= 2 && buffer[0] == 0x1F && buffer[1] == 0x8B) {
        return util::inflateGzip(buffer.data(), buffer.size(), elfSize);
    }
    if (buffer.size() >= 4 && (std::memcmp(buffer.data(), "KL4E", 4) == 0 || std::memcmp(buffer.data(), "KL3E", 4) == 0)) {
        // decompressKl4e's expectedSize is only a safe upper bound (the
        // format self-terminates via its own end marker, see kl4e.h) --
        // elfSize is usually already a generous, correct bound, but for at
        // least one real module shape (reboot.prx, embedded inside
        // loadexec.prx rather than its own top-level PSAR entry) it turned
        // out to equal the *compressed* size instead, which is far too
        // small. Padding it up against the compressed size itself covers
        // that case without changing behavior for the common one.
        const bool isKl4e = buffer[2] == '4'; // "KL4E" vs "KL3E"
        const uint32_t sizeBound = std::max(elfSize, static_cast<uint32_t>(buffer.size()) * 8u);
        return util::decompressKl4e(std::vector<uint8_t>(buffer.begin() + 4, buffer.end()), sizeBound, isKl4e);
    }
    if ((compAttr & kCompressedFlag) == 0) {
        return buffer;
    }
    throw std::runtime_error("decryptModule: compressed flag set but no recognized compression magic found");
}

} // namespace beta::crypto
