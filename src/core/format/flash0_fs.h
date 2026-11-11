#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../util/nand_image.h"

namespace beta::format {

// Read-only view of the FAT12 volume "flash0:" inside a raw PSP NAND dump (528-byte page+spare records).
// Ported from jpcsp (sceNand.java descramblePage / MMIOHandlerNand.getScramble*) and uofw lowio/nand.c:
//  - logical block n lives in the physical block whose spare bytes 6..7 (big endian) equal n (spare[5]==0xFF);
//  - flash0 starts at lbn 2; lbn 3 is the FAT boot sector, its sector 0 scrambled with (fuseLo ^ rol(fuseHi,0)),
//    every later sector with (fuseLo ^ ror(fuseHi,0) ^ 0x556D81FE) (6.60+ constant), physical ppn as the page key.
// This is the real-hardware storage path: no HLE, only the console's own fuse ID decides whether it decodes.
class Flash0Fs {
public:
    Flash0Fs(const NandImage& nand, uint32_t fuseLo, uint32_t fuseHi) : nand_(nand), fuseLo_(fuseLo), fuseHi_(fuseHi) {
        for (uint32_t b = 0x40; b < nand.pageCount() / kPagesPerBlock; ++b) {
            const uint8_t* sp = nand.spareData(b * kPagesPerBlock);
            if (!sp || sp[5] != 0xFF) continue;
            bool allFF = true;
            for (int i = 0; i < 16; ++i) if (sp[i] != 0xFF) { allFF = false; break; }
            if (allFF) continue;
            lbnToBlock_[static_cast<uint16_t>((sp[6] << 8) | sp[7])] = b;
        }
        valid_ = parseBoot();
    }

    bool valid() const { return valid_; }

    // Every regular file as ("flash0:/kd/foo.prx", bytes).
    std::vector<std::pair<std::string, std::vector<uint8_t>>> allFiles() const {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
        if (valid_) walk(0, "flash0:", out);
        return out;
    }

private:
    static constexpr uint32_t kPagesPerBlock = 32;
    static constexpr uint32_t kFlash0Lbn = 2;
    const NandImage& nand_;
    uint32_t fuseLo_, fuseHi_;
    std::map<uint16_t, uint32_t> lbnToBlock_;
    bool valid_ = false;
    uint32_t spc_ = 0, res_ = 0, nfat_ = 0, spf_ = 0, rootEnt_ = 0, rootSec_ = 0, data0_ = 0;
    std::vector<uint8_t> fat_;

