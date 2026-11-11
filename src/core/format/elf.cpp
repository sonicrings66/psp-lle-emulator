#include "elf.h"

#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace beta::format {

namespace {

constexpr uint32_t kSegmentNotLoaded = 0xFFFFFFFFu;

constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtPspRel1 = 0x700000a0u;
constexpr uint32_t kPtPspRel2 = 0x700000a1u;
constexpr uint16_t kEtExec = 2;
constexpr uint16_t kEtPspPrx = 0xFFA0;
constexpr uint16_t kEmMips = 8;

uint16_t readU16LE(const uint8_t* p, std::size_t off) {
    return static_cast<uint16_t>(p[off] | (p[off + 1] << 8));
}

uint32_t readU32LE(const uint8_t* p, std::size_t off) {
    return static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
           (static_cast<uint32_t>(p[off + 2]) << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
}

bool segmentIsLoaded(const std::vector<uint32_t>& segmentVAddr, int seg) {
    return seg >= 0 && static_cast<std::size_t>(seg) < segmentVAddr.size() && segmentVAddr[seg] != kSegmentNotLoaded;
}

// sysmem.prx (real module name "sceSystemMemoryManager") and loadcore.prx
// ("sceLoaderCore") are relocated by real PSP hardware's own reboot-code ELF
// loader, not the ordinary module loader every other PRX goes through --
// confirmed via jpcsp's Loader.java (relocateFromBufferA1/
// getOffsetFromRelocationA1), which special-cases exactly these two real
// filenames and remaps the PT_PSPREL2 type-table's values before use. Read
// directly from FILE bytes (this runs before the module is loaded onto the
// bus) via the same PspModuleInfo file-offset convention findModuleInfo()
// uses (segment 0's p_paddr, masked, IS the file offset directly: the vaddr
// formula there simplifies to exactly that).
bool isRealRebootRelocationModule(const std::vector<uint8_t>& elf) {
    if (elf.size() < 0x34) return false;
    const uint32_t ePhoff = readU32LE(elf.data(), 0x1C);
    const uint16_t ePhnum = readU16LE(elf.data(), 0x2C);
    if (ePhnum == 0 || static_cast<std::size_t>(ePhoff) + 0x20 > elf.size()) return false;
    const uint32_t seg0Type = readU32LE(elf.data(), ePhoff + 0x00);
    const uint32_t seg0Offset = readU32LE(elf.data(), ePhoff + 0x04);
    const uint32_t seg0Paddr = readU32LE(elf.data(), ePhoff + 0x0C);
    if (seg0Type != kPtLoad) return false;
    const uint32_t maskedPaddr = seg0Paddr & 0x7FFFFFFFu;
    if (maskedPaddr < seg0Offset) return false;
    const std::size_t nameFileOffset = static_cast<std::size_t>(maskedPaddr) + 0x04;
    if (nameFileOffset + 28 > elf.size()) return false;
    std::string name(reinterpret_cast<const char*>(elf.data() + nameFileOffset), 28);
    const std::size_t nul = name.find('\0');
    if (nul != std::string::npos) name.resize(nul);
    return name == "sceSystemMemoryManager" || name == "sceLoaderCore";
}

// Ported from PPSSPP's ElfReader::LoadRelocations2 (Core/ELF/ElfReader.cpp).
// A compact, bitpacked relocation command stream unique to the PSP: each
// 16-bit command packs a flag/segment/type triple (widths given by the two
// bytes right after the 4-byte header) that index into two small tables,
// with a handful of variable-length extra fields depending on the flag.
// Ported field-for-field, including one quirk kept intentionally: `lastType`
// is initialized once and never reassigned in the original, so the branch
// that reads it always takes the same path. That's presumably harmless
// (or intentional) in a format this battle-tested; changing it would be
// guessing at a fix for a bug that may not even be one.
void applyPspRel2(const uint8_t* buf0, uint32_t bufSize, int relSegmentIndex,
                   const std::vector<uint32_t>& segmentVAddr, Bus& bus, bool useRebootTypeRemap) {
    const uint8_t* buf = buf0;
    const uint8_t* end = buf0 + bufSize;

    auto haveBytes = [&](std::ptrdiff_t n) {
        if (end - buf < n) {
            throw std::runtime_error("loadElf: truncated PT_PSPREL2 relocation data");
        }
    };

    haveBytes(4);
    const int flagBits = buf[2];
    const int typeBits = buf[3];

    int segBits = 1;
    while ((1 << segBits) < relSegmentIndex) {
        segBits += 1;
    }

    buf += 4;

    haveBytes(1);
    const uint8_t* flagTable = buf;
    const int flagTableSize = flagTable[0];
    haveBytes(flagTableSize);
    buf += flagTableSize;

    haveBytes(1);
    const uint8_t* rawTypeTable = buf;
    const int typeTableSize = rawTypeTable[0];
    haveBytes(typeTableSize);
    buf += typeTableSize;

    // sysmem.prx/loadcore.prx: real hardware's reboot-code ELF loader uses a
    // different type-index ordering than every other PRX (confirmed against
    // jpcsp's Loader.java, which applies this exact remap for these two
    // real filenames specifically before using the type table). Applied to
    // every entry except index 0 (always NONE), matching jpcsp's own
    // `for (i = 1; i < types.length; i++)` loop bound exactly.
    static constexpr uint8_t kRebootTypeRemap[8] = {0, 3, 6, 7, 1, 2, 4, 5};
    std::vector<uint8_t> remappedTypeTable;
    const uint8_t* typeTable = rawTypeTable;
    if (useRebootTypeRemap) {
        remappedTypeTable.assign(rawTypeTable, rawTypeTable + typeTableSize);
        for (int i = 1; i < typeTableSize; ++i) {
            if (remappedTypeTable[i] < 8) {
                remappedTypeTable[i] = kRebootTypeRemap[remappedTypeTable[i]];
            }
        }
        typeTable = remappedTypeTable.data();
    }

    int offSeg = 0;
    int relBase = 0;
    int lastType = -1;
    int lo16 = 0;

    while (buf < end) {
        haveBytes(2);
        const int cmd = buf[0] | (buf[1] << 8);
        buf += 2;

        int flag = (cmd << (16 - flagBits)) & 0xFFFF;
        flag = (flag >> (16 - flagBits)) & 0xFFFF;
        if (flag >= flagTableSize) {
            throw std::runtime_error("loadElf: PT_PSPREL2 flag index out of range");
        }
        flag = flagTable[flag];

        int seg = (cmd << (16 - segBits - flagBits)) & 0xFFFF;
        seg = (seg >> (16 - segBits)) & 0xFFFF;

        int type = (cmd << (16 - typeBits - segBits - flagBits)) & 0xFFFF;
        type = (type >> (16 - typeBits)) & 0xFFFF;
        if (type >= typeTableSize) {
            throw std::runtime_error("loadElf: PT_PSPREL2 type index out of range");
        }
        type = typeTable[type];

        if ((flag & 0x01) == 0) {
            offSeg = seg;
            if ((flag & 0x06) == 0) {
                relBase = cmd >> (segBits + flagBits);
            } else if ((flag & 0x06) == 4) {
                haveBytes(4);
                relBase = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
                buf += 4;
            } else {
                throw std::runtime_error("loadElf: PT_PSPREL2 invalid size flag");
            }
            continue;
        }

        const int addrSeg = seg;
        if (!segmentIsLoaded(segmentVAddr, addrSeg)) {
            continue; // relocating against a segment we didn't load -- skip it, not fatal
        }
        const int relocateTo = static_cast<int>(segmentVAddr[addrSeg]);

        int relOffset;
        if ((flag & 0x06) == 0x00) {
            relOffset = cmd;
            if (cmd & 0x8000) {
                relOffset |= static_cast<int>(0xFFFF0000u);
                relOffset >>= (typeBits + segBits + flagBits);
                relOffset |= static_cast<int>(0xFFFF0000u);
            } else {
                relOffset >>= (typeBits + segBits + flagBits);
            }
            relBase += relOffset;
        } else if ((flag & 0x06) == 0x02) {
            relOffset = cmd;
            if (cmd & 0x8000) {
                relOffset |= static_cast<int>(0xFFFF0000u);
            }
            relOffset >>= (typeBits + segBits + flagBits);
            haveBytes(2);
            relOffset = (relOffset << 16) | buf[0] | (buf[1] << 8);
            buf += 2;
            relBase += relOffset;
        } else if ((flag & 0x06) == 0x04) {
            haveBytes(4);
            relBase = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
            buf += 4;
        } else {
            throw std::runtime_error("loadElf: PT_PSPREL2 invalid relocation size flag");
        }

        if (!segmentIsLoaded(segmentVAddr, offSeg)) {
            continue;
        }
        const uint32_t relocAddr = static_cast<uint32_t>(relBase) + segmentVAddr[offSeg];

        if ((flag & 0x38) == 0x00) {
            lo16 = 0;
        } else if ((flag & 0x38) == 0x08) {
            if (lastType != 0x04) {
                lo16 = 0;
            }
        } else if ((flag & 0x38) == 0x10) {
            haveBytes(2);
            lo16 = buf[0] | (buf[1] << 8);
            if (lo16 & 0x8000) {
                lo16 |= static_cast<int>(0xFFFF0000u);
            }
            buf += 2;
        } else {
            throw std::runtime_error("loadElf: PT_PSPREL2 invalid lo16 type");
        }

        uint32_t op = bus.read32(relocAddr);

        switch (type) {
            case 0:
                continue;
            case 2: // R_MIPS_32
                op = static_cast<uint32_t>(static_cast<int>(op) + relocateTo);
                break;
            case 3: // R_MIPS_26
            case 6: // R_MIPS_J26
            case 7: // R_MIPS_JAL26
                op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (static_cast<uint32_t>(relocateTo) >> 2)) & 0x03FFFFFFu);
                if (type == 6) {
                    op = (op & ~0xFC000000u) | 0x08000000u;
                } else if (type == 7) {
                    op = (op & ~0xFC000000u) | 0x0C000000u;
                }
                break;
            case 4: { // R_MIPS_HI16
                uint32_t addr = static_cast<uint32_t>((static_cast<int>(op) << 16) + lo16 + relocateTo);
                if (addr & 0x8000) {
                    addr += 0x00010000u;
                }
                op = (op & 0xFFFF0000u) | (addr >> 16);
                break;
            }
            case 1:
            case 5: // R_MIPS_LO16
                op = (op & 0xFFFF0000u) | (((op & 0xFFFFu) + static_cast<uint32_t>(relocateTo)) & 0xFFFFu);
                break;
            default:
                throw std::runtime_error("loadElf: PT_PSPREL2 unexpected relocation type " + std::to_string(type));
        }

        bus.write32(relocAddr, op);
    }
}

