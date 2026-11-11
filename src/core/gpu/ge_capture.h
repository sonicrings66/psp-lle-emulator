#pragma once
// GE capture: records everything the GE does so it can be replayed without the guest CPU (docs/GPU_DESIGN.md, phase 0).
//
// What is recorded, in order, as a flat event stream:
//   STATE  the renderer's whole logical state at capture start (GeRenderer::exportState)
//   PAGE   a 4 KB page of emulated memory (physical address): its content BEFORE the GE first touched it (a snapshot), or its new content
//          after something other than the GE changed it between display-list runs (a patch)
//   CMD    one command word, exactly as GeCoreStub hands it to GeRenderer::command() (jumps/calls/returns are already resolved away)
//   CHECK  at the end of every list: a hash of the frame buffer region as the live run left it (the replay must reproduce it byte for byte)
//
// How memory is observed: while a capture is active the renderer's direct host windows are switched off (GeRenderer::beginObservedMemory),
// so every read and write goes through the callbacks below. The first access to a page snapshots it (BEFORE a write is applied, so a
// partially written page still replays correctly); the recorder keeps a shadow copy of every page it has seen and updates it with the GE's
// own writes, so a difference found at the start of the next list run can only have been written by the CPU, a DMA or the display hardware,
// and is logged as a patch. A command's PAGE events are appended before its CMD event, which is the order a replay applies them in.
//
// File format (little endian): header { "PSPGECAP", u32 version, u32 vramBase, u32 vramSize, u32 ramBase, u32 ramSize }, then events:
//   1 PAGE: u32 phys, 4096 bytes | 2 CMD: u32 word | 3 CHECK: u32 fbPhys, u32 bytes, u64 hash | 4 STATE: u32 size, bytes.
#include "ge_renderer.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace beta {

class GeCapture {
public:
    static constexpr uint32_t kPage = 4096, kVersion = 1;
    enum EventType : uint8_t { kEvPage = 1, kEvCmd = 2, kEvCheck = 3, kEvState = 4 };

    // `orig`: the real memory callbacks; wins: the real host windows (used for fast bulk reads: snapshot, change detection, hashing).
    GeCapture(const GeRenderer::WinInfo* wins, int nWins, GeMemory orig) : orig_(std::move(orig)) { for (int i = 0; i < nWins; ++i) wins_.push_back(wins[i]); }

    // Memory callbacks that observe every access and forward it to the real memory.
    GeMemory wrap() {
        GeMemory m;
        m.r8 = [this](uint32_t a) { touch(a, 1); return orig_.r8(a); };
        m.r16 = [this](uint32_t a) { touch(a, 2); return orig_.r16(a); };
        m.r32 = [this](uint32_t a) { touch(a, 4); return orig_.r32(a); };
        m.w8 = [this](uint32_t a, uint8_t v) { touch(a, 1); orig_.w8(a, v); shadowWrite(a, &v, 1); };
        m.w16 = [this](uint32_t a, uint16_t v) { touch(a, 2); orig_.w16(a, v); shadowWrite(a, &v, 2); };
        m.w32 = [this](uint32_t a, uint32_t v) { touch(a, 4); orig_.w32(a, v); shadowWrite(a, &v, 4); };
        return m;
    }

    void begin(const std::vector<uint8_t>& rendererState) {
        out_.clear(); shadow_.clear(); cmds_ = lists_ = pagesLogged_ = patches_ = 0;
        const char magic[8] = {'P', 'S', 'P', 'G', 'E', 'C', 'A', 'P'};
        out_.insert(out_.end(), magic, magic + 8);
        put32(kVersion);
        for (const GeRenderer::WinInfo& w : wins_) { put32(w.base); put32(w.size); }
        for (size_t i = wins_.size(); i < 2; ++i) { put32(0); put32(0); }
        out_.push_back(kEvState); put32(static_cast<uint32_t>(rendererState.size())); out_.insert(out_.end(), rendererState.begin(), rendererState.end());
    }

