#include "test_framework.h"
#include "golden_trace.h"

#include <string>

#ifndef BETA_TESTS_SRC_DIR
#error "BETA_TESTS_SRC_DIR must be defined by CMake (see tests/CMakeLists.txt)"
#endif

using beta::testsupport::GoldenTrace;

namespace {

std::string fixturePath(const char* name) {
    return std::string(BETA_TESTS_SRC_DIR) + "/fixtures/" + name;
}

void testLoadParsesRealFormatLines(TestStats& stats) {
    // This fixture's five [STATE] lines are copied verbatim (same
    // instruction counts, hashes, pc, count) from a real psp-lle-emu
    // PSP_STATE_HASH run log -- this test is the Phase 0 milestone from
    // the plan: "can load an old-project PSP_STATE_HASH log file."
    GoldenTrace trace;
    CHECK_TRUE(stats, trace.loadFromFile(fixturePath("sample_state_trace.log")));
    CHECK_EQ(stats, trace.size(), 5u);

    const auto& first = trace.at(0);
    CHECK_EQ(stats, static_cast<uint64_t>(first.instr), 0ull);
    CHECK_EQ(stats, first.hash.cpu, 0x93C65E6EC214CC4Bull);
    CHECK_EQ(stats, first.hash.ram, 0xA0905E2E3DB573F5ull);
    CHECK_EQ(stats, first.pc, 0x040E0000u);
    CHECK_EQ(stats, first.count, 0x00000000u);

    const auto& last = trace.at(4);
    CHECK_EQ(stats, static_cast<uint64_t>(last.instr), 20000000ull);
    CHECK_EQ(stats, last.hash.cpu, 0xFCB5CB89CFA18039ull);
    CHECK_EQ(stats, last.pc, 0x040ED2F8u);
}

void testLoadMissingFileFails(TestStats& stats) {
    GoldenTrace trace;
    CHECK_TRUE(stats, !trace.loadFromFile(fixturePath("does_not_exist.log")));
}

void testFirstMismatchIdenticalTracesReturnsNegativeOne(TestStats& stats) {
    GoldenTrace a, b;
    CHECK_TRUE(stats, a.loadFromFile(fixturePath("sample_state_trace.log")));
    CHECK_TRUE(stats, b.loadFromFile(fixturePath("sample_state_trace.log")));
    CHECK_EQ(stats, static_cast<uint64_t>(a.firstMismatch(b)), static_cast<uint64_t>(-1));
}

void testFirstMismatchFindsTheExactDivergingEntry(TestStats& stats) {
    // Mutation test (docs/TESTING.md discipline): the mutated fixture
    // differs from the original ONLY in the instr=10000000 line's cpu
    // hash (last hex digit 0 -> 1), which is index 2 (0, 5M, 10M, ...).
    // This is the exact check that makes a future beta-vs-old-project
    // parity regression (the plan's "Verification" gate) catch a real
    // divergence instead of silently passing.
    GoldenTrace original, mutated;
    CHECK_TRUE(stats, original.loadFromFile(fixturePath("sample_state_trace.log")));
    CHECK_TRUE(stats, mutated.loadFromFile(fixturePath("sample_state_trace_mutated.log")));
    CHECK_EQ(stats, static_cast<uint64_t>(original.firstMismatch(mutated)), 2ull);
}

void testFirstMismatchDifferentSizesReturnsShorterLength(TestStats& stats) {
    // The truncated fixture is the full fixture's first 3 entries,
    // byte-for-byte identical up to that point (as would happen if a
    // real run stopped early, e.g. hit a budget). firstMismatch should
    // report index 3 (one past the last entry both traces actually
    // have), not -1 -- a truncated run must never silently read as
    // "parity confirmed."
    GoldenTrace full, truncated;
    CHECK_TRUE(stats, full.loadFromFile(fixturePath("sample_state_trace.log")));
    CHECK_TRUE(stats, truncated.loadFromFile(fixturePath("sample_state_trace_truncated.log")));
    CHECK_EQ(stats, truncated.size(), 3u);
    CHECK_EQ(stats, static_cast<uint64_t>(full.firstMismatch(truncated)), 3ull);
}

} // namespace

void runAllGoldenTraceTests(TestStats& stats) {
    runTest(stats, "GoldenTrace.LoadParsesRealFormatLines", testLoadParsesRealFormatLines);
    runTest(stats, "GoldenTrace.LoadMissingFileFails", testLoadMissingFileFails);
    runTest(stats, "GoldenTrace.FirstMismatchIdenticalTracesReturnsNegativeOne", testFirstMismatchIdenticalTracesReturnsNegativeOne);
    runTest(stats, "GoldenTrace.FirstMismatchFindsTheExactDivergingEntry", testFirstMismatchFindsTheExactDivergingEntry);
    runTest(stats, "GoldenTrace.FirstMismatchDifferentSizesReturnsShorterLength", testFirstMismatchDifferentSizesReturnsShorterLength);
}
