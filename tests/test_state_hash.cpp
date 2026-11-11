#include "test_framework.h"
#include "state_hash.h"

#include <cstring>

using beta::testsupport::StateHash;
using beta::testsupport::StateHasher;

namespace {

void testFnv1aDeterministicAndSeedCorrect(TestStats& stats) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    const uint64_t h1 = StateHasher::fnv1a(data, sizeof(data));
    const uint64_t h2 = StateHasher::fnv1a(data, sizeof(data));
    CHECK_EQ(stats, h1, h2); // same input -> same hash, every time

    // Empty input: FNV-1a defines this as exactly the offset basis
    // (the loop body never executes) -- pin this as a known value so a
    // future accidental change to the constants is caught immediately,
    // not just "some other hash changed somewhere."
    const uint64_t hEmpty = StateHasher::fnv1a(data, 0);
    CHECK_EQ(stats, hEmpty, StateHasher::kFnvOffsetBasis);
}

void testFnv1aMutationChangesHash(TestStats& stats) {
    // Per docs/TESTING.md's own discipline ("a test that cannot fail is
    // not a test"): deliberately corrupt one byte and confirm the hash
    // actually moves, rather than only checking it doesn't crash.
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    const uint64_t before = StateHasher::fnv1a(data, sizeof(data));
    data[2] ^= 0x01; // flip exactly one bit
    const uint64_t after = StateHasher::fnv1a(data, sizeof(data));
    CHECK_TRUE(stats, before != after);
}

void testCheckpointSplitsCpuAndRam(TestStats& stats) {
    const uint8_t cpuBytes[] = {0x11, 0x22, 0x33};
    const uint8_t ramBytes[] = {0x44, 0x55, 0x66, 0x77};

    const StateHash withRam = StateHasher::checkpoint(cpuBytes, sizeof(cpuBytes), ramBytes, sizeof(ramBytes));
    CHECK_EQ(stats, withRam.cpu, StateHasher::fnv1a(cpuBytes, sizeof(cpuBytes)));
    CHECK_EQ(stats, withRam.ram, StateHasher::fnv1a(ramBytes, sizeof(ramBytes)));
    CHECK_TRUE(stats, withRam.cpu != withRam.ram); // sanity: not accidentally hashing the same bytes twice

    // No RAM window (nullptr): old-project semantics are ram hash == 0,
    // not "hash of zero bytes" (which would equal the offset basis) --
    // these are different values, so this pins the right one.
    const StateHash noRam = StateHasher::checkpoint(cpuBytes, sizeof(cpuBytes), nullptr, 0);
    CHECK_EQ(stats, noRam.ram, 0ull);
    CHECK_TRUE(stats, noRam.ram != StateHasher::kFnvOffsetBasis);
}

void testStateHashEquality(TestStats& stats) {
    StateHash a{0x1111ull, 0x2222ull};
    StateHash b{0x1111ull, 0x2222ull};
    StateHash c{0x1111ull, 0x3333ull};
    CHECK_TRUE(stats, a == b);
    CHECK_TRUE(stats, a != c);
}

} // namespace

void runAllStateHashTests(TestStats& stats) {
    runTest(stats, "StateHash.Fnv1aDeterministicAndSeedCorrect", testFnv1aDeterministicAndSeedCorrect);
    runTest(stats, "StateHash.Fnv1aMutationChangesHash", testFnv1aMutationChangesHash);
    runTest(stats, "StateHash.CheckpointSplitsCpuAndRam", testCheckpointSplitsCpuAndRam);
    runTest(stats, "StateHash.Equality", testStateHashEquality);
}
