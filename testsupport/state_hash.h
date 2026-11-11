#pragma once

#include <cstddef>
#include <cstdint>

// StateHasher: the equivalence/regression-check primitive every later
// phase's milestone depends on (see the plan's "Verification" section).
// Deliberately generic over raw byte spans in Phase 0/A so it's usable
// and unit-testable before a real Cpu/SystemBus exists -- a
// checkpoint(const Cpu&, const SystemBus&) overload is added once those
// types land (Phase A), implemented in terms of this same primitive.
//
// Semantics are pinned to match the old project's PSP_STATE_HASH exactly
// (FNV-1a, same offset basis/prime, same two-hash split between
// architectural CPU state and RAM) specifically so the old project's own
// golden [STATE] log lines remain valid comparison baselines for beta --
// see GoldenTrace. Do not change the constants below without also
// re-deriving every stored golden trace.

namespace beta::testsupport {

struct StateHash {
    uint64_t cpu = 0;
    uint64_t ram = 0;

    bool operator==(const StateHash& other) const { return cpu == other.cpu && ram == other.ram; }
    bool operator!=(const StateHash& other) const { return !(*this == other); }
};

class StateHasher {
public:
    static constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    static constexpr uint64_t kFnvPrime = 1099511628211ull;

    // FNV-1a over [data, data+len). Pass kFnvOffsetBasis as `seed` for a
    // fresh hash, or a previous result to continue hashing (not used by
    // checkpoint() below, but useful for incremental/streaming callers).
    static uint64_t fnv1a(const uint8_t* data, std::size_t len, uint64_t seed = kFnvOffsetBasis) {
        uint64_t h = seed;
        for (std::size_t i = 0; i < len; ++i) {
            h ^= data[i];
            h *= kFnvPrime;
        }
        return h;
    }

    // cpuState: the ARCHITECTURAL register bytes only -- the caller is
    // responsible for excluding non-architectural scratch (the old
    // project's jitRam/jitWatch/... fields; beta's cpu_debug_state.h
    // split makes this the natural default once CpuState exists).
    // ram may be nullptr with ramLen == 0 (hash becomes 0), matching the
    // old project's own `rp ? fnv(...) : 0ull` fallback for a bus with no
    // direct RAM window.
    static StateHash checkpoint(const uint8_t* cpuState, std::size_t cpuStateLen,
                                 const uint8_t* ram, std::size_t ramLen) {
        StateHash out;
        out.cpu = fnv1a(cpuState, cpuStateLen);
        out.ram = ram ? fnv1a(ram, ramLen) : 0ull;
        return out;
    }
};

} // namespace beta::testsupport
