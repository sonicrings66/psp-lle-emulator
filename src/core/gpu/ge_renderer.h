#pragma once
// Software renderer for the PSP Graphics Engine (GE) command stream. MINIMAL and deliberately simple:
//  - through-mode (2D, pre-transformed) points/lines/triangles/rectangles, clear mode, texturing (all colour and
//    CLUT formats, swizzle), texture functions, alpha blending, alpha test, scissor, colour/alpha masks, block
//    transfer;
//  - transform-mode (3D) primitives are counted but not drawn yet; depth/stencil/fog/lighting are ignored.
// The GE's state registers and command numbering follow the public PSP GE documentation and PPSSPP's GeCommands.h
// (which is how uofw/jpcsp identify the same commands); the rasteriser itself is written from scratch here.
// Rendering is an HLE stand-in for the real graphics hardware, not firmware code.

#include <climits>
#include <chrono>
#include <emmintrin.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <map>
#include <vector>

#include "../util/png_writer.h"
#include "../../config/options.h"

// Opt-in GPU acceleration (Windows/D3D11 only, matching frontend/live_window.h's own #ifdef BETA_GE_GPU_ACCEL pattern) -- see attachGpu() below.
// Guarded so this file keeps compiling standalone (no D3D11 dependency) unless GPU support is actually wired in.
#ifdef BETA_GE_GPU_ACCEL
#include "gpu_ge_renderer.h"
#include "ge_texture_decode.h"
#include "ge_gpu.h"
#endif

#ifdef GE_PROFILE_NOINLINE
#define GE_NI __attribute__((noinline))
#else
#define GE_NI
#endif
namespace beta {

struct GeMemory {
    std::function<uint8_t(uint32_t)> r8;
    std::function<uint16_t(uint32_t)> r16;
    std::function<uint32_t(uint32_t)> r32;
    std::function<void(uint32_t, uint8_t)> w8;
    std::function<void(uint32_t, uint16_t)> w16;
    std::function<void(uint32_t, uint32_t)> w32;
};

// The eye-space normal used by environment mapping (texture map mode 2). `world` and `view` are the 4x3 matrices (three rows of the 3x3 part, then the translation; row-vector
// convention v' = v * M). inverseTranspose = true transforms the normal by the inverse transpose of the model-view 3x3, which is the standard rule and what jpcsp does; false
// multiplies by the matrix itself (PPSSPP's choice). The two agree when the matrix is a rotation with a uniform scale (the XMB's own wave); they differ for a stretched matrix, like the
// music player's "wave" visualiser (found 2026-10-14: the direct form pushed almost every normal to face straight toward or away from the viewer, leaving only thin lit lines).
inline void geEyeNormal(const float* world, const float* view, bool inverseTranspose, float nx, float ny, float nz, float* out) {
    auto mul3 = [](const float* a, const float* b, float* o) { for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]; };
    float mv[9]; mul3(world, view, mv);
    if (inverseTranspose) {
        const float c00 = mv[4] * mv[8] - mv[5] * mv[7], c01 = mv[5] * mv[6] - mv[3] * mv[8], c02 = mv[3] * mv[7] - mv[4] * mv[6];
        const float det = mv[0] * c00 + mv[1] * c01 + mv[2] * c02;
        if (std::fabs(det) > 1e-9f) {   // row vector: n'_j = sum_i n_i * cof_ij / det (the cofactor matrix over the determinant is the inverse transpose)
            const float cof[9] = {c00, c01, c02, mv[2] * mv[7] - mv[1] * mv[8], mv[0] * mv[8] - mv[2] * mv[6], mv[1] * mv[6] - mv[0] * mv[7], mv[1] * mv[5] - mv[2] * mv[4], mv[2] * mv[3] - mv[0] * mv[5], mv[0] * mv[4] - mv[1] * mv[3]};
            const float inv = 1.f / det;
            out[0] = (cof[0] * nx + cof[3] * ny + cof[6] * nz) * inv; out[1] = (cof[1] * nx + cof[4] * ny + cof[7] * nz) * inv; out[2] = (cof[2] * nx + cof[5] * ny + cof[8] * nz) * inv;
            return;
        }
    }
    out[0] = nx * mv[0] + ny * mv[3] + nz * mv[6]; out[1] = nx * mv[1] + ny * mv[4] + nz * mv[7]; out[2] = nx * mv[2] + ny * mv[5] + nz * mv[8];
}
class GeRenderer {
public:
    // GeOptions::normalMatInverseTranspose (was PSP_GE_NORMALMAT=0 selecting the plain
    // matrix, the behaviour before 2026-10-14; anything else, or unset, the inverse
    // transpose), GeOptions::analyticNormals (was PSP_GE_ANALYTICN=0 restoring the old
    // forward-difference spline normals), and every other per-instance toggle below are
    // now threaded through the constructor instead of read via std::getenv() directly --
    // see the project's own "no getenv() outside options_cli.cpp" rule (README.md).
    explicit GeRenderer(const config::GeOptions& opts = {})
        : normalMatInvT_(opts.normalMatInverseTranspose),
          analyticNormalsOn_(opts.analyticNormals),
          recentPrimsOn_(opts.recentPrims),
          texDumpRaw_(opts.texDumpRaw),
          patchDebug_(opts.patchDebug),
          recentMax_(opts.recentMax ? opts.recentMax : 40u),
          texDumpDir_(opts.texDumpDir),
          xfBoxFrom_(opts.xfBoxFrom),
          pool_(opts.threads) {
        stats_.on = opts.stats;
    }
    void attach(GeMemory mem) { mem_ = std::move(mem); }
    // Direct access windows (RAM / VRAM by physical address). Anything outside them still goes through the callbacks. Speed: the rasteriser touches
    // memory for every pixel and texel, and the std::function round trip used to dominate.
    void attachWindow(int slot, uint8_t* data, uint32_t physBase, uint32_t size) { if (slot >= 0 && slot < 2) win_[slot] = {data, physBase, size}; }

    // ---- capture / replay support (see ge_capture.h and docs/GPU_DESIGN.md phase 0) ----------------------------------------------------
    struct WinInfo { uint8_t* p; uint32_t base, size; };
    WinInfo windowInfo(int slot) const { return {win_[slot].p, win_[slot].base, win_[slot].size}; }
    GeMemory memory() const { return mem_; }
    // Routes EVERY memory access through `m` (the direct host windows are switched off so nothing can bypass it) until endObservedMemory().
    // Slower, and the rasteriser takes its scalar callback paths (bit-identical to the fast ones); used only while a capture is being recorded.
    void beginObservedMemory(GeMemory m) { if (observed_) return; savedMem_ = mem_; savedWin_[0] = win_[0]; savedWin_[1] = win_[1]; mem_ = std::move(m); win_[0] = Win{}; win_[1] = Win{}; observed_ = true; }
    void endObservedMemory() { if (!observed_) return; mem_ = savedMem_; win_[0] = savedWin_[0]; win_[1] = savedWin_[1]; observed_ = false; }
    // The renderer's whole logical state (command registers, base/vertex/index addresses, the loaded CLUT, the four matrices and their write
    // indices). A replay imports this so it starts from exactly the state the capture started in.
    void exportState(std::vector<uint8_t>& out) const {
        out.clear();
        auto put = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
        const uint32_t magic = 0x54534547u; // "GEST"
        put(&magic, 4); put(state_, sizeof(state_)); put(&base_, 4); put(&vaddr_, 4); put(&iaddr_, 4); put(clut_, sizeof(clut_));
        put(world_, sizeof(world_)); put(view_, sizeof(view_)); put(tgen_, sizeof(tgen_)); put(proj_, sizeof(proj_));
        put(&worldIdx_, 4); put(&viewIdx_, 4); put(&projIdx_, 4); put(&tgenIdx_, 4); const uint8_t cp = colorPresent_ ? 1 : 0; put(&cp, 1);
    }
    bool importState(const uint8_t* p, size_t n) {
        size_t o = 0;
        auto get = [&](void* d, size_t k) { if (o + k > n) return false; std::memcpy(d, p + o, k); o += k; return true; };
        uint32_t magic = 0;
        if (!get(&magic, 4) || magic != 0x54534547u) return false;
        uint8_t cp = 0;
        const bool ok = get(state_, sizeof(state_)) && get(&base_, 4) && get(&vaddr_, 4) && get(&iaddr_, 4) && get(clut_, sizeof(clut_)) && get(world_, sizeof(world_)) &&
                        get(view_, sizeof(view_)) && get(tgen_, sizeof(tgen_)) && get(proj_, sizeof(proj_)) && get(&worldIdx_, 4) && get(&viewIdx_, 4) && get(&projIdx_, 4) &&
                        get(&tgenIdx_, 4) && get(&cp, 1);
        colorPresent_ = cp != 0;
#ifdef BETA_GE_GPU_ACCEL
        clutHashValid_ = false;
#endif
        return ok;
    }

#ifdef BETA_GE_GPU_ACCEL
    // Opt-in GPU acceleration for through-mode triangles (PRIM types 3/4/5 -- plain lists/strips/fans; points, lines, rects, and all of
    // transform-mode stay on the software path for now). OFF by default (gpu_ == nullptr): pass an already-`init()`-ed GpuGeRenderer
    // to opt in, matching gpu_ge_renderer.h's own proven feature set exactly. Every state combination that renderer doesn't cover yet
    // (clear mode, an exotic texture/blend mode, a non-standard 480-wide framebuffer) falls back to the existing, completely unmodified
    // software path for that one PRIM -- attaching a GPU renderer changes nothing else about this class's behaviour or defaults.
    // A real, known cost for now: each GPU-accelerated PRIM does a full CPU<->GPU 480x272 framebuffer sync around it (see
    // gpuSyncFramebufferToGpu/FromGpu below), needed for correctness (so blending, and content drawn since the last GPU draw, is
    // visible) before any batching-across-PRIMs optimisation is attempted.
    void attachGpu(GpuGeRenderer* gpu) { gpu_ = gpu; }

    // GPU-resident surface renderer (ge_gpu.h, docs/GPU_DESIGN.md phase 1). OFF by default (ggpu_ == nullptr). When attached, transform-mode
    // batches and through-mode triangles/sprites whose draw state it can reproduce are drawn on the GPU into a persistent copy of the frame buffer
    // (no per-primitive copies); everything else takes the software path after the GPU copy has been written back to emulated memory.
    // The embedding code must call runBegin() when a display-list run starts (memory may have been changed by the CPU since the last run),
    // runEnd() when it ends or is about to be observed (the GPU copy is written back), and externalWrite() after writing emulated memory itself.
    void attachGeGpu(GeGpu* g) { ggpu_ = g; }
    GeGpu* geGpu() const { return ggpu_; }
    // Coherence (docs/GPU_DESIGN.md 4.2): the GPU copy of a frame buffer is written back to emulated memory LAZILY, when something observes or
    // modifies that memory: the CPU/DMA/ME/display through the bus (vramAccess(), called by SystemBus), a software draw or transfer (internal), a
    // capture starting. syncToMemory() forces it; externalWrite() says memory was modified behind the renderer's back (replay patches).
    void syncToMemory() { if (ggpu_) ggpu_->syncToMemory(); }
    void externalWrite() { if (ggpu_) ggpu_->memoryChanged(); }
    bool gpuHookActive() const { return ggpu_ != nullptr; }
    // A bus access to VRAM (physical address, size, store?). A load/store overlapping a frame buffer the GPU holds newer data for first writes it
    // back; a store also invalidates the GPU's idea of memory (the next draw re-validates the surface against it).
    void vramAccess(uint32_t phys, uint32_t n, bool write) {
        if (ggpu_->anyNewer() && ggpu_->anyNewerOverlaps(phys, n)) ggpu_->syncToMemory();
        if (write) ggpu_->memoryChanged();
    }
    // Why draws were not sent to the GPU (index: 1 target not usable, 2 depth test, 3 write masks, 4 texture, 5 blend, 6 vertex, 7 other).
    uint64_t gpuWhy[8] = {};
    uint64_t gpuBatches = 0, gpuThroughBatches = 0;
#else
    // Software-only build (no GPU-resident renderer attached, ever): these
    // are called unconditionally by GeCoreStub (mmio/ge_edram.h), so give
    // them real, correct no-op/false semantics rather than guarding every
    // call site -- a software-only renderer genuinely has nothing to sync,
    // and every VRAM access is already fully reflected in emulated memory.
    void syncToMemory() {}
    void externalWrite() {}
    bool gpuHookActive() const { return false; }
    void vramAccess(uint32_t, uint32_t, bool) {}
#endif

    // Statistics (tooling).
    uint64_t primsThrough = 0, primsTransformSkipped = 0, primsTransform = 0, xfVertsBehind = 0, xfPointsLines = 0, xfTrisClippedAway = 0, pixelsWritten = 0, transfers = 0;
    uint64_t bezierCount = 0, splineCount = 0;
    std::string dbgDir; int dbgLeft = 0, dbgSeq = 0, dbgTexSeq = 0; // frame debugger: while dbgLeft > 0 every draw that changes pixels dumps the target frame buffer
    std::vector<std::string> recentPrims; // last few primitives (tooling), newest last -- only filled when GeOptions::recentPrims is set (formatting it cost ~10% of the run)
    bool recentPrimsOn() const { return recentPrimsOn_; }

    // Frame buffer currently targeted by the GE (for dumps).
    uint32_t frameBufferAddress() const { return fbAddress(); }
    uint32_t frameBufferStride() const { return state_[0x9D] & 0xFFFFu; }
    uint32_t frameBufferFormat() const { return state_[0xD2] & 3u; }

    void command(uint32_t cmd, uint32_t arg) {
        arg &= 0xFFFFFFu;
        switch (cmd) {
            case 0x10: base_ = (arg << 8) & 0x0F000000u; state_[cmd] = arg; return;      // BASE
            case 0x01: vaddr_ = absAddr(arg); state_[cmd] = arg; return;                  // VADDR
            case 0x02: iaddr_ = absAddr(arg); state_[cmd] = arg; return;                  // IADDR
            case 0x04: prim(arg); return;                                                 // PRIM
            case 0x05: ++bezierCount; patch(arg, false); return;                          // BEZIER
            case 0x06: ++splineCount; patch(arg, true); return;                           // SPLINE
            case 0xC4: loadClut(arg); return;                                             // LOADCLUT
            case 0x3A: state_[cmd] = arg; worldIdx_ = arg & 15u; return;                  // WORLDMATRIXNUMBER
            case 0x3B: if (worldIdx_ < 12u) world_[worldIdx_++] = fbits(arg); return;      // WORLDMATRIXDATA
            case 0x3C: state_[cmd] = arg; viewIdx_ = arg & 15u; return;                   // VIEWMATRIXNUMBER
            case 0x3D: if (viewIdx_ < 12u) view_[viewIdx_++] = fbits(arg); return;         // VIEWMATRIXDATA
            case 0x3E: state_[cmd] = arg; projIdx_ = arg & 31u; return;                   // PROJMATRIXNUMBER
            case 0x3F: if (projIdx_ < 16u) proj_[projIdx_++] = fbits(arg); return;         // PROJMATRIXDATA
            case 0x40: state_[cmd] = arg; tgenIdx_ = arg & 15u; return;                   // TGENMATRIXNUMBER
            case 0x41: if (tgenIdx_ < 12u) tgen_[tgenIdx_++] = fbits(arg); return;         // TGENMATRIXDATA
            case 0xEA: state_[cmd] = arg; transfer(); return;                             // TRANSFERSTART
            default: state_[cmd & 0xFF] = arg; return;
        }
    }

private:
    // Per-instance toggles threaded from GeOptions (constructor init list above) --
    // declared in the same order the constructor initializes them.
    bool normalMatInvT_, analyticNormalsOn_, recentPrimsOn_, texDumpRaw_, patchDebug_;
    size_t recentMax_;
    std::string texDumpDir_;
    int64_t xfBoxFrom_;

    struct Win { uint8_t* p = nullptr; uint32_t base = 0, size = 0; };
    Win win_[2];
    uint8_t* winPtr(uint32_t a, uint32_t n) const {
        const uint32_t p = a & 0x1FFFFFFFu;
        for (int i = 0; i < 2; ++i) { const uint32_t off = p - win_[i].base; if (win_[i].p && off < win_[i].size && win_[i].size - off >= n) return win_[i].p + off; }
        return nullptr;
    }
    uint8_t dr8(uint32_t a) const { if (const uint8_t* q = winPtr(a, 1)) return *q; return mem_.r8(a); }
    uint16_t dr16(uint32_t a) const { if (const uint8_t* q = winPtr(a, 2)) { uint16_t v; std::memcpy(&v, q, 2); return v; } return mem_.r16(a); }
    uint32_t dr32(uint32_t a) const { if (const uint8_t* q = winPtr(a, 4)) { uint32_t v; std::memcpy(&v, q, 4); return v; } return mem_.r32(a); }
    void dw8(uint32_t a, uint8_t v) const { if (uint8_t* q = winPtr(a, 1)) { *q = v; return; } mem_.w8(a, v); }
    void dw16(uint32_t a, uint16_t v) const { if (uint8_t* q = winPtr(a, 2)) { std::memcpy(q, &v, 2); return; } mem_.w16(a, v); }
    void dw32(uint32_t a, uint32_t v) const { if (uint8_t* q = winPtr(a, 4)) { std::memcpy(q, &v, 4); return; } mem_.w32(a, v); }

    GeMemory mem_;
    GeMemory savedMem_; Win savedWin_[2]; bool observed_ = false; // beginObservedMemory()'s saved state
#ifdef BETA_GE_GPU_ACCEL
    GpuGeRenderer* gpu_ = nullptr; // see attachGpu()'s own comment; the actual GPU logic lives further down, after struct Vertex.
    GeGpu* ggpu_ = nullptr;        // see attachGeGpu()
#endif
    uint32_t state_[256] = {};
    uint32_t base_ = 0, vaddr_ = 0, iaddr_ = 0;
    uint32_t clut_[1024] = {};
    float world_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, view_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, tgen_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float proj_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool colorPresent_ = false;
    uint32_t worldIdx_ = 0, viewIdx_ = 0, projIdx_ = 0, tgenIdx_ = 0;
    static float fbits(uint32_t arg24) { const uint32_t w = arg24 << 8; float f; std::memcpy(&f, &w, 4); return f; }
    float stateF(uint32_t cmd) const { return fbits(state_[cmd]); }