// Ported from PPSSPP's ElfReader::LoadRelocations (Core/ELF/ElfReader.cpp).
// The classic, pre-PT_PSPREL2 real PSP relocation format: a plain array of
// standard 8-byte Elf32_Rel entries (r_offset, r_info), found via a real
// SHT_PSPREL section (sh_info names which section it targets) rather than a
// PT_PSPREL2 program-header segment. PSP's r_info packs three fields that
// don't matter for its exec-privilege class of file: instead of a symbol
// index it holds a "readwrite" and "relative" *segment* index (matching
// PT_PSPREL2's segment-index approach, just laid out differently) --
// PPSSPP: `type = info & 0xf`, `readwrite = (info>>8) & 0xff`,
// `relative = (info>>16) & 0xff`. R_MIPS_HI16 is intentionally two-pass:
// every relocation's original (pre-relocation) word is read first, because a
// HI16 needs the *original* addend from its paired LO16 even if that LO16
// entry sorts later in the same table.
struct Elf32Rel {
    uint32_t offset;
    uint32_t info;
};

constexpr uint32_t kRMipsNone = 0;
constexpr uint32_t kRMips16 = 1;
constexpr uint32_t kRMips32 = 2;
constexpr uint32_t kRMips26 = 4;
constexpr uint32_t kRMipsHi16 = 5;
constexpr uint32_t kRMipsLo16 = 6;
constexpr uint32_t kRMipsGprel16 = 7;

