#pragma once

#include <string_view>

// Logger: replaces the old project's printf-behind-getenv debug taps
// (~45 sites in src/core alone, before counting the test harness).
// Channel levels are set once from EmuOptions at startup; a subsystem
// checks enabled() before formatting so the hot path stays cheap, same
// reason the old code memoized getenv() into a static const bool.

namespace beta::config {

enum class LogChannel {
    Cpu, Gpu, Mmio, Boot, Reboot, Hle, Me, Nand, Net, Test,
};

enum class LogLevel { Trace, Debug, Info, Warn, Error };

class Logger {
public:
    virtual ~Logger() = default;
    virtual void log(LogChannel channel, LogLevel level, std::string_view msg) = 0;
    virtual bool enabled(LogChannel channel, LogLevel level) const = 0;
};

// Default used by any constructor that isn't handed a real Logger (unit
// tests that don't care about logging, mostly). Never the only Logger in
// a real boot driver run -- options_cli.cpp always builds a real one.
class NullLogger final : public Logger {
public:
    void log(LogChannel, LogLevel, std::string_view) override {}
    bool enabled(LogChannel, LogLevel) const override { return false; }

    static NullLogger& instance() {
        static NullLogger logger;
        return logger;
    }
};

} // namespace beta::config
