#pragma once

#include <cstdint>
#include <vector>

#include "../cpu_bus.h"

namespace beta::format {

// Real MIPS "jr $ra" (rs=$ra, funct=0x08) and a "syscall N" encoding
// (opcode 0, funct 0x0C, N packed into bits [25:6]) -- the two words of a
// real kernel/user privilege-crossing stub (uofw's SceStub union: `Syscall
// { returnAddr; syscall; }`, include/loadcore.h). Exposed here (not
// file-local to module_link.cpp) so any other real syscall-stub producer
// (e.g. a host-callback patch site, see syscall_resolver.h's
// tryHostSyscall/registerHostSyscall) can reuse the exact same real
// encoding.
constexpr uint32_t kJrRaInstr = 0x03E00008u;
uint32_t syscallInstr(uint32_t code);

struct LinkStats {
    int modulesWithInfo = 0;   // modules where a PspModuleInfo was actually found
    int exportsFound = 0;
    int importsResolved = 0;
    int importsUnresolved = 0;
    int syscallsAssigned = 0;  // imports resolved against a real
                                // SCE_LIB_SYSCALL_EXPORT library (see
                                // uofw's include/loadcore.h) -- patched as
                                // a real `jr $ra; syscall N` stub and
                                // registered in the bus's syscall table,
                                // not a plain `j <address>; nop`.
    int variablesResolved = 0;   // real cross-module *data* imports (as
    int variablesUnresolved = 0; // opposed to function imports/calls above)
                                  // resolved via each NID's real relocation
                                  // chain (jpcsp's Elf32StubHeader V-Stub) --
                                  // see module_link.cpp's applyVStubReloc.
};

// Locates a loaded module's PspModuleInfo in runtime memory (already
// relocated). Real modules always have one; it's found via a PSP-specific
// convention (segment 0's p_paddr field reused as a *file* byte offset --
// none of the modules this project has seen carry real section headers to
// name a ".rodata.sceModuleInfo" section instead, per format/elf.h's
// finding). `elf` must be the original file bytes passed to loadElf (the
// lookup needs the program header table's file-offset fields, not
// anything that ended up loaded). Returns false if none was found.
//
// Callers that need it: the module's `gp` (global pointer) value lives at
// moduleInfoAddr+0x20 -- real PSP code leans heavily on $gp-relative
// addressing for global data, and the kernel always loads $gp with this
// value before running a module's entry point or any function it exports,
// so a caller executing a module directly (as opposed to going through
// the real kernel's own call path) needs to set it manually first.
bool findModuleInfo(const std::vector<uint8_t>& elf, uint32_t baseAddress, uint32_t& moduleInfoAddrOut);

// One already-loaded module (see loadElf()), ready to be linked.
struct ModuleToLink {
    std::vector<uint8_t> elf;   // the raw decrypted ELF bytes -- PspModuleInfo
                                 // is found via a *file* offset (segment 0's
                                 // p_paddr, PSP's reuse of that ELF field),
                                 // so the original file bytes are needed, not
                                 // just what ended up in memory.
    uint32_t baseAddress = 0;   // the base loadElf() loaded this module at
};

// Real PSP inter-module linking: walks every module's export table
// (.lib.ent, found via its PspModuleInfo) to build a (library name, NID) ->
// address registry, then walks every module's import table (.lib.stub) and
// patches each stub that matches. Real PSP hardware uses one of two stub
// shapes depending on the exporting library's real attribute flags (uofw's
// include/loadcore.h, SceLibAttr): an ordinary same-privilege (kernel/kernel
// or user/user) export patches to `j <address>; nop` -- the exact same two
// words real hardware's module loader (and jpcsp's genuine LLE resolution
// path, DeferredStub.resolve) write. A library exported with
// SCE_LIB_SYSCALL_EXPORT (kernel-resident, callable from user-mode code --
// e.g. sceDisplay, sceCtrl) instead patches to `jr $ra; syscall N`, where N
// is a syscall number newly assigned here and registered against the real
// target address via SyscallResolver::registerSyscall() (Cpu::step()
// resolves it back via resolveSyscall() when it decodes that SYSCALL).
// Either way this is real address-to-address linking to real code: an
// import only resolves if the exporting module is also loaded and included
// in this same call. Unresolved imports are left as whatever loadElf's
// relocation pass wrote (i.e. execution hits the original stub content,
// most likely an unmapped-memory or unimplemented-opcode fault) -- not
// patched to anything synthetic.
//
// Takes CpuBus (not plain Bus) because registering a syscall needs
// SyscallResolver, which plain Bus no longer provides after the interface
// split (see cpu_bus.h) -- every real caller (SystemBus) already is one.
LinkStats linkModules(const std::vector<ModuleToLink>& modules, CpuBus& bus);

} // namespace beta::format
