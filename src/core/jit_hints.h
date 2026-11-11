#pragma once

#include <cstdint>

// JIT-support mixin -- split out of the old project's bus.h (see
// docs/PORTING_LOG.md §1a). Every method here exists purely so a future
// JIT (deferred to Phase F, see the plan) can safely skip work the
// interpreter always does one instruction at a time. Nothing requires a
// plain Bus (or a Bus that only cares about TimingSource) to implement
// any of this -- the defaults below are always correct for a bus with no
// JIT fast path.

namespace beta {

class JitHints {
public:
    virtual ~JitHints() = default;

    // Direct-access window onto plain RAM (physical base + size), or
    // nullptr when the bus has none. Lets the CPU fetch and load without
    // a virtual call and an address-decode chain per access. Only a
    // range that decodes to RAM on every page may be returned.
    virtual const uint8_t* ramWindow(uint32_t& physBase, uint32_t& size) const {
        (void)physBase; (void)size; return nullptr;
    }
    // Polled by the CPU every few instructions: false while some
    // diagnostic hook wrapping this bus can affect RAM accesses (direct
    // access is then off).
    virtual bool ramDirectOk() const { return true; }
    // Counter the bus bumps on every store; the CPU bumps it too when it
    // stores to RAM directly (a hook recorder reads it).
    virtual uint64_t* ramWriteCounter() { return nullptr; }

    // True if accessing `addr` can have an effect beyond reading/writing
    // a stored value (a device register: it may raise an interrupt,
    // start DMA, return a time-dependent value, ...). A JIT never
    // performs such an access itself -- it bails out and lets the
    // interpreter do it with exactly the right device time. Plain
    // RAM-like memory (and anything unmapped, which faults) answers
    // false.
    virtual bool hasSideEffects(uint32_t addr) const { (void)addr; return false; }

    // ---- Exact, cheap invalidation of translated code (old project,
    // 2026-10-02). Re-reading every source word of a cached block on
    // each entry costs as much as the interpreter's own instruction
    // fetch, so a Bus that can track writes lets a dispatcher trust a
    // cached block until something writes the memory it was built from.
    //   tracksCodeWrites(): this Bus reliably bumps codeWriteEpoch()
    //                       when a WATCHED location is written, by any
    //                       path. False (the default) means the
    //                       dispatcher must re-validate by reading words.
    //   watchCode(addr,size): the caller holds translations of
    //                       [addr, addr+size); writes to those 32-byte
    //                       lines must bump the epoch. A write to a
    //                       watched line bumps the epoch AND unwatches
    //                       it (callers re-watch when they re-trust).
    //   codeWriteEpoch():   monotonically increasing; a cached block
    //                       built/validated at epoch E is still valid
    //                       iff the epoch is still E.
    //   storeNeedsInterpreter(addr): like hasSideEffects but for STORES
    //                       -- also true for a watched code line, so a
    //                       store from inside a block can never modify
    //                       code that same block (or a later part of it)
    //                       is about to run; the interpreter does it.
    virtual bool tracksCodeWrites() const { return false; }
    virtual void watchCode(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
    virtual uint64_t codeWriteEpoch() const { return 0; }
    virtual bool storeNeedsInterpreter(uint32_t addr) const { return hasSideEffects(addr); }

    // True if executing code at `addr` has the SAME timing behaviour
    // whether it is interpreted or run as a compiled block. SystemBus:
    // only plain RAM -- the interpreter's instruction fetch from any
    // OTHER region (VRAM, scratchpad, IPL ROM) goes through the bus slow
    // path, which flushes pending device ticks on every fetch, i.e.
    // devices effectively tick per instruction there; a compiled block
    // does no fetches, so a JIT must leave such code to the interpreter.
    // Plain test buses have no such difference.
    virtual bool codeFetchIsTimingNeutral(uint32_t addr) const { (void)addr; return true; }

    // The watch bitmap behind the RAM window (ramWindow()): one byte per
    // 32-byte line, nonzero = watched. The CPU's direct-store fast path
    // bypasses write8/16/32, so it consults this map and sends a store
    // to a watched line through the bus instead. nullptr = none.
    virtual const uint8_t* ramWatchMap() const { return nullptr; }
};

} // namespace beta