    struct Vertex { float x = 0, y = 0, z = 0, u = 0, v = 0, nx = 0, ny = 0, nz = 0; uint8_t r = 255, g = 255, b = 255, a = 255; };

#ifdef BETA_GE_GPU_ACCEL
    // ---- opt-in GPU acceleration (through-mode triangles only; see attachGpu()'s own comment for scope) --------------------------
    bool gpuStateSupported() const {
        if ((state_[0xD3] & 1u) != 0u) return false;                 // clear mode: not modelled by the GPU shaders at all
        if (frameBufferStride() != 480u) return false;                // the GPU target is a fixed 480x272 (the PSP's real display)
        if (state_[0x1E] & 1u) { if ((state_[0xC3] & 0xFu) > 5u) return false; } // texture format 6/7 (DXT): unsupported by either path
        if (state_[0x21] & 1u) {
            const uint32_t bm = state_[0xDF];
            if ((bm & 15u) > 5u || ((bm >> 4) & 15u) > 5u || ((bm >> 8) & 7u) > 4u) return false; // an unsupported blend factor/equation
        }
        return true;
    }

#endif
    // Texture address 0 (TEXADDR0 = 0 and no high bits) is neither VRAM (0x04000000..) nor RAM (0x08000000..): nothing is there to read, so every texel is
    // transparent black. The Internet Browser's text boxes draw an empty, already freed image this way (id -1, texture pointer 0) as one patch 65535 pixels
    // wide; reading address 0 as "the start of VRAM" (what vram() does for small addresses) painted the frame buffer's top-left content across the screen.
    // (Pure state_[] bit-checks, no GPU dependency -- used unconditionally by the software rendering path below, so this stays outside the GPU guard.)
    bool texNullAddress() const { return (state_[0xA0] & 0xFFFFFFu) == 0 && (state_[0xA8] & 0x0F0000u) == 0; }
    // True when drawing with the current state cannot change a pixel's colour: the texture is the null one (every texel transparent black), the texture
    // function takes its alpha from the texture (so the fragment alpha is 0), and the blend is the usual SRC_ALPHA / ONE_MINUS_SRC_ALPHA add, which then
    // returns the destination colour unchanged. (No depth or stencil or logic-op side effects.) The one thing a real draw would still do is write alpha 0
    // into the frame buffer's alpha byte, which nothing reads back; skipping spares the browser's giant empty-text-box patches (65535 pixels wide, ~70%
    // of this scene's GE time).
    bool nullTextureDrawIsNoOp() const {
        if (!(state_[0x1E] & 1u) || !texNullAddress() || !(state_[0x21] & 1u) || (state_[0xD3] & 1u)) return false;
        const uint32_t bm = state_[0xDF], tf = state_[0xC9];
        if ((bm & 15u) != 2u || ((bm >> 4) & 15u) != 3u || ((bm >> 8) & 7u) != 0u) return false;
        const uint32_t func = tf & 7u;
        if (!(tf & 0x100u) || func == 1u || func > 4u) return false;     // alpha must come from the texture (not decal)
        return !(state_[0x23] & 1u) && !(state_[0x24] & 1u) && !(state_[0x28] & 1u);   // depth test (and so depth write), stencil test, logic op all off
    }
#ifdef BETA_GE_GPU_ACCEL
    // Decodes the CURRENTLY bound texture (whatever format/CLUT/swizzle state_[] describes) to plain RGBA8 and uploads it to the GPU,
    // reusing ge_texture_decode.h's independent decoders -- the same ones proven in tests/test_gpu_ge_renderer.cpp -- NOT texel(),
    // which stays private to this class and untouched.
    void gpuUploadCurrentTexture() {
        const uint32_t addr = vram((state_[0xA0] & 0xFFFFFFu) | ((state_[0xA8] & 0x0F0000u) << 8));
        const uint32_t stride = std::max<uint32_t>(state_[0xA8] & 0xFFFFu, 1u);
        const uint32_t w = 1u << (state_[0xB8] & 0xFu), h = 1u << ((state_[0xB8] >> 8) & 0xFu);
        if (texNullAddress()) { const std::vector<uint8_t> blank(static_cast<size_t>(w) * h * 4u, 0u); gpu_->setTexture(blank.data(), w, h); return; }
        const int fmt = static_cast<int>(state_[0xC3] & 0xFu);
        const bool swz = (state_[0xC2] & 1u) != 0u;
        const auto r8 = [this](uint32_t a) { return dr8(a); };
        if (fmt == 4 || fmt == 5) {
            // CBP0/CBP1 (the CLUT's own source address) is read completely RAW by loadClut() -- unlike a texture's own pixel data
            // address, which DOES go through vram(). Two GE address registers that look almost identical, different rules.
            const uint32_t clutAddr = (state_[0xB0] & 0xFFFFFFu) | ((state_[0xB1] & 0x0F0000u) << 8);
            const uint32_t cf = state_[0xC5];
            const int clutFmt = static_cast<int>(cf & 3u);
            const uint32_t entries = clutFmt == 3 ? 256u : 1024u; // generous upper bound; clutLookup's own index math wraps mod 1024 anyway
            const std::vector<uint32_t> clut = decodeGeClutPalette(clutFmt, clutAddr, entries, r8);
            const GeClutIndexParams idxParams{(cf >> 2) & 0x1Fu, (cf >> 8) & 0xFFu, (cf >> 16) & 0x1Fu};
            const std::vector<uint8_t> rgba = decodeGeClutTexture(fmt == 4 ? 4 : 8, addr, w, h, stride, r8, clut, idxParams, swz);
            gpu_->setTexture(rgba.data(), w, h);
        } else if (fmt <= 2) {
            const std::vector<uint8_t> rgba = decodeGeDirectTexture16(fmt, addr, w, h, stride, r8, swz);
            gpu_->setTexture(rgba.data(), w, h);
        } else { // format 3: a raw byte-identity decode (matches unpack()'s own default case: no scaling at all)
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
                const uint32_t byteAddr = addr + geTexelByteOffset(x, y, 32u, stride, swz);
                const size_t o = (static_cast<size_t>(y) * w + x) * 4;
                rgba[o + 0] = dr8(byteAddr); rgba[o + 1] = dr8(byteAddr + 1); rgba[o + 2] = dr8(byteAddr + 2); rgba[o + 3] = dr8(byteAddr + 3);
            }
            gpu_->setTexture(rgba.data(), w, h);
        }
    }

    // CPU-to-GPU half of the sync every GPU-accelerated PRIM needs: reads the current 480x272 framebuffer into a plain RGBA8 buffer,
    // reusing this class's own existing private unpack()/fbAddress()/etc, and uploads it -- so blending, and anything drawn (via either
    // path) since the last GPU draw, is visible to this one.
    void gpuSyncFramebufferToGpu() {
        const uint32_t fb = fbAddress(), stride = std::max<uint32_t>(frameBufferStride(), 1u);
        const int ffmt = static_cast<int>(frameBufferFormat());
        const uint32_t bytes = ffmt == 3 ? 4u : 2u;
        std::vector<uint8_t> rgba(480u * 272u * 4u);
        for (uint32_t y = 0; y < 272u; ++y) for (uint32_t x = 0; x < 480u; ++x) {
            const uint32_t raw = bytes == 4 ? dr32(fb + (y * stride + x) * 4u) : dr16(fb + (y * stride + x) * 2u);
            uint8_t r, g, b, a; unpack(raw, ffmt, r, g, b, a);
            const size_t o = (static_cast<size_t>(y) * 480u + x) * 4;
            rgba[o] = r; rgba[o + 1] = g; rgba[o + 2] = b; rgba[o + 3] = a;
        }
        gpu_->uploadPixels(rgba.data());
    }

    // GPU-to-CPU half: reads the GPU's render target back and writes it into the real emulated framebuffer bytes (via this class's own
    // pack()/dw32()/dw16()), so everything else (the display, subsequent software-path draws, screenshots) sees the GPU's output
    // exactly like any other write.
    void gpuSyncFramebufferFromGpu() {
        std::vector<uint8_t> rgba;
        gpu_->readPixels(rgba);
        const uint32_t fb = fbAddress(), stride = std::max<uint32_t>(frameBufferStride(), 1u);
        const int ffmt = static_cast<int>(frameBufferFormat());
        const uint32_t bytes = ffmt == 3 ? 4u : 2u;
        for (uint32_t y = 0; y < 272u; ++y) for (uint32_t x = 0; x < 480u; ++x) {
            const size_t o = (static_cast<size_t>(y) * 480u + x) * 4;
            const uint32_t packed = pack(ffmt, rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]);
            const uint32_t a = fb + (y * stride + x) * bytes;
            if (bytes == 4) dw32(a, packed); else dw16(a, static_cast<uint16_t>(packed));
        }
    }

    // Expands vs[]/type into a flat triangle list (matching the software switch's own strip/fan expansion in primImpl()) and draws it
    // via the GPU. Flat shading (state 0x50 bit0 == 0, the default) is handled by overwriting each triangle's first two vertices with
    // its provoking (last) vertex's colour BEFORE handing it to the GPU -- GpuGeRenderer has no separate flat-shading mode, but
    // interpolating three identical colours trivially reduces to that same flat colour everywhere, exactly matching plot()'s own
    // `p.r=c.r` rule with no new GPU-side code needed. Does a full CPU<->GPU framebuffer sync around the draw for correctness (a real,
    // known performance cost documented on attachGpu()).
    bool gpuDrawThrough(const std::vector<Vertex>& vs, uint32_t type, uint32_t count) {
        std::vector<Vertex> tris; tris.reserve(count);
        switch (type) {
            case 3: for (uint32_t i = 0; i + 2 < count; i += 3) { tris.push_back(vs[i]); tris.push_back(vs[i + 1]); tris.push_back(vs[i + 2]); } break;
            case 4: for (uint32_t i = 0; i + 2 < count; ++i) {
                        if (i & 1u) { tris.push_back(vs[i + 1]); tris.push_back(vs[i]); tris.push_back(vs[i + 2]); }
                        else { tris.push_back(vs[i]); tris.push_back(vs[i + 1]); tris.push_back(vs[i + 2]); }
                    } break;
            case 5: for (uint32_t i = 1; i + 1 < count; ++i) { tris.push_back(vs[0]); tris.push_back(vs[i]); tris.push_back(vs[i + 1]); } break;
            default: return false;
        }
        if (tris.empty()) return false;

        const bool flat = (state_[0x50] & 1u) == 0u;
        if (flat) for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            tris[i].r = tris[i + 2].r; tris[i].g = tris[i + 2].g; tris[i].b = tris[i + 2].b; tris[i].a = tris[i + 2].a;
            tris[i + 1].r = tris[i + 2].r; tris[i + 1].g = tris[i + 2].g; tris[i + 1].b = tris[i + 2].b; tris[i + 1].a = tris[i + 2].a;
        }

        gpuSyncFramebufferToGpu();

        const bool textured = (state_[0x1E] & 1u) != 0u;
        if (textured) {
            gpuUploadCurrentTexture();
            const uint32_t tf = state_[0xC9];
            const uint32_t filt = state_[0xC6];
            const bool bilinear = ((filt & 1u) != 0u) || (((filt >> 8) & 1u) != 0u);
            const uint32_t at = state_[0xDB];
            gpu_->beginTextured(tf & 7u, (tf & 0x100u) != 0u, (tf & 0x10000u) != 0u,
                                 (state_[0x22] & 1u) != 0u, at & 7u, (at >> 8) & 0xFFu, (at >> 16) & 0xFFu, bilinear);
            std::vector<GpuGeRenderer::TexVertex> gv; gv.reserve(tris.size());
            for (const Vertex& v : tris) gv.push_back({v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a});
            if (state_[0x21] & 1u) { const uint32_t bm = state_[0xDF]; gpu_->setBlend(true, bm & 15u, (bm >> 4) & 15u, (bm >> 8) & 7u); }
            else gpu_->setBlend(false, 0, 0, 0);
            gpu_->drawTexturedTriangles(gv.data(), gv.size());
        } else {
            const uint32_t at = state_[0xDB];
            gpu_->begin((state_[0x22] & 1u) != 0u, at & 7u, (at >> 8) & 0xFFu, (at >> 16) & 0xFFu);
            std::vector<GpuGeRenderer::Vertex> gv; gv.reserve(tris.size());
            for (const Vertex& v : tris) gv.push_back({v.x, v.y, v.z, v.r, v.g, v.b, v.a});
            if (state_[0x21] & 1u) { const uint32_t bm = state_[0xDF]; gpu_->setBlend(true, bm & 15u, (bm >> 4) & 15u, (bm >> 8) & 7u); }
            else gpu_->setBlend(false, 0, 0, 0);
            gpu_->drawTriangles(gv.data(), gv.size());
        }

        gpuSyncFramebufferFromGpu();
        pixelsWritten += static_cast<uint64_t>(tris.size()); // rough tooling count, not pixel-exact like the software path's own counter
        return true;
    }
#endif

    uint32_t absAddr(uint32_t arg) const { return (base_ | arg) & 0x0FFFFFFFu; }
    static uint32_t vram(uint32_t a) { a &= 0x0FFFFFFFu; return a < 0x04000000u ? (0x04000000u + (a & 0x1FFFFFu)) : a; }
    uint32_t fbAddress() const {
        const uint32_t a = (state_[0x9C] & 0xFFFFFFu) | ((state_[0x9D] & 0xFF0000u) << 8);
        return vram(a);
    }

    // ---- pixel helpers ------------------------------------------------------------------------------------------
    static void unpack(uint32_t v, int fmt, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
        switch (fmt) {
            case 0: r = static_cast<uint8_t>((v & 31u) * 255u / 31u); g = static_cast<uint8_t>(((v >> 5) & 63u) * 255u / 63u); b = static_cast<uint8_t>(((v >> 11) & 31u) * 255u / 31u); a = 255; break;
            case 1: r = static_cast<uint8_t>((v & 31u) * 255u / 31u); g = static_cast<uint8_t>(((v >> 5) & 31u) * 255u / 31u); b = static_cast<uint8_t>(((v >> 10) & 31u) * 255u / 31u); a = (v & 0x8000u) ? 255 : 0; break;
            case 2: r = static_cast<uint8_t>((v & 15u) * 17u); g = static_cast<uint8_t>(((v >> 4) & 15u) * 17u); b = static_cast<uint8_t>(((v >> 8) & 15u) * 17u); a = static_cast<uint8_t>(((v >> 12) & 15u) * 17u); break;
            default: r = v & 0xFF; g = (v >> 8) & 0xFF; b = (v >> 16) & 0xFF; a = (v >> 24) & 0xFF; break;
        }
    }
    static uint32_t pack(int fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        switch (fmt) {
            case 0: return (r * 31u / 255u) | ((g * 63u / 255u) << 5) | ((b * 31u / 255u) << 11);
            case 1: return (r * 31u / 255u) | ((g * 31u / 255u) << 5) | ((b * 31u / 255u) << 10) | (a >= 128 ? 0x8000u : 0u);
            case 2: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
            default: return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(a) << 24);
        }
    }

    // ---- textures -----------------------------------------------------------------------------------------------
    void loadClut(uint32_t blocks) {
        const uint32_t addr = (state_[0xB0] & 0xFFFFFFu) | ((state_[0xB1] & 0x0F0000u) << 8);
        const int fmt = state_[0xC5] & 3;
        const uint32_t entries = std::min<uint32_t>(blocks * (fmt == 3 ? 8u : 16u), 1024u); // one block = 32 bytes: 8 32-bit entries or 16 16-bit entries
#ifdef BETA_GE_GPU_ACCEL
        clutHashValid_ = false;
#endif
        for (uint32_t i = 0; i < entries; ++i) {
            uint8_t r, g, b, a; uint32_t raw;
            if (fmt == 3) raw = dr32(addr + i * 4u); else raw = dr16(addr + i * 2u);
            unpack(raw, fmt, r, g, b, a);
            clut_[i] = r | (g << 8) | (b << 16) | (static_cast<uint32_t>(a) << 24);
        }
    }
    // Texture sample at (u, v) in texel units. TEXFILTER (0xC6): bits 0-2 minify, 8-10 magnify; odd values are linear. Linear = bilinear over the four
    // surrounding texels (the GE samples texel centres at +0.5), which is what keeps thin glyph strokes alive when the XMB scales its text.
    uint32_t sampleTex(float u, float v) const {
        const uint32_t f = state_[0xC6];
        if (!((f & 1u) || ((f >> 8) & 1u))) return texel(0, static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));
        const float fu = u - 0.5f, fv = v - 0.5f;
        const int x0 = static_cast<int>(std::floor(fu)), y0 = static_cast<int>(std::floor(fv));
        const float ax = fu - static_cast<float>(x0), ay = fv - static_cast<float>(y0);
        const uint32_t c[4] = {texel(0, x0, y0), texel(0, x0 + 1, y0), texel(0, x0, y0 + 1), texel(0, x0 + 1, y0 + 1)};
        const float w[4] = {(1.f - ax) * (1.f - ay), ax * (1.f - ay), (1.f - ax) * ay, ax * ay};
        uint32_t out = 0;
        for (int ch = 0; ch < 4; ++ch) {
            float acc = 0.f;
            for (int k = 0; k < 4; ++k) acc += w[k] * static_cast<float>((c[k] >> (8 * ch)) & 0xFFu);
            out |= static_cast<uint32_t>(std::min(255.f, std::max(0.f, acc + 0.5f))) << (8 * ch);
        }
        return out;
    }
    uint32_t texel(int level0, int x, int y) const {
        if (texNullAddress()) return 0;
        const uint32_t w = 1u << (state_[0xB8] & 0xF), h = 1u << ((state_[0xB8] >> 8) & 0xF);
        const uint32_t wrap = state_[0xC7];
        auto wrapc = [](int c, uint32_t n, bool clamp) -> uint32_t {
            if (clamp) return static_cast<uint32_t>(std::max(0, std::min(c, static_cast<int>(n) - 1)));
            return static_cast<uint32_t>(c) & (n - 1u);
        };
        (void)level0;
        const uint32_t px = wrapc(x, w, (wrap & 1u) != 0), py = wrapc(y, h, (wrap & 0x100u) != 0);
        const uint32_t addr = (state_[0xA0] & 0xFFFFFFu) | ((state_[0xA8] & 0x0F0000u) << 8);
        const uint32_t stride = std::max<uint32_t>(state_[0xA8] & 0xFFFFu, 1u);
        const int fmt = state_[0xC3] & 0xF;
        const bool swz = (state_[0xC2] & 1u) != 0;
        auto bitsPerPixel = [&](int f) { switch (f) { case 3: case 7: return 32; case 4: return 4; case 5: return 8; default: return 16; } };
        const int bpp = bitsPerPixel(fmt);
        uint32_t byteOff;
        const uint32_t rowBytes = stride * static_cast<uint32_t>(bpp) / 8u;
        const uint32_t xb = px * static_cast<uint32_t>(bpp) / 8u;
        if (swz) {   // narrower than a 16-byte block: the rows are still 16 bytes apart (see geTexelByteOffset)
            const uint32_t blocksPerRow = rowBytes >= 16u ? rowBytes / 16u : 1u;
            byteOff = ((py / 8u) * blocksPerRow + xb / 16u) * 128u + (py % 8u) * 16u + (xb % 16u);
        } else {
            byteOff = py * rowBytes + xb;
        }
        const uint32_t a = vram(addr) + byteOff;
        auto clutLookup = [&](uint32_t idx) -> uint32_t {
            const uint32_t cf = state_[0xC5];
            const uint32_t shift = (cf >> 2) & 0x1F, mask = (cf >> 8) & 0xFF, start = (cf >> 16) & 0x1F;
            return clut_[(((idx >> shift) & mask) + (start << 4)) & 1023u];
        };
        uint8_t r = 255, g = 255, b = 255, al = 255;
        switch (fmt) {
            case 0: case 1: case 2: unpack(dr16(a), fmt, r, g, b, al); break;
            case 3: unpack(dr32(a), 3, r, g, b, al); break;
            case 4: { const uint8_t v = dr8(a); const uint32_t idx = (px & 1u) ? (v >> 4) : (v & 15u); const uint32_t c = clutLookup(idx); r = c & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF; al = (c >> 24) & 0xFF; break; }
            case 5: { const uint32_t c = clutLookup(dr8(a)); r = c & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF; al = (c >> 24) & 0xFF; break; }
            case 6: { const uint32_t c = clutLookup(dr16(a)); r = c & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF; al = (c >> 24) & 0xFF; break; }
            case 7: { const uint32_t c = clutLookup(dr32(a)); r = c & 0xFF; g = (c >> 8) & 0xFF; b = (c >> 16) & 0xFF; al = (c >> 24) & 0xFF; break; }
            default: break; // DXT: not supported yet, sampled as opaque white
        }
        return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(al) << 24);
    }

    // ---- vertices -----------------------------------------------------------------------------------------------
    uint32_t vertexSize(uint32_t vt) const {
        static const uint32_t texSz[4] = {0, 2, 4, 8}, colSz[8] = {0, 0, 0, 0, 2, 2, 2, 4}, nrmSz[4] = {0, 3, 6, 12}, posSz[4] = {0, 3, 6, 12}, wSz[4] = {0, 1, 2, 4};
        const uint32_t nw = ((vt >> 14) & 7u) + 1u;
        uint32_t off = 0; uint32_t maxAlign = 1;
        auto add = [&](uint32_t size, uint32_t align) { off = (off + align - 1u) / align * align; off += size; maxAlign = std::max(maxAlign, align); };
        if (vt & 0x600u) add(wSz[(vt >> 9) & 3u] * nw, wSz[(vt >> 9) & 3u]);
        if (vt & 3u) add(texSz[vt & 3u], (vt & 3u) == 3u ? 4u : (vt & 3u));
        if (vt & 0x1Cu) add(colSz[(vt >> 2) & 7u], colSz[(vt >> 2) & 7u] == 4u ? 4u : 2u);
        if (vt & 0x60u) add(nrmSz[(vt >> 5) & 3u], (vt >> 5 & 3u) == 3u ? 4u : (vt >> 5 & 3u));
        if (vt & 0x180u) add(posSz[(vt >> 7) & 3u], (vt >> 7 & 3u) == 3u ? 4u : (vt >> 7 & 3u));
        return (off + maxAlign - 1u) / maxAlign * maxAlign;
    }
    float readF(uint32_t a) const { const uint32_t w = dr32(a); float f; std::memcpy(&f, &w, 4); return f; }
    Vertex decode(uint32_t vt, uint32_t a) const {
        Vertex v; const bool through = (vt & 0x800000u) != 0;
        static const uint32_t wSz[4] = {0, 1, 2, 4};
        const uint32_t nw = ((vt >> 14) & 7u) + 1u;
        uint32_t off = 0;
        auto align = [&](uint32_t al) { off = (off + al - 1u) / al * al; };
        if (vt & 0x600u) { const uint32_t s = wSz[(vt >> 9) & 3u]; align(s); off += s * nw; }
        const uint32_t tf = vt & 3u;
        if (tf) {
            if (tf == 1) { v.u = dr8(a + off); v.v = dr8(a + off + 1); off += 2; if (!through) { v.u /= 128.f; v.v /= 128.f; } }
            else if (tf == 2) { align(2); v.u = dr16(a + off); v.v = dr16(a + off + 2); off += 4; if (!through) { v.u /= 32768.f; v.v /= 32768.f; } }
            else { align(4); v.u = readF(a + off); v.v = readF(a + off + 4); off += 8; }
        }
        const uint32_t cf = (vt >> 2) & 7u;
        if (cf >= 4) {
            if (cf == 7) { align(4); unpack(dr32(a + off), 3, v.r, v.g, v.b, v.a); off += 4; }
            else { align(2); unpack(dr16(a + off), static_cast<int>(cf - 4), v.r, v.g, v.b, v.a); off += 2; }
        }
        static const uint32_t nSz[4] = {0, 3, 6, 12};
        const uint32_t nf = (vt >> 5) & 3u;
        if (nf) {
            align(nf == 3 ? 4u : nf);
            if (nf == 1) { v.nx = static_cast<int8_t>(dr8(a + off)) / 127.f; v.ny = static_cast<int8_t>(dr8(a + off + 1)) / 127.f; v.nz = static_cast<int8_t>(dr8(a + off + 2)) / 127.f; }
            else if (nf == 2) { v.nx = static_cast<int16_t>(dr16(a + off)) / 32767.f; v.ny = static_cast<int16_t>(dr16(a + off + 2)) / 32767.f; v.nz = static_cast<int16_t>(dr16(a + off + 4)) / 32767.f; }
            else { v.nx = readF(a + off); v.ny = readF(a + off + 4); v.nz = readF(a + off + 8); }
            off += nSz[nf];
        }
        const uint32_t pf = (vt >> 7) & 3u;
        if (pf == 1) { v.x = static_cast<int8_t>(dr8(a + off)); v.y = static_cast<int8_t>(dr8(a + off + 1)); v.z = dr8(a + off + 2); }
        else if (pf == 2) { align(2); v.x = static_cast<int16_t>(dr16(a + off)); v.y = static_cast<int16_t>(dr16(a + off + 2)); v.z = dr16(a + off + 4); }
        else if (pf == 3) { align(4); v.x = readF(a + off); v.y = readF(a + off + 4); v.z = readF(a + off + 8); }
        return v;
    }

    // ---- primitives ---------------------------------------------------------------------------------------------
    void prim(uint32_t arg) {
        const uint64_t pb = pixelsWritten; StatRegistry& sr = statRegistry(); const auto t0 = sr.on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        primImpl(arg); dbgAfterDraw("THROUGH", pb);
        if (sr.on && (state_[0x12] & 0x800000u)) { sr.primThroughSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); ++sr.primThroughCalls; } // (transform prims are timed by primTransform())
    }
    void primImpl(uint32_t arg) {
        const uint32_t type = (arg >> 16) & 7u, count = arg & 0xFFFFu;
        const uint32_t vt = state_[0x12];
        if (!(vt & 0x800000u)) { primTransform(type, count, vt); return; }
        ++primsThrough;
        if (count == 0 || !mem_.r32) return;
        const uint32_t vsz = vertexSize(vt);
        const uint32_t idxFmt = (vt >> 11) & 3u;
        std::vector<Vertex> vs(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = i;
            if (idxFmt == 1) idx = dr8(iaddr_ + i); else if (idxFmt == 2) idx = dr16(iaddr_ + i * 2u);
            vs[i] = decode(vt, vaddr_ + idx * vsz);
        }
        { static int dbg = 0; if (dbg < 8) { ++dbg;
            std::printf("  [GE prim] type=%u count=%u vt=0x%06X vaddr=0x%08X vsz=%u clear=%u fb=0x%08X stride=%u fmt=%u", type, count, vt, vaddr_, vsz, state_[0xD3] & 1u, fbAddress(), frameBufferStride(), frameBufferFormat());
            for (uint32_t i = 0; i < count && i < 3; ++i) std::printf(" v%u=(%.1f,%.1f,%.1f rgba=%02X%02X%02X%02X uv=%.1f,%.1f)", i, vs[i].x, vs[i].y, vs[i].z, vs[i].r, vs[i].g, vs[i].b, vs[i].a, vs[i].u, vs[i].v);
            std::printf("\n");
            std::printf("  [GE prim] raw vertex bytes @0x%08X:", vaddr_);
            for (uint32_t i = 0; i < 32; ++i) std::printf(" %02X", dr8(vaddr_ + i));
            std::printf("\n"); } }
        if (recentPrimsOn()) {
            char buf[420];
            const uint64_t before = pixelsWritten;
            (void)before;
            std::snprintf(buf, sizeof(buf), "type=%u n=%u vt=%06X clear=%u tex=%u tf=%X/%X texaddr=%06X bw=%u size=%X blend=%u bm=%03X fixA=%06X atest=%u fb=%08X/%u/%u v0=(%.0f,%.0f rgba=%02X%02X%02X%02X uv=%.0f,%.0f)%s",
                          type, count, vt, state_[0xD3] & 1u, state_[0x1E] & 1u, state_[0xC3] & 0xF, state_[0xC9] & 0x1FFu, state_[0xA0] & 0xFFFFFFu, state_[0xA8] & 0xFFFFu, state_[0xB8] & 0xFFFFu,
                          state_[0x21] & 1u, state_[0xDF] & 0xFFFu, state_[0xE0] & 0xFFFFFFu, state_[0x22] & 1u, fbAddress(), frameBufferStride(), frameBufferFormat(),
                          vs[0].x, vs[0].y, vs[0].r, vs[0].g, vs[0].b, vs[0].a, vs[0].u, vs[0].v,
                          count > 1 ? "" : "");
            std::string d(buf);
            if (count > 1) { char b2[120]; std::snprintf(b2, sizeof(b2), " v1=(%.0f,%.0f rgba=%02X%02X%02X%02X uv=%.0f,%.0f)", vs[1].x, vs[1].y, vs[1].r, vs[1].g, vs[1].b, vs[1].a, vs[1].u, vs[1].v); d += b2; }
            recentPrims.push_back(d);
            if (recentPrims.size() > recentMax()) recentPrims.erase(recentPrims.begin());
        }
        const bool clear = (state_[0xD3] & 1u) != 0;
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) {
            if (gpuThroughPrim(vs, type, count, clear)) return;
            ggpu_->syncToMemory(); // a software draw is about to read and write emulated memory
        } else if (gpu_ && (type == 3 || type == 4 || type == 5) && gpuStateSupported() && gpuDrawThrough(vs, type, count)) return;
