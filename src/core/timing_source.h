#pragma once

#include <cstdint>

// Device-timing mixin -- split out of the old project's bus.h (see
// docs/PORTING_LOG.md §1a and bus.h's own header comment in this
// directory). A Bus that also implements TimingSource lets devices with
// real time-based behavior (a hardware timer counting toward a compare
// match) advance using instruction count as their time base, since the
// interpreter has no real wall clock. Nothing requires a plain
// read/write-only Bus (e.g. a unit-test fixture) to implement this.

namespace beta {

class TimingSource {
public:
    virtual ~TimingSource() = default;

    // 2026-09-19 (old project): lets Cpu::step() push the CPU-internal
    // COP0 Count==Compare timer match through to any bus device that
    // needs to see it directly -- specifically real Sony's own
    // SystemControl MMIO block (0xBC100000), whose real `nmiHandler`
    // reads a live pending-interrupt summary there as its OWN, separate,
    // software-visible confirmation that something is pending (distinct
    // from Cause.IP2, which the CPU itself already aggregates for
    // trap-taking). Without this, that summary read always came back 0
    // for a Count==Compare-sourced interrupt (the internal timer isn't
    // one of InterruptController's 64 real per-bit sources), so real
    // code fell into its own "no real interrupt source found" panic
    // path. The default no-op is correct for any bus without a
    // SystemControl-like device that cares.
    virtual void setTimerInterruptPending(bool) {}

    // Called exactly once per instruction by Cpu::step(), before
    // checking for a pending interrupt -- lets a device with real
    // time-based behavior advance using instruction count as its time
    // base. The default no-op is correct for every device without that
    // kind of behavior.
    virtual void tick() {}

    // Advances device time by n instructions (default: n calls of tick()).
    virtual void tickN(uint32_t n) { for (uint32_t i = 0; i < n; ++i) tick(); }

    // ---- Exact block-execution support (old project, 2026-10-02). The
    // JIT runs N guest instructions without calling tick() N times, which
    // is only behavior-preserving if nothing observable can happen
    // during those N steps. Every device tick() in this project is a
    // pure counter ("ticks++, fire at a precomputed deadline"), so the
    // number of ticks until the next device event is exactly computable
    // and N ticks can be applied in O(1).
    //
    // ticksUntilEvent(): the 1-based tick on which the next device event
    // (an interrupt line being raised, a completion, ...) will occur,
    // counting from now -- i.e. the event happens DURING the k-th tick()
    // call, and Cpu::step() checks for interrupts right after ticking, so
    // the k-th instruction would already be preempted. N instructions are
    // therefore safe to run as a block iff N < ticksUntilEvent().
    // UINT64_MAX means "no event pending". The default (no devices)
    // never has one.
    virtual uint64_t ticksUntilEvent() const { return UINT64_MAX; }

    // Applies `n` ticks at once. Only valid for n < ticksUntilEvent() (no
    // event inside); the default just loops tick() so any Bus that
    // overrides tick() stays correct without overriding this.
    virtual void advanceTicks(uint64_t n) { for (uint64_t i = 0; i < n; ++i) tick(); }
};

} // namespace beta