    static uint32_t ror(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
    static uint32_t rol(uint32_t x, unsigned n) { return ror(x, 32 - (n & 31)); }
    static uint32_t rev32(uint32_t x) {
        uint32_t r = 0;
        for (int i = 0; i < 32; ++i) { r = (r << 1) | (x & 1); x >>= 1; }
        return r;
    }

    static void descramble(uint32_t scramble, uint32_t ppn, const uint8_t* src, uint8_t* dst) {
        uint32_t in[128], out[128];
        std::memcpy(in, src, 512);
        const uint32_t scrmb = ror(scramble, 21);
        uint32_t key = ror(ppn, 17) ^ (scrmb * 7u);
        uint32_t off = (((ppn ^ scrmb) & 0x1Fu) << 4) >> 2;
        for (int i = 0; i < 128;) {
            uint32_t v0 = in[off], v1 = in[off + 1], v2 = in[off + 2], v3 = in[off + 3];
            off += 4;
            if (off >= 128) off -= 128;
            v0 -= key; key += v0; out[i++] = v0;
            v1 -= key; key ^= v1; out[i++] = v1;
            v2 -= key; key -= v2; out[i++] = v2;
            v3 -= key; key += v3; out[i++] = v3;
            key += scrmb;
            key = rev32(key);
        }
        std::memcpy(dst, out, 512);
    }

    std::vector<uint8_t> sector(uint32_t n) const {
        std::vector<uint8_t> s(512, 0xFF);
        const uint32_t lbn = kFlash0Lbn + 1 + n / kPagesPerBlock;
        auto it = lbnToBlock_.find(static_cast<uint16_t>(lbn));
        if (it == lbnToBlock_.end()) return s;
        const uint32_t ppn = it->second * kPagesPerBlock + n % kPagesPerBlock;
        const uint8_t* raw = nand_.mainData(ppn);
        if (!raw) return s;
        const uint32_t sc = (n == 0) ? (fuseLo_ ^ rol(fuseHi_, 0)) : (fuseLo_ ^ ror(fuseHi_, 0) ^ 0x556D81FEu);
        descramble(sc, ppn, raw, s.data());
        return s;
    }

    static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
    static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

    bool parseBoot() {
        const auto bs = sector(0);
        if (bs[510] != 0x55 || bs[511] != 0xAA || le16(&bs[11]) != 512) return false;
        spc_ = bs[13]; res_ = le16(&bs[14]); nfat_ = bs[16]; rootEnt_ = le16(&bs[17]); spf_ = le16(&bs[22]);
        if (!spc_ || !spf_) return false;
        for (uint32_t i = 0; i < spf_; ++i) { auto s = sector(res_ + i); fat_.insert(fat_.end(), s.begin(), s.end()); }
        rootSec_ = res_ + nfat_ * spf_;
        data0_ = rootSec_ + rootEnt_ * 32 / 512;
        return true;
    }

    uint32_t fatNext(uint32_t c) const {
        const size_t o = c + c / 2;
        if (o + 1 >= fat_.size()) return 0xFFF;
        const uint32_t v = le16(&fat_[o]);
        return (c & 1) ? (v >> 4) : (v & 0xFFF);
    }

    std::vector<uint8_t> readChain(uint32_t c, size_t limit) const {
        std::vector<uint8_t> out;
        int guard = 0;
        while (c >= 2 && c < 0xFF8 && out.size() < limit && guard++ < 100000) {
            for (uint32_t i = 0; i < spc_; ++i) { auto s = sector(data0_ + (c - 2) * spc_ + i); out.insert(out.end(), s.begin(), s.end()); }
            c = fatNext(c);
        }
        if (out.size() > limit) out.resize(limit);
        return out;
    }

    void walk(uint32_t cluster, const std::string& path, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out) const {
        std::vector<uint8_t> dir;
        if (cluster == 0) {
            for (uint32_t i = 0; i < rootEnt_ * 32 / 512; ++i) { auto s = sector(rootSec_ + i); dir.insert(dir.end(), s.begin(), s.end()); }
        } else {
            dir = readChain(cluster, 1u << 24);
        }
        std::u16string lfn;
        for (size_t j = 0; j + 32 <= dir.size(); j += 32) {
            const uint8_t* e = &dir[j];
            if (e[0] == 0) break;
            if (e[0] == 0xE5) { lfn.clear(); continue; }
            if (e[11] == 0x0F) {
                char16_t part[13];
                const int idx[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int k = 0; k < 13; ++k) part[k] = static_cast<char16_t>(e[idx[k]] | (e[idx[k] + 1] << 8));
                std::u16string seg;
                for (int k = 0; k < 13 && part[k] != 0 && part[k] != 0xFFFF; ++k) seg.push_back(part[k]);
                lfn = seg + lfn;
                continue;
            }
            std::string name;
            if (!lfn.empty()) { for (char16_t ch : lfn) name.push_back(static_cast<char>(ch)); }
            else {
                std::string n(reinterpret_cast<const char*>(e), 8), x(reinterpret_cast<const char*>(e + 8), 3);
                while (!n.empty() && n.back() == ' ') n.pop_back();
                while (!x.empty() && x.back() == ' ') x.pop_back();
                name = n + (x.empty() ? "" : "." + x);
            }
            lfn.clear();
            if (e[11] & 0x08) continue; // volume label
            if (name == "." || name == "..") continue;
            const uint32_t cl = le16(e + 26), size = le32(e + 28);
            if (e[11] & 0x10) walk(cl, path + "/" + name, out);
            else out.emplace_back(path + "/" + name, readChain(cl, size));
        }
    }
};

} // namespace beta::format
