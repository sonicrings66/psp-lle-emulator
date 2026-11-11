#pragma once
// Live display window + keyboard input (Win32, no third-party dependencies).
// Ported verbatim (namespace only) from psp-lle-emu/src/core/frontend/
// live_window.h (see docs/PORTING_LOG.md).
//
// The emulator thread calls submit() with each scanned-out frame (480x272 RGBA); a separate UI thread owns the window,
// blits the latest frame (nearest-neighbour, scaled to fit, aspect kept) and turns the keyboard into a SceCtrl button mask
// that the emulator thread reads with buttons(). Default mapping follows PPSSPP:
//   arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle, Q = L, W = R, Enter = Start, Space = Select.
// F3 shows/hides a two-line text overlay (top-left) set with setOverlay(); it is drawn into the window's copy of the frame only.
// Non-Windows builds get a no-op stub so the core still compiles.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace beta {

class LiveWindow {
public:
    static constexpr int kWidth = 480, kHeight = 272;

    ~LiveWindow() { stop(); }

    bool start(const char* title = "PSP LLE") {
#ifdef _WIN32
        if (thread_.joinable()) return true;
        title_ = title;
        frame_.assign(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
        pending_.assign(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
        ready_ = false;
        thread_ = std::thread([this] { run(); });
        while (!ready_ && !failed_) std::this_thread::yield();
        return !failed_;
#else
        (void)title; return false;
#endif
    }

    void stop() {
#ifdef _WIN32
        if (!thread_.joinable()) return;
        if (hwnd_) PostMessage(hwnd_, WM_CLOSE, 0, 0);
        thread_.join();
#endif
    }

    bool open() const { return ready_ && !closed_; }
#ifdef _WIN32
    HWND hwnd() const { return hwnd_; }
#endif
    // GPU present: a DXGI swap chain draws the client area (GeGpu::initPresent), so the window stops painting it with GDI.
    void setGpuPresent(bool on) { gpuPresent_ = on; }
    bool overlayEnabled() const { return overlayOn_; }
    bool takeOverlayDirty() { return overlayDirty_.exchange(false); } // F3 was pressed: the overlay must be redrawn / hidden
    // The overlay text as a transparent RGBA image (w x h, straight alpha: black outline, white glyphs) for the GPU presenter; empty (all
    // transparent) when there is no text.
    void renderOverlayRgba(std::vector<uint8_t>& rgba, int w, int h) {
#ifdef _WIN32
        rgba.assign(static_cast<size_t>(w) * h * 4u, 0u);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!(ovl1_[0] || ovl2_[0])) return;
        drawTextRgba(rgba, w, h, ovl1_, 6, 6); drawTextRgba(rgba, w, h, ovl2_, 6, 6 + 7 * kScale + 4); if (ovl3_[0]) drawTextRgba(rgba, w, h, ovl3_, 6, 6 + 2 * (7 * kScale + 4));
#else
        (void)rgba; (void)w; (void)h;
#endif
    }

    // rgba: kWidth*kHeight*4 bytes, R,G,B,A order.
    void submit(const uint8_t* rgba) {
#ifdef _WIN32
        {   // Only a copy on the caller's (emulation) thread; the BGRX conversion and the overlay text are done by the window thread when it paints.
            std::lock_guard<std::mutex> lock(mutex_);
            std::memcpy(pending_.data(), rgba, pending_.size());
            pendingNew_ = true;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
#else
        (void)rgba;
#endif
    }

    uint32_t buttons() const { return buttons_.load(std::memory_order_relaxed); }

    // Two short lines of text for the top-left overlay (characters: 0-9 A-Z a few symbols; anything else is blank).
    void setOverlay(const char* line1, const char* line2, const char* line3 = "") {
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mutex_);
        std::snprintf(ovl1_, sizeof(ovl1_), "%s", line1 ? line1 : ""); std::snprintf(ovl2_, sizeof(ovl2_), "%s", line2 ? line2 : ""); std::snprintf(ovl3_, sizeof(ovl3_), "%s", line3 ? line3 : "");
#else
        (void)line1; (void)line2; (void)line3;
#endif
    }
    void setOverlayEnabled(bool on) { overlayOn_ = on; }
    // F4: true once per key press (the harness uses it to start/stop a GE capture).
    bool takeCaptureToggle() { return captureToggle_.exchange(false); }
    // F5: true once per key press (the harness cycles the frame-rate mode 60 -> 30 -> uncapped).
    bool takeSpeedToggle() { return speedToggle_.exchange(false); }
    // F6: true once per key press (the harness zeroes the NAND watch counters, so a settings change can be watched from a clean baseline).
    bool takeMark() { return markToggle_.exchange(false); }
    // F7: true once per key press (the harness writes the 64 MB guest RAM to scratch/hang_ram.bin, for working out what a hung guest is waiting for).
    bool takeRamDump() { return ramDumpToggle_.exchange(false); }
    // F8: the console's WLAN slider (true once per key press).
    bool takeWlanToggle() { return wlanToggle_.exchange(false); }

private:
#ifdef _WIN32
    static uint32_t keyBit(WPARAM vk) {
        switch (vk) {
            case VK_SPACE: return 0x0001u;   // SELECT
            case VK_RETURN: return 0x0008u;  // START
            case VK_UP: return 0x0010u;
            case VK_RIGHT: return 0x0020u;
            case VK_DOWN: return 0x0040u;
            case VK_LEFT: return 0x0080u;
            case 'Q': return 0x0100u;        // L
            case 'W': return 0x0200u;        // R
            case 'S': return 0x1000u;        // TRIANGLE
            case 'X': return 0x2000u;        // CIRCLE
            case 'Z': return 0x4000u;        // CROSS
            case 'A': return 0x8000u;        // SQUARE
            default: return 0u;
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        LiveWindow* self = reinterpret_cast<LiveWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                if (self && wp == VK_F5) { if (!(lp & 0x40000000)) self->speedToggle_ = true; return 0; } // F5: frame-rate mode (ignore auto-repeat)
                if (self && wp == VK_F8) { if (!(lp & 0x40000000)) self->wlanToggle_ = true; return 0; } // F8: flip the WLAN slider (ignore auto-repeat)
                if (self && wp == VK_F7) { if (!(lp & 0x40000000)) self->ramDumpToggle_ = true; return 0; } // F7: guest RAM dump (ignore auto-repeat)
                if (self && wp == VK_F6) { if (!(lp & 0x40000000)) self->markToggle_ = true; return 0; } // F6: NAND watch baseline (ignore auto-repeat)
                if (self && wp == VK_F4) { if (!(lp & 0x40000000)) self->captureToggle_ = true; return 0; } // F4: GE capture start/stop (ignore auto-repeat)
                if (self && wp == VK_F3) { if (!(lp & 0x40000000)) { self->overlayOn_ = !self->overlayOn_; self->overlayDirty_ = true; InvalidateRect(hwnd, nullptr, FALSE); } return 0; } // F3: overlay on/off (ignore auto-repeat)
                if (self) self->buttons_ |= keyBit(wp);
                return 0;
            case WM_KEYUP: case WM_SYSKEYUP: if (self) self->buttons_ &= ~keyBit(wp); return 0;
            case WM_KILLFOCUS: if (self) self->buttons_ = 0; return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: if (self) self->paint(hwnd); else { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); } return 0;
            case WM_CLOSE: DestroyWindow(hwnd); return 0;
            case WM_DESTROY: if (self) self->closed_ = true; PostQuitMessage(0); return 0;
            default: return DefWindowProc(hwnd, msg, wp, lp);
        }
    }