    // Called at the start of every display-list run: logs a patch for each known page that changed since the GE last saw it.
    void onRunBegin() {
        std::vector<uint8_t> cur(kPage);
        for (auto& kv : shadow_) {
            readPhys(kv.first, cur.data(), kPage);
            if (std::memcmp(cur.data(), kv.second.data(), kPage) != 0) { kv.second = cur; logPage(kv.first, cur.data()); ++patches_; }
        }
    }
    // Called after the renderer executed a command (so the command's own PAGE events precede it in the stream).
    void afterCommand(uint32_t word) { out_.push_back(kEvCmd); put32(word); ++cmds_; }
    // Called when a list ends: the frame buffer region [fbPhys, fbPhys + bytes) as the live run left it.
    void onListEnd(uint32_t fbPhys, uint32_t bytes) {
        out_.push_back(kEvCheck); put32(fbPhys); put32(bytes); put64(hashRegion(fbPhys, bytes)); ++lists_;
    }

    bool save(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(out_.data(), 1, out_.size(), f) == out_.size();
        std::fclose(f);
        return ok;
    }

    static uint64_t hashBytes(const uint8_t* p, size_t n) { // FNV-1a over 64-bit words (n is a multiple of 8 in practice; a tail is folded byte-wise)
        uint64_t h = 1469598103934665603ull; size_t i = 0;
        for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); h ^= w; h *= 1099511628211ull; }
        for (; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    uint64_t commands() const { return cmds_; }
    uint64_t lists() const { return lists_; }
    uint64_t pagesLogged() const { return pagesLogged_; }
    uint64_t patches() const { return patches_; }
    size_t bytes() const { return out_.size(); }
    size_t knownPages() const { return shadow_.size(); }

private:
    void put32(uint32_t v) { for (int i = 0; i < 4; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void put64(uint64_t v) { for (int i = 0; i < 8; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    static uint32_t phys(uint32_t a) { return a & 0x1FFFFFFFu; }

    // Reads `n` bytes of real memory at physical address p (host window when one covers it, else through the real callbacks).
    void readPhys(uint32_t p, uint8_t* dst, size_t n) const {
        for (const GeRenderer::WinInfo& w : wins_) {
            const uint32_t off = p - w.base;
            if (w.p && off < w.size && w.size - off >= n) { std::memcpy(dst, w.p + off, n); return; }
        }
        for (size_t i = 0; i < n; ++i) dst[i] = orig_.r8(p + static_cast<uint32_t>(i));
    }
    uint64_t hashRegion(uint32_t p, uint32_t n) const {
        std::vector<uint8_t> buf(n);
        readPhys(p, buf.data(), n);
        return hashBytes(buf.data(), n);
    }
    void logPage(uint32_t pagePhys, const uint8_t* data) { out_.push_back(kEvPage); put32(pagePhys); out_.insert(out_.end(), data, data + kPage); ++pagesLogged_; }

    // First access to a page: snapshot its content as it is right now (before this access takes effect).
    void touch(uint32_t a, uint32_t n) {
        const uint32_t p0 = phys(a) & ~(kPage - 1u), p1 = phys(a + n - 1u) & ~(kPage - 1u);
        for (uint32_t pg = p0;; pg += kPage) {
            if (shadow_.find(pg) == shadow_.end()) {
                std::vector<uint8_t> data(kPage);
                readPhys(pg, data.data(), kPage);
                logPage(pg, data.data());
                shadow_.emplace(pg, std::move(data));
            }
            if (pg == p1) break;
        }
    }
    // The GE's own write: keep the shadow equal to memory so it is not mistaken for an external change at the next run.
    void shadowWrite(uint32_t a, const void* v, uint32_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(v);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t p = phys(a + i);
            auto it = shadow_.find(p & ~(kPage - 1u));
            if (it != shadow_.end()) it->second[p & (kPage - 1u)] = b[i];
        }
    }

    GeMemory orig_;
    std::vector<GeRenderer::WinInfo> wins_;
    std::vector<uint8_t> out_;
    std::unordered_map<uint32_t, std::vector<uint8_t>> shadow_;
    uint64_t cmds_ = 0, lists_ = 0, pagesLogged_ = 0, patches_ = 0;
};

} // namespace beta