void applyShtPspRel(const std::vector<Elf32Rel>& rels, const std::vector<uint32_t>& segmentVAddr, Bus& bus) {
    const std::size_t numRelocs = rels.size();
    std::vector<uint32_t> relocOps(numRelocs, 0);

    // Pass 1: snapshot every relocation target's original word, unmodified.
    for (std::size_t r = 0; r < numRelocs; ++r) {
        const int readwrite = static_cast<int>((rels[r].info >> 8) & 0xffu);
        if (!segmentIsLoaded(segmentVAddr, readwrite)) {
            continue;
        }
        const uint32_t addr = rels[r].offset + segmentVAddr[readwrite];
        relocOps[r] = bus.read32(addr & ~3u);
    }

    // Pass 2: apply.
    for (std::size_t r = 0; r < numRelocs; ++r) {
        const uint32_t info = rels[r].info;
        const uint32_t type = info & 0xfu;
        const int readwrite = static_cast<int>((info >> 8) & 0xffu);
        const int relative = static_cast<int>((info >> 16) & 0xffu);
        if (!segmentIsLoaded(segmentVAddr, readwrite)) {
            continue;
        }
        const uint32_t addr = rels[r].offset + segmentVAddr[readwrite];
        uint32_t op = relocOps[r];
        const uint32_t relocateTo = segmentIsLoaded(segmentVAddr, relative) ? segmentVAddr[relative] : 0;

        switch (type) {
        case kRMips32:
            op += relocateTo;
            break;
        case kRMips26:
            op = (op & 0xFC000000u) | (((op & 0x03FFFFFFu) + (relocateTo >> 2)) & 0x03FFFFFFu);
            break;
        case kRMipsHi16: {
            const uint32_t cur16 = (op & 0xFFFFu) << 16;
            bool found = false;
            uint16_t hi = 0;
            for (std::size_t t = r + 1; t < numRelocs && !found; ++t) {
                const uint32_t tType = rels[t].info & 0xfu;
                if (tType == kRMipsHi16) {
                    continue;
                }
                const int tReadwrite = static_cast<int>((rels[t].info >> 8) & 0xffu);
                if (!segmentIsLoaded(segmentVAddr, tReadwrite)) {
                    continue;
                }
                if (tType != kRMipsLo16 && tType != kRMips16) {
                    continue;
                }
                const int16_t lo = static_cast<int16_t>(relocOps[t] & 0xFFFFu);
                uint32_t cur = cur16 + static_cast<uint32_t>(static_cast<int32_t>(lo)) + relocateTo;
                const uint16_t newLo = static_cast<uint16_t>(cur & 0xFFFFu);
                uint32_t naddr = cur - newLo;
                hi = static_cast<uint16_t>(naddr >> 16);
                found = true;
            }
            if (found) {
                op = (op & 0xFFFF0000u) | hi;
            }
            // Leave the instruction alone if no LO16 pair was found, same as
            // PPSSPP -- writing hi's initial 0 would be a worse guess.
            if (!found) {
                continue;
            }
            break;
        }
        case kRMipsLo16: {
            uint32_t cur = op & 0xFFFFu;
            cur += relocateTo;
            cur &= 0xFFFFu;
            op = (op & 0xFFFF0000u) | cur;
            break;
        }
        case kRMipsGprel16:
            // Not a real address fixup -- ignored, same as PPSSPP.
            continue;
        case kRMips16:
            op = (op & 0xFFFF0000u) | ((op + relocateTo) & 0xFFFFu);
            break;
        case kRMipsNone:
            continue;
        default:
            throw std::runtime_error("loadElf: unexpected SHT_PSPREL relocation type " + std::to_string(type));
        }

        bus.write32(addr, op);
    }
}

} // namespace