#endif
        switch (type) {
            case 0: for (uint32_t i = 0; i < count; ++i) plot(static_cast<int>(vs[i].x), static_cast<int>(vs[i].y), vs[i], clear); break;
            case 1: for (uint32_t i = 0; i + 1 < count; i += 2) line(vs[i], vs[i + 1], clear); break;
            case 2: for (uint32_t i = 0; i + 1 < count; ++i) line(vs[i], vs[i + 1], clear); break;
            case 3: for (uint32_t i = 0; i + 2 < count; i += 3) triangle(vs[i], vs[i + 1], vs[i + 2], clear); break;
            case 4: for (uint32_t i = 0; i + 2 < count; ++i) (i & 1u) ? triangle(vs[i + 1], vs[i], vs[i + 2], clear) : triangle(vs[i], vs[i + 1], vs[i + 2], clear); break;
            case 5: for (uint32_t i = 1; i + 1 < count; ++i) triangle(vs[0], vs[i], vs[i + 1], clear); break;
            case 6: for (uint32_t i = 0; i + 1 < count; i += 2) rect(vs[i], vs[i + 1], clear); break;
            default: break;
        }
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) ggpu_->memoryChanged();
#endif
    }

    // ---- transform mode (3D) -----------------------------------------------------------------------------------
    // How many recent primitives recentPrims keeps (GeOptions::recentMax, default 40): a replay can ask for a whole frame's worth.
    size_t recentMax() const { return recentMax_; }

    struct SVert { float x = 0, y = 0, z = 0, iw = 1, u = 0, v = 0; uint8_t r = 255, g = 255, b = 255, a = 255; bool ok = false; };

    Vertex decodeXf(uint32_t vt, uint32_t a) const {
        Vertex v; static const uint32_t wSz[4] = {0, 1, 2, 4};
        const uint32_t nw = ((vt >> 14) & 7u) + 1u; uint32_t off = 0;
        auto align = [&](uint32_t al) { off = (off + al - 1u) / al * al; };
        if (vt & 0x600u) { const uint32_t sz = wSz[(vt >> 9) & 3u]; align(sz); off += sz * nw; }
        const uint32_t tf = vt & 3u;
        if (tf == 1) { v.u = dr8(a + off) / 128.f; v.v = dr8(a + off + 1) / 128.f; off += 2; }
        else if (tf == 2) { align(2); v.u = dr16(a + off) / 32768.f; v.v = dr16(a + off + 2) / 32768.f; off += 4; }
        else if (tf == 3) { align(4); v.u = readF(a + off); v.v = readF(a + off + 4); off += 8; }
        const uint32_t cf = (vt >> 2) & 7u;
        bool haveColor = false;
        if (cf >= 4) {
            haveColor = true;
            if (cf == 7) { align(4); unpack(dr32(a + off), 3, v.r, v.g, v.b, v.a); off += 4; }
            else { align(2); unpack(dr16(a + off), static_cast<int>(cf - 4), v.r, v.g, v.b, v.a); off += 2; }
        }
        if (!haveColor) { // no vertex colour: the material ambient colour/alpha stands in
            const uint32_t ma = state_[0x55], mal = state_[0x58];
            v.r = ma & 0xFF; v.g = (ma >> 8) & 0xFF; v.b = (ma >> 16) & 0xFF; v.a = mal & 0xFF;
        }
        static const uint32_t nSz[4] = {0, 3, 6, 12};
        const uint32_t nf = (vt >> 5) & 3u;
        if (nf) { align(nf == 3 ? 4u : nf); off += nSz[nf]; }
        const uint32_t pf = (vt >> 7) & 3u;
        if (pf == 1) { v.x = static_cast<int8_t>(dr8(a + off)) / 128.f; v.y = static_cast<int8_t>(dr8(a + off + 1)) / 128.f; v.z = static_cast<int8_t>(dr8(a + off + 2)) / 128.f; }
        else if (pf == 2) { align(2); v.x = static_cast<int16_t>(dr16(a + off)) / 32768.f; v.y = static_cast<int16_t>(dr16(a + off + 2)) / 32768.f; v.z = static_cast<int16_t>(dr16(a + off + 4)) / 32768.f; }
        else if (pf == 3) { align(4); v.x = readF(a + off); v.y = readF(a + off + 4); v.z = readF(a + off + 8); }
        return v;
    }

    SVert project(const Vertex& in) const {
        SVert o;
        // model -> world -> view (4x3 matrices: three columns of the 3x3 part, then the translation)
        const float wx = in.x * world_[0] + in.y * world_[3] + in.z * world_[6] + world_[9];
        const float wy = in.x * world_[1] + in.y * world_[4] + in.z * world_[7] + world_[10];
        const float wz = in.x * world_[2] + in.y * world_[5] + in.z * world_[8] + world_[11];
        const float ex = wx * view_[0] + wy * view_[3] + wz * view_[6] + view_[9];
        const float ey = wx * view_[1] + wy * view_[4] + wz * view_[7] + view_[10];
        const float ez = wx * view_[2] + wy * view_[5] + wz * view_[8] + view_[11];
        const float cx = ex * proj_[0] + ey * proj_[4] + ez * proj_[8] + proj_[12];
        const float cy = ex * proj_[1] + ey * proj_[5] + ez * proj_[9] + proj_[13];
        const float cz = ex * proj_[2] + ey * proj_[6] + ez * proj_[10] + proj_[14];
        const float cw = ex * proj_[3] + ey * proj_[7] + ez * proj_[11] + proj_[15];
        if (!(cw > 1e-6f)) { ++const_cast<GeRenderer*>(this)->xfVertsBehind; return o; } // behind the camera / degenerate (no near-plane clipping yet)
        const float iw = 1.f / cw;
        const float ndx = cx * iw, ndy = cy * iw, ndz = cz * iw;
        const float offx = static_cast<float>(state_[0x4C] & 0xFFFFu) / 16.f, offy = static_cast<float>(state_[0x4D] & 0xFFFFu) / 16.f;
        o.x = ndx * stateF(0x42) + stateF(0x45) - offx;
        o.y = ndy * stateF(0x43) + stateF(0x46) - offy;
        o.z = ndz * stateF(0x44) + stateF(0x47);
        o.iw = iw; o.ok = true;
        o.r = in.r; o.g = in.g; o.b = in.b; o.a = in.a;
        if (state_[0x17] & 1u) light(in, wx, wy, wz, o);
        // texture coordinates: mode 0 = UV with scale/offset
        const uint32_t tmm = state_[0xC0] & 3u;
        if (tmm == 2u) { // environment map (jpcsp RendererTemplate / shader.vert ApplyTexture): coordinates from the eye-space normal and two lights
            float ne[3]; geEyeNormal(world_, view_, normalMatInvT_, in.nx, in.ny, in.nz, ne);
            const float nl = std::sqrt(ne[0] * ne[0] + ne[1] * ne[1] + ne[2] * ne[2]);
            if (nl > 1e-9f) { ne[0] /= nl; ne[1] /= nl; ne[2] /= nl; }
            const float ve[3] = {ex, ey, ez};
            const uint32_t shade = state_[0xC1];   // TEXSHADELS: the two lights whose directions give U and V (bits 0-1 and 8-9). It was read from 0xD0 until 2026-10-14, so both always came from light 0.
            float uv[2];
            for (int k = 0; k < 2; ++k) {
                const uint32_t l = (k == 0 ? shade : (shade >> 8)) & 3u;
                const uint32_t lt = state_[0x5F + l];
                const float w = ((lt >> 8) & 3u) == 0u ? 0.f : 1.f; // directional lights have w = 0
                float L[3] = {stateF(0x63 + l * 3) - ve[0] * w, stateF(0x64 + l * 3) - ve[1] * w, stateF(0x65 + l * 3) - ve[2] * w};
                float P;
                if ((lt & 3u) == 0u) { // ambient+diffuse light: cosine
                    const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); if (ll > 1e-9f) { L[0] /= ll; L[1] /= ll; L[2] /= ll; }
                    P = ne[0] * L[0] + ne[1] * L[1] + ne[2] * L[2];
                } else { // specular kinds: power of the half vector
                    L[2] += 1.f; const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]); if (ll > 1e-9f) { L[0] /= ll; L[1] /= ll; L[2] /= ll; }
                    const float d = ne[0] * L[0] + ne[1] * L[1] + ne[2] * L[2]; P = d > 0.f ? std::pow(d, std::max(stateF(0x5B), 1e-3f)) : 0.f;
                }
                uv[k] = 0.5f * (1.f + P);
            }
            o.u = uv[0]; o.v = uv[1];
        } else if (tmm == 0u) { o.u = in.u * stateF(0x48) + stateF(0x4A); o.v = in.v * stateF(0x49) + stateF(0x4B); }
        else if (tmm == 1u) { // texture matrix from position
            o.u = in.x * tgen_[0] + in.y * tgen_[3] + in.z * tgen_[6] + tgen_[9];
            o.v = in.x * tgen_[1] + in.y * tgen_[4] + in.z * tgen_[7] + tgen_[10];
        } else { o.u = in.u; o.v = in.v; }
        return o;
    }

    static void rgb24(uint32_t c, float* o) { o[0] = (c & 0xFF) / 255.f; o[1] = ((c >> 8) & 0xFF) / 255.f; o[2] = ((c >> 16) & 0xFF) / 255.f; }

    // Fixed-function lighting (PSP GE): emissive + scene ambient*material ambient + per light (ambient*matAmb + diffuse*matDiff*N.L).
    // Specular is not modelled yet. Material colours come from the vertex colour when MATERIALUPDATE selects it and one is present.
    void light(const Vertex& in, float wx, float wy, float wz, SVert& o) const {
        float vcol[3] = {in.r / 255.f, in.g / 255.f, in.b / 255.f};
        float matA[3], matD[3], emis[3], amb[3];
        rgb24(state_[0x55], matA); rgb24(state_[0x56], matD); rgb24(state_[0x54], emis); rgb24(state_[0x5C], amb);
        if (colorPresent_) { const uint32_t mu = state_[0x53] & 7u; for (int i = 0; i < 3; ++i) { if (mu & 1u) matA[i] = vcol[i]; if (mu & 2u) matD[i] = vcol[i]; } }
        // normal -> world space (rotation part of the world matrix), normalised
        float nx = in.nx * world_[0] + in.ny * world_[3] + in.nz * world_[6];
        float ny = in.nx * world_[1] + in.ny * world_[4] + in.nz * world_[7];
        float nz = in.nx * world_[2] + in.ny * world_[5] + in.nz * world_[8];
        if (state_[0x51] & 1u) { nx = -nx; ny = -ny; nz = -nz; }
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
        float res[3];
        for (int i = 0; i < 3; ++i) res[i] = emis[i] + amb[i] * matA[i];
        for (uint32_t l = 0; l < 4; ++l) {
            if (!(state_[0x18 + l] & 1u)) continue;
            float lp[3] = {stateF(0x63 + l * 3), stateF(0x64 + l * 3), stateF(0x65 + l * 3)};
            float la[3], ld[3];
            rgb24(state_[0x8F + l * 3], la); rgb24(state_[0x90 + l * 3], ld);
            const uint32_t ltype = (state_[0x5F + l] >> 8) & 3u;
            float lx, ly, lz, att = 1.f;
            if (ltype == 0) { lx = lp[0]; ly = lp[1]; lz = lp[2]; }
            else {
                lx = lp[0] - wx; ly = lp[1] - wy; lz = lp[2] - wz;
                const float d = std::sqrt(lx * lx + ly * ly + lz * lz);
                const float k0 = stateF(0x7B + l * 3), k1 = stateF(0x7C + l * 3), k2 = stateF(0x7D + l * 3);
                const float den = k0 + k1 * d + k2 * d * d; att = den > 1e-6f ? std::min(1.f, 1.f / den) : 1.f;
            }
            const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
            if (ll > 1e-6f) { lx /= ll; ly /= ll; lz /= ll; }
            const float ndl = std::max(0.f, nx * lx + ny * ly + nz * lz);
            for (int i = 0; i < 3; ++i) res[i] += att * (la[i] * matA[i] + ld[i] * matD[i] * ndl);
        }
        auto cv = [](float f) { return static_cast<uint8_t>(std::min(255.f, std::max(0.f, f * 255.f + 0.5f))); };
        o.r = cv(res[0]); o.g = cv(res[1]); o.b = cv(res[2]);
        o.a = colorPresent_ && (state_[0x53] & 2u) ? in.a : static_cast<uint8_t>(state_[0x58] & 0xFF);
    }


    // ---- curved surfaces (BEZIER / SPLINE), algorithms after jpcsp VideoEngine.drawBezier/drawSpline --------------------------
    struct FV { float p[3] = {0, 0, 0}, t[2] = {0, 0}, c[4] = {0, 0, 0, 0}, n[3] = {0, 0, 0}; };
    static FV toFV(const Vertex& v) { FV f; f.p[0] = v.x; f.p[1] = v.y; f.p[2] = v.z; f.t[0] = v.u; f.t[1] = v.v; f.c[0] = v.r; f.c[1] = v.g; f.c[2] = v.b; f.c[3] = v.a; f.n[0] = v.nx; f.n[1] = v.ny; f.n[2] = v.nz; return f; }
    static Vertex fromFV(const FV& f) {
        Vertex v; auto cl = [](float x) { return static_cast<uint8_t>(std::min(255.f, std::max(0.f, x + 0.5f))); };
        v.x = f.p[0]; v.y = f.p[1]; v.z = f.p[2]; v.u = f.t[0]; v.v = f.t[1]; v.nx = f.n[0]; v.ny = f.n[1]; v.nz = f.n[2];
        v.r = cl(f.c[0]); v.g = cl(f.c[1]); v.b = cl(f.c[2]); v.a = cl(f.c[3]); return v;
    }
    static void multAdd(FV& d, const FV& s, float f) {
        for (int i = 0; i < 3; ++i) { d.p[i] += f * s.p[i]; d.n[i] += f * s.n[i]; }
        for (int i = 0; i < 2; ++i) d.t[i] += f * s.t[i];
        for (int i = 0; i < 4; ++i) d.c[i] += f * s.c[i];
    }
    static float splineN(int i, int j, float u, const std::vector<int>& knot) {
        if (j == 0) return (knot[i] <= u && u < knot[i + 1]) ? 1.f : 0.f;
        float res = 0;
        if (knot[i + j] - knot[i] != 0) res += (u - knot[i]) / (knot[i + j] - knot[i]) * splineN(i, j - 1, u, knot);
        if (knot[i + j + 1] - knot[i + 1] != 0) res += (knot[i + j + 1] - u) / (knot[i + j + 1] - knot[i + 1]) * splineN(i + 1, j - 1, u, knot);
        return res;
    }
    static std::vector<int> splineKnot(int n, int type) {
        std::vector<int> knot(static_cast<size_t>(n + 5), 0);
        for (int i = 0; i < n - 1; ++i) knot[static_cast<size_t>(i + 3)] = i;
        if ((type & 1) == 0) { knot[0] = -3; knot[1] = -2; knot[2] = -1; }
        if ((type & 2) == 0) { knot[static_cast<size_t>(n + 2)] = n - 1; knot[static_cast<size_t>(n + 3)] = n; knot[static_cast<size_t>(n + 4)] = n + 1; }
        else { knot[static_cast<size_t>(n + 2)] = n - 2; knot[static_cast<size_t>(n + 3)] = n - 2; knot[static_cast<size_t>(n + 4)] = n - 2; }
        return knot;
    }
    static void bernstein(float u, float* o) { const float u1 = 1.f - u; o[0] = u1 * u1 * u1; o[1] = 3.f * u * u1 * u1; o[2] = 3.f * u * u * u1; o[3] = u * u * u; }

    // debug (GeOptions::texDumpDir): write every distinct texture bound by a transform-mode draw, decoded through the renderer's own sampler
    void maybeDumpTexture(const char* what) {
        const bool force = dbgLeft > 0 && !dbgDir.empty();   // a frame-debug dump (ge_replay --framedbg) saves the exact texture of every draw, even one seen before
        const char* dd = force ? dbgDir.c_str() : (texDumpDir_.empty() ? nullptr : texDumpDir_.c_str());
        if (!dd || !(state_[0x1E] & 1u)) return;
        static std::map<uint64_t, int> seen; static int n = 0;
        const int w = 1 << (state_[0xB8] & 0xF), h = 1 << ((state_[0xB8] >> 8) & 0xF);
        const uint64_t key = (static_cast<uint64_t>(state_[0xA0] & 0xFFFFFFu) << 20) ^ (static_cast<uint64_t>(state_[0xB8] & 0xFFFFu) << 4) ^ (state_[0xC3] & 0xF) ^ (static_cast<uint64_t>(state_[0xB0] & 0xFFFFFFu) << 40);
        if (!force && (seen.count(key) || n >= 300)) return;
        if (w > 512 || h > 512) return;
        if (force) ++dbgTexSeq; else seen[key] = ++n;
        std::vector<uint8_t> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { const uint32_t c = texel(0, x, y); uint8_t* o = &px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u]; o[0] = c & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = (c >> 16) & 0xFF; o[3] = static_cast<uint8_t>(c >> 24); }
        char path[512]; if (force) std::snprintf(path, sizeof(path), "%s/fdtex%03d_%06X_%dx%d_f%u.png", dd, dbgTexSeq, state_[0xA0] & 0xFFFFFFu, w, h, state_[0xC3] & 0xF); else std::snprintf(path, sizeof(path), "%s/tex%02d_%06X_%dx%d_f%u.png", dd, n, state_[0xA0] & 0xFFFFFFu, w, h, state_[0xC3] & 0xF);
        pngw::writeRgba(path, w, h, px);
        if (texDumpRaw_) { const uint32_t base = vram((state_[0xA0] & 0xFFFFFFu) | ((state_[0xA8] & 0x0F0000u) << 8)); std::printf("  [TEXRAW] #%d bw=%u fmt=%u swz=%u bytes:", n, state_[0xA8] & 0xFFFFu, state_[0xC3] & 0xF, state_[0xC2] & 1u); for (int q = 0; q < 160; ++q) std::printf(" %02X", dr8(base + static_cast<uint32_t>(q))); std::printf("%c", 10); }
        { std::printf("  [CLUT] #%d start=%u shift=%u mask=%02X fmt=%u loadaddr=%06X e0..7:", n, (state_[0xC5] >> 16) & 0x1F, (state_[0xC5] >> 2) & 0x1F, (state_[0xC5] >> 8) & 0xFF, state_[0xC5] & 3u, (state_[0xB0] & 0xFFFFFF) | ((state_[0xB1] & 0x0F0000u) << 8));
          for (int q = 0; q < 16; ++q) std::printf(" %08X", clut_[q]); std::printf("%c", 10);
          if (force) { std::printf("  [CLUT256]"); for (int q = 0; q < 256; q += 8) std::printf(" %d:%08X", q, clut_[q]); std::printf("%c", 10); } }
        std::printf("  [TEXDUMP] #%d %s addr=%06X %dx%d fmt=%u swz=%u vt=%06X blend=%u bm=%03X%c", n, what, state_[0xA0] & 0xFFFFFFu, w, h, state_[0xC3] & 0xF, state_[0xC2] & 1u, state_[0x12], state_[0x21] & 1u, state_[0xDF] & 0xFFFu, 10);
    }

    void dbgAfterDraw(const char* kind, uint64_t pxBefore) {
        if (dbgLeft <= 0 || pixelsWritten == pxBefore || !mem_.r32) return;
        --dbgLeft; ++dbgSeq;
        const uint32_t fb = vram(fbAddress()), stride = std::max<uint32_t>(frameBufferStride(), 1u); const int ffmt = static_cast<int>(frameBufferFormat());
        std::vector<uint8_t> px(480u * 272u * 4u);
        for (uint32_t y = 0; y < 272u; ++y) for (uint32_t x = 0; x < 480u; ++x) {
            uint8_t r, g, b, a; uint32_t raw = ffmt == 3 ? dr32(fb + (y * stride + x) * 4u) : dr16(fb + (y * stride + x) * 2u);
            unpack(raw, ffmt, r, g, b, a); uint8_t* o = &px[(y * 480u + x) * 4u]; o[0] = r; o[1] = g; o[2] = b; o[3] = 255;
        }
        char path[512]; std::snprintf(path, sizeof(path), "%s/dbg%03d.png", dbgDir.c_str(), dbgSeq);
        if (!pngw::writeRgba(path, 480, 272, px)) std::printf("  [DBG WRITE FAILED] path='%s'\n", path);
        if (!recentPrims.empty()) std::printf("  [DBGPRIM] #%d %s%c", dbgSeq, recentPrims.back().c_str(), 10);
        std::printf("  [DBG] #%d %s fb=%08X px=%llu tex=%u texaddr=%06X fmt=%u size=%X blend=%u bm=%03X fixb=%06X texfunc=%X vt=%06X%c", dbgSeq, kind, fbAddress(), static_cast<unsigned long long>(pixelsWritten - pxBefore),
                    state_[0x1E] & 1u, state_[0xA0] & 0xFFFFFFu, state_[0xC3] & 0xF, state_[0xB8] & 0xFFFFu, state_[0x21] & 1u, state_[0xDF] & 0xFFFu, state_[0xE1] & 0xFFFFFFu, state_[0xC9] & 0x1FFu, state_[0x12], 10);
    }

    void patch(uint32_t arg, bool spline) {
        const uint64_t pb = pixelsWritten; StatRegistry& sr = statRegistry(); const auto t0 = sr.on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        patchImpl(arg, spline); dbgAfterDraw(spline ? "SPLINE" : "BEZIER", pb);
        if (sr.on) { sr.patchSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); ++sr.patchCalls; }
    }
    void patchImpl(uint32_t arg, bool spline) {
        maybeDumpTexture(spline ? "spline" : "bezier");
        const int ucount = static_cast<int>(arg & 0xFFu), vcount = static_cast<int>((arg >> 8) & 0xFFu);
        const int utype = static_cast<int>((arg >> 16) & 3u), vtype = static_cast<int>((arg >> 18) & 3u);
        const uint32_t vt = state_[0x12];
        if (!mem_.r32 || (vt & 0x800000u)) return;
        if (ucount < 4 || vcount < 4) return;
        if (nullTextureDrawIsNoOp()) return;
        const int divS = std::max<int>(static_cast<int>(state_[0x36] & 0xFFu), 1), divT = std::max<int>(static_cast<int>((state_[0x36] >> 8) & 0xFFu), 1);
        colorPresent_ = ((vt >> 2) & 7u) >= 4u;
        const bool texEnabled = (state_[0x1E] & 1u) != 0, hasUv = (vt & 3u) != 0;
        const uint32_t vsz = vertexSize(vt), idxFmt = (vt >> 11) & 3u;
        StatRegistry& psr = statRegistry(); const auto pT0 = psr.on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        auto lapT = pT0;
        auto pLap = [&](double& acc) { if (psr.on) { const auto n = std::chrono::steady_clock::now(); acc += std::chrono::duration<double>(n - lapT).count(); lapT = n; } };
        std::vector<FV> ctrl(static_cast<size_t>(ucount) * static_cast<size_t>(vcount));
        for (int cu = 0; cu < ucount; ++cu) for (int cv = 0; cv < vcount; ++cv) {
            const uint32_t i = static_cast<uint32_t>(cv * ucount + cu); uint32_t idx = i;
            if (idxFmt == 1) idx = dr8(iaddr_ + i); else if (idxFmt == 2) idx = dr16(iaddr_ + i * 2u);
            FV acc; const uint32_t mc = ((vt >> 18) & 7u) + 1u;
            if (mc > 1) { for (uint32_t m = 0; m < mc; ++m) multAdd(acc, toFV(decodeXf(vt, vaddr_ + (idx * mc + m) * vsz)), stateF(0x2C + m)); }
            else acc = toFV(decodeXf(vt, vaddr_ + idx * vsz));
            ctrl[static_cast<size_t>(cu * vcount + cv)] = acc;
        }
        auto C = [&](int u, int v) -> const FV& { return ctrl[static_cast<size_t>(u * vcount + v)]; };
        { const bool pdbg = patchDebug_; static int nP = 0;   // debug: the control points as read from memory and the matrices in effect
          float extent = 0; for (int cu = 0; cu < ucount; ++cu) for (int cv = 0; cv < vcount; ++cv) { const FV& f = C(cu, cv); extent = std::max(extent, std::max(std::fabs(f.p[0]), std::fabs(f.p[1]))); }
          static uint64_t seenPatches = 0; ++seenPatches;
          if (pdbg && ((extent > 2000.f && nP < 14) || (dbgLeft > 0 && spline))) { ++nP; std::printf("  [PATCH] (patch #%llu of the run)%c", static_cast<unsigned long long>(seenPatches), 10);
            std::printf("  [PATCH] %s u=%d v=%d vt=%06X vaddr=%08X vsz=%u idxFmt=%u div=%ux%u texA0=%08X texA8=%08X texen=%u%c", spline ? "SPLINE" : "BEZIER", ucount, vcount, vt, vaddr_, vsz, idxFmt, divS, divT, state_[0xA0], state_[0xA8], state_[0x1E] & 1u, 10);
            for (int cv = 0; cv < vcount && cv < 4; ++cv) { std::printf("    row %d:", cv); for (int cu = 0; cu < ucount && cu < 4; ++cu) std::printf(" (%.1f,%.1f,%.1f)", C(cu, cv).p[0], C(cu, cv).p[1], C(cu, cv).p[2]); std::printf("%c", 10); }
            std::printf("    alphatest enable=%u (0xDB)=%08X blend=%u fixa=%06X fixb=%06X material: update(0x53)=%08X ambient(0x55)=%08X diffuse(0x56)=%08X matalpha(0x58)=%08X sceneambient(0x5C)=%08X ambalpha(0x5D)=%08X%c", state_[0x22] & 1u, state_[0xDB], state_[0x21] & 1u, state_[0xE0] & 0xFFFFFFu, state_[0xE1] & 0xFFFFFFu, state_[0x53], state_[0x55], state_[0x56], state_[0x58], state_[0x5C], state_[0x5D], 10);
            std::printf("    mip: texmode(0xC2)=%08X texlevel(0xC8)=%08X maxlevel=%u size0=%04X size1=%04X size2=%04X addr0=%06X addr1=%06X addr2=%06X bw1=%04X%c", state_[0xC2], state_[0xC8], (state_[0xC2] >> 16) & 7u, state_[0xB8] & 0xFFFFu, state_[0xB9] & 0xFFFFu, state_[0xBA] & 0xFFFFu, state_[0xA0] & 0xFFFFFFu, state_[0xA1] & 0xFFFFFFu, state_[0xA2] & 0xFFFFFFu, state_[0xA9] & 0xFFFFu, 10);
            std::printf("    texfilter=%08X texwrap=%08X texfunc=%08X texmapmode=%08X (mode %u) shadelights=%08X texscale=(%g,%g) texoffset=(%g,%g) lighting=%u%c", state_[0xC6], state_[0xC7], state_[0xC9], state_[0xC0], state_[0xC0] & 3u, state_[0xC1], stateF(0x48), stateF(0x49), stateF(0x4A), stateF(0x4B), state_[0x17] & 1u, 10);
            for (uint32_t l = 0; l < 4; ++l) std::printf("    light%u type=%08X pos=(%g,%g,%g)%c", l, state_[0x5F + l], stateF(0x63 + l * 3), stateF(0x64 + l * 3), stateF(0x65 + l * 3), 10);
            std::printf("    morph weights:"); for (uint32_t m = 0; m < 8; ++m) std::printf(" %g", stateF(0x2C + m)); std::printf("%c", 10);
            std::printf("    world:"); for (int q = 0; q < 12; ++q) std::printf(" %g", world_[q]); std::printf("%c    view:", 10); for (int q = 0; q < 12; ++q) std::printf(" %g", view_[q]);
            std::printf("%c    proj:", 10); for (int q = 0; q < 16; ++q) std::printf(" %g", proj_[q]); std::printf("%c    viewport scale=(%g,%g,%g) offset=(%g,%g,%g)%c", 10, stateF(0x42), stateF(0x43), stateF(0x44), stateF(0x45), stateF(0x46), stateF(0x47), 10); } }
        int divU, divV;
        if (spline) { divU = divS * (ucount - 3); divV = divT * (vcount - 3); } else { divU = divS * ((ucount - 1) / 3); divV = divT * ((vcount - 1) / 3); }
        if (divU <= 0 || divV <= 0 || divU > 512 || divV > 512) return;
        std::vector<FV> grid(static_cast<size_t>(divU + 1) * static_cast<size_t>(divV + 1));
        auto G = [&](int i, int j) -> FV& { return grid[static_cast<size_t>(j) * static_cast<size_t>(divU + 1) + static_cast<size_t>(i)]; };
        // Spline surface normals (control points without normals) from the exact derivatives of the basis functions, as PPSSPP does, instead of forward differences of the
        // generated grid (one-sided and half a cell off; at the surface's last row they made the XMB wave's lower edge a razor-thin dashed line). GeOptions::analyticNormals=false restores the old way.
        const bool analyticN = analyticNormalsOn_ && spline && ((vt >> 5) & 3u) == 0u;
        std::vector<float> tang;
        if (spline) {
            const int n = ucount - 1, m = vcount - 1;
            const std::vector<int> ku = splineKnot(n, utype), kv = splineKnot(m, vtype);
            const float limit = 2.000001f; const int order = 3;
            // The last sample sits at the end of the domain (n-2). In float, n-2.000001 rounds to exactly n-2 for some sizes (n=21), which falls outside the half-open last
            // knot interval: every basis value was zero and the whole last column of the surface collapsed to the origin (the XMB wave's right end was cut off). Keep it just inside.
            const float uEnd = std::nextafter(static_cast<float>(n - 2), 0.f), vEnd = std::nextafter(static_cast<float>(m - 2), 0.f);
            // The basis values depend only on the U position (per column) or only on the V position (per row), so each is evaluated once and reused
            // (the original recomputed splineN for every one of the 16 control points of every grid point). The same function is called with the same
            // arguments, so every value -- and the product/accumulation order below -- is unchanged.
            std::vector<float> suDer(analyticN ? (static_cast<size_t>(divU) + 1u) * 4u : 0u); if (analyticN) tang.assign(grid.size() * 6u, 0.f);
            auto dN = [&](int i3, float t, const std::vector<int>& kn) { float r = 0.f; const float d1 = static_cast<float>(kn[static_cast<size_t>(i3 + 3)] - kn[static_cast<size_t>(i3)]), d2 = static_cast<float>(kn[static_cast<size_t>(i3 + 4)] - kn[static_cast<size_t>(i3 + 1)]);
                if (d1 != 0.f) r += splineN(i3, 2, t, kn) / d1; if (d2 != 0.f) r -= splineN(i3 + 1, 2, t, kn) / d2; return 3.f * r; };
            std::vector<int> endUs(static_cast<size_t>(divU) + 1u); std::vector<float> cus(static_cast<size_t>(divU) + 1u), suBasis((static_cast<size_t>(divU) + 1u) * 4u);
            {
                int endU = order;
                for (int i = 0; i <= divU; ++i) {
                    const float cu = std::min(static_cast<float>(i) * (static_cast<float>(n) - limit) / static_cast<float>(divU), uEnd);
                    while (endU < n && cu >= static_cast<float>(ku[static_cast<size_t>(endU + 1)])) ++endU;
                    endUs[static_cast<size_t>(i)] = endU; cus[static_cast<size_t>(i)] = cu;
                    for (int k = 0; k < 4; ++k) suBasis[static_cast<size_t>(i) * 4u + static_cast<size_t>(k)] = splineN(endU - order + k, order, cu, ku);
                    if (analyticN) for (int k = 0; k < 4; ++k) suDer[static_cast<size_t>(i) * 4u + static_cast<size_t>(k)] = dN(endU - order + k, cu, ku);
                }
            }
            int endV = order;
            for (int j = 0; j <= divV; ++j) {
                const float cv = std::min(static_cast<float>(j) * (static_cast<float>(m) - limit) / static_cast<float>(divV), vEnd);
                while (endV < m && cv >= static_cast<float>(kv[static_cast<size_t>(endV + 1)])) ++endV;
                float svBasis[4];
                for (int k = 0; k < 4; ++k) svBasis[k] = splineN(endV - order + k, order, cv, kv);
                float svDer[4] = {0, 0, 0, 0}; if (analyticN) for (int k = 0; k < 4; ++k) svDer[k] = dN(endV - order + k, cv, kv);
                for (int i = 0; i <= divU; ++i) {
                    const int endU = endUs[static_cast<size_t>(i)]; const float cu = cus[static_cast<size_t>(i)];
                    FV pt;
                    for (int ii = endU - order; ii <= endU; ++ii) for (int jj = endV - order; jj <= endV; ++jj) {
                        const float f = suBasis[static_cast<size_t>(i) * 4u + static_cast<size_t>(ii - (endU - order))] * svBasis[jj - (endV - order)];
                        if (f != 0.f) multAdd(pt, C(ii, jj), f);
                    }
                    if (analyticN) { float* tg = &tang[(static_cast<size_t>(j) * static_cast<size_t>(divU + 1) + static_cast<size_t>(i)) * 6u];
                        for (int ii = endU - order; ii <= endU; ++ii) for (int jj = endV - order; jj <= endV; ++jj) {
                            const FV& c = C(ii, jj); const float bu = suBasis[static_cast<size_t>(i) * 4u + static_cast<size_t>(ii - (endU - order))], bv = svBasis[jj - (endV - order)];
                            const float du = suDer[static_cast<size_t>(i) * 4u + static_cast<size_t>(ii - (endU - order))] * bv, dv = bu * svDer[jj - (endV - order)];
                            for (int q = 0; q < 3; ++q) { tg[q] += du * c.p[q]; tg[3 + q] += dv * c.p[q]; } } }
                    if (texEnabled && !hasUv) { pt.t[0] = cu; pt.t[1] = cv; }
                    G(i, j) = pt;
                }
            }
        } else {
            const int upc = ucount / 3, vpc = vcount / 3;
            for (int j = 0; j <= divV; ++j) {
                const float vg = static_cast<float>(j) * static_cast<float>(vpc) / static_cast<float>(divV);
                int vp = static_cast<int>(vg); float cv = vg - static_cast<float>(vp);
                if (j == divV) { --vp; cv = 1.f; }
                float vc[4]; bernstein(cv, vc);
                for (int i = 0; i <= divU; ++i) {
                    const float ug = static_cast<float>(i) * static_cast<float>(upc) / static_cast<float>(divU);
                    int up = static_cast<int>(ug); float cu = ug - static_cast<float>(up);
                    if (i == divU) { --up; cu = 1.f; }
                    float uc[4]; bernstein(cu, uc);
                    FV pt;
                    for (int ii = 0; ii < 4; ++ii) for (int jj = 0; jj < 4; ++jj) multAdd(pt, C(3 * up + ii, 3 * vp + jj), uc[ii] * vc[jj]);
                    if (texEnabled && !hasUv) { pt.t[0] = ug; pt.t[1] = vg; }
                    G(i, j) = pt;
                }
            }
        }
        if (((vt >> 5) & 3u) == 0u) { // no normals in the control points: derive them from the surface
            const bool reverse = (state_[0x38] & 1u) != 0;
            auto dir = [](float* d, const FV& a, const FV& b) { for (int q = 0; q < 3; ++q) d[q] = b.p[q] - a.p[q]; };
            for (int j = 0; j <= divV; ++j) for (int i = 0; i <= divU; ++i) {
                float d1[3], d2[3];
                if (analyticN) { const float* tg = &tang[(static_cast<size_t>(j) * static_cast<size_t>(divU + 1) + static_cast<size_t>(i)) * 6u]; for (int q = 0; q < 3; ++q) { d1[q] = tg[q]; d2[q] = tg[3 + q]; } }
                else {
                if (i < divU) dir(d1, G(i, j), G(i + 1, j)); else dir(d1, G(i - 1, j), G(i, j));
                if (j < divV) dir(d2, G(i, j), G(i, j + 1)); else dir(d2, G(i, j - 1), G(i, j));
                }
                float* n = G(i, j).n;
                n[0] = d1[1] * d2[2] - d1[2] * d2[1]; n[1] = d1[2] * d2[0] - d1[0] * d2[2]; n[2] = d1[0] * d2[1] - d1[1] * d2[0];
                if (reverse) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
            }
        }
        pLap(psr.pGridSec);
        std::vector<SVert> sv(grid.size());
        for (size_t k = 0; k < grid.size(); ++k) sv[k] = project(fromFV(grid[k]));
        pLap(psr.pProjSec);
        auto S = [&](int i, int j) -> const SVert& { return sv[static_cast<size_t>(j) * static_cast<size_t>(divU + 1) + static_cast<size_t>(i)]; };
        if (dbgLeft > 0 && spline) { // frame debugger: where the generated texture coordinates and screen positions fall
            int bins[10] = {}; float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f, xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f; size_t okc = 0;
            for (const SVert& v : sv) { if (!v.ok) continue; ++okc; umin = std::min(umin, v.u); umax = std::max(umax, v.u); vmin = std::min(vmin, v.v); vmax = std::max(vmax, v.v); xmin = std::min(xmin, v.x); xmax = std::max(xmax, v.x); ymin = std::min(ymin, v.y); ymax = std::max(ymax, v.y);
                const int b = static_cast<int>(std::floor(std::min(0.999f, std::max(0.f, v.u)) * 10.f)); ++bins[b]; }
            { float zlo = 1e30f, zhi = -1e30f; for (const SVert& v : sv) if (v.ok) { zlo = std::min(zlo, v.z); zhi = std::max(zhi, v.z); } std::printf("    [SPLINE z] range [%.0f,%.0f]; ztest=%u zwrite-disable=%u%c", zlo, zhi, state_[0x23] & 1u, state_[0xE7] & 1u, 10); }
            std::printf("    [SPLINE uv] vertices ok=%zu/%zu u=[%.3f,%.3f] v=[%.3f,%.3f] screen x=[%.0f,%.0f] y=[%.0f,%.0f] u histogram (tenths):", okc, sv.size(), umin, umax, vmin, vmax, xmin, xmax, ymin, ymax);
            for (int b = 0; b < 10; ++b) std::printf(" %d", bins[b]); std::printf("%c", 10);
            { int vb[10] = {}; for (const SVert& v : sv) if (v.ok) ++vb[static_cast<int>(std::floor(std::min(0.999f, std::max(0.f, v.v)) * 10.f))]; std::printf("    v histogram (tenths):"); for (int b = 0; b < 10; ++b) std::printf(" %d", vb[b]); std::printf("%c", 10); }
            for (int jj : {0, 6, 12, 18, 23}) { if (jj > divV) continue; std::printf("    row j=%d rgba:", jj); for (int ii = 0; ii <= divU; ii += 6) std::printf(" %02X%02X%02X/%02X", S(ii, jj).r, S(ii, jj).g, S(ii, jj).b, S(ii, jj).a); std::printf("%c", 10); }
            { float amin = 1e9f, amax = -1e9f; for (const auto& g2 : grid) { amin = std::min(amin, g2.c[3]); amax = std::max(amax, g2.c[3]); } std::printf("    raw interpolated vertex alpha range [%g,%g]%c", amin, amax, 10); }
            for (int jj : {2, 8, 16, 24, 30}) { if (jj > divV) continue; std::printf("    row j=%d u:", jj); for (int ii = 0; ii <= divU; ii += 3) std::printf(" %.2f", S(ii, jj).u); std::printf("%c", 10); }
        }
        ++primsTransform;
        const uint64_t pxBefore = pixelsWritten;
        struct Logger { GeRenderer* r; const char* kind; int uc, vc, du, dv; uint32_t vt; uint64_t before; const SVert* sv0; const SVert* svm;
            ~Logger() { if (!r->recentPrimsOn()) return; char buf[320]; std::snprintf(buf, sizeof(buf), "%s u=%d v=%d div=%dx%d vt=%06X tex=%u texaddr=%06X blend=%u bm=%03X ztest=%u light=%u px=%llu | s0=(%.0f,%.0f ok=%d rgba=%02X%02X%02X%02X) sm=(%.0f,%.0f)",
                kind, uc, vc, du, dv, vt, r->state_[0x1E] & 1u, r->state_[0xA0] & 0xFFFFFFu, r->state_[0x21] & 1u, r->state_[0xDF] & 0xFFFu, r->state_[0x23] & 1u, r->state_[0x17] & 1u,
                static_cast<unsigned long long>(r->pixelsWritten - before), sv0->x, sv0->y, sv0->ok, sv0->r, sv0->g, sv0->b, sv0->a, svm->x, svm->y);
                r->recentPrims.push_back(buf); if (r->recentPrims.size() > r->recentMax()) r->recentPrims.erase(r->recentPrims.begin()); } } lg{this, spline ? "SPLINE" : "BEZIER", ucount, vcount, divU, divV, vt, pxBefore, &sv[0], &sv[sv.size() / 2]};
        if ((state_[0x37] & 3u) != 0) return; // patch lines/points: not drawn yet
        {
            BatchScope batch(this);
            struct PatchFlag { bool& f; explicit PatchFlag(bool& x) : f(x) { f = true; } ~PatchFlag() { f = false; } } patchFlag(inPatch_);
            for (int j = 0; j < divV; ++j) for (int i = 0; i < divU; ++i) {
                triXf(S(i, j), S(i, j + 1), S(i + 1, j));
                triXf(S(i + 1, j), S(i, j + 1), S(i + 1, j + 1));
            }
            if (dbgLeft > 0) { // frame debugger: how many triangles survived setup, and what the texture gives at a sample vertex
                const int tw = 1 << (state_[0xB8] & 0xF), th = 1 << ((state_[0xB8] >> 8) & 0xF);
                std::printf("    queued triangles: %zu of %d; cull enable=%u mode=%u patchcull(0x26)=%u patchface(0x38)=%u; ", triQueue_.size(), 2 * divU * divV, state_[0x1D] & 1u, state_[0x9B] & 1u, state_[0x26] & 1u, state_[0x38] & 1u);
                for (int jj : {2, 8, 14}) { const SVert& v = S(divU / 2, jj); const uint32_t c = texel(0, std::min(tw - 1, std::max(0, static_cast<int>(v.u * tw))), std::min(th - 1, std::max(0, static_cast<int>(v.v * th)))); std::printf("[j=%d uv=(%.2f,%.2f) texel=%08X] ", jj, v.u, v.v, c); }
                std::printf("%c", 10);
            }
            pLap(psr.pSetupSec);
        }
    }

    void primTransform(uint32_t type, uint32_t count, uint32_t vt) {
        const uint64_t pb = pixelsWritten; StatRegistry& sr = statRegistry(); const auto t0 = sr.on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        primTransformImpl(type, count, vt); dbgAfterDraw("PRIM", pb);
        if (sr.on) { sr.primXfSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); ++sr.primXfCalls; }
    }
    void primTransformImpl(uint32_t type, uint32_t count, uint32_t vt) {
        maybeDumpTexture("prim");
        colorPresent_ = ((vt >> 2) & 7u) >= 4u;
        if (count == 0 || !mem_.r32) return;
        const uint32_t vsz = vertexSize(vt), idxFmt = (vt >> 11) & 3u;
        const uint32_t mc = ((vt >> 18) & 7u) + 1u;
        std::vector<SVert> sv(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = i;
            if (idxFmt == 1) idx = dr8(iaddr_ + i); else if (idxFmt == 2) idx = dr16(iaddr_ + i * 2u);
            if (mc > 1) {
                FV acc;
                for (uint32_t m = 0; m < mc; ++m) multAdd(acc, toFV(decodeXf(vt, vaddr_ + (idx * mc + m) * vsz)), stateF(0x2C + m));
                sv[i] = project(fromFV(acc));
            } else sv[i] = project(decodeXf(vt, vaddr_ + idx * vsz));
        }
        ++primsTransform;
        { const int64_t boxFrom = xfBoxFrom_; static int nBox = 0; const bool box = boxFrom >= 0 && static_cast<int64_t>(primsTransform) >= boxFrom;   // debug: the screen-space bounding box of each big transform primitive
          if (box && count >= 100 && nBox < 60) { float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f; uint32_t ok = 0; for (const SVert& v : sv) if (v.ok) { ++ok; x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
            ++nBox; std::printf("  [XFBOX] type=%u n=%u ok=%u x=[%.1f,%.1f] y=[%.1f,%.1f] tex=%06X bm=%03X%c", type, count, ok, x0, x1, y0, y1, state_[0xA0] & 0xFFFFFFu, state_[0xDF] & 0xFFFu, 10); } }
        if (recentPrimsOn()) {
            char buf[300];
            std::snprintf(buf, sizeof(buf), "XF type=%u n=%u vt=%06X tex=%u tf=%X/%X texaddr=%06X size=%X blend=%u bm=%03X ztest=%u zfunc=%u | v0=(%.1f,%.1f z=%.0f rgba=%02X%02X%02X%02X uv=%.2f,%.2f)",
                          type, count, vt, state_[0x1E] & 1u, state_[0xC3] & 0xF, state_[0xC9] & 0x1FFu, state_[0xA0] & 0xFFFFFFu, state_[0xB8] & 0xFFFFu, state_[0x21] & 1u, state_[0xDF] & 0xFFFu,
                          state_[0x23] & 1u, state_[0xDE] & 7u, sv[0].x, sv[0].y, sv[0].z, sv[0].r, sv[0].g, sv[0].b, sv[0].a, sv[0].u, sv[0].v);
            recentPrims.push_back(buf);
            if (recentPrims.size() > recentMax()) recentPrims.erase(recentPrims.begin());
        }
        const uint64_t pxBeforeXf = pixelsWritten;
        {
            BatchScope batch(this); // queue the primitive's triangles and rasterise them together (bands in parallel, per-pixel order preserved)
            switch (type) {
                case 3: for (uint32_t i = 0; i + 2 < count; i += 3) triXf(sv[i], sv[i + 1], sv[i + 2]); break;
                case 4: for (uint32_t i = 0; i + 2 < count; ++i) (i & 1u) ? triXf(sv[i + 1], sv[i], sv[i + 2]) : triXf(sv[i], sv[i + 1], sv[i + 2]); break;
                case 5: for (uint32_t i = 1; i + 1 < count; ++i) triXf(sv[0], sv[i], sv[i + 1]); break;
                case 6: for (uint32_t i = 0; i + 1 < count; i += 2) rectXf(sv[i], sv[i + 1]); break;
                case 0: case 1: case 2: ++xfPointsLines; drawXfPointsLines(type, sv, count); break;
                default: break;
            }
        }
        if (!recentPrims.empty()) { char tail[64]; std::snprintf(tail, sizeof(tail), " px=%llu", static_cast<unsigned long long>(pixelsWritten - pxBeforeXf)); recentPrims.back() += tail; }
    }

    // Points (type 0), lines (1) and line strips (2) in transform mode: the projected screen-space vertices go through the same plot()/line() code as through-mode points
    // and lines (one pixel wide, no z test, a vertex that could not be projected skips its line). Until 2026-10-14 these were not drawn at all, which left the music player's
    // "electric" wire-wave visualiser a black screen.
    void drawXfPointsLines(uint32_t type, const std::vector<SVert>& sv, uint32_t count) {
        auto V = [](const SVert& s) { Vertex v; v.x = s.x; v.y = s.y; v.z = s.z; v.u = s.u; v.v = s.v; v.r = s.r; v.g = s.g; v.b = s.b; v.a = s.a; return v; };
        const bool clear = (state_[0xD3] & 1u) != 0;
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) ggpu_->syncToMemory(); // a software draw is about to read and write emulated memory
#endif
        if (type == 0) { for (uint32_t i = 0; i < count; ++i) if (sv[i].ok) plot(static_cast<int>(sv[i].x), static_cast<int>(sv[i].y), V(sv[i]), clear); }
        else if (type == 1) { for (uint32_t i = 0; i + 1 < count; i += 2) if (sv[i].ok && sv[i + 1].ok) line(V(sv[i]), V(sv[i + 1]), clear); }
        else { for (uint32_t i = 0; i + 1 < count; ++i) if (sv[i].ok && sv[i + 1].ok) line(V(sv[i]), V(sv[i + 1]), clear); }
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) ggpu_->memoryChanged();
#endif
    }

    void rectXf(const SVert& a, const SVert& b) {
        if (!a.ok || !b.ok) return;
        // A transformed rectangle is two triangles spanning the two corners.
        SVert c = a, d = a;
        c.x = b.x; c.u = b.u; c.y = a.y; c.v = a.v; d.x = a.x; d.u = a.u; d.y = b.y; d.v = b.v;
        SVert bb = b; bb.r = b.r; bb.g = b.g; bb.b = b.b; bb.a = b.a;
        c.r = d.r = b.r; c.g = d.g = b.g; c.b = d.b = b.b; c.a = d.a = b.a; SVert aa = a; aa.r = b.r; aa.g = b.g; aa.b = b.b; aa.a = b.a;
        triXf(aa, c, bb, true); triXf(aa, bb, d, true);
    }

    uint32_t zAddr() const { return vram((state_[0x9E] & 0xFFFFFFu) | ((state_[0x9F] & 0xFF0000u) << 8)); }

    // The last bilinear footprint a rasteriser fetched: magnified textures give long runs of pixels with the same (x0, y0), whose four texels
    // (as float lanes) are then reused instead of re-fetched. Only used when PlotCtx::texCacheOk (so the texels cannot have changed).
    struct TexCache { int x0 = INT_MIN, y0 = INT_MIN; __m128 f[4]; };
    struct PlotCtx {
        int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
        int ffmt = 3; uint32_t stride = 1, bytes = 4, fbBase = 0; bool texNull = false;
        uint8_t* fbHost = nullptr; uint32_t fbLimit = 0;            // direct host window of the framebuffer (fbHost == nullptr: use dr/dw)
        bool texOn = false, texLinear = false, texUseTa = false, texDouble = false; uint32_t texFunc = 0;
        uint32_t tw = 1, th = 1, texStride = 1, rowBytes = 0, texBaseA = 0; int texFmt = 0, texBpp = 16; bool clampU = false, clampV = false, swz = false;
        uint32_t clutCf = 0, clutShift = 0, clutMask = 0, clutStart = 0, swzBlocks = 0; // clutCf decoded once; swzBlocks = rowBytes / 16
        uint8_t* texHost = nullptr; uint32_t texLimit = 0;
        bool alphaTest = false; uint32_t atF = 0, atRef = 0, atMask = 0;
        bool blend = false; uint32_t sf = 0, df = 0, eq = 0, fixa = 0, fixb = 0;
        uint32_t cm = 0, mrgb = 0, mal = 0;
        bool texCacheOk = false;                                     // the texture lies in a host window and cannot be overwritten by this batch: texel footprints may be reused between pixels
        const uint32_t* colTab = nullptr;                            // per-batch lookup tables (only with texCacheOk): texColPart(px) for px < tw and texRowPart(py) for py < th,
        const uint32_t* rowTab = nullptr;                            // so a texel address is two table reads, and every texel read is inside the host window (no bounds checks)
        bool needDst = false;                                        // destination pixel needed even outside clear mode (blend or a write mask)
    };

    // ---- transform-mode triangle rasteriser (2026-10-02): setup / queue / rasterise-in-bands ------------------------------------------------
    // triXf() only does the per-triangle setup (culling, bounding box) and queues the triangle. flushTris() then rasterises everything queued for
    // one primitive: the screen rows are cut into chunks that a small thread pool pulls from an atomic counter, and EVERY thread processes ALL the
    // queued triangles, in their original order, restricted to its own rows. A pixel is only ever touched by the thread owning its row, and it sees
    // the same triangles in the same order as the serial rasteriser did, so the output is bit-identical regardless of the thread count.
    struct TriJob { SVert a, b, c; float area = 0.f; int minx = 0, maxx = 0, miny = 0, maxy = 0; };
    struct RasterCtx {
        PlotCtx pc; bool ztest = false, zwrite = false, zwriteLive = false, flat = false; uint32_t zfn = 0, zbase = 0, zstride = 1;
        uint8_t* zHost = nullptr; uint32_t zLimit = 0; float uScale = 1.f, vScale = 1.f;
    };
