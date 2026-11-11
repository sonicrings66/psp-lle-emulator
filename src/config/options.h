#pragma once

#include <cstdint>
#include <string>

// EmuOptions and friends: the ONLY configuration surface in this project.
//
// Rule (see the plan's "Guiding principles" #2): no file outside
// src/config/options_cli.cpp calls std::getenv or reads argv. A subsystem
// that wants a toggle adds a field to its own sub-struct here -- visibly,
// in one place -- instead of an inline std::getenv() check somewhere deep
// in its own source. This is the direct structural fix for the old
// project's PSP_WINDOW-gated-reboot incident: a conditional code path
// cannot exist without a field here that shows up in a code review.
//
// Each subsystem constructor takes only ITS OWN sub-struct by const&,
// never the whole EmuOptions -- grepping for "EmuOptions" outside the
// driver/CLI layer should always come back empty. That's what keeps this
// from becoming a new god-object in place of the old getenv sprawl.

namespace beta::config {

struct CpuOptions {
    bool ringTrace = false;       // was PSP_CPU_RING
    bool logHalts = false;        // was PSP_HALT_LOG
    bool logBusErrors = false;    // was PSP_LOG_BUSERR
    bool traceSyscall21 = false;  // was PSP_TRACE_SYSCALL21 -- diagnostic trace of unlinked-import ("SYSCALL_NOT_LINKED") call sites
};

struct RebootOptions {
    // No "strategy" field on purpose: beta has exactly one reboot
    // strategy (in-process soft reset, see RebootController). There is
    // nothing here to gate a process-restart path with, because that
    // path does not exist in this codebase.
};

struct GeOptions {
    bool gpuAccel = false;        // was PSP_GPU_GE -- unset through Phase D
    bool stats = false;           // was PSP_GE_STATS
    bool recentPrims = false;     // was PSP_GE_RECENT -- keep a rolling log of recent primitives (tooling/replay)
    size_t recentMax = 40;        // was PSP_GE_RECENT_MAX
    bool normalMatInverseTranspose = true; // was PSP_GE_NORMALMAT (0 = plain matrix; else/unset = inverse transpose)
    bool analyticNormals = true;  // was PSP_GE_ANALYTICN (0 = old forward-difference spline normals)
    std::string texDumpDir;       // was PSP_TEXDUMP=<dir> -- dumps every distinct bound texture there
    bool texDumpRaw = false;      // was PSP_TEXDUMP_RAW
    bool patchDebug = false;      // was PSP_GE_PATCHDBG
    int64_t xfBoxFrom = -1;       // was PSP_GE_XFBOX -- transform-prim index to start dumping screen-space bounding boxes from (-1 = off)
    int threads = -1;             // was PSP_GE_THREADS -- raster worker-pool size; -1 = auto (hardware_concurrency()-based default)
};

struct MeOptions {
    uint32_t ratio = 1;    // was PSP_ME_RATIO -- ME steps per main-core step (1-64)
    bool horizonAlways = false; // was PSP_ME_HORIZON_ALWAYS -- A/B switch: a running ME bounds every burst even when parked
    bool log = false;      // was PSP_ME_LOG
    bool shadow = false;   // was PSP_ME_SHADOW -- experiment only, see me_core.h
    bool realBoot = false; // was PSP_ME_REALBOOT -- default false means host-side first-stage boot (see MeCore::bootFirstStageOnHost)
};

struct MsOptions {
    bool present = true;          // was (default-on) MS-present hardware bit
    std::string folderPath;       // was PSP_MS_FOLDER
    std::string imagePath;        // was PSP_MS_IMAGE
};

struct AudioOptions {
    bool enabled = true;          // was !PSP_NO_AUDIO
    double dmaMs = 2.0;           // was PSP_AUDIO_DMA_MS -- real DMA-FIFO pacing threshold, not diagnostic
    std::string wavDumpDir;       // was PSP_AUDIO_OUT -- optional real feature (dumps each line to .wav), not diagnostic noise
};

struct UmdOptions {
    double readyMs = 20.0;        // was PSP_UMD_READY_MS -- real simulated disc-tray-ready delay
};

struct WlanOptions {
    bool allowInternet = true;    // was !PSP_WLAN_NO_INTERNET -- real toggle: with this off, every DNS/TCP/UDP connector stays unset (every lookup fails)
    std::string dnsOverride;      // was PSP_WLAN_DNS -- asks this server directly instead of the host resolver
};

struct SysconOptions {
    bool wlanSwitchOn = false;    // was PSP_WLAN_SWITCH -- the console's real physical WLAN slider state
    int rtcSeedFlags = 0;         // was PSP_RTC_SEED -- seeds the emulated clock/scratchpad as already-initialized (bit 2: also seed scratch[8..11])
};

struct BusOptions {
    bool flushPlainMemory = false; // was PSP_FLUSH_PLAIN -- legacy: flush device ticks even for plain-memory (RAM/scratchpad/VRAM/IPL) accesses
    uint32_t tickBatch = 8;        // was PSP_TICK_BATCH -- device-time batching granularity (instructions per flush); 1 = tick every instruction
};

struct TestOptions {
    int64_t stateHashInterval = 0;   // was PSP_STATE_HASH
    int64_t stateHashFrom = 0;       // was PSP_STATE_FROM
    std::string scriptedInputPath;   // was PSP_SEQ / PSP_SEQ2 (file-based in beta)
};

struct WindowOptions {
    bool open = false;            // was PSP_WINDOW
    std::string title;            // was PSP_WINDOW_TITLE
};

struct EmuOptions {
    CpuOptions cpu;
    RebootOptions reboot;
    GeOptions ge;
    MeOptions me;
    MsOptions ms;
    AudioOptions audio;
    UmdOptions umd;
    WlanOptions wlan;
    SysconOptions syscon;
    BusOptions bus;
    TestOptions test;
    WindowOptions window;
};

} // namespace beta::config
