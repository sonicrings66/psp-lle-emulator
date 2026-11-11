#include "module_link.h"

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace beta::format {

// Real MIPS "jr $ra" (rs=$ra, funct=0x08) and a "syscall N" encoding
// (opcode 0, funct 0x0C, N packed into bits [25:6]) -- the two words of a
// real kernel/user privilege-crossing stub (uofw's SceStub union: `Syscall
// { returnAddr; syscall; }`, include/loadcore.h). Exposed (not file-local)
// so other real syscall-stub producers -- e.g. a host-callback patch site,
// see syscall_resolver.h's tryHostSyscall/registerHostSyscall -- can reuse
// the exact same real encoding instead of duplicating it.
uint32_t syscallInstr(uint32_t code) { return (code << 6) | 0x0Cu; }

namespace {

uint16_t readU16LE(const uint8_t* p, std::size_t off) {
    return static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
}

uint32_t readU32LE(const uint8_t* p, std::size_t off) {
    return static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
           (static_cast<uint32_t>(p[off + 2]) << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
}

std::string readCString(Bus& bus, uint32_t addr, std::size_t maxLen = 64) {
    std::string s;
    for (std::size_t i = 0; i < maxLen; ++i) {
        const char c = static_cast<char>(bus.read8(addr + static_cast<uint32_t>(i)));
        if (c == '\0') {
            break;
        }
        s.push_back(c);
    }
    return s;
}

struct ModuleInfoFields {
    uint32_t nameAddr;
    uint32_t libent;
    uint32_t libentend;
    uint32_t libstub;
    uint32_t libstubend;
};

} // namespace

// PspModuleInfo isn't found via a section (all real modules here have
// none, per format/elf.h's own finding) but via a PSP-specific reuse of
// segment 0's p_paddr field as a *file* byte offset (masked with
// 0x7FFFFFFF -- PPSSPP: ElfReader::LoadInto's modInfo fallback). That
// offset is relative to the start of the file, not the segment, so it has
// to be converted to a virtual address via segment 0's own
// p_vaddr/p_offset before it means anything as a runtime bus address.
bool findModuleInfo(const std::vector<uint8_t>& elf, uint32_t baseAddress, uint32_t& outAddr) {
    if (elf.size() < 0x34) {
        return false;
    }
    const uint32_t ePhoff = readU32LE(elf.data(), 0x1C);
    const uint16_t ePhnum = readU16LE(elf.data(), 0x2C);
    if (ePhnum == 0 || static_cast<std::size_t>(ePhoff) + 0x20 > elf.size()) {
        return false;
    }

    const uint32_t seg0Type = readU32LE(elf.data(), ePhoff + 0x00);
    const uint32_t seg0Offset = readU32LE(elf.data(), ePhoff + 0x04);
    const uint32_t seg0Vaddr = readU32LE(elf.data(), ePhoff + 0x08);
    const uint32_t seg0Paddr = readU32LE(elf.data(), ePhoff + 0x0C);
    if (seg0Type != 1 /* PT_LOAD */) {
        return false;
    }

    const uint32_t maskedPaddr = seg0Paddr & 0x7FFFFFFFu;
    if (maskedPaddr < seg0Offset) {
        return false;
    }

    const uint32_t moduleInfoVaddr = seg0Vaddr + (maskedPaddr - seg0Offset);
    outAddr = baseAddress + moduleInfoVaddr;
    return true;
}

namespace {

ModuleInfoFields readModuleInfoFields(Bus& bus, uint32_t addr) {
    ModuleInfoFields fields;
    fields.nameAddr = addr + 0x04; // the 28-byte name lives inline, not behind a pointer
    fields.libent = bus.read32(addr + 0x24);
    fields.libentend = bus.read32(addr + 0x28);
    fields.libstub = bus.read32(addr + 0x2C);
    fields.libstubend = bus.read32(addr + 0x30);
    return fields;
}

using ExportKey = std::pair<std::string, uint32_t>; // (library name, NID)

struct ExportEntry {
    uint32_t address = 0;
    bool isSyscallExport = false; // real SCE_LIB_SYSCALL_EXPORT attribute
                                   // (0x4000, uofw's include/loadcore.h) on
                                   // the exporting library
};
using ExportRegistry = std::map<ExportKey, ExportEntry>;

// (library name, NID) -> real address of the exported variable itself
// (as opposed to ExportRegistry, which maps to a function's entry point).
using VariableRegistry = std::map<ExportKey, uint32_t>;

// Real SceLibAttr bit (uofw's include/loadcore.h): "Indicates the use of
// the SYSCALL technique for linking" -- set on a resident library's own
// export-table attribute field when it's meant to be called from user-mode
// code across the real privilege boundary (e.g. sceDisplay), as opposed to
// an ordinary same-privilege kernel export (e.g. sceDisplay_driver).
constexpr uint16_t kSceLibSyscallExport = 0x4000;

// Walks one module's .lib.ent (export) table. Ported from PPSSPP's export
// walk in sceKernelModule.cpp, minus the module_start/stop special-casing
// (not needed for cross-module *call* resolution, only for the kernel's
// own module lifecycle management). Real layout (jpcsp's Loader.java,
// Elf32EntHeader): the resident table holds function NIDs, then variable
// NIDs, then function export addresses, then variable export addresses --
// each block variableCount/functionCount words long.
void walkExports(Bus& bus, const std::string& ownName, uint32_t libent, uint32_t libentend,
                  ExportRegistry& registry, VariableRegistry& variableRegistry, int& exportsFound) {
    uint32_t pos = libent;
    while (pos < libentend) {
        const uint32_t nameAddr = bus.read32(pos + 0x00);
        const uint16_t attribute = bus.read16(pos + 0x06);
        const uint8_t sizeWords = bus.read8(pos + 0x08);
        if (sizeWords == 0) {
            pos += 4;
            continue;
        }
        const uint8_t vcount = bus.read8(pos + 0x09);
        const uint16_t fcount = bus.read16(pos + 0x0A);
        const uint32_t resident = bus.read32(pos + 0x0C);
        uint16_t vcountNew = 0;
        if (sizeWords > 4) {
            vcountNew = bus.read16(pos + 0x10);
        }
        const uint32_t variableCount = sizeWords <= 4 ? vcount : std::max<uint32_t>(vcount, vcountNew);

        const std::string libName = nameAddr != 0 ? readCString(bus, nameAddr) : ownName;
        const bool isSyscallExport = (attribute & kSceLibSyscallExport) != 0;

        for (uint16_t j = 0; j < fcount; ++j) {
            const uint32_t nid = bus.read32(resident + j * 4);
            const uint32_t exportAddr = bus.read32(resident + (static_cast<uint32_t>(fcount) + variableCount) * 4 + j * 4);
            registry[{libName, nid}] = ExportEntry{exportAddr, isSyscallExport};
            ++exportsFound;
        }

        for (uint32_t j = 0; j < variableCount; ++j) {
            const uint32_t nid = bus.read32(resident + (static_cast<uint32_t>(fcount) + j) * 4);
            const uint32_t varAddr = bus.read32(resident + (static_cast<uint32_t>(fcount) + variableCount) * 4 +
                                                 static_cast<uint32_t>(fcount) * 4 + j * 4);
            variableRegistry[{libName, nid}] = varAddr;
        }

        pos += static_cast<uint32_t>(sizeWords) * 4;
    }
}

// Real MIPS opcode field values (top 6 bits of a 32-bit word) used to tag
// each entry in a variable import's relocation chain (jpcsp's Loader.java,
// hasVStub() handling) -- chosen because they're otherwise-meaningless as
// real branch targets in this exact packed-word context, not because a
// BNE/BLEZ/J instruction is actually executed here.
constexpr uint32_t kVStubRelocHi16 = 0x05u; // BNE
constexpr uint32_t kVStubRelocLo16 = 0x06u; // BLEZ
constexpr uint32_t kVStubReloc32   = 0x02u; // J

// Applies one real R_MIPS_HI16/LO16/32-style patch to a resolved variable's
// address, matching jpcsp's DeferredVStubHI16/DeferredVstubLO16/
// DeferredVStub32 exactly (the LO16 and 32-bit forms are addend-preserving;
// the HI16 form is not -- jpcsp's own HI16.resolve() shifts the existing
// placeholder out of a 32-bit value before adding the address, discarding
// it, which is harmless in practice since a real compiled lui's own
// placeholder immediate is always 0 -- any addend from a paired
// lui/addiu pair lives entirely in the LO16 half).
void applyVStubReloc(Bus& bus, uint32_t opcode, uint32_t patchAddr, uint32_t resolvedAddr) {
    switch (opcode) {
        case kVStubRelocHi16: {
            const uint32_t hi16 = (resolvedAddr >> 16) + ((resolvedAddr & 0x8000u) ? 1u : 0u);
            bus.write16(patchAddr, static_cast<uint16_t>(hi16));
            break;
        }
        case kVStubRelocLo16: {
            const int32_t existingLo = static_cast<int16_t>(bus.read16(patchAddr));
            bus.write16(patchAddr, static_cast<uint16_t>(existingLo + static_cast<int32_t>(resolvedAddr)));
            break;
        }
        case kVStubReloc32: {
            const uint32_t existing = bus.read32(patchAddr);
            bus.write32(patchAddr, existing + resolvedAddr);
            break;
        }
        default:
            break; // Unknown tag; real PSP toolchains don't emit one, matching jpcsp's own warn-and-skip.
    }
}

// Walks one module's .lib.stub (import) table and patches every stub that
// resolves against `registry`/`variableRegistry`. An ordinary
// (same-privilege) function export patches to `j <address>; nop` -- exactly
// the two words jpcsp's DeferredStub.resolve writes for a genuinely-resolved
// import (confirmed against jpcsp/src/jpcsp/format/DeferredStub.java). An
// export whose owning library carries the real SCE_LIB_SYSCALL_EXPORT
// attribute instead patches to `jr $ra; syscall N` (uofw's real stub shape)
// -- N assigned fresh from `nextSyscallCode` and registered against the real
// target via `bus.registerSyscall()`, the same (libName,nid)->address value
// already resolved into `registry`.
//
// A *variable* import (as opposed to a function one) works completely
// differently on real hardware: there's no callable stub to redirect, since
// the referencing code just does an ordinary lui/addiu-relative load or
// store. Real PSP toolchains instead emit, per variable NID, a linked list
// of (opcode, address) relocation entries -- one per place in the code that
// references the variable -- terminated by a 0 word (jpcsp's Loader.java,
// Elf32StubHeader.hasVStub()/getVStub()/getVStubSize()). Each entry's real
// MIPS-opcode-shaped tag (top 6 bits) says whether `address` holds a lui
// (patch HI16), an addiu/ori/load/store immediate (patch LO16), or a plain
// 32-bit pointer slot (patch the whole word) -- see applyVStubReloc.
//
// Either way this is real address-to-address linking; an import with no
// match in the registry (the exporting module isn't loaded/linked) is left
// as whatever loadElf's own relocation pass wrote, not patched to a
// synthetic substitute.
//
// Note: the old project's version of this function carried several
// unconditional (not even getenv-gated) `std::printf` diagnostics left
// over from specific past bug hunts (a header-by-header import-walk trace,
// a hardcoded-NID lookup dump, a hardcoded-address V-Stub reloc-chain
// trace). Dropped entirely here, same policy as every other pure
// diagnostic found during this port (see docs/PORTING_LOG.md) -- they had
// zero effect on control flow or data, and left unconditional would spam
// stdout on every real module link in beta.
void walkImportsAndResolve(CpuBus& bus, uint32_t libstub, uint32_t libstubend, const ExportRegistry& registry,
                            const VariableRegistry& variableRegistry, std::map<ExportKey, uint32_t>& syscallNumbers,
                            uint32_t& nextSyscallCode, int& resolved, int& unresolved, int& syscallsAssigned,
                            int& variablesResolved, int& variablesUnresolved) {
    uint32_t pos = libstub;
    while (pos < libstubend) {
        const uint32_t nameAddr = bus.read32(pos + 0x00);
        const uint8_t sizeWords = bus.read8(pos + 0x08);
        if (sizeWords == 0) {
            pos += 4;
            continue;
        }
        const uint8_t vstubSize = bus.read8(pos + 0x09);
        const uint16_t numFuncs = bus.read16(pos + 0x0A);
        const uint32_t nidData = bus.read32(pos + 0x0C);
        const uint32_t firstSymAddr = bus.read32(pos + 0x10);
        // s_vstub only exists in the header at all when s_size >= 6
        // (Elf32StubHeader.hasVStub()); reading it otherwise would read into
        // the NEXT stub header entirely.
        const uint32_t vstub = sizeWords >= 6 ? bus.read32(pos + 0x14) : 0;

        const std::string libName = nameAddr != 0 ? readCString(bus, nameAddr) : "syslib";

        if (numFuncs > 0 && nidData != 0) {
            for (uint16_t i = 0; i < numFuncs; ++i) {
                const uint32_t nid = bus.read32(nidData + i * 4);
                const uint32_t stubAddr = firstSymAddr + i * 8;

                const auto found = registry.find({libName, nid});
                if (found == registry.end()) {
                    ++unresolved;
                    continue;
                }
                const ExportEntry& entry = found->second;
                if (entry.isSyscallExport) {
                    const ExportKey key{libName, nid};
                    auto numIt = syscallNumbers.find(key);
                    if (numIt == syscallNumbers.end()) {
                        numIt = syscallNumbers.emplace(key, nextSyscallCode++).first;
                        bus.registerSyscall(numIt->second, entry.address);
                    }
                    bus.write32(stubAddr, kJrRaInstr);
                    bus.write32(stubAddr + 4, syscallInstr(numIt->second));
                    ++syscallsAssigned;
                } else {
                    const uint32_t jInstr = (2u << 26) | ((entry.address >> 2) & 0x03FFFFFFu);
                    bus.write32(stubAddr, jInstr);
                    bus.write32(stubAddr + 4, 0); // nop, the delay slot
                }
                ++resolved;
            }
        }

        if (vstub != 0 && vstubSize > 0) {
            for (uint8_t i = 0; i < vstubSize; ++i) {
                const uint32_t relocListAddr = bus.read32(vstub + static_cast<uint32_t>(i) * 8 + 0);
                const uint32_t nid = bus.read32(vstub + static_cast<uint32_t>(i) * 8 + 4);

                const auto found = variableRegistry.find({libName, nid});
                if (found == variableRegistry.end()) {
                    ++variablesUnresolved;
                    continue;
                }
                const uint32_t resolvedAddr = found->second;

                uint32_t relocAddr = relocListAddr;
                for (;;) {
                    const uint32_t reloc = bus.read32(relocAddr);
                    relocAddr += 4;
                    if (reloc == 0) {
                        break;
                    }
                    const uint32_t opcode = reloc >> 26;
                    const uint32_t patchAddr = (reloc & 0x03FFFFFFu) << 2;
                    applyVStubReloc(bus, opcode, patchAddr, resolvedAddr);
                }
                ++variablesResolved;
            }
        }

        pos += static_cast<uint32_t>(sizeWords) * 4;
    }
}

} // namespace

LinkStats linkModules(const std::vector<ModuleToLink>& modules, CpuBus& bus) {
    LinkStats stats;
    ExportRegistry registry;
    VariableRegistry variableRegistry;
    std::vector<uint32_t> moduleInfoAddrs(modules.size(), 0);

    for (std::size_t m = 0; m < modules.size(); ++m) {
        uint32_t infoAddr = 0;
        if (!findModuleInfo(modules[m].elf, modules[m].baseAddress, infoAddr)) {
            continue;
        }
        moduleInfoAddrs[m] = infoAddr;
        ++stats.modulesWithInfo;

        try {
            const ModuleInfoFields fields = readModuleInfoFields(bus, infoAddr);
            const std::string ownName = readCString(bus, fields.nameAddr, 28);
            if (fields.libentend >= fields.libent) {
                walkExports(bus, ownName, fields.libent, fields.libentend, registry, variableRegistry, stats.exportsFound);
            }
        } catch (const std::exception&) {
            // This module's export table is malformed or points outside
            // loaded memory; skip it rather than losing every other
            // module's linking progress.
        }
    }

    std::map<ExportKey, uint32_t> syscallNumbers;
    uint32_t nextSyscallCode = 0;
    for (std::size_t m = 0; m < modules.size(); ++m) {
        if (moduleInfoAddrs[m] == 0) {
            continue;
        }
        try {
            const ModuleInfoFields fields = readModuleInfoFields(bus, moduleInfoAddrs[m]);
            if (fields.libstubend >= fields.libstub) {
                walkImportsAndResolve(bus, fields.libstub, fields.libstubend, registry, variableRegistry, syscallNumbers,
                                      nextSyscallCode, stats.importsResolved, stats.importsUnresolved,
                                      stats.syscallsAssigned, stats.variablesResolved, stats.variablesUnresolved);
            }
        } catch (const std::exception&) {
            // Same reasoning as above.
        }
    }

    return stats;
}

} // namespace beta::format