#ifdef BETA_GE_GPU_ACCEL
    // ---- GPU-resident surface path (ge_gpu.h) ----------------------------------------------------------------------------------------------
    // 0 = the GPU can draw this state, > 0 = the reason it cannot (see gpuWhy), -1 = nothing would be visible (empty scissor).
    int gpuPrepare(const PlotCtx& pc, bool ztest, bool clear, GeGpu::Draw& d) const {
        if (pc.ffmt != 3 || !pc.fbHost || pc.stride < 64u || pc.stride > 1024u || (pc.fbBase & 3u) != 0u) return 1;
        if (!win_[0].p || (pc.fbBase & 0x1FFFFFFFu) - win_[0].base >= win_[0].size) return 1; // only VRAM: accesses to it all pass through the bus hook
        if (static_cast<uint64_t>(pc.stride) * GeGpu::kRows * 4u > pc.fbLimit) return 1;
        if (pc.sx1 > pc.sx2 || pc.sy1 > pc.sy2) return -1;
        if (pc.sx2 >= static_cast<int>(pc.stride) || pc.sy2 >= static_cast<int>(GeGpu::kRows)) return 1;
        if (ztest) return 2;
        if ((pc.mrgb & 0xFFFFFFu) != 0u || (pc.mal & 0xFFu) != 0u) return 3;
        d.base = pc.fbBase; d.stride = pc.stride; d.host = pc.fbHost;
        d.sx1 = pc.sx1; d.sy1 = pc.sy1; d.sx2 = pc.sx2; d.sy2 = pc.sy2;
        if (clear) { d.clear = true; d.clearColor = (pc.cm & 0x100u) != 0u; d.clearAlpha = (pc.cm & 0x200u) != 0u; return 0; }
        if (pc.texOn) {
            if (pc.texFmt > 7 || !pc.texHost || pc.tw > 1024u || pc.th > 1024u) return 4;
            d.texOn = true; d.linear = pc.texLinear; d.func = pc.texFunc; d.useTa = pc.texUseTa; d.dbl = pc.texDouble;
            d.clampU = pc.clampU; d.clampV = pc.clampV; d.tw = pc.tw; d.th = pc.th;
        }
        if (pc.alphaTest) { d.atest = true; d.atF = pc.atF; d.atRef = pc.atRef; d.atMask = pc.atMask; }
        if (pc.blend) {
            uint32_t fix = 0;
            if (!GeGpu::blendSupported(pc.sf, pc.df, pc.eq, pc.fixa, pc.fixb, fix)) return 5;
            d.blend = true; d.sf = pc.sf; d.df = pc.df; d.eq = pc.eq; d.fix = fix;
        }
        return 0;
    }
    uint64_t clutHash() {
        if (!clutHashValid_) {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < 1024u; ++i) { h ^= clut_[i]; h *= 1099511628211ull; }
            clutHash_ = h; clutHashValid_ = true;
        }
        return clutHash_;
    }
    bool clutHashValid_ = false; uint64_t clutHash_ = 0;
    // The GPU texture for the currently bound texture (decoded once, reused while the source bytes and palette are unchanged); null if it cannot be used.
    ID3D11ShaderResourceView* gpuTexture(const PlotCtx& pc) {
        const uint32_t rows = pc.swz ? ((pc.th + 7u) & ~7u) : pc.th;
        const uint64_t foot = static_cast<uint64_t>(pc.swz && pc.rowBytes < 16u ? 16u : pc.rowBytes) * rows;
        if (foot == 0 || foot > pc.texLimit) return nullptr;
        if (ggpu_->anyNewerOverlaps(pc.texBaseA, foot)) ggpu_->syncToMemory(); // render-to-texture: the bytes must be current before they are read
        const bool indexed = pc.texFmt >= 4 && pc.texFmt <= 7;
        const GeGpu::TexKey key{pc.texBaseA, pc.tw, pc.th, pc.rowBytes, static_cast<uint32_t>(pc.texFmt), pc.swz ? 1u : 0u, indexed ? pc.clutCf : 0u, indexed ? clutHash() : 0ull};
        return ggpu_->texture(key, pc.texHost, static_cast<size_t>(foot), indexed ? clut_ : nullptr, [&](std::vector<uint8_t>& rgba) {
            for (uint32_t y = 0; y < pc.th; ++y) {
                const uint32_t row = texRowPart(pc, y);
                for (uint32_t x = 0; x < pc.tw; ++x) { const uint32_t t = texelAtOffset(pc, x, row + texColPart(pc, x)); std::memcpy(&rgba[(static_cast<size_t>(y) * pc.tw + x) * 4u], &t, 4); }
            }
        });
    }
    std::vector<GeGpu::Vtx> gpuVerts_;
    // Draws the queued transform-mode triangles on the GPU. false = the caller must rasterise them in software.
    bool gpuFlushQueue(const RasterCtx& rc) {
        GeGpu::Draw d;
        const int why = gpuPrepare(rc.pc, rc.ztest, false, d);
        if (why > 0) { ++gpuWhy[why]; return false; }
        if (why < 0) return true;
        ID3D11ShaderResourceView* srv = nullptr;
        if (d.texOn) { srv = gpuTexture(rc.pc); if (!srv) { ++gpuWhy[4]; return false; } }
        gpuVerts_.clear();
        for (const TriJob& t : triQueue_) {
            const SVert* vs3[3] = {&t.a, &t.b, &t.c};
            for (int k = 0; k < 3; ++k) {
                const SVert& v = *vs3[k];
                if (!(v.iw > 0.f) || !std::isfinite(v.iw)) { ++gpuWhy[6]; return false; }
                const SVert& cc = rc.flat ? t.c : v;
                gpuVerts_.push_back({v.x, v.y, static_cast<float>(1.0 / static_cast<double>(v.iw)), v.u * rc.uScale, v.v * rc.vScale, cc.r, cc.g, cc.b, cc.a});
            }
        }
        if (!ggpu_->draw(d, gpuVerts_.data(), gpuVerts_.size(), srv)) { ++gpuWhy[7]; return false; }
        ++gpuBatches;
        return true;
    }
    // Through-mode triangles (types 3-5) and sprites (6), including clear mode. false = software.
    bool gpuThroughPrim(const std::vector<Vertex>& vs, uint32_t type, uint32_t count, bool clear) {
        if (type < 3 || type > 6) return false;
        const PlotCtx pc = makePlotCtx();
        GeGpu::Draw d;
        const int why = gpuPrepare(pc, false, clear, d);
        if (why > 0) { ++gpuWhy[why]; return false; }
        if (why < 0) return true;
        ID3D11ShaderResourceView* srv = nullptr;
        if (d.texOn) { srv = gpuTexture(pc); if (!srv) { ++gpuWhy[4]; return false; } }
        const bool flat = (state_[0x50] & 1u) == 0u;
        gpuVerts_.clear();
        auto V = [&](const Vertex& v, const Vertex& col) { gpuVerts_.push_back({v.x, v.y, 1.f, v.u, v.v, col.r, col.g, col.b, col.a}); };
        auto tri = [&](const Vertex& a, const Vertex& b, const Vertex& c) { V(a, flat ? c : a); V(b, flat ? c : b); V(c, flat ? c : c); };
        switch (type) {
            case 3: for (uint32_t i = 0; i + 2 < count; i += 3) tri(vs[i], vs[i + 1], vs[i + 2]); break;
            case 4: for (uint32_t i = 0; i + 2 < count; ++i) (i & 1u) ? tri(vs[i + 1], vs[i], vs[i + 2]) : tri(vs[i], vs[i + 1], vs[i + 2]); break;
            case 5: for (uint32_t i = 1; i + 1 < count; ++i) tri(vs[0], vs[i], vs[i + 1]); break;
            default:
                for (uint32_t i = 0; i + 1 < count; i += 2) {
                    const Vertex& a = vs[i]; const Vertex& b = vs[i + 1];
                    const int x0 = static_cast<int>(a.x), y0 = static_cast<int>(a.y), x1 = static_cast<int>(b.x), y1 = static_cast<int>(b.y);
                    const int lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
                    if (lx == hx || ly == hy) continue;
                    const float w = static_cast<float>(x1 - x0), h = static_cast<float>(y1 - y0);
                    auto uAt = [&](int x) { return a.u + (b.u - a.u) * static_cast<float>(x - x0) / w; };
                    auto vAt = [&](int y) { return a.v + (b.v - a.v) * static_cast<float>(y - y0) / h; };
                    Vertex q00 = b, q10 = b, q01 = b, q11 = b;
                    q00.x = static_cast<float>(lx); q00.y = static_cast<float>(ly); q00.u = uAt(lx); q00.v = vAt(ly);
                    q10.x = static_cast<float>(hx); q10.y = static_cast<float>(ly); q10.u = uAt(hx); q10.v = vAt(ly);
                    q01.x = static_cast<float>(lx); q01.y = static_cast<float>(hy); q01.u = uAt(lx); q01.v = vAt(hy);
                    q11.x = static_cast<float>(hx); q11.y = static_cast<float>(hy); q11.u = uAt(hx); q11.v = vAt(hy);
                    V(q00, b); V(q10, b); V(q11, b); V(q00, b); V(q11, b); V(q01, b);
                }
                break;
        }
        if (!ggpu_->draw(d, gpuVerts_.data(), gpuVerts_.size(), srv)) { ++gpuWhy[7]; return false; }
        ++gpuThroughBatches;
        return true;
    }