ElfLoadResult loadElf(const std::vector<uint8_t>& elf, Bus& bus, uint32_t baseAddress) {
    constexpr std::size_t kEhdrSize = 0x34;
    constexpr std::size_t kPhentSize = 0x20;

    if (elf.size() < kEhdrSize) {
        throw std::runtime_error("loadElf: too small for an ELF header");
    }
    if (!(elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F')) {
        throw std::runtime_error("loadElf: missing ELF magic");
    }
    if (elf[4] != 1) {
        throw std::runtime_error("loadElf: not a 32-bit ELF");
    }
    if (elf[5] != 1) {
        throw std::runtime_error("loadElf: not little-endian");
    }

    const uint16_t eType = readU16LE(elf.data(), 0x10);
    const uint16_t eMachine = readU16LE(elf.data(), 0x12);
    const uint32_t eEntry = readU32LE(elf.data(), 0x18);
    const uint32_t ePhoff = readU32LE(elf.data(), 0x1C);
    const uint32_t eShoff = readU32LE(elf.data(), 0x20);
    const uint16_t ePhentsize = readU16LE(elf.data(), 0x2A);
    const uint16_t ePhnum = readU16LE(elf.data(), 0x2C);
    const uint16_t eShentsize = readU16LE(elf.data(), 0x2E);
    const uint16_t eShnum = readU16LE(elf.data(), 0x30);

    if (eMachine != kEmMips) {
        throw std::runtime_error("loadElf: not a MIPS ELF");
    }
    if (eType != kEtExec && eType != kEtPspPrx) {
        throw std::runtime_error("loadElf: unsupported e_type 0x" + std::to_string(eType));
    }
    const bool relocatable = (eType != kEtExec);

    constexpr std::size_t kShentSize = 0x28;
    if (eShnum > 0 && eShentsize != kShentSize) {
        throw std::runtime_error("loadElf: unexpected section header entry size");
    }
    if (static_cast<std::size_t>(eShoff) + static_cast<std::size_t>(eShnum) * eShentsize > elf.size()) {
        throw std::runtime_error("loadElf: section header table runs past the end of the file");
    }
    if (ePhnum > 0 && ePhentsize != kPhentSize) {
        throw std::runtime_error("loadElf: unexpected program header entry size");
    }
    if (static_cast<std::size_t>(ePhoff) + static_cast<std::size_t>(ePhnum) * ePhentsize > elf.size()) {
        throw std::runtime_error("loadElf: program header table runs past the end of the file");
    }

    std::vector<uint32_t> segmentVAddr(ePhnum, kSegmentNotLoaded);
    uint32_t totalStart = 0xFFFFFFFFu;
    uint32_t totalEnd = 0;
    int loadedSegments = 0;

    // Pass 1: copy PT_LOAD segment bytes into the bus, zero-filling bss.
    for (uint16_t i = 0; i < ePhnum; ++i) {
        const std::size_t phBase = static_cast<std::size_t>(ePhoff) + static_cast<std::size_t>(i) * ePhentsize;
        if (readU32LE(elf.data(), phBase + 0x00) != kPtLoad) {
            continue;
        }

        const uint32_t pOffset = readU32LE(elf.data(), phBase + 0x04);
        const uint32_t pVaddr = readU32LE(elf.data(), phBase + 0x08);
        const uint32_t pFilesz = readU32LE(elf.data(), phBase + 0x10);
        const uint32_t pMemsz = readU32LE(elf.data(), phBase + 0x14);

        if (pFilesz > pMemsz) {
            throw std::runtime_error("loadElf: segment " + std::to_string(i) + " has filesz > memsz");
        }
        if (static_cast<std::size_t>(pOffset) + pFilesz > elf.size()) {
            throw std::runtime_error("loadElf: segment " + std::to_string(i) + " runs past the end of the file");
        }

        const uint32_t writeAddr = baseAddress + pVaddr;
        segmentVAddr[i] = writeAddr;
        ++loadedSegments;

        for (uint32_t b = 0; b < pFilesz; ++b) {
            bus.write8(writeAddr + b, elf[pOffset + b]);
        }
        for (uint32_t b = pFilesz; b < pMemsz; ++b) {
            bus.write8(writeAddr + b, 0);
        }

        if (pVaddr < totalStart) {
            totalStart = pVaddr;
        }
        if (pVaddr + pMemsz > totalEnd) {
            totalEnd = pVaddr + pMemsz;
        }
    }

    if (loadedSegments == 0) {
        throw std::runtime_error("loadElf: no PT_LOAD segments");
    }

    // Pass 2: apply relocations (relocatable modules only). Real PSP modules
    // use one of two mutually exclusive conventions (confirmed against
    // PPSSPP's ElfReader::LoadInto, which gates on the same condition):
    // modules with real section headers (eShnum != 0) carry their
    // relocations in SHT_PSPREL *sections*; headerless modules (eShnum == 0,
    // every kernel module in this project's 661.PBP test firmware) carry
    // them in a PT_PSPREL2 *segment* instead.
    if (relocatable) {
        bool appliedAny = false;
        if (eShnum != 0) {
            for (uint16_t i = 0; i < eShnum; ++i) {
                const std::size_t shBase = static_cast<std::size_t>(eShoff) + static_cast<std::size_t>(i) * eShentsize;
                const uint32_t shType = readU32LE(elf.data(), shBase + 0x04);
                if (shType != kPtPspRel1) { // SHT_PSPREL shares PT_PSPREL1's numeric value
                    continue;
                }
                const uint32_t shOffset = readU32LE(elf.data(), shBase + 0x10);
                const uint32_t shSize = readU32LE(elf.data(), shBase + 0x14);
                constexpr uint32_t kElf32RelSize = 8;
                const uint32_t numRelocs = shSize / kElf32RelSize;
                if (static_cast<std::size_t>(shOffset) + static_cast<std::size_t>(numRelocs) * kElf32RelSize > elf.size()) {
                    throw std::runtime_error("loadElf: SHT_PSPREL section runs past the end of the file");
                }
                std::vector<Elf32Rel> rels(numRelocs);
                for (uint32_t r = 0; r < numRelocs; ++r) {
                    const std::size_t relBase = shOffset + static_cast<std::size_t>(r) * kElf32RelSize;
                    rels[r].offset = readU32LE(elf.data(), relBase + 0x00);
                    rels[r].info = readU32LE(elf.data(), relBase + 0x04);
                }
                applyShtPspRel(rels, segmentVAddr, bus);
                appliedAny = true;
            }
        } else {
            const bool useRebootTypeRemap = isRealRebootRelocationModule(elf);
            for (uint16_t i = 0; i < ePhnum; ++i) {
                const std::size_t phBase = static_cast<std::size_t>(ePhoff) + static_cast<std::size_t>(i) * ePhentsize;
                const uint32_t pType = readU32LE(elf.data(), phBase + 0x00);
                if (pType == kPtPspRel1) {
                    throw std::runtime_error("loadElf: PT_PSPREL1 (section-style segment relocations) is not implemented");
                }
                if (pType == kPtPspRel2) {
                    const uint32_t pOffset = readU32LE(elf.data(), phBase + 0x04);
                    const uint32_t pFilesz = readU32LE(elf.data(), phBase + 0x10);
                    if (static_cast<std::size_t>(pOffset) + pFilesz > elf.size()) {
                        throw std::runtime_error("loadElf: PT_PSPREL2 segment runs past the end of the file");
                    }
                    applyPspRel2(elf.data() + pOffset, pFilesz, i, segmentVAddr, bus, useRebootTypeRemap);
                    appliedAny = true;
                }
            }
        }
        if (!appliedAny) {
            throw std::runtime_error("loadElf: relocatable module has no relocation section/segment to relocate against");
        }
    }

    ElfLoadResult result;
    result.baseAddress = baseAddress;
    result.entryPoint = eEntry + (relocatable ? baseAddress : 0);
    result.totalSize = totalEnd - totalStart;
    result.loadedSegments = loadedSegments;
    return result;
}

} // namespace beta::format