    // 5x7 bitmap font for the overlay (digits, the letters the counters use, a few symbols); drawn at kScale with a black outline.
    static constexpr int kScale = 2;
    static const uint8_t* glyph(char c) {
        static const uint8_t g[][7] = {
            {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2},   // 0-4
            {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}, // 5-9
            {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},           // A B C D
            {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {15,16,16,23,17,17,15}, {17,17,17,31,17,17,17},          // E F G H
            {14,4,4,4,4,4,14}, {1,1,1,1,1,17,14}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},                    // I J K L
            {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},          // M N O P
            {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},                  // Q R S T
            {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},            // U V W X
            {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},                                                                  // Y Z
            {25,26,2,4,8,11,19}, {0,0,0,0,0,0,4}, {0,0,0,0,0,0,0}, {1,2,2,4,8,8,16}, {0,4,0,0,0,4,0}};               // % . space / :
        static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
        if (c >= '0' && c <= '9') return g[c - '0'];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return g[10 + (c - 'A')];
        switch (c) { case '%': return g[36]; case '.': return g[37]; case ' ': return g[38]; case '/': return g[39]; case ':': return g[40]; default: return blank; }
    }
    void plotPx(int x, int y, uint8_t v) { // frame_ is BGRX; v = 255 white, 0 black
        if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
        uint8_t* p = &frame_[(static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)) * 4u]; p[0] = p[1] = p[2] = v;
    }
    void drawTextRgba(std::vector<uint8_t>& rgba, int W, int H, const char* text, int x0, int y0) { // same glyphs as drawText, into a transparent RGBA image
        auto plot = [&](int x, int y, uint8_t v) { if (x < 0 || y < 0 || x >= W || y >= H) return; uint8_t* p = &rgba[(static_cast<size_t>(y) * W + x) * 4u]; p[0] = p[1] = p[2] = v; p[3] = 255; };
        for (int pass = 0; pass < 2; ++pass) {
            int x = x0;
            for (const char* t = text; *t; ++t, x += 6 * kScale) {
                const uint8_t* g = glyph(*t);
                for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col) {
                    if (!((g[row] >> (4 - col)) & 1)) continue;
                    const int px = x + col * kScale, py = y0 + row * kScale;
                    for (int dy = 0; dy < kScale; ++dy) for (int dx = 0; dx < kScale; ++dx) {
                        if (pass == 0) { for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) plot(px + dx + ox, py + dy + oy, 0); }
                        else plot(px + dx, py + dy, 255);
                    }
                }
            }
        }
    }
    void drawText(const char* text, int x0, int y0) { // caller holds mutex_
        for (int pass = 0; pass < 2; ++pass) { // pass 0: black outline offsets, pass 1: white glyph
            int x = x0;
            for (const char* t = text; *t; ++t, x += 6 * kScale) {
                const uint8_t* g = glyph(*t);
                for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col) {
                    if (!((g[row] >> (4 - col)) & 1)) continue;
                    const int px = x + col * kScale, py = y0 + row * kScale;
                    for (int dy = 0; dy < kScale; ++dy) for (int dx = 0; dx < kScale; ++dx) {
                        if (pass == 0) { for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) plotPx(px + dx + ox, py + dy + oy, 0); }
                        else plotPx(px + dx, py + dy, 255);
                    }
                }
            }
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        if (gpuPresent_) { EndPaint(hwnd, &ps); return; } // the swap chain owns the client area
        RECT rc; GetClientRect(hwnd, &rc);
        const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        int w = cw, h = cw * kHeight / kWidth;
        if (h > ch) { h = ch; w = ch * kWidth / kHeight; }
        const int x = (cw - w) / 2, y = (ch - h) / 2;
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = kWidth; bi.bmiHeader.biHeight = -kHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        { std::lock_guard<std::mutex> lock(mutex_);
          if (pendingNew_ || overlayDirty_) { // newest submitted frame -> BGRX for GDI, then the overlay text on top
              pendingNew_ = false; overlayDirty_ = false;
              for (size_t i = 0; i < static_cast<size_t>(kWidth) * kHeight; ++i) {
                  frame_[i * 4 + 0] = pending_[i * 4 + 2]; frame_[i * 4 + 1] = pending_[i * 4 + 1]; frame_[i * 4 + 2] = pending_[i * 4 + 0]; frame_[i * 4 + 3] = 0;
              }
              if (overlayOn_ && (ovl1_[0] || ovl2_[0])) { drawText(ovl1_, 6, 6); drawText(ovl2_, 6, 6 + 7 * kScale + 4); if (ovl3_[0]) drawText(ovl3_, 6, 6 + 2 * (7 * kScale + 4)); }
          }
          SetStretchBltMode(dc, COLORONCOLOR);
          StretchDIBits(dc, x, y, w, h, 0, 0, kWidth, kHeight, frame_.data(), &bi, DIB_RGB_COLORS, SRCCOPY); }
        PatBlt(dc, 0, 0, x, ch, BLACKNESS); PatBlt(dc, x + w, 0, cw - x - w, ch, BLACKNESS);
        PatBlt(dc, 0, 0, cw, y, BLACKNESS); PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
        EndPaint(hwnd, &ps);
    }

    void run() {
        WNDCLASSA wc = {}; wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandle(nullptr); wc.lpszClassName = "PspLleLiveWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        RECT r = {0, 0, kWidth * 2, kHeight * 2}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowA(wc.lpszClassName, title_.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) { failed_ = true; return; }
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ready_ = true;
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
        closed_ = true; hwnd_ = nullptr;
    }

    HWND hwnd_ = nullptr;
    std::string title_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<uint8_t> frame_, pending_; bool pendingNew_ = false; // pending_: the newest submitted RGBA frame (under mutex_); frame_: BGRX + overlay as painted
    char ovl1_[32] = {0}, ovl2_[32] = {0}, ovl3_[32] = {0};
#endif
    std::atomic<bool> overlayOn_{true}, captureToggle_{false}, speedToggle_{false}, markToggle_{false}, ramDumpToggle_{false}, wlanToggle_{false}, overlayDirty_{false}, gpuPresent_{false};
    std::atomic<bool> ready_{false}, failed_{false}, closed_{false};
    std::atomic<uint32_t> buttons_{0};
};

} // namespace beta