#endif
    // Optional draw-state statistics (PSP_GE_STATS=1): which feature combinations the transform-mode triangles use, weighted by triangles and by
    // pixels actually written -- used to decide what a GPU transform-mode pipeline must support to matter.
    struct StatKey { uint32_t k; };
    struct StatRow { uint64_t flushes = 0, tris = 0, pixels = 0; };
    struct StatRegistry {
        std::map<std::string, StatRow> rows; bool on = false; // set from GeOptions::stats by GeRenderer's own constructor
        double parSec = 0, serSec = 0, patchSec = 0, primXfSec = 0, primThroughSec = 0; uint64_t patchCalls = 0, primXfCalls = 0, primThroughCalls = 0; uint64_t parFlushes = 0, serFlushes = 0, parPixels = 0, serPixels = 0, parTris = 0, serTris = 0; uint64_t bucket[6] = {0, 0, 0, 0, 0, 0}, bucketPx[6] = {0, 0, 0, 0, 0, 0}; uint64_t whyN[8] = {}, whyPx[8] = {}; double whySec[8] = {}; std::atomic<uint64_t> p4Hit{0}, p4Why[6]{}, p4Lanes{0}; double pEvalSec = 0, pSetupSec = 0, pGridSec = 0, pProjSec = 0;
        ~StatRegistry() {
            if (!on) return;
            std::printf("  [GESTATS] time inside flushTris: parallel batches %.2f s, serial batches %.2f s%c", parSec, serSec, 10);
            std::printf("  [GESTATS] plot4: %llu quads taken (%llu lanes); refused: not-8888/no-host %llu, scissor edge %llu, texture may alias %llu%c", static_cast<unsigned long long>(p4Hit.load()), static_cast<unsigned long long>(p4Lanes.load()), static_cast<unsigned long long>(p4Why[0].load()), static_cast<unsigned long long>(p4Why[1].load()), static_cast<unsigned long long>(p4Why[2].load()), 10);
            std::printf("  [GESTATS] patch breakdown: control+grid eval %.2f s, project %.2f s, triangle setup/queue %.2f s (flush counted in the wrapper total)%c", pGridSec, pProjSec, pSetupSec, 10);
            std::printf("  [GESTATS] wrappers (include their flushTris time): patch/spline %.2f s in %llu calls, transform prims %.2f s in %llu calls, through prims %.2f s in %llu calls%c", patchSec,
                        static_cast<unsigned long long>(patchCalls), primXfSec, static_cast<unsigned long long>(primXfCalls), primThroughSec, static_cast<unsigned long long>(primThroughCalls), 10);
            std::printf("  [GESTATS] batches: %llu parallel (%llu px, %llu tris), %llu serial (%llu px, %llu tris)%c", static_cast<unsigned long long>(parFlushes), static_cast<unsigned long long>(parPixels),
                        static_cast<unsigned long long>(parTris), static_cast<unsigned long long>(serFlushes), static_cast<unsigned long long>(serPixels), static_cast<unsigned long long>(serTris), 10);
            static const char* names[6] = {"<500", "<2k", "<8k", "<20k", "<80k", ">=80k"};
            { static const char* wn[8] = {"parallel", "no pool/small(<3000px or <8 rows)", "fb not host/limit", "z not host/limit", "texture not host/limit", "texture overlaps fb", "texture overlaps z", "?"};
              for (int i = 1; i < 7; ++i) std::printf("  [GESTATS] serial because %-34s: %6llu batches, %12llu px, %.2f s%c", wn[i], static_cast<unsigned long long>(whyN[i]), static_cast<unsigned long long>(whyPx[i]), whySec[i], 10); }
            for (int i = 0; i < 6; ++i) std::printf("  [GESTATS] batch bbox-pixels %-6s : %8llu batches, %12llu pixels written%c", names[i], static_cast<unsigned long long>(bucket[i]), static_cast<unsigned long long>(bucketPx[i]), 10);
            std::vector<std::pair<std::string, StatRow>> v(rows.begin(), rows.end());
            std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.pixels > y.second.pixels; });
            uint64_t tp = 0, tt = 0; for (const auto& r : v) { tp += r.second.pixels; tt += r.second.tris; }
            std::printf("  [GESTATS] transform-mode: %llu triangles, %llu pixels written; top states by pixels:%c", static_cast<unsigned long long>(tt), static_cast<unsigned long long>(tp), 10);
            for (size_t i = 0; i < v.size() && i < 16; ++i)
                std::printf("  [GESTATS] %5.1f%% px %6.1f%% tris  %s%c", tp ? 100.0 * static_cast<double>(v[i].second.pixels) / static_cast<double>(tp) : 0.0,
                            tt ? 100.0 * static_cast<double>(v[i].second.tris) / static_cast<double>(tt) : 0.0, v[i].first.c_str(), 10);
        }
    };
    // Was a function-local static singleton (`static StatRegistry r`) shared across every
    // GeRenderer instance in the process -- confirmed (grep) there is only ever ONE
    // GeRenderer constructed anywhere in this codebase (mmio/ge_edram.h's GeCoreStub),
    // so the sharing bought nothing; now a plain per-instance member, set up from
    // GeOptions in the constructor instead of reading PSP_GE_STATS here directly.
    StatRegistry& statRegistry() { return stats_; }
    StatRegistry stats_;
    std::string statKey(const RasterCtx& rc) const {
        char b[200];
        std::snprintf(b, sizeof(b), "tex=%d fmt=%d func=%u%s filt=%s blend=%d(%u,%u,%u) zt=%d(f%u) zw=%d atest=%d gouraud=%d clutfmt/swz=%d", rc.pc.texOn ? 1 : 0, rc.pc.texFmt, rc.pc.texFunc,
                      rc.pc.texDouble ? "x2" : "", rc.pc.texLinear ? "linear" : "nearest", rc.pc.blend ? 1 : 0, rc.pc.sf, rc.pc.df, rc.pc.eq, rc.ztest ? 1 : 0, rc.zfn,
                      rc.zwriteLive ? 1 : 0, rc.pc.alphaTest ? 1 : 0, rc.flat ? 0 : 1, rc.pc.swz ? 1 : 0);
        return b;
    }
    std::vector<TriJob> triQueue_;
    bool batching_ = false;
    struct BatchScope { GeRenderer* r; explicit BatchScope(GeRenderer* g) : r(g) { r->batching_ = true; r->triQueue_.clear(); } ~BatchScope() { r->flushTris(); } };

    // A small persistent worker pool (GeOptions::threads overrides the worker count: -1 =
    // auto, 0 = serial). Was a function-local static singleton (`static RasterPool pool`)
    // shared across every GeRenderer instance -- same "only one GeRenderer ever exists"
    // reasoning as StatRegistry above, so this is now a plain per-instance member too.
    class RasterPool {
    public:
        explicit RasterPool(int threadsHint) {
            int n = threadsHint;
            if (n < 0) { const unsigned hw = std::thread::hardware_concurrency(); n = hw > 1 ? static_cast<int>(std::min(3u, hw - 1u)) : 0; }
            for (int i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
            for (std::thread& t : threads_) t.detach(); // process-lifetime pool
        }
        int workers() const { return static_cast<int>(threads_.size()); }
        void run(int chunks, const std::function<void(int)>& fn) {
            auto job = std::make_shared<Job>(); job->fn = fn; job->total = chunks;
            { std::lock_guard<std::mutex> lock(m_); job_ = job; ++gen_; }
            cv_.notify_all();
            work(*job);
            while (job->done.load(std::memory_order_acquire) < job->total) std::this_thread::yield();
        }
    private:
        struct Job { std::function<void(int)> fn; int total = 0; std::atomic<int> next{0}, done{0}; };
        void work(Job& j) { for (;;) { const int i = j.next.fetch_add(1); if (i >= j.total) break; j.fn(i); j.done.fetch_add(1, std::memory_order_release); } }
        void loop() {
            uint64_t seen = 0;
            for (;;) {
                std::shared_ptr<Job> job;
                { std::unique_lock<std::mutex> lock(m_); cv_.wait(lock, [&] { return gen_ != seen; }); seen = gen_; job = job_; }
                if (job) work(*job);
            }
        }
        std::vector<std::thread> threads_;
        std::mutex m_; std::condition_variable cv_; std::shared_ptr<Job> job_; uint64_t gen_ = 0;
    };
    RasterPool pool_; // constructed by GeRenderer's own constructor init list (needs GeOptions::threads)

    // Per-triangle setup. Returns false if nothing can be drawn.
    bool inPatch_ = false;   // set while a patch (spline/Bezier) is being drawn
    bool triSetup(const SVert& a, const SVert& b, const SVert& c, bool noCull, TriJob& job) const {
        if (!a.ok || !b.ok || !c.ok) return false;
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (area == 0.f) return false;
        if (!noCull && (state_[0x1D] & 1u)) { // back-face culling: CULL (0x9B) 1 = cull clockwise ... (screen y points down)
            const bool cw = area < 0.f;
            // PATCHFACING (0x38) bit 0 selects which side of a patch is the front: besides reversing the normals it reverses the facing
            // for culling (the Original XMB theme sets it and its waves are otherwise culled entirely; the pink theme leaves it 0).
            const bool flip = inPatch_ && (state_[0x38] & 1u) != 0;
            if (((state_[0x9B] & 1u) ? !cw : cw) != flip) return false;
        }
        const uint32_t sc1 = state_[0xD4], sc2 = state_[0xD5];
        const int sx1 = sc1 & 0x3FF, sy1 = (sc1 >> 10) & 0x3FF, sx2 = sc2 & 0x3FF, sy2 = (sc2 >> 10) & 0x3FF;
        int minx = static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))), maxx = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
        int miny = static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))), maxy = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
        minx = std::max(minx, sx1); miny = std::max(miny, sy1); maxx = std::min(maxx, sx2 + 1); maxy = std::min(maxy, sy2 + 1);
        if (minx >= maxx || miny >= maxy) return false;
        job.a = a; job.b = b; job.c = c; job.area = area; job.minx = minx; job.maxx = maxx; job.miny = miny; job.maxy = maxy;
        return true;
    }
    RasterCtx makeRasterCtx() const {
        RasterCtx r;
        r.ztest = (state_[0x23] & 1u) != 0; r.zfn = state_[0xDE] & 7u; r.zwrite = (state_[0xE7] & 1u) == 0;
        r.zwriteLive = r.zwrite && (state_[0x23] & 1u);
        r.zbase = zAddr(); r.zstride = std::max<uint32_t>(state_[0x9F] & 0xFFFFu, 1u);
        if (r.ztest || r.zwrite) hostWindow(r.zbase, r.zHost, r.zLimit);
        r.flat = (state_[0x50] & 1u) == 0u;
        r.uScale = static_cast<float>(1u << (state_[0xB8] & 0xF)); r.vScale = static_cast<float>(1u << ((state_[0xB8] >> 8) & 0xF));
        r.pc = makePlotCtx();
        return r;
    }
    // Rasterises rows [ya, yb) (already clipped to the triangle's own row range by the caller) of one triangle. Per-pixel arithmetic is the original's.
    // One covered pixel's work after coverage: z range/test/write, then the (already interpolated) attributes go to plotFast. Shared by the
    // 4-wide path and the scalar tail so both do exactly the same thing.
    inline void rasterCoveredPixel(const RasterCtx& rc, int x, int y, float z, float u, float v, int r, int g, int b, int a, const SVert& tc, uint64_t& pw, TexCache* tcache) {
        if (rc.ztest || rc.zwrite) {
            const uint32_t zoff = (static_cast<uint32_t>(y) * rc.zstride + static_cast<uint32_t>(x)) * 2u;
            const uint32_t za = rc.zbase + zoff;
            const bool zDirect = rc.zHost && static_cast<uint64_t>(zoff) + 2u <= rc.zLimit;
            if (rc.ztest) {
                uint32_t old; if (zDirect) { uint16_t tz; std::memcpy(&tz, rc.zHost + zoff, 2); old = tz; } else old = dr16(za);
                const uint32_t nz = static_cast<uint32_t>(z);
                bool pass;
                switch (rc.zfn) { case 0: pass = false; break; case 1: pass = true; break; case 2: pass = nz == old; break; case 3: pass = nz != old; break;
                                  case 4: pass = nz < old; break; case 5: pass = nz <= old; break; case 6: pass = nz > old; break; default: pass = nz >= old; break; }
                if (!pass) return;
            }
            if (rc.zwriteLive) { const uint16_t zv = static_cast<uint16_t>(z); if (zDirect) std::memcpy(rc.zHost + zoff, &zv, 2); else dw16(za, zv); }
        }
        Vertex p;
        p.u = u; p.v = v;
        if (rc.flat) { p.r = tc.r; p.g = tc.g; p.b = tc.b; p.a = tc.a; }
        else { p.r = static_cast<uint8_t>(r); p.g = static_cast<uint8_t>(g); p.b = static_cast<uint8_t>(b); p.a = static_cast<uint8_t>(a); }
        plotFast(rc.pc, x, y, p, false, pw, tcache);
    }
    // Rasterises rows [ya, yb) of one triangle. Coverage and attribute interpolation run four pixels at a time with SSE2: every lane performs the
    // very same IEEE single-precision operations in the very same order as the scalar original (no fused multiply-add is ever generated -- the
    // build does not enable FMA), so each pixel gets bit-identical values; the speed-up is just four divisions/multiplies per instruction.
    GE_NI void rasterTriRows(const RasterCtx& rc, const TriJob& t, int ya, int yb, uint64_t& pw) {
        const SVert& a = t.a; const SVert& b = t.b; const SVert& c = t.c; const float area = t.area;
        const __m128 vArea = _mm_set1_ps(area), vAx = _mm_set1_ps(a.x), vBx = _mm_set1_ps(b.x), vCx = _mm_set1_ps(c.x);
        const __m128 vAz = _mm_set1_ps(a.z), vBz = _mm_set1_ps(b.z), vCz = _mm_set1_ps(c.z);
        const __m128 vAiw = _mm_set1_ps(a.iw), vBiw = _mm_set1_ps(b.iw), vCiw = _mm_set1_ps(c.iw);
        const __m128 vAu = _mm_set1_ps(a.u), vBu = _mm_set1_ps(b.u), vCu = _mm_set1_ps(c.u), vAv = _mm_set1_ps(a.v), vBv = _mm_set1_ps(b.v), vCv = _mm_set1_ps(c.v);
        const __m128 vUs = _mm_set1_ps(rc.uScale), vVs = _mm_set1_ps(rc.vScale);
        const __m128 vAr = _mm_set1_ps(static_cast<float>(a.r)), vBr = _mm_set1_ps(static_cast<float>(b.r)), vCr = _mm_set1_ps(static_cast<float>(c.r));
        const __m128 vAg = _mm_set1_ps(static_cast<float>(a.g)), vBg = _mm_set1_ps(static_cast<float>(b.g)), vCg = _mm_set1_ps(static_cast<float>(c.g));
        const __m128 vAb = _mm_set1_ps(static_cast<float>(a.b)), vBb = _mm_set1_ps(static_cast<float>(b.b)), vCb = _mm_set1_ps(static_cast<float>(c.b));
        const __m128 vAa = _mm_set1_ps(static_cast<float>(a.a)), vBa = _mm_set1_ps(static_cast<float>(b.a)), vCa = _mm_set1_ps(static_cast<float>(c.a));
        TexCache tcache; TexCache* const tcp = (rc.pc.texOn && rc.pc.texLinear && rc.pc.texCacheOk) ? &tcache : nullptr;
        const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.f), half = _mm_set1_ps(0.5f), zmax = _mm_set1_ps(65535.f);
        for (int y = ya; y < yb; ++y) {
            const float py = static_cast<float>(y) + 0.5f;
            const float cy_py = c.y - py, by_py = b.y - py, ay_py = a.y - py; // row invariants (same values the original recomputed per pixel)
            const __m128 vCyPy = _mm_set1_ps(cy_py), vByPy = _mm_set1_ps(by_py), vAyPy = _mm_set1_ps(ay_py);
            int x = t.minx;
            for (; x + 4 <= t.maxx; x += 4) {
                const __m128 px = _mm_add_ps(_mm_cvtepi32_ps(_mm_setr_epi32(x, x + 1, x + 2, x + 3)), half);
                const __m128 bxp = _mm_sub_ps(vBx, px), cxp = _mm_sub_ps(vCx, px), axp = _mm_sub_ps(vAx, px);
                const __m128 w0 = _mm_div_ps(_mm_sub_ps(_mm_mul_ps(bxp, vCyPy), _mm_mul_ps(cxp, vByPy)), vArea);
                const __m128 w1 = _mm_div_ps(_mm_sub_ps(_mm_mul_ps(cxp, vAyPy), _mm_mul_ps(axp, vCyPy)), vArea);
                const __m128 w2 = _mm_sub_ps(_mm_sub_ps(one, w0), w1);
                // "not less than zero" matches the original's `!(w < 0)` exactly, including for NaN.
                __m128 ok = _mm_and_ps(_mm_and_ps(_mm_cmpnlt_ps(w0, zero), _mm_cmpnlt_ps(w1, zero)), _mm_cmpnlt_ps(w2, zero));
                if (!_mm_movemask_ps(ok)) continue;
                const __m128 z = _mm_add_ps(_mm_add_ps(_mm_mul_ps(vAz, w0), _mm_mul_ps(vBz, w1)), _mm_mul_ps(vCz, w2));
                ok = _mm_and_ps(ok, _mm_and_ps(_mm_cmpnlt_ps(z, zero), _mm_cmpngt_ps(z, zmax)));
                const int mask = _mm_movemask_ps(ok);
                if (!mask) continue;
                const __m128 aw0 = _mm_mul_ps(vAiw, w0), bw1 = _mm_mul_ps(vBiw, w1), cw2 = _mm_mul_ps(vCiw, w2);
                const __m128 iw = _mm_add_ps(_mm_add_ps(aw0, bw1), cw2);
                const __m128 pw0 = _mm_div_ps(aw0, iw), pw1 = _mm_div_ps(bw1, iw), pw2 = _mm_div_ps(cw2, iw);
                const __m128 uu = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(vAu, pw0), _mm_mul_ps(vBu, pw1)), _mm_mul_ps(vCu, pw2)), vUs);
                const __m128 vv = _mm_mul_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(vAv, pw0), _mm_mul_ps(vBv, pw1)), _mm_mul_ps(vCv, pw2)), vVs);
                alignas(16) float Z[4], U[4], V[4]; alignas(16) int R[4] = {0, 0, 0, 0}, G[4] = {0, 0, 0, 0}, B[4] = {0, 0, 0, 0}, A[4] = {0, 0, 0, 0};
                _mm_store_ps(Z, z); _mm_store_ps(U, uu); _mm_store_ps(V, vv);
                if (!rc.flat) {
                    // The original truncated each interpolated channel with static_cast<uint8_t>(float): cvttps gives the same truncation, and the low
                    // byte is what that cast keeps.
                    auto chan = [&](__m128 ca, __m128 cb, __m128 cc, int* out) {
                        const __m128 sum = _mm_add_ps(_mm_add_ps(_mm_mul_ps(ca, w0), _mm_mul_ps(cb, w1)), _mm_mul_ps(cc, w2));
                        _mm_store_si128(reinterpret_cast<__m128i*>(out), _mm_and_si128(_mm_cvttps_epi32(sum), _mm_set1_epi32(0xFF)));
                    };
                    chan(vAr, vBr, vCr, R); chan(vAg, vBg, vCg, G); chan(vAb, vBb, vCb, B); chan(vAa, vBa, vCa, A);
                }
                if (!(rc.ztest || rc.zwriteLive) && plot4(rc, x, y, mask, U, V, R, G, B, A, c, pw, tcp)) continue;
                for (int lane = 0; lane < 4; ++lane)
                    if (mask & (1 << lane)) rasterCoveredPixel(rc, x + lane, y, Z[lane], U[lane], V[lane], R[lane], G[lane], B[lane], A[lane], c, pw, tcp);
            }
            for (; x < t.maxx; ++x) { // the (at most three) leftover pixels of the row: the original scalar arithmetic
                const float px = static_cast<float>(x) + 0.5f;
                const float w0 = ((b.x - px) * cy_py - (c.x - px) * by_py) / area;
                const float w1 = ((c.x - px) * ay_py - (a.x - px) * cy_py) / area;
                const float w2 = 1.f - w0 - w1;
                if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                const float z = a.z * w0 + b.z * w1 + c.z * w2;
                if (z < 0.f || z > 65535.f) continue;
                const float iw = a.iw * w0 + b.iw * w1 + c.iw * w2;
                const float pw0 = a.iw * w0 / iw, pw1 = b.iw * w1 / iw, pw2 = c.iw * w2 / iw;
                const float u = (a.u * pw0 + b.u * pw1 + c.u * pw2) * rc.uScale, v = (a.v * pw0 + b.v * pw1 + c.v * pw2) * rc.vScale;
                int R = 0, G = 0, B = 0, A = 0;
                if (!rc.flat) {
                    R = static_cast<uint8_t>(a.r * w0 + b.r * w1 + c.r * w2); G = static_cast<uint8_t>(a.g * w0 + b.g * w1 + c.g * w2);
                    B = static_cast<uint8_t>(a.b * w0 + b.b * w1 + c.b * w2); A = static_cast<uint8_t>(a.a * w0 + b.a * w1 + c.a * w2);
                }
                rasterCoveredPixel(rc, x, y, z, u, v, R, G, B, A, c, pw, tcp);
            }
        }
    }
    static bool rangesOverlap(uint32_t a0, uint64_t an, uint32_t b0, uint64_t bn) {
        const uint64_t x0 = a0 & 0x1FFFFFFFu, y0 = b0 & 0x1FFFFFFFu;
        return x0 < y0 + bn && y0 < x0 + an;
    }
    void flushTris() {
        batching_ = false;
        if (triQueue_.empty()) return;
        RasterCtx rc = makeRasterCtx();
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) {
            if (gpuFlushQueue(rc)) { triQueue_.clear(); return; }
            ggpu_->syncToMemory(); // software rasterisation below reads and writes emulated memory
        }
