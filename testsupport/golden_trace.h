#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "state_hash.h"

// GoldenTrace: loads a `[STATE] instr ... cpu=... ram=... pc=... count=...`
// log -- the exact line format the old project's PSP_STATE_HASH already
// emits -- so its existing golden traces stay valid comparison baselines
// for beta, without needing to re-run the old project to regenerate them.
//
// This is what turns the plan's "parity is the acceptance gate" milestone
// into an automated check (tests/boot/test_boot_parity.cpp) instead of a
// manual `diff` of two log files.

namespace beta::testsupport {

struct GoldenEntry {
    int64_t instr = 0;
    StateHash hash;
    uint32_t pc = 0;
    uint32_t count = 0;
};

class GoldenTrace {
public:
    // Parses every "[STATE] instr ..." line found anywhere in the file
    // (lines without that marker are ignored, so this can load a full,
    // noisy old-project run log directly, not just a pre-filtered one).
    // Returns false if the file can't be opened; an openable file with
    // zero matching lines returns true with size() == 0 (that's a real,
    // reportable condition for a caller to check, not a load failure).
    bool loadFromFile(const std::string& path);

    std::size_t size() const { return entries_.size(); }
    const GoldenEntry& at(std::size_t i) const { return entries_[i]; }
    const std::vector<GoldenEntry>& entries() const { return entries_; }

    // Compares position-by-position (not by matching `instr` values --
    // callers that sampled at different intervals should resample first).
    // Returns -1 if both traces have the same size and every entry's
    // hash/pc/count matches; otherwise the index of the first mismatch.
    int64_t firstMismatch(const GoldenTrace& other) const;

private:
    std::vector<GoldenEntry> entries_;
};

} // namespace beta::testsupport
