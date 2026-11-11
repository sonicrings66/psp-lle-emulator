#include "options_cli.h"

#include <cstdlib>
#include <cstring>

namespace beta::config {

namespace {

bool envFlag(const char* name) { return std::getenv(name) != nullptr; }

std::string envString(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

int64_t envInt(const char* name, int64_t fallback) {
    const char* v = std::getenv(name);
    return v ? std::atoll(v) : fallback;
}

double envDouble(const char* name, double fallback) {
    const char* v = std::getenv(name);
    return v ? std::atof(v) : fallback;
}

} // namespace

EmuOptions parseOptionsFromEnvironment() {
    EmuOptions o;

    o.cpu.ringTrace = envFlag("PSP_CPU_RING");
    o.cpu.logHalts = envFlag("PSP_HALT_LOG");
    o.cpu.logBusErrors = envFlag("PSP_LOG_BUSERR");
    o.cpu.traceSyscall21 = envFlag("PSP_TRACE_SYSCALL21");

    o.ge.gpuAccel = envFlag("PSP_GPU_GE");
    o.ge.stats = envFlag("PSP_GE_STATS");
    o.ge.recentPrims = envFlag("PSP_GE_RECENT");
    {
        const int64_t v = envInt("PSP_GE_RECENT_MAX", 40);
        if (v > 0) o.ge.recentMax = static_cast<size_t>(v);
    }
    // PSP_GE_NORMALMAT/PSP_GE_ANALYTICN's real semantics are "unset or anything but a
    // leading '0' means on" (ge_renderer.h's own prior inline checks), not a plain
    // envFlag() presence check -- read directly here, the one file allowed to.
    o.ge.normalMatInverseTranspose = !(std::getenv("PSP_GE_NORMALMAT") && std::getenv("PSP_GE_NORMALMAT")[0] == '0');
    o.ge.analyticNormals = !(std::getenv("PSP_GE_ANALYTICN") && std::getenv("PSP_GE_ANALYTICN")[0] == '0');
    o.ge.texDumpDir = envString("PSP_TEXDUMP", "");
    o.ge.texDumpRaw = envFlag("PSP_TEXDUMP_RAW");
    o.ge.patchDebug = envFlag("PSP_GE_PATCHDBG");
    o.ge.xfBoxFrom = envInt("PSP_GE_XFBOX", -1);
    o.ge.threads = static_cast<int>(envInt("PSP_GE_THREADS", -1));

    {
        const int64_t v = envInt("PSP_ME_RATIO", 1);
        if (v > 0 && v <= 64) o.me.ratio = static_cast<uint32_t>(v);
    }
    o.me.horizonAlways = envFlag("PSP_ME_HORIZON_ALWAYS");
    o.me.log = envFlag("PSP_ME_LOG");
    o.me.shadow = envFlag("PSP_ME_SHADOW");
    o.me.realBoot = envFlag("PSP_ME_REALBOOT");

    o.ms.present = !envFlag("PSP_MS_ABSENT"); // beta default: present
    o.ms.folderPath = envString("PSP_MS_FOLDER", "");
    o.ms.imagePath = envString("PSP_MS_IMAGE", "");

    o.audio.enabled = !envFlag("PSP_NO_AUDIO");
    o.audio.dmaMs = envDouble("PSP_AUDIO_DMA_MS", 2.0);
    o.audio.wavDumpDir = envString("PSP_AUDIO_OUT", "");

    o.umd.readyMs = envDouble("PSP_UMD_READY_MS", 20.0);

    o.wlan.allowInternet = !envFlag("PSP_WLAN_NO_INTERNET");
    o.wlan.dnsOverride = envString("PSP_WLAN_DNS", "");

    o.syscon.wlanSwitchOn = envFlag("PSP_WLAN_SWITCH");
    o.syscon.rtcSeedFlags = static_cast<int>(envInt("PSP_RTC_SEED", 0));

    o.bus.flushPlainMemory = envFlag("PSP_FLUSH_PLAIN");
    {
        const int64_t v = envInt("PSP_TICK_BATCH", 8);
        if (v >= 1 && v <= 256) o.bus.tickBatch = static_cast<uint32_t>(v);
    }

    o.test.stateHashInterval = envInt("PSP_STATE_HASH", 0);
    o.test.stateHashFrom = envInt("PSP_STATE_FROM", 0);
    o.test.scriptedInputPath = envString("PSP_SCRIPTED_INPUT", "");

    o.window.open = envFlag("PSP_WINDOW");
    o.window.title = envString("PSP_WINDOW_TITLE", "");

    return o;
}

void applyCommandLineArgs(EmuOptions& options, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--window") {
            options.window.open = true;
        } else if (arg.rfind("--window-title=", 0) == 0) {
            options.window.title = std::string(arg.substr(std::strlen("--window-title=")));
        } else if (arg.rfind("--state-hash-every=", 0) == 0) {
            options.test.stateHashInterval = std::atoll(std::string(arg.substr(std::strlen("--state-hash-every="))).c_str());
        } else if (arg.rfind("--scripted-input=", 0) == 0) {
            options.test.scriptedInputPath = std::string(arg.substr(std::strlen("--scripted-input=")));
        } else if (arg == "--gpu-accel") {
            options.ge.gpuAccel = true;
        } else if (arg == "--no-audio") {
            options.audio.enabled = false;
        }
        // Unrecognized arguments are deliberately ignored here -- a
        // dedicated --help pass belongs in the boot driver (Phase A),
        // not in this parsing helper.
    }
}

} // namespace beta::config
