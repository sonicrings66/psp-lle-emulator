#pragma once

// Minimal shared test-running utilities, ported from psp-lle-emu's
// tests/test_framework.h verbatim in spirit (same no-external-dependency
// rationale: offline-buildable). Extended with a 64-bit check macro since
// StateHash fields are 64-bit, unlike the old project's mostly-32-bit
// register/memory checks.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>

struct TestStats {
    int totalTests = 0;
    int totalFailures = 0;
    int checksInTest = 0;
    int failuresInTest = 0;
};

inline void reportCheck(TestStats& stats, bool ok, const std::string& expr, uint64_t actual, uint64_t expected) {
    ++stats.checksInTest;
    if (!ok) {
        ++stats.failuresInTest;
        std::fprintf(stderr, "  [FAIL] %s: got 0x%016llX, expected 0x%016llX\n", expr.c_str(),
                     static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    }
}

inline void reportCheckBool(TestStats& stats, bool ok, const std::string& expr) {
    ++stats.checksInTest;
    if (!ok) {
        ++stats.failuresInTest;
        std::fprintf(stderr, "  [FAIL] %s\n", expr.c_str());
    }
}

#define CHECK_EQ(stats, actual, expected) \
    reportCheck((stats), (actual) == (expected), #actual, static_cast<uint64_t>(actual), static_cast<uint64_t>(expected))

#define CHECK_TRUE(stats, expr) reportCheckBool((stats), (expr), #expr)

inline void runTest(TestStats& stats, const std::string& name, const std::function<void(TestStats&)>& fn) {
    if (const char* filter = std::getenv("BETA_TEST_FILTER")) { if (name.find(filter) == std::string::npos) return; }
    stats.checksInTest = 0;
    stats.failuresInTest = 0;
    std::printf("RUN  %s\n", name.c_str());
    try {
        fn(stats);
    } catch (const std::exception& e) {
        ++stats.failuresInTest;
        std::fprintf(stderr, "  [FAIL] uncaught exception: %s\n", e.what());
    }
    ++stats.totalTests;
    if (stats.failuresInTest == 0) {
        std::printf("PASS %s (%d checks)\n", name.c_str(), stats.checksInTest);
    } else {
        ++stats.totalFailures;
        std::printf("FAIL %s (%d/%d checks failed)\n", name.c_str(), stats.failuresInTest, stats.checksInTest);
    }
}
