#pragma once
// Minimal PNG writer for framebuffer dumps: 8-bit RGBA, zlib "stored" (uncompressed) deflate blocks,
// so it needs no compression library. Files are large-ish (about 0.5 MB for 480x272) but every viewer opens them.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pngw {

inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24)); v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));  v.push_back(static_cast<uint8_t>(x));
}

inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    put32be(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32be(out, crc32(body.data(), body.size()));
}

// rgba: width*height*4 bytes, row-major, top row first.
inline bool writeRgba(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve((width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        raw.insert(raw.end(), rgba.begin() + static_cast<size_t>(y) * width * 4, rgba.begin() + static_cast<size_t>(y + 1) * width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF)); z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF)); z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    put32be(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, width); put32be(ihdr, height);
    ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

} // namespace pngw
