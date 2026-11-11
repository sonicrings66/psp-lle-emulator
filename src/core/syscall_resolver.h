#pragma once

#include <cstdint>
#include <functional>

#include "bus.h"

// Syscall/module-linking mixin -- split out of the old project's bus.h
// (see docs/PORTING_LOG.md §1a). Covers both real inter-module syscall
// resolution and the host-callback escape hatch real hardware has no
// equivalent for. A plain Bus (e.g. a test fixture, or a device with no
// role in module linking) implements none of this; the defaults below
// are always correct for it.

namespace beta {

class SyscallResolver {
public:
    virtual ~SyscallResolver() = default;

    // Real PSP hardware resolves a user/kernel privilege-crossing import
    // (a resident library exported with SCE_LIB_SYSCALL_EXPORT, e.g.
    // sceDisplay -- see uofw's include/loadcore.h) to a stub containing
    // `jr $ra` followed by `syscall N` in its delay slot, where N is a
    // syscall number the real kernel's loadcore assigns at link time and
    // maps to the real resident function's address in a real syscall
    // table (uofw's src/kd/loadcore/systable.c). Cpu::step() calls this
    // when it decodes a SYSCALL instruction, to look up the real target
    // address that syscall number was linked against. Returns 0 (never
    // a valid code address) if nothing is registered for `code`. The
    // default no-op is correct for every Bus that doesn't participate in
    // real inter-module linking (plain Memory, test fixtures, etc).
    virtual uint32_t resolveSyscall(uint32_t code) const { (void)code; return 0; }

    // Registers a real syscall number -> real target address mapping,
    // called while resolving a SYSCALL_EXPORT import (see
    // format::linkModules once ported). The default no-op matches
    // resolveSyscall()'s default.
    virtual void registerSyscall(uint32_t code, uint32_t target) { (void)code; (void)target; }

    // A syscall number that dispatches to real HOST C++ code instead of
    // real guest MIPS code. Real Sony hardware has no equivalent -- this
    // exists purely as an emulator-side shortcut for the rare cases
    // where faithfully emulating a real low-level hardware protocol
    // register-by-register isn't tractable, matching jpcsp's own real
    // LLE approach: jpcsp's reboot.java patches a `jr $ra; syscall N`
    // stub directly into a real, already-decrypted Sony binary at one
    // specific low-level routine (e.g. "read one real NAND page"), wiring
    // N to a native method instead of another guest address -- while
    // every OTHER real Sony instruction, including the higher-level
    // logic built on top of that one routine, keeps running unmodified.
    // `topBus` is always the same top-level Bus Cpu::step() itself holds
    // (threaded through unchanged by every wrapper's own override, not
    // each wrapper's own `this`), so a callback's writes into guest
    // memory route through whichever decorator chain (logging, forced-
    // value overrides, etc.) is actually active, the same as any real
    // guest-issued write would. Returns false (not handled) by default.
    using HostSyscallFn = std::function<uint32_t(Bus& topBus, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)>;
    virtual bool tryHostSyscall(uint32_t code, Bus& topBus, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t& outV0) {
        (void)code; (void)topBus; (void)a0; (void)a1; (void)a2; (void)a3; (void)outV0;
        return false;
    }

    // Registers a real syscall number -> host callback mapping. Default
    // no-op matches tryHostSyscall()'s default.
    virtual void registerHostSyscall(uint32_t code, HostSyscallFn fn) { (void)code; (void)fn; }
};

} // namespace beta
