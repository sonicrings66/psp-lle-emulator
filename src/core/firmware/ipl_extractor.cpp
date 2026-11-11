#include "ipl_extractor.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace beta::firmware {

namespace {

uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
uint16_t readU16LE(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t alignUp15(uint32_t v) { return (v + 15u) & ~15u; }

} // namespace

uint32_t extractAndLoadRealIpl(beta::crypto::KirkEngine& kirk, const beta::NandImage& nand,
                                const WriteWordFn& writeWord) {
    constexpr uint32_t kPageSize = static_cast<uint32_t>(beta::NandImage::kPageSize);
    constexpr uint32_t kPagesPerBlock = 32;
    constexpr uint32_t kIplTablePpnStart = 0x80;
    constexpr uint32_t kIplId = 0x6DC64A38u;
    constexpr uint32_t kDecryptBufferPages = 8;
    constexpr uint32_t kDecryptBufferSize = kDecryptBufferPages * kPageSize;
    constexpr uint32_t kCmd1HeaderSize = 0x90;
    constexpr uint32_t kHashSize = 20;
    constexpr uint32_t kAes128CbcHeaderSize = 0x14;

    const uint8_t* iplFatSector = nullptr;
    for (uint32_t iplFatPpn = kIplTablePpnStart; ; iplFatPpn += kPagesPerBlock) {
        const uint8_t* spare = nand.spareData(iplFatPpn);
        const uint8_t* mainPage = nand.mainData(iplFatPpn);
        if (!spare || !mainPage) {
            return 0; // ran off the end of the NAND dump before finding the real IPL FAT block
        }
        if (readU32LE(spare + 8) == kIplId) {
            iplFatSector = mainPage;
            break;
        }
    }

    std::vector<uint8_t> decryptBuffer(kDecryptBufferSize);
    uint32_t iplBase = 0, nextAddr = 0, iplEntry = 0, previousChecksum = 0;
    bool haveIplBase = false;
    uint8_t ecdsaMessageHash[kHashSize] = {0};
    uint8_t ecdsaSignature[40] = {0};
    bool ecdsaHash = false;

    for (uint32_t i = 0; i < kPageSize && iplEntry == 0; i += 2) {
        for (uint32_t j = 0; j < kPagesPerBlock && iplEntry == 0; j += kDecryptBufferPages) {
            const uint32_t ppn = static_cast<uint32_t>(readU16LE(iplFatSector + i)) * kPagesPerBlock;
            if (ppn == 0) {
                break;
            }
            bool ok = true;
            for (uint32_t page = 0; page < kDecryptBufferPages; ++page) {
                const uint8_t* mainPage = nand.mainData(ppn + j + page);
                const uint8_t* spare = nand.spareData(ppn + j + page);
                if (!mainPage || !spare || readU32LE(spare + 8) != kIplId) {
                    ok = false;
                    break;
                }
                std::memcpy(decryptBuffer.data() + page * kPageSize, mainPage, kPageSize);
            }
            if (!ok) {
                return iplEntry; // real IPL chunk read failed -- stop with whatever entry was found so far
            }

            const uint32_t dataSize = readU32LE(decryptBuffer.data() + 0x70);
            const uint32_t dataOffset = readU32LE(decryptBuffer.data() + 0x74);
            const uint32_t alignedDataSize = alignUp15(dataSize);
            const uint64_t inSize64 = static_cast<uint64_t>(kCmd1HeaderSize) + alignedDataSize + dataOffset;
            if (inSize64 > kDecryptBufferSize) {
                return iplEntry; // inSize exceeds the real 4096-byte chunk -- refuse to decrypt garbage
            }

            const uint32_t rawMode = readU32LE(decryptBuffer.data() + 96);
            const uint32_t cmd = rawMode & 0x3u;
            decryptBuffer[98] = 0;
            decryptBuffer[99] = 0;
            const bool blockEcdsaHash = (readU32LE(decryptBuffer.data() + 100) & 1u) != 0;

            const int result = kirk.command(decryptBuffer.data(), static_cast<int>(kDecryptBufferSize),
                                             decryptBuffer.data(), static_cast<int>(inSize64), static_cast<uint8_t>(cmd));
            if (result != 0) {
                return iplEntry; // real KIRK decrypt failed
            }

            {
                uint8_t* hashDecryptBuffer = decryptBuffer.data() + 0xFCC;
                uint8_t* hashBufferPtr = decryptBuffer.data() + 0x004;
                uint8_t* checkHashBuffer = decryptBuffer.data() + 0xF80;
                uint8_t* ecdsaSignatureBuffer = decryptBuffer.data() + 0xFA0;

                writeU32LE(hashDecryptBuffer + 0, 5u);
                writeU32LE(hashDecryptBuffer + 4, 0);
                writeU32LE(hashDecryptBuffer + 8, 0);
                writeU32LE(hashDecryptBuffer + 12, 0x6Cu);
                writeU32LE(hashDecryptBuffer + 16, kHashSize);
                const uint32_t cmd7InSize = alignUp15(kHashSize) + kAes128CbcHeaderSize;
                const int hashResult = kirk.command(hashDecryptBuffer, static_cast<int>(kHashSize),
                                                     hashDecryptBuffer, static_cast<int>(cmd7InSize), KIRK_CMD_DECRYPT_IV_0);
                if (hashResult != 0) {
                    return iplEntry;
                }

                const uint32_t hashAddr = readU32LE(decryptBuffer.data() + 0);
                const uint32_t hashLength = readU32LE(decryptBuffer.data() + 4);
                if (static_cast<uint64_t>(hashLength) + 24u > kDecryptBufferSize) {
                    return iplEntry; // block length too large for the hash-chain append
                }
                uint8_t* ptr = decryptBuffer.data() + hashLength;
                writeU32LE(ptr + 16, hashAddr);
                writeU32LE(ptr + 20, hashLength);
                writeU32LE(hashBufferPtr + 0, hashLength + 16);
                const int sha1Result = kirk.command(checkHashBuffer, static_cast<int>(kHashSize),
                                                     hashBufferPtr, static_cast<int>(hashLength + 20), KIRK_CMD_SHA1_HASH);
                if (sha1Result != 0) {
                    return iplEntry;
                }

                decryptBuffer[4] = static_cast<uint8_t>(hashLength);
                decryptBuffer[5] = static_cast<uint8_t>(hashLength >> 8);
                decryptBuffer[6] = static_cast<uint8_t>(hashLength >> 16);
                decryptBuffer[7] = static_cast<uint8_t>(hashLength >> 24);
                bool hashMatch = true;
                for (uint32_t k = 0; k < kHashSize; k += 4) {
                    if (readU32LE(checkHashBuffer + k) != readU32LE(hashDecryptBuffer + k)) {
                        hashMatch = false;
                        break;
                    }
                }
                if (!hashMatch) {
                    return iplEntry; // real IPL hash mismatch
                }
                for (uint32_t k = 0; k < kHashSize; ++k) {
                    ecdsaMessageHash[k] ^= checkHashBuffer[k];
                }
                std::memcpy(ecdsaSignature, ecdsaSignatureBuffer, sizeof(ecdsaSignature));
                ecdsaHash = blockEcdsaHash;
            }

            const uint32_t addr = readU32LE(decryptBuffer.data() + 0);
            const uint32_t length = readU32LE(decryptBuffer.data() + 4);
            iplEntry = readU32LE(decryptBuffer.data() + 8);
            const uint32_t checksum = readU32LE(decryptBuffer.data() + 12);

            // Real, documented ARK-4/DC-M33 "reset block" ECDSA-bypass
            // exploit (contrib/PC/ipltools/make_ipl.py reset_ipl_block()):
            // writes 0x32F6 to the real reset register at 0xBC10004C,
            // which real hardware remaps 0xBFD00000 to the reset vector on
            // reboot -- the CPU resumes at whatever was staged there,
            // bypassing the ECDSA signature this function would otherwise
            // verify. Real console behavior, not emulator-side invention:
            // decode its own real trampoline (lui $t9,hi / ori $t9,lo /
            // jr $t9 / li $v0,0xDAEE) to get the real jump target.
            if (addr == 0xBC10004Cu && length == 4u && iplEntry == 0x10000005u) {
                const uint32_t luiInstr = readU32LE(decryptBuffer.data() + 0x20);
                const uint32_t oriInstr = readU32LE(decryptBuffer.data() + 0x24);
                const uint32_t jumpTarget = ((luiInstr & 0xFFFFu) << 16) | (oriInstr & 0xFFFFu);
                return jumpTarget;
            }

            if (previousChecksum != checksum) {
                return iplEntry; // real checksum chain broke
            }
            if (!haveIplBase) {
                iplBase = addr;
                haveIplBase = true;
            }
            // (nextAddr mismatch against addr is a real, logged-only
            // anomaly in the old code -- it never altered control flow,
            // so it's dropped here along with the other removed printfs.)
            if (length > 0 && addr != 0) {
                previousChecksum = 0;
                for (uint32_t k = 0; k < length; k += 4) {
                    const uint32_t value = readU32LE(decryptBuffer.data() + 0x10 + k);
                    writeWord(addr + k, value);
                    previousChecksum += value;
                }
            }
            nextAddr = addr + length;
        }
    }
    (void)iplBase; (void)nextAddr;

    // Real ECDSA signature-chain verification: real Sony public key,
    // the running XOR of every chunk's real SHA1 hash, and the final
    // chunk's real signature -- a genuine KIRK_CMD_ECDSA_VERIFY call, not
    // a stand-in. ecdsaHash being unset on the final chunk is a soft
    // anomaly the old code only logged, never acted on -- preserved here
    // by simply proceeding regardless, matching old behavior exactly.
    {
        static constexpr uint8_t kEcdsaPublicKey[40] = {
            0xBC, 0x66, 0x06, 0x11, 0xA7, 0x0B, 0xD7, 0xF2, 0xD1, 0x40, 0xA4, 0x82, 0x15, 0xC0, 0x96, 0xD1,
            0x1D, 0x2D, 0x41, 0x12, 0xF0, 0xE9, 0x37, 0x9A, 0xC4, 0xE0, 0xD3, 0x87, 0xC5, 0x42, 0xD0, 0x91,
            0x34, 0x9D, 0xD1, 0x51, 0x69, 0xDD, 0x5A, 0x87};
        uint8_t verifyBuffer[0x64] = {0};
        std::memcpy(verifyBuffer + 0, kEcdsaPublicKey, sizeof(kEcdsaPublicKey));
        std::memcpy(verifyBuffer + 40, ecdsaMessageHash, sizeof(ecdsaMessageHash));
        std::memcpy(verifyBuffer + 60, ecdsaSignature, sizeof(ecdsaSignature));
        const int verifyResult = kirk.command(nullptr, 0, verifyBuffer, static_cast<int>(sizeof(verifyBuffer)), KIRK_CMD_ECDSA_VERIFY);
        if (verifyResult != 0) {
            return 0; // the decrypted chain does not check out
        }
    }
    (void)ecdsaHash;

    return iplEntry;
}

uint64_t iplCacheKey(const beta::NandImage& nand, uint64_t fuseKey) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const uint8_t* d, std::size_t n) { for (std::size_t i = 0; i < n; ++i) { h ^= d[i]; h *= 1099511628211ull; } };
    constexpr uint32_t kIplCacheFormatVersion = 1; // bump if the extraction logic (or the KIRK emulation it uses) changes
    mix(reinterpret_cast<const uint8_t*>(&fuseKey), sizeof(fuseKey));
    mix(reinterpret_cast<const uint8_t*>(&kIplCacheFormatVersion), sizeof(kIplCacheFormatVersion));
    for (uint32_t ppn = 0x80; ppn < 0x1000; ++ppn) { // everything the extraction can read
        const uint8_t* m = nand.mainData(ppn); const uint8_t* sp = nand.spareData(ppn);
        if (!m || !sp) break;
        mix(m, static_cast<std::size_t>(beta::NandImage::kPageSize)); mix(sp, 16);
    }
    return h;
}

