#pragma once

#include <array>
#include <cstdio>

#include "logging.h"

// Concrete Logger implementations and the ScopedLogOverride helper
// (see the plan's Config/logging injection section): the one place a
// future one-off investigation can bump a channel's level for a single
// run, from tools/investigate/, without touching core source.

namespace beta::config {

// Console sink: one level per channel, set once at startup from
// EmuOptions (never read from the environment directly -- that's
// options_cli.cpp's job alone).
class ConsoleLogger final : public Logger {
public:
    void setLevel(LogChannel channel, LogLevel level) {
        levels_[static_cast<std::size_t>(channel)] = level;
    }

    void log(LogChannel channel, LogLevel level, std::string_view msg) override {
        if (!enabled(channel, level)) return;
        std::fprintf(stdout, "[%s/%s] %.*s\n", channelName(channel), levelName(level),
                     static_cast<int>(msg.size()), msg.data());
    }

    bool enabled(LogChannel channel, LogLevel level) const override {
        return level >= levels_[static_cast<std::size_t>(channel)];
    }

private:
    static constexpr std::size_t kChannelCount = 10; // keep in sync with LogChannel
    std::array<LogLevel, kChannelCount> levels_{
        LogLevel::Warn, LogLevel::Warn, LogLevel::Warn, LogLevel::Warn, LogLevel::Warn,
        LogLevel::Warn, LogLevel::Warn, LogLevel::Warn, LogLevel::Warn, LogLevel::Warn,
    };

    static const char* channelName(LogChannel c) {
        switch (c) {
            case LogChannel::Cpu: return "cpu";
            case LogChannel::Gpu: return "gpu";
            case LogChannel::Mmio: return "mmio";
            case LogChannel::Boot: return "boot";
            case LogChannel::Reboot: return "reboot";
            case LogChannel::Hle: return "hle";
            case LogChannel::Me: return "me";
            case LogChannel::Nand: return "nand";
            case LogChannel::Net: return "net";
            case LogChannel::Test: return "test";
        }
        return "?";
    }
    static const char* levelName(LogLevel l) {
        switch (l) {
            case LogLevel::Trace: return "trace";
            case LogLevel::Debug: return "debug";
            case LogLevel::Info: return "info";
            case LogLevel::Warn: return "warn";
            case LogLevel::Error: return "error";
        }
        return "?";
    }
};

// RAII: temporarily lowers one channel's threshold (e.g. to Trace) for
// the lifetime of the override, then restores it. Intended for
// tools/investigate/ one-off sessions -- never for a permanent toggle,
// which belongs in EmuOptions/options_cli.cpp instead.
class ScopedLogOverride {
public:
    ScopedLogOverride(ConsoleLogger& logger, LogChannel channel, LogLevel temporaryLevel)
        : logger_(logger), channel_(channel) {
        // ConsoleLogger doesn't expose a getter by design (keeps the
        // common path small); callers that need true restore-on-exit
        // semantics pass back the level they already know they set.
        logger_.setLevel(channel_, temporaryLevel);
    }
    ScopedLogOverride(const ScopedLogOverride&) = delete;
    ScopedLogOverride& operator=(const ScopedLogOverride&) = delete;

    ~ScopedLogOverride() { logger_.setLevel(channel_, restoreLevel_); }

    void setRestoreLevel(LogLevel level) { restoreLevel_ = level; }

private:
    ConsoleLogger& logger_;
    LogChannel channel_;
    LogLevel restoreLevel_ = LogLevel::Warn;
};

} // namespace beta::config