#endif
        const uint64_t statPixelsBefore = pixelsWritten;
        const bool statOn = statRegistry().on;
        const auto statT0 = statOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        uint64_t estPixels = 0; int ymin = 1 << 30, ymax = -1;
        for (const TriJob& t : triQueue_) { estPixels += static_cast<uint64_t>(t.maxx - t.minx) * static_cast<uint64_t>(t.maxy - t.miny); ymin = std::min(ymin, t.miny); ymax = std::max(ymax, t.maxy); }
        RasterPool& pool = pool_;
        // Parallel only when it can pay for the hand-off and when no texture could alias the surfaces being written (a thread would then read
        // texels another thread is writing).
        bool parallel = pool.workers() > 0 && estPixels >= 3000u && ymax - ymin >= 8;
        int why = parallel ? 0 : 1;
        // Worker threads must never fall back to the slow memory accessors (they call into the bus), so every surface touched must lie entirely
        // inside a direct host window.
        if (parallel) {
            const uint64_t fbNeed = static_cast<uint64_t>(rc.pc.stride) * static_cast<uint64_t>(ymax) * rc.pc.bytes;
            if (!rc.pc.fbHost || fbNeed > rc.pc.fbLimit) { parallel = false; why = 2; }
            else if ((rc.ztest || rc.zwrite) && (!rc.zHost || static_cast<uint64_t>(rc.zstride) * static_cast<uint64_t>(ymax) * 2u > rc.zLimit)) { parallel = false; why = 3; }
            else if (rc.pc.texOn && (!rc.pc.texHost || static_cast<uint64_t>(rc.pc.rowBytes) * std::max<uint32_t>(rc.pc.th, 8u) + 256u > rc.pc.texLimit)) { parallel = false; why = 4; }
        }
        if (parallel && rc.pc.texOn) {
            const uint64_t fbBytes = static_cast<uint64_t>(rc.pc.stride) * static_cast<uint64_t>(ymax) * rc.pc.bytes;
            const uint64_t texBytes = static_cast<uint64_t>((rc.pc.swz && rc.pc.rowBytes < 16u) ? 16u : rc.pc.rowBytes) * std::max<uint32_t>(rc.pc.th, 8u) + 256u;
            if (rangesOverlap(rc.pc.texBaseA, texBytes, rc.pc.fbBase, fbBytes)) { parallel = false; why = 5; }
            else if ((rc.ztest || rc.zwrite) && rangesOverlap(rc.pc.texBaseA, texBytes, rc.zbase, static_cast<uint64_t>(rc.zstride) * static_cast<uint64_t>(ymax) * 2u)) { parallel = false; why = 6; }
        }
        if (rc.pc.texOn && rc.pc.texHost) { // texel footprints may be reused iff the texture is wholly in a host window and nothing this batch writes can alias it
            const uint64_t texBytes = static_cast<uint64_t>((rc.pc.swz && rc.pc.rowBytes < 16u) ? 16u : rc.pc.rowBytes) * std::max<uint32_t>(rc.pc.th, 8u) + 256u;
            const uint64_t fbBytes = static_cast<uint64_t>(rc.pc.stride) * static_cast<uint64_t>(ymax) * rc.pc.bytes;
            rc.pc.texCacheOk = texBytes <= rc.pc.texLimit && !rangesOverlap(rc.pc.texBaseA, texBytes, rc.pc.fbBase, fbBytes)
                && !((rc.ztest || rc.zwrite) && rangesOverlap(rc.pc.texBaseA, texBytes, rc.zbase, static_cast<uint64_t>(rc.zstride) * static_cast<uint64_t>(ymax) * 2u));
        }
        if (estPixels >= 1500u) prepareTexTables(rc.pc); // tiny batches would not repay building the tables; they keep the general path
        if (!parallel) {
            uint64_t pw = 0;
            for (const TriJob& t : triQueue_) rasterTriRows(rc, t, t.miny, t.maxy, pw);
            pixelsWritten += pw;
        } else {
            constexpr int kChunkRows = 8;
            const int chunks = (ymax - ymin + kChunkRows - 1) / kChunkRows;
            std::atomic<uint64_t> total{0};
            pool.run(chunks, [&](int ci) {
                const int ya = ymin + ci * kChunkRows, yb = std::min(ya + kChunkRows, ymax);
                uint64_t pw = 0;
                for (const TriJob& t : triQueue_) { const int y0 = std::max(ya, t.miny), y1 = std::min(yb, t.maxy); if (y0 < y1) rasterTriRows(rc, t, y0, y1, pw); }
                total.fetch_add(pw, std::memory_order_relaxed);
            });
            pixelsWritten += total.load();
        }
        { StatRegistry& sr = statRegistry(); if (sr.on) { StatRow& row = sr.rows[statKey(rc)]; ++row.flushes; row.tris += triQueue_.size(); row.pixels += pixelsWritten - statPixelsBefore;
            const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - statT0).count(); (parallel ? sr.parSec : sr.serSec) += dt; ++sr.whyN[why]; sr.whySec[why] += dt;
            const uint64_t wr = pixelsWritten - statPixelsBefore; sr.whyPx[why] += wr; if (parallel) { ++sr.parFlushes; sr.parPixels += wr; sr.parTris += triQueue_.size(); } else { ++sr.serFlushes; sr.serPixels += wr; sr.serTris += triQueue_.size(); }
            const int bi = estPixels < 500 ? 0 : estPixels < 2000 ? 1 : estPixels < 8000 ? 2 : estPixels < 20000 ? 3 : estPixels < 80000 ? 4 : 5; ++sr.bucket[bi]; sr.bucketPx[bi] += wr; } }
        triQueue_.clear();
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) ggpu_->memoryChanged();
#endif
    }
    void triXf(const SVert& a, const SVert& b, const SVert& c, bool noCull = false) {
        TriJob job;
        if (!triSetup(a, b, c, noCull, job)) return;
        triQueue_.push_back(job);
        if (!batching_) flushTris();
    }

    void line(const Vertex& a, const Vertex& b, bool clear) {
        int x0 = static_cast<int>(a.x), y0 = static_cast<int>(a.y), x1 = static_cast<int>(b.x), y1 = static_cast<int>(b.y);
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 4096; ++guard) {
            plot(x0, y0, b, clear);
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    // A rect of ONE constant colour (no texture, blend, alpha test or write mask -- or a clear that writes both colour and alpha) just stores
    // pack(colour) into every pixel inside the scissor, which is exactly what plotFast() computes for each pixel in those states; so write the rows
    // directly. Returns false (and draws nothing) if the generic path must be used.
    bool fillRectFast(const PlotCtx& c, int lx, int hx, int ly, int hy, const Vertex& col, bool clear) {
        if (!c.fbHost || (c.mrgb & 0xFFFFFFu) != 0 || (c.mal & 0xFFu) != 0) return false;
        if (clear) { if (!(c.cm & 0x100u) || !(c.cm & 0x200u)) return false; }
        else if (c.texOn || c.alphaTest || c.blend) return false;
        const int x0 = std::max(lx, c.sx1), x1 = std::min(hx - 1, c.sx2), y0 = std::max(ly, c.sy1), y1 = std::min(hy - 1, c.sy2);
        if (x0 > x1 || y0 > y1) return true; // nothing inside the scissor: the per-pixel path would draw nothing either
        const uint64_t lastOff = (static_cast<uint64_t>(static_cast<uint32_t>(y1)) * c.stride + static_cast<uint32_t>(x1)) * c.bytes + c.bytes;
        if (lastOff > c.fbLimit) return false;
        const uint32_t out = pack(c.ffmt, col.r, col.g, col.b, col.a);
        for (int y = y0; y <= y1; ++y) {
            uint8_t* row = c.fbHost + (static_cast<uint64_t>(static_cast<uint32_t>(y)) * c.stride + static_cast<uint32_t>(x0)) * c.bytes;
            const int n = x1 - x0 + 1;
            if (c.bytes == 4) { uint32_t* d = reinterpret_cast<uint32_t*>(row); for (int i = 0; i < n; ++i) std::memcpy(d + i, &out, 4); }
            else { const uint16_t o16 = static_cast<uint16_t>(out); for (int i = 0; i < n; ++i) std::memcpy(row + 2 * static_cast<size_t>(i), &o16, 2); }
        }
        pixelsWritten += static_cast<uint64_t>(x1 - x0 + 1) * static_cast<uint64_t>(y1 - y0 + 1);
        return true;
    }
    void rect(const Vertex& a, const Vertex& b, bool clear) {
        const int x0 = static_cast<int>(a.x), y0 = static_cast<int>(a.y), x1 = static_cast<int>(b.x), y1 = static_cast<int>(b.y);
        const int lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
        const float w = static_cast<float>(x1 - x0), h = static_cast<float>(y1 - y0);
        const PlotCtx ctx = makePlotCtx();
        if (fillRectFast(ctx, lx, hx, ly, hy, b, clear)) return;
        for (int y = ly; y < hy; ++y) for (int x = lx; x < hx; ++x) {
            Vertex p = b;
            p.u = a.u + (w != 0.f ? (b.u - a.u) * (static_cast<float>(x - x0) + 0.5f) / w : 0.f);
            p.v = a.v + (h != 0.f ? (b.v - a.v) * (static_cast<float>(y - y0) + 0.5f) / h : 0.f);
            plotFast(ctx, x, y, p, clear);
        }
    }
    void triangle(const Vertex& a, const Vertex& b, const Vertex& c, bool clear) {
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (area == 0.f) return;
        const int minx = static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))), maxx = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})));
        const int miny = static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))), maxy = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})));
        const PlotCtx ctx = makePlotCtx();
        for (int y = miny; y < maxy; ++y) for (int x = minx; x < maxx; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) / area;
            float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) / area;
            float w2 = 1.f - w0 - w1;
            if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
            Vertex p;
            p.u = a.u * w0 + b.u * w1 + c.u * w2; p.v = a.v * w0 + b.v * w1 + c.v * w2;
            const bool flat = (state_[0x50] & 1u) == 0u;
            if (flat) { p.r = c.r; p.g = c.g; p.b = c.b; p.a = c.a; }
            else {
                p.r = static_cast<uint8_t>(a.r * w0 + b.r * w1 + c.r * w2); p.g = static_cast<uint8_t>(a.g * w0 + b.g * w1 + c.g * w2);
                p.b = static_cast<uint8_t>(a.b * w0 + b.b * w1 + c.b * w2); p.a = static_cast<uint8_t>(a.a * w0 + b.a * w1 + c.a * w2);
            }
            plotFast(ctx, x, y, p, clear);
        }
    }

    // ---- per-pixel pipeline -------------------------------------------------------------------------------------
    static uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
    // ---- fast pixel pipeline (2026-10-02). The software GE spent ~72% of the emulator's wall time here (profiled): for EVERY pixel the original
    // plot() re-decoded a dozen GE registers, read the framebuffer through a windowed accessor, re-decoded the texture state inside texel(), and
    // blended through lambdas. All of that is constant for a whole primitive, so PlotCtx decodes it once and plotFast() does only per-pixel
    // work. The arithmetic is operation-for-operation the same as plotRef() below (kept as the reference), so the output is bit-identical.
    // Finds the host window containing guest address `a` (same window search and 0x1FFFFFFF masking as winPtr()); `limit` = bytes available from a.
    bool hostWindow(uint32_t a, uint8_t*& host, uint32_t& limit) const {
        const uint32_t p = a & 0x1FFFFFFFu;
        for (int i = 0; i < 2; ++i) { const uint32_t off = p - win_[i].base; if (win_[i].p && off < win_[i].size) { host = win_[i].p + off; limit = win_[i].size - off; return true; } }
        host = nullptr; limit = 0; return false;
    }
    PlotCtx makePlotCtx() const {
        PlotCtx c;
        const uint32_t sc1 = state_[0xD4], sc2 = state_[0xD5];
        c.sx1 = sc1 & 0x3FF; c.sy1 = (sc1 >> 10) & 0x3FF; c.sx2 = sc2 & 0x3FF; c.sy2 = (sc2 >> 10) & 0x3FF;
        c.ffmt = static_cast<int>(frameBufferFormat());
        c.stride = std::max<uint32_t>(frameBufferStride(), 1u);
        c.bytes = c.ffmt == 3 ? 4u : 2u;
        c.fbBase = fbAddress();
        hostWindow(c.fbBase, c.fbHost, c.fbLimit);
        if ((c.texOn = (state_[0x1E] & 1u) != 0)) {
            const uint32_t tf = state_[0xC9]; c.texFunc = tf & 7u; c.texUseTa = (tf & 0x100u) != 0; c.texDouble = (tf & 0x10000u) != 0;
            const uint32_t f = state_[0xC6]; c.texLinear = (f & 1u) || ((f >> 8) & 1u);
            c.tw = 1u << (state_[0xB8] & 0xF); c.th = 1u << ((state_[0xB8] >> 8) & 0xF);
            const uint32_t wrap = state_[0xC7]; c.clampU = (wrap & 1u) != 0; c.clampV = (wrap & 0x100u) != 0;
            const uint32_t addr = (state_[0xA0] & 0xFFFFFFu) | ((state_[0xA8] & 0x0F0000u) << 8);
            c.texStride = std::max<uint32_t>(state_[0xA8] & 0xFFFFu, 1u);
            c.texFmt = static_cast<int>(state_[0xC3] & 0xF); c.swz = (state_[0xC2] & 1u) != 0;
            switch (c.texFmt) { case 3: case 7: c.texBpp = 32; break; case 4: c.texBpp = 4; break; case 5: c.texBpp = 8; break; default: c.texBpp = 16; break; }
            c.rowBytes = c.texStride * static_cast<uint32_t>(c.texBpp) / 8u;
            c.texNull = texNullAddress(); c.texBaseA = c.texNull ? 0x0FFFFFF0u : vram(addr);   // (an address nothing maps, so the texture cache key cannot collide with the start of VRAM)
            c.clutCf = state_[0xC5]; c.clutShift = (c.clutCf >> 2) & 0x1F; c.clutMask = (c.clutCf >> 8) & 0xFF; c.clutStart = (c.clutCf >> 16) & 0x1F; c.swzBlocks = c.rowBytes >= 16u ? c.rowBytes / 16u : 1u;
            if (c.texNull) { c.texHost = nullptr; c.texLimit = 0; } else hostWindow(c.texBaseA, c.texHost, c.texLimit);
        }
        if ((c.alphaTest = (state_[0x22] & 1u) != 0)) { const uint32_t at = state_[0xDB]; c.atF = at & 7u; c.atRef = (at >> 8) & 0xFF; c.atMask = (at >> 16) & 0xFF; }
        if ((c.blend = (state_[0x21] & 1u) != 0)) { const uint32_t bm = state_[0xDF]; c.sf = bm & 15u; c.df = (bm >> 4) & 15u; c.eq = (bm >> 8) & 7u; c.fixa = state_[0xE0]; c.fixb = state_[0xE1]; }
        c.cm = state_[0xD3]; c.mrgb = state_[0xE8]; c.mal = state_[0xE9];
        c.needDst = c.blend || (c.mrgb & 0xFFFFFFu) != 0 || (c.mal & 0xFFu) != 0;
        return c;
    }
    // static_cast<int>(std::floor(f)) without the libm call (this build has no SSE4.1 roundss): truncate, then step down for negative fractions.
    // Out of range / NaN: cvttss2si yields INT_MIN, exactly as it did for the original's cast of the (equally out-of-range) floor result.
    static int ifloor(float f) {
        const int i = _mm_cvttss_si32(_mm_set_ss(f));
        return (i != INT_MIN && f < static_cast<float>(i)) ? i - 1 : i;
    }
    static uint32_t wrapCoord(int v, uint32_t n, bool clamp) {
        if (clamp) return static_cast<uint32_t>(std::max(0, std::min(v, static_cast<int>(n) - 1)));
        return static_cast<uint32_t>(v) & (n - 1u);
    }
    // Byte offset of a texel from the texture base, split into a row part and a column part (the original added them in one expression; the sum
    // is the same modular uint32 value, and the split lets bilinear sampling compute two rows and two columns instead of four texel addresses).
    uint32_t texRowPart(const PlotCtx& c, uint32_t py) const {
        if (c.swz) return ((py / 8u) * c.swzBlocks) * 128u + (py % 8u) * 16u;
        return py * c.rowBytes;
    }
    uint32_t texColPart(const PlotCtx& c, uint32_t px) const {
        const uint32_t xb = px * static_cast<uint32_t>(c.texBpp) / 8u;
        if (c.swz) return (xb / 16u) * 128u + xb % 16u;
        return xb;
    }
    // The packed RGBA8888 value (r | g<<8 | b<<16 | a<<24) of the texel at wrapped column px, at byte offset byteOff from the base.
    uint32_t texelAtOffset(const PlotCtx& c, uint32_t px, uint32_t byteOff) const {
        if (c.texNull) return 0;
        const uint32_t a = c.texBaseA + byteOff;
        auto rd8 = [&]() -> uint32_t { if (c.texHost && static_cast<uint64_t>(byteOff) + 1u <= c.texLimit) return c.texHost[byteOff]; return dr8(a); };
        auto rd16 = [&]() -> uint32_t { if (c.texHost && static_cast<uint64_t>(byteOff) + 2u <= c.texLimit) { uint16_t v; std::memcpy(&v, c.texHost + byteOff, 2); return v; } return dr16(a); };
        auto rd32 = [&]() -> uint32_t { if (c.texHost && static_cast<uint64_t>(byteOff) + 4u <= c.texLimit) { uint32_t v; std::memcpy(&v, c.texHost + byteOff, 4); return v; } return dr32(a); };
        const uint32_t shift = c.clutShift, mask = c.clutMask, start = c.clutStart;
        auto clutLookup = [&](uint32_t idx) -> uint32_t { return clut_[(((idx >> shift) & mask) + (start << 4)) & 1023u]; };
        switch (c.texFmt) {
            case 0: case 1: case 2: { uint8_t r, g, b, al; unpack(rd16(), c.texFmt, r, g, b, al); return r | (g << 8) | (b << 16) | (static_cast<uint32_t>(al) << 24); }
            case 3: return rd32(); // unpack(...,3) then repacking r|g<<8|b<<16|a<<24 is the identity
            case 4: { const uint32_t v = rd8(); return clutLookup((px & 1u) ? (v >> 4) : (v & 15u)); }
            case 5: return clutLookup(rd8());
            case 6: return clutLookup(rd16());
            case 7: return clutLookup(rd32());
            default: return 0xFFFFFFFFu; // DXT: not supported yet, sampled as opaque white
        }
    }
    // Texel read for batches that have the lookup tables (so the whole texture is in a host window): same values as texelAtOffset, without the
    // per-texel bounds checks and format lambdas for the formats the XMB uses (8888, CLUT4, CLUT8); anything else takes the general path.
    uint32_t texelUnchecked(const PlotCtx& c, uint32_t px, uint32_t byteOff) const {
        const uint8_t* p = c.texHost + byteOff;
        switch (c.texFmt) {
            case 3: { uint32_t v; std::memcpy(&v, p, 4); return v; }
            case 4: { const uint32_t v = *p; const uint32_t idx = (px & 1u) ? (v >> 4) : (v & 15u); return clut_[(((idx >> c.clutShift) & c.clutMask) + (c.clutStart << 4)) & 1023u]; }
            case 5: return clut_[((((static_cast<uint32_t>(*p)) >> c.clutShift) & c.clutMask) + (c.clutStart << 4)) & 1023u];
            default: return texelAtOffset(c, px, byteOff);
        }
    }
    // (Re)builds the column/row offset tables when the texture geometry changed since the last batch. Single-threaded: called from flushTris before the
    // workers start; the workers only read the tables.
    void prepareTexTables(PlotCtx& pc) {
        pc.colTab = pc.rowTab = nullptr;
        if (!pc.texOn || !pc.texCacheOk || pc.tw > 1024u || pc.th > 1024u) return;
        const TexTabKey k{pc.tw, pc.th, pc.rowBytes, pc.texBpp, pc.swz};
        if (!texTabValid_ || !(k == texTabKey_)) {
            texTabKey_ = k; texTabValid_ = true;
            colTab_.resize(pc.tw); rowTab_.resize(pc.th);
            for (uint32_t x = 0; x < pc.tw; ++x) colTab_[x] = texColPart(pc, x);
            for (uint32_t y = 0; y < pc.th; ++y) rowTab_[y] = texRowPart(pc, y);
        }
        pc.colTab = colTab_.data(); pc.rowTab = rowTab_.data();
    }
    struct TexTabKey { uint32_t tw, th, rowBytes; int bpp; bool swz; bool operator==(const TexTabKey& o) const { return tw == o.tw && th == o.th && rowBytes == o.rowBytes && bpp == o.bpp && swz == o.swz; } };
    TexTabKey texTabKey_{}; bool texTabValid_ = false;
    std::vector<uint32_t> colTab_, rowTab_;
    uint32_t texelFast(const PlotCtx& c, int x, int y) const {
        const uint32_t px = wrapCoord(x, c.tw, c.clampU), py = wrapCoord(y, c.th, c.clampV);
        if (c.colTab) return texelUnchecked(c, px, c.rowTab[py] + c.colTab[px]);
        return texelAtOffset(c, px, texRowPart(c, py) + texColPart(c, px));
    }
    static __m128 bytesToFloats(uint32_t c) {
        const __m128i z = _mm_setzero_si128();
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(c)), z), z));
    }
    uint32_t sampleTexFast(const PlotCtx& c, float u, float v, TexCache* tcache = nullptr) const {
        if (!c.texLinear) return texelFast(c, ifloor(u), ifloor(v));
        const float fu = u - 0.5f, fv = v - 0.5f;
        const int x0 = ifloor(fu), y0 = ifloor(fv);
        const float ax = fu - static_cast<float>(x0), ay = fv - static_cast<float>(y0);
        __m128 tf[4];
        if (tcache && tcache->x0 == x0 && tcache->y0 == y0) { tf[0] = tcache->f[0]; tf[1] = tcache->f[1]; tf[2] = tcache->f[2]; tf[3] = tcache->f[3]; }
        else {
            const uint32_t pxs[2] = {wrapCoord(x0, c.tw, c.clampU), wrapCoord(x0 + 1, c.tw, c.clampU)};
            const uint32_t pys[2] = {wrapCoord(y0, c.th, c.clampV), wrapCoord(y0 + 1, c.th, c.clampV)};
            uint32_t cc[4];
            if (c.colTab) { // lookup tables + unchecked reads (identical offsets and values, see prepareTexTables / texelUnchecked)
                const uint32_t cols[2] = {c.colTab[pxs[0]], c.colTab[pxs[1]]}, rows[2] = {c.rowTab[pys[0]], c.rowTab[pys[1]]};
                cc[0] = texelUnchecked(c, pxs[0], rows[0] + cols[0]); cc[1] = texelUnchecked(c, pxs[1], rows[0] + cols[1]);
                cc[2] = texelUnchecked(c, pxs[0], rows[1] + cols[0]); cc[3] = texelUnchecked(c, pxs[1], rows[1] + cols[1]);
            } else {
                const uint32_t cols[2] = {texColPart(c, pxs[0]), texColPart(c, pxs[1])}, rows[2] = {texRowPart(c, pys[0]), texRowPart(c, pys[1])};
                cc[0] = texelAtOffset(c, pxs[0], rows[0] + cols[0]); cc[1] = texelAtOffset(c, pxs[1], rows[0] + cols[1]);
                cc[2] = texelAtOffset(c, pxs[0], rows[1] + cols[0]); cc[3] = texelAtOffset(c, pxs[1], rows[1] + cols[1]);
            }
            for (int k = 0; k < 4; ++k) tf[k] = bytesToFloats(cc[k]);
            if (tcache) { tcache->x0 = x0; tcache->y0 = y0; for (int k = 0; k < 4; ++k) tcache->f[k] = tf[k]; }
        }
        const float w[4] = {(1.f - ax) * (1.f - ay), ax * (1.f - ay), (1.f - ax) * ay, ax * ay};
        // The four channels are the four lanes; each lane accumulates w0*c0, then +w1*c1, +w2*c2, +w3*c3 in that order, exactly like the scalar loop.
        __m128 acc = _mm_mul_ps(_mm_set1_ps(w[0]), tf[0]);
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(w[1]), tf[1]));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(w[2]), tf[2]));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(w[3]), tf[3]));
        acc = _mm_add_ps(acc, _mm_set1_ps(0.5f));
        acc = _mm_min_ps(_mm_max_ps(_mm_setzero_ps(), acc), _mm_set1_ps(255.f)); // std::min(255, std::max(0, x))
        const __m128i i32 = _mm_cvttps_epi32(acc);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(i32, i32), _mm_setzero_si128())));
    }
    GE_NI void plotFast(const PlotCtx& c, int x, int y, const Vertex& vin, bool clear, uint64_t& pixelCount, TexCache* tcache = nullptr) {
        if (x < c.sx1 || y < c.sy1 || x > c.sx2 || y > c.sy2) return;
        const uint32_t byteOff = (static_cast<uint32_t>(y) * c.stride + static_cast<uint32_t>(x)) * c.bytes;
        uint8_t* fbp = (c.fbHost && static_cast<uint64_t>(byteOff) + c.bytes <= c.fbLimit) ? c.fbHost + byteOff : nullptr;
        const uint32_t addr = c.fbBase + byteOff;
        uint8_t r = vin.r, g = vin.g, b = vin.b, a = vin.a;
        uint8_t dr = 0, dg = 0, db = 0, da = 255;
        if (c.needDst || clear) { // the destination pixel only matters for blending, write masks and clear mode (unused otherwise)
            uint32_t raw;
            if (fbp) { if (c.bytes == 4) { uint32_t t; std::memcpy(&t, fbp, 4); raw = t; } else { uint16_t t; std::memcpy(&t, fbp, 2); raw = t; } }
            else raw = c.bytes == 4 ? dr32(addr) : dr16(addr);
            unpack(raw, c.ffmt, dr, dg, db, da);
        }
        if (!clear) {
            if (c.texOn) { // texturing
                const uint32_t tc = sampleTexFast(c, vin.u, vin.v, tcache);
                const uint8_t tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = (tc >> 24) & 0xFF;
                const bool useTa = c.texUseTa;
                switch (c.texFunc) {
                    case 1: // decal
                        if (useTa) { r = clamp8((r * (255 - ta) + tr * ta) / 255); g = clamp8((g * (255 - ta) + tg * ta) / 255); b = clamp8((b * (255 - ta) + tb * ta) / 255); }
                        else { r = tr; g = tg; b = tb; }
                        break;
                    case 3: r = tr; g = tg; b = tb; if (useTa) a = ta; break; // replace
                    case 4: r = clamp8(r + tr); g = clamp8(g + tg); b = clamp8(b + tb); if (useTa) a = static_cast<uint8_t>(a * ta / 255); break; // add
                    default: r = static_cast<uint8_t>(r * tr / 255); g = static_cast<uint8_t>(g * tg / 255); b = static_cast<uint8_t>(b * tb / 255); if (useTa) a = static_cast<uint8_t>(a * ta / 255); break; // modulate
                }
                if (c.texDouble) { r = clamp8(r * 2); g = clamp8(g * 2); b = clamp8(b * 2); }
            }
            if (c.alphaTest) { // alpha test
                const uint32_t av = a & c.atMask, rv = c.atRef & c.atMask; bool pass = true;
                switch (c.atF) { case 0: pass = false; break; case 1: pass = true; break; case 2: pass = av == rv; break; case 3: pass = av != rv; break;
                                 case 4: pass = av < rv; break; case 5: pass = av <= rv; break; case 6: pass = av > rv; break; default: pass = av >= rv; break; }
                if (!pass) return;
            }
            if (c.blend) { // alpha blend
                const int sc[3] = {r, g, b}, dc[3] = {dr, dg, db}; int sfv[3], dfv[3], out[3];
                // Resolve both blend factors once for the pixel (they only depend on the channel for the colour-based and fixed-colour codes).
                auto alphaFactor = [&](uint32_t f, int& v) -> bool {
                    switch (f) { case 2: v = a; return true; case 3: v = 255 - a; return true; case 4: v = da; return true; case 5: v = 255 - da; return true;
                                 case 6: v = std::min(255, a * 2); return true; case 7: v = 255 - std::min(255, a * 2); return true;
                                 case 8: v = std::min(255, da * 2); return true; case 9: v = 255 - std::min(255, da * 2); return true; default: return false; }
                };
                int fv;
                if (c.sf == 0) { for (int ch = 0; ch < 3; ++ch) sfv[ch] = dc[ch]; }
                else if (c.sf == 1) { for (int ch = 0; ch < 3; ++ch) sfv[ch] = 255 - dc[ch]; }
                else if (alphaFactor(c.sf, fv)) { sfv[0] = sfv[1] = sfv[2] = fv; }
                else { for (int ch = 0; ch < 3; ++ch) sfv[ch] = static_cast<int>((c.fixa >> (8 * ch)) & 0xFF); }
                if (c.df == 0) { for (int ch = 0; ch < 3; ++ch) dfv[ch] = sc[ch]; }
                else if (c.df == 1) { for (int ch = 0; ch < 3; ++ch) dfv[ch] = 255 - sc[ch]; }
                else if (alphaFactor(c.df, fv)) { dfv[0] = dfv[1] = dfv[2] = fv; }
                else { for (int ch = 0; ch < 3; ++ch) dfv[ch] = static_cast<int>((c.fixb >> (8 * ch)) & 0xFF); }
                for (int ch = 0; ch < 3; ++ch) {
                    // MIN, MAX and ABSDIFF ignore the blend factors on the hardware (result = min/max/|diff| of the plain source and destination colours); the music
                    // player's wave strands blend with MAX and factors that made both sides the product of the two colours, which dimmed them to nothing (found 2026-10-14)
                    const int sv = c.eq >= 3 ? sc[ch] : sc[ch] * sfv[ch] / 255, dv = c.eq >= 3 ? dc[ch] : dc[ch] * dfv[ch] / 255;
                    switch (c.eq) { case 0: out[ch] = sv + dv; break; case 1: out[ch] = sv - dv; break; case 2: out[ch] = dv - sv; break;
                                    case 3: out[ch] = std::min(sv, dv); break; case 4: out[ch] = std::max(sv, dv); break; default: out[ch] = std::abs(sv - dv); break; }
                }
                r = clamp8(out[0]); g = clamp8(out[1]); b = clamp8(out[2]);
            }
        }
        // Colour/alpha write masks (bits set = protected). In clear mode only the flagged planes are written.
        uint8_t nr = r, ng = g, nb = b, na = a;
        if (clear) {
            if (!(c.cm & 0x100u)) { nr = dr; ng = dg; nb = db; }
            if (!(c.cm & 0x200u)) na = da;
        }
        if (c.needDst || clear) {
            nr = static_cast<uint8_t>((nr & ~(c.mrgb & 0xFF)) | (dr & (c.mrgb & 0xFF)));
            ng = static_cast<uint8_t>((ng & ~((c.mrgb >> 8) & 0xFF)) | (dg & ((c.mrgb >> 8) & 0xFF)));
            nb = static_cast<uint8_t>((nb & ~((c.mrgb >> 16) & 0xFF)) | (db & ((c.mrgb >> 16) & 0xFF)));
            na = static_cast<uint8_t>((na & ~(c.mal & 0xFF)) | (da & (c.mal & 0xFF)));
        }
        const uint32_t out = pack(c.ffmt, nr, ng, nb, na);
        if (fbp) { if (c.bytes == 4) std::memcpy(fbp, &out, 4); else { const uint16_t o16 = static_cast<uint16_t>(out); std::memcpy(fbp, &o16, 2); } }
        else if (c.bytes == 4) dw32(addr, out); else dw16(addr, static_cast<uint16_t>(out));
        ++pixelCount;
    }
    // ---- 4-wide pixel stage (2026-10-03). Four horizontally adjacent covered pixels go through texture function, alpha test, blend and the
    // colour/alpha write mask together. Integer arithmetic only, and every operation is the scalar plotFast() one: the products are <= 255*255
    // so x*y/255 is computed as (t + 1 + ((t + 1) >> 8)) >> 8 (exhaustively identical for 0..65025), signed intermediates stay in 32-bit lanes.
    // Texel fetches stay per lane (sampleTexFast, with the footprint cache). Used only when it is exactly equivalent: 8888 target in a host window,
    // no Z, all four pixels inside the scissor, and the texture cannot alias the surface being written (the four fetches happen before the four
    // stores). Returns false otherwise and the caller runs the scalar path for the lanes.
    static __m128i selI(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }
    static __m128i div255v(__m128i t) { const __m128i x = _mm_add_epi32(t, _mm_set1_epi32(1)); return _mm_srli_epi32(_mm_add_epi32(x, _mm_srli_epi32(x, 8)), 8); }
    static __m128i mul16v(__m128i a, __m128i b) { return _mm_madd_epi16(a, b); } // operands in 0..255 with a zero high half: the exact product
    static __m128i clamp8v(__m128i v) { // clamp8(int): <0 -> 0, >255 -> 255 (signed 32-bit lanes)
        v = _mm_and_si128(v, _mm_cmpgt_epi32(v, _mm_set1_epi32(-1)));
        return selI(_mm_cmpgt_epi32(v, _mm_set1_epi32(255)), _mm_set1_epi32(255), v);
    }
    GE_NI bool plot4(const RasterCtx& rc, int x, int y, int mask, const float* U, const float* V, const int* R, const int* G, const int* B, const int* A,
                     const SVert& tcv, uint64_t& pw, TexCache* tcache) {
        const PlotCtx& c = rc.pc;
        StatRegistry& sr4 = statRegistry();
        if (c.ffmt != 3 || !c.fbHost) { if (sr4.on) ++sr4.p4Why[0]; return false; }
        if (x < c.sx1 || x + 3 > c.sx2 || y < c.sy1 || y > c.sy2) { if (sr4.on) ++sr4.p4Why[1]; return false; }
        if (c.texOn && !c.texCacheOk) { if (sr4.on) ++sr4.p4Why[2]; return false; }
        if (sr4.on) { ++sr4.p4Hit; sr4.p4Lanes += static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(mask))); }
        const uint32_t byteOff = (static_cast<uint32_t>(y) * c.stride + static_cast<uint32_t>(x)) * 4u;
        if (static_cast<uint64_t>(byteOff) + 16u > c.fbLimit) return false;
        uint8_t* const fbp = c.fbHost + byteOff;
        const __m128i ff = _mm_set1_epi32(0xFF), c255 = _mm_set1_epi32(255);
        __m128i sr, sg, sb, sa;
        if (rc.flat) { sr = _mm_set1_epi32(tcv.r); sg = _mm_set1_epi32(tcv.g); sb = _mm_set1_epi32(tcv.b); sa = _mm_set1_epi32(tcv.a); }
        else { sr = _mm_load_si128(reinterpret_cast<const __m128i*>(R)); sg = _mm_load_si128(reinterpret_cast<const __m128i*>(G)); sb = _mm_load_si128(reinterpret_cast<const __m128i*>(B)); sa = _mm_load_si128(reinterpret_cast<const __m128i*>(A)); }
        if (c.texOn) {
            alignas(16) uint32_t texel[4] = {0, 0, 0, 0};
            for (int l = 0; l < 4; ++l) if (mask & (1 << l)) texel[l] = sampleTexFast(c, U[l], V[l], tcache);
            const __m128i t = _mm_load_si128(reinterpret_cast<const __m128i*>(texel));
            const __m128i tr = _mm_and_si128(t, ff), tg = _mm_and_si128(_mm_srli_epi32(t, 8), ff), tb = _mm_and_si128(_mm_srli_epi32(t, 16), ff), ta = _mm_srli_epi32(t, 24);
            const bool useTa = c.texUseTa;
            switch (c.texFunc) {
                case 1: // decal
                    if (useTa) {
                        const __m128i ita = _mm_sub_epi32(c255, ta);
                        sr = clamp8v(div255v(_mm_add_epi32(mul16v(sr, ita), mul16v(tr, ta)))); sg = clamp8v(div255v(_mm_add_epi32(mul16v(sg, ita), mul16v(tg, ta))));
                        sb = clamp8v(div255v(_mm_add_epi32(mul16v(sb, ita), mul16v(tb, ta))));
                    } else { sr = tr; sg = tg; sb = tb; }
                    break;
                case 3: sr = tr; sg = tg; sb = tb; if (useTa) sa = ta; break; // replace
                case 4: sr = clamp8v(_mm_add_epi32(sr, tr)); sg = clamp8v(_mm_add_epi32(sg, tg)); sb = clamp8v(_mm_add_epi32(sb, tb)); if (useTa) sa = div255v(mul16v(sa, ta)); break; // add
                default: sr = div255v(mul16v(sr, tr)); sg = div255v(mul16v(sg, tg)); sb = div255v(mul16v(sb, tb)); if (useTa) sa = div255v(mul16v(sa, ta)); break; // modulate
            }
            if (c.texDouble) { sr = clamp8v(_mm_add_epi32(sr, sr)); sg = clamp8v(_mm_add_epi32(sg, sg)); sb = clamp8v(_mm_add_epi32(sb, sb)); }
        }
        int laneMask = mask;
        if (c.alphaTest) {
            const __m128i av = _mm_and_si128(sa, _mm_set1_epi32(static_cast<int>(c.atMask))), rv = _mm_set1_epi32(static_cast<int>(c.atRef & c.atMask));
            const __m128i allOnes = _mm_set1_epi32(-1);
            __m128i pass;
            switch (c.atF) {
                case 0: pass = _mm_setzero_si128(); break;
                case 1: pass = allOnes; break;
                case 2: pass = _mm_cmpeq_epi32(av, rv); break;
                case 3: pass = _mm_xor_si128(_mm_cmpeq_epi32(av, rv), allOnes); break;
                case 4: pass = _mm_cmplt_epi32(av, rv); break;
                case 5: pass = _mm_xor_si128(_mm_cmpgt_epi32(av, rv), allOnes); break;
                case 6: pass = _mm_cmpgt_epi32(av, rv); break;
                default: pass = _mm_xor_si128(_mm_cmplt_epi32(av, rv), allOnes); break;
            }
            laneMask &= _mm_movemask_ps(_mm_castsi128_ps(pass));
            if (!laneMask) return true; // every lane failed the alpha test: nothing is written, nothing counted (as in the scalar path)
        }
        const __m128i dst = _mm_loadu_si128(reinterpret_cast<const __m128i*>(fbp));
        __m128i dr = _mm_setzero_si128(), dg = dr, db = dr, da = c255;
        if (c.needDst) { dr = _mm_and_si128(dst, ff); dg = _mm_and_si128(_mm_srli_epi32(dst, 8), ff); db = _mm_and_si128(_mm_srli_epi32(dst, 16), ff); da = _mm_srli_epi32(dst, 24); }
        if (c.blend) {
            auto dbl255 = [&](__m128i a) { const __m128i d = _mm_add_epi32(a, a); return selI(_mm_cmpgt_epi32(d, c255), c255, d); }; // std::min(255, a * 2)
            auto alphaFactor = [&](uint32_t f, __m128i& v) -> bool {
                switch (f) {
                    case 2: v = sa; return true;
                    case 3: v = _mm_sub_epi32(c255, sa); return true;
                    case 4: v = da; return true;
                    case 5: v = _mm_sub_epi32(c255, da); return true;
                    case 6: v = dbl255(sa); return true;
                    case 7: v = _mm_sub_epi32(c255, dbl255(sa)); return true;
                    case 8: v = dbl255(da); return true;
                    case 9: v = _mm_sub_epi32(c255, dbl255(da)); return true;
                    default: return false;
                }
            };
            __m128i sfr, sfg, sfb, dfr, dfg, dfb, fv = _mm_setzero_si128();
            if (c.sf == 0) { sfr = dr; sfg = dg; sfb = db; }
            else if (c.sf == 1) { sfr = _mm_sub_epi32(c255, dr); sfg = _mm_sub_epi32(c255, dg); sfb = _mm_sub_epi32(c255, db); }
            else if (alphaFactor(c.sf, fv)) { sfr = sfg = sfb = fv; }
            else { sfr = _mm_set1_epi32(static_cast<int>(c.fixa & 0xFF)); sfg = _mm_set1_epi32(static_cast<int>((c.fixa >> 8) & 0xFF)); sfb = _mm_set1_epi32(static_cast<int>((c.fixa >> 16) & 0xFF)); }
            if (c.df == 0) { dfr = sr; dfg = sg; dfb = sb; }
            else if (c.df == 1) { dfr = _mm_sub_epi32(c255, sr); dfg = _mm_sub_epi32(c255, sg); dfb = _mm_sub_epi32(c255, sb); }
            else if (alphaFactor(c.df, fv)) { dfr = dfg = dfb = fv; }
            else { dfr = _mm_set1_epi32(static_cast<int>(c.fixb & 0xFF)); dfg = _mm_set1_epi32(static_cast<int>((c.fixb >> 8) & 0xFF)); dfb = _mm_set1_epi32(static_cast<int>((c.fixb >> 16) & 0xFF)); }
            auto eqv = [&](__m128i sv, __m128i dv) -> __m128i {
                switch (c.eq) {
                    case 0: return _mm_add_epi32(sv, dv);
                    case 1: return _mm_sub_epi32(sv, dv);
                    case 2: return _mm_sub_epi32(dv, sv);
                    case 3: return selI(_mm_cmpgt_epi32(sv, dv), dv, sv);
                    case 4: return selI(_mm_cmpgt_epi32(sv, dv), sv, dv);
                    default: { const __m128i d = _mm_sub_epi32(sv, dv); return selI(_mm_cmpgt_epi32(d, _mm_set1_epi32(-1)), d, _mm_sub_epi32(_mm_setzero_si128(), d)); }
                }
            };
            if (c.eq >= 3) { sr = clamp8v(eqv(sr, dr)); sg = clamp8v(eqv(sg, dg)); sb = clamp8v(eqv(sb, db)); } // MIN / MAX / ABSDIFF: the factors are ignored
            else {
                sr = clamp8v(eqv(div255v(mul16v(sr, sfr)), div255v(mul16v(dr, dfr))));
                sg = clamp8v(eqv(div255v(mul16v(sg, sfg)), div255v(mul16v(dg, dfg))));
                sb = clamp8v(eqv(div255v(mul16v(sb, sfb)), div255v(mul16v(db, dfb))));
            }
        }
        if (c.needDst) { // write masks: protected bits keep the destination
            const __m128i mr = _mm_set1_epi32(static_cast<int>(c.mrgb & 0xFF)), mg = _mm_set1_epi32(static_cast<int>((c.mrgb >> 8) & 0xFF)), mb = _mm_set1_epi32(static_cast<int>((c.mrgb >> 16) & 0xFF)), ma = _mm_set1_epi32(static_cast<int>(c.mal & 0xFF));
            sr = _mm_or_si128(_mm_andnot_si128(mr, sr), _mm_and_si128(dr, mr)); sg = _mm_or_si128(_mm_andnot_si128(mg, sg), _mm_and_si128(dg, mg));
            sb = _mm_or_si128(_mm_andnot_si128(mb, sb), _mm_and_si128(db, mb)); sa = _mm_or_si128(_mm_andnot_si128(ma, sa), _mm_and_si128(da, ma));
        }
        const __m128i out = _mm_or_si128(_mm_or_si128(_mm_and_si128(sr, ff), _mm_slli_epi32(_mm_and_si128(sg, ff), 8)), _mm_or_si128(_mm_slli_epi32(_mm_and_si128(sb, ff), 16), _mm_slli_epi32(_mm_and_si128(sa, ff), 24)));
        if (laneMask == 0xF) _mm_storeu_si128(reinterpret_cast<__m128i*>(fbp), out);
        else {
            const __m128i bit = _mm_setr_epi32(1, 2, 4, 8);
            const __m128i lm = _mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32(laneMask), bit), bit);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(fbp), selI(lm, out, dst));
        }
        static const uint8_t kPop[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
        pw += kPop[laneMask & 15];
        return true;
    }
    void plotFast(const PlotCtx& c, int x, int y, const Vertex& vin, bool clear) { plotFast(c, x, y, vin, clear, pixelsWritten); }
    void plot(int x, int y, const Vertex& vin, bool clear) { const PlotCtx c = makePlotCtx(); plotFast(c, x, y, vin, clear, pixelsWritten); }

    // The original per-pixel pipeline, kept as the reference the fast path above must match bit for bit (the harness test and the frame
    // comparison against a baseline build are what verify that).
    void plotRef(int x, int y, const Vertex& vin, bool clear) {
        const uint32_t sc1 = state_[0xD4], sc2 = state_[0xD5];
        const int sx1 = sc1 & 0x3FF, sy1 = (sc1 >> 10) & 0x3FF, sx2 = sc2 & 0x3FF, sy2 = (sc2 >> 10) & 0x3FF;
        if (x < sx1 || y < sy1 || x > sx2 || y > sy2) return;
        const int ffmt = static_cast<int>(frameBufferFormat());
        const uint32_t stride = std::max<uint32_t>(frameBufferStride(), 1u);
        const uint32_t bytes = ffmt == 3 ? 4u : 2u;
        const uint32_t addr = fbAddress() + (static_cast<uint32_t>(y) * stride + static_cast<uint32_t>(x)) * bytes;
        uint8_t r = vin.r, g = vin.g, b = vin.b, a = vin.a;
        uint8_t dr = 0, dg = 0, db = 0, da = 255;
        {
            const uint32_t raw = bytes == 4 ? dr32(addr) : dr16(addr);
            unpack(raw, ffmt, dr, dg, db, da);
        }
        if (!clear) {
            if (state_[0x1E] & 1u) { // texturing
                const uint32_t tsz = state_[0xB8]; (void)tsz;
                const uint32_t tc = sampleTex(vin.u, vin.v);
                const uint8_t tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = (tc >> 24) & 0xFF;
                const uint32_t tf = state_[0xC9]; const uint32_t func = tf & 7u; const bool useTa = (tf & 0x100u) != 0;
                switch (func) {
                    case 1: // decal
                        if (useTa) { r = clamp8((r * (255 - ta) + tr * ta) / 255); g = clamp8((g * (255 - ta) + tg * ta) / 255); b = clamp8((b * (255 - ta) + tb * ta) / 255); }
                        else { r = tr; g = tg; b = tb; }
                        break;
                    case 3: r = tr; g = tg; b = tb; if (useTa) a = ta; break; // replace
                    case 4: r = clamp8(r + tr); g = clamp8(g + tg); b = clamp8(b + tb); if (useTa) a = static_cast<uint8_t>(a * ta / 255); break; // add
                    default: r = static_cast<uint8_t>(r * tr / 255); g = static_cast<uint8_t>(g * tg / 255); b = static_cast<uint8_t>(b * tb / 255); if (useTa) a = static_cast<uint8_t>(a * ta / 255); break; // modulate/blend
                }
                if (tf & 0x10000u) { r = clamp8(r * 2); g = clamp8(g * 2); b = clamp8(b * 2); }
            }
            if (state_[0x22] & 1u) { // alpha test
                const uint32_t at = state_[0xDB]; const uint32_t f = at & 7u, ref = (at >> 8) & 0xFF, mask = (at >> 16) & 0xFF;
                const uint32_t av = a & mask, rv = ref & mask; bool pass = true;
                switch (f) { case 0: pass = false; break; case 1: pass = true; break; case 2: pass = av == rv; break; case 3: pass = av != rv; break;
                             case 4: pass = av < rv; break; case 5: pass = av <= rv; break; case 6: pass = av > rv; break; default: pass = av >= rv; break; }
                if (!pass) return;
            }
            if (state_[0x21] & 1u) { // alpha blend
                const uint32_t bm = state_[0xDF]; const uint32_t sf = bm & 15u, df = (bm >> 4) & 15u, eq = (bm >> 8) & 7u;
                auto factor = [&](uint32_t f, bool isDst, int ch) -> int { // returns 0..255 for channel ch (0 r,1 g,2 b)
                    const int s[3] = {r, g, b}, d[3] = {dr, dg, db};
                    const uint32_t fixa = state_[0xE0], fixb = state_[0xE1];
                    const int fix[3] = {static_cast<int>((isDst ? fixb : fixa) & 0xFF), static_cast<int>(((isDst ? fixb : fixa) >> 8) & 0xFF), static_cast<int>(((isDst ? fixb : fixa) >> 16) & 0xFF)};
                    if (!isDst) { switch (f) { case 0: return d[ch]; case 1: return 255 - d[ch]; default: break; } }
                    else { switch (f) { case 0: return s[ch]; case 1: return 255 - s[ch]; default: break; } }
                    switch (f) { case 2: return a; case 3: return 255 - a; case 4: return da; case 5: return 255 - da;
                                 case 6: return std::min(255, a * 2); case 7: return 255 - std::min(255, a * 2);
                                 case 8: return std::min(255, da * 2); case 9: return 255 - std::min(255, da * 2); default: return fix[ch]; }
                };
                const int s[3] = {r, g, b}, d[3] = {dr, dg, db}; int out[3];
                for (int ch = 0; ch < 3; ++ch) {
                    const int sv = eq >= 3 ? s[ch] : s[ch] * factor(sf, false, ch) / 255, dv = eq >= 3 ? d[ch] : d[ch] * factor(df, true, ch) / 255; // MIN/MAX/ABSDIFF ignore the factors
                    switch (eq) { case 0: out[ch] = sv + dv; break; case 1: out[ch] = sv - dv; break; case 2: out[ch] = dv - sv; break;
                                  case 3: out[ch] = std::min(sv, dv); break; case 4: out[ch] = std::max(sv, dv); break; default: out[ch] = std::abs(sv - dv); break; }
                }
                r = clamp8(out[0]); g = clamp8(out[1]); b = clamp8(out[2]);
            }
        }
        // Colour/alpha write masks (bits set = protected). In clear mode only the flagged planes are written.
        uint8_t nr = r, ng = g, nb = b, na = a;
        if (clear) {
            const uint32_t cm = state_[0xD3];
            if (!(cm & 0x100u)) { nr = dr; ng = dg; nb = db; }
            if (!(cm & 0x200u)) na = da;
        }
        const uint32_t mrgb = state_[0xE8], mal = state_[0xE9];
        nr = static_cast<uint8_t>((nr & ~(mrgb & 0xFF)) | (dr & (mrgb & 0xFF)));
        ng = static_cast<uint8_t>((ng & ~((mrgb >> 8) & 0xFF)) | (dg & ((mrgb >> 8) & 0xFF)));
        nb = static_cast<uint8_t>((nb & ~((mrgb >> 16) & 0xFF)) | (db & ((mrgb >> 16) & 0xFF)));
        na = static_cast<uint8_t>((na & ~(mal & 0xFF)) | (da & (mal & 0xFF)));
        const uint32_t out = pack(ffmt, nr, ng, nb, na);
        if (bytes == 4) dw32(addr, out); else dw16(addr, static_cast<uint16_t>(out));
        ++pixelsWritten;
    }

    // ---- block transfer (sceGuCopyImage) ------------------------------------------------------------------------
    void transfer() {
        const uint32_t src = vram((state_[0xB2] & 0xFFFFFFu) | ((state_[0xB3] & 0xFF0000u) << 8));
        const uint32_t dst = vram((state_[0xB4] & 0xFFFFFFu) | ((state_[0xB5] & 0xFF0000u) << 8));
        const uint32_t sw = state_[0xB3] & 0xFFFFu, dw = state_[0xB5] & 0xFFFFu;
        const uint32_t sx = state_[0xEB] & 0x3FFu, sy = (state_[0xEB] >> 10) & 0x3FFu, dx = state_[0xEC] & 0x3FFu, dy = (state_[0xEC] >> 10) & 0x3FFu;
        const uint32_t w = (state_[0xEE] & 0x3FFu) + 1u, h = ((state_[0xEE] >> 10) & 0x3FFu) + 1u;
        const uint32_t bpp = (state_[0xEA] & 1u) ? 4u : 2u;
        ++transfers;
#ifdef BETA_GE_GPU_ACCEL
        if (ggpu_) ggpu_->syncToMemory();
        struct Changed { GeGpu* g; ~Changed() { if (g) g->memoryChanged(); } } changed{ggpu_};
#endif
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            const uint32_t sa = src + ((sy + y) * sw + sx + x) * bpp, da = dst + ((dy + y) * dw + dx + x) * bpp;
            if (bpp == 4) dw32(da, dr32(sa)); else dw16(da, dr16(sa));
        }
    }
};

} // namespace beta
