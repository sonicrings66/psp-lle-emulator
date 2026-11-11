#pragma once

#include <cstdint>
#include <vector>

#include "../bus.h"

namespace beta::format {

struct ElfLoadResult {
    uint32_t baseAddress = 0;
    uint32_t entryPoint = 0;
    uint32_t totalSize = 0;   // span from the lowest to the highest PT_LOAD segment
    int loadedSegments = 0;
};

// Loads a real PSP module (already decrypted and decompressed to a plain
// ELF -- see crypto/prx_decrypt.h's decryptModule()) into `bus`, applying
// relocations if the module needs them.
//
// Two module shapes are supported:
//   - ET_EXEC: prerelocated. Segments load at their own p_vaddr (plus
//     `baseAddress`, which callers pass as 0 for a truly fixed-address
//     module); no relocation is applied.
//   - ET_PSP_PRX (0xFFA0): relocatable. Segments load at
//     baseAddress + p_vaddr, then one of two mutually exclusive real
//     relocation formats is applied depending on whether the module has
//     real section headers (confirmed against PPSSPP's ElfReader::LoadInto,
//     which gates on the same condition): headerless modules (every one of
//     the 302 real kernel modules in this project's 661.PBP test firmware)
//     carry a PT_PSPREL2 *segment* -- a compact, bitpacked per-module
//     command stream, ported from PPSSPP's ElfReader::LoadRelocations2.
//     Modules with real section headers (e.g. the firmware updater's own
//     installer executable, PBP's PspData section) instead carry one or
//     more SHT_PSPREL *sections*, each a plain array of classic 8-byte
//     Elf32_Rel entries, ported from PPSSPP's ElfReader::LoadRelocations.
//     PT_PSPREL1 (the segment-based sibling of SHT_PSPREL, "a few games use
//     them" per PPSSPP) is the one real format left unimplemented -- not
//     encountered in any real file this project has decrypted so far.
ElfLoadResult loadElf(const std::vector<uint8_t>& elf, Bus& bus, uint32_t baseAddress);

} // namespace beta::format