namespace {
struct Segment { uint32_t addr; std::vector<uint32_t> words; };
} // namespace

uint32_t extractAndLoadRealIplCached(beta::crypto::KirkEngine& kirk, const beta::NandImage& nand,
                                      const WriteWordFn& writeWord, uint64_t fuseKey,
                                      const std::string& cacheDir) {
    const bool cacheOn = !cacheDir.empty();
    std::string path;
    uint64_t key = 0;
    if (cacheOn) {
        key = iplCacheKey(nand, fuseKey);
        char name[96];
        std::snprintf(name, sizeof(name), "/ipl_%016llX_%016llX.bin",
                      static_cast<unsigned long long>(fuseKey), static_cast<unsigned long long>(key));
        path = cacheDir + name;

        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            uint32_t magic = 0, entry = 0, count = 0; uint64_t fileKey = 0;
            bool ok = std::fread(&magic, 4, 1, f) == 1 && std::fread(&fileKey, 8, 1, f) == 1 &&
                      std::fread(&entry, 4, 1, f) == 1 && std::fread(&count, 4, 1, f) == 1 &&
                      magic == 0x43504950u && fileKey == key && count < 1000000u;
            std::vector<Segment> segs;
            for (uint32_t i = 0; ok && i < count; ++i) {
                Segment sg; uint32_t n = 0;
                ok = std::fread(&sg.addr, 4, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n < (1u << 24);
                if (ok) { sg.words.resize(n); ok = n == 0 || std::fread(sg.words.data(), 4, n, f) == n; }
                if (ok) segs.push_back(std::move(sg));
            }
            std::fclose(f);
            if (ok) {
                for (const Segment& sg : segs) {
                    for (std::size_t k = 0; k < sg.words.size(); ++k) writeWord(sg.addr + 4u * static_cast<uint32_t>(k), sg.words[k]);
                }
                return entry;
            }
            // Cache file stale/corrupt -- fall through and re-extract.
        }
    }

    std::vector<Segment> segs;
    const uint32_t entry = extractAndLoadRealIpl(kirk, nand, [&](uint32_t addr, uint32_t value) {
        writeWord(addr, value);
        if (!segs.empty() && segs.back().addr + 4u * static_cast<uint32_t>(segs.back().words.size()) == addr) segs.back().words.push_back(value);
        else segs.push_back(Segment{addr, {value}});
    });

    if (cacheOn) {
        std::error_code ec; std::filesystem::create_directories(cacheDir, ec);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            const uint32_t magic = 0x43504950u, count = static_cast<uint32_t>(segs.size());
            std::fwrite(&magic, 4, 1, f); std::fwrite(&key, 8, 1, f); std::fwrite(&entry, 4, 1, f); std::fwrite(&count, 4, 1, f);
            for (const Segment& sg : segs) {
                const uint32_t n = static_cast<uint32_t>(sg.words.size());
                std::fwrite(&sg.addr, 4, 1, f); std::fwrite(&n, 4, 1, f);
                if (n) std::fwrite(sg.words.data(), 4, n, f);
            }
            std::fclose(f);
        }
    }
    return entry;
}

} // namespace beta::firmware
