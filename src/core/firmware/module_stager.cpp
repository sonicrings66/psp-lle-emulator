#include "module_stager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <set>
#include <stdexcept>

#include "../crypto/prx_decrypt.h"
#include "../format/elf.h"
#include "../memory_map.h"
#include "pspbtcnf_parser.h"

namespace beta::firmware {

namespace {

// Real boot-mode bits in a pspbtcnf record's marker field -- NOW FULLY
// TRACED (2026-11-03, see docs/PORTING_LOG.md), closing the earlier
// "empirical, not independently confirmed" gap. uofw's reboot/main.c
// sub_71A8 (direct decompile) walks a SECOND, outer table this project's
// parser didn't previously read at all: a per-apiType "section directory"
// at header+0x10 (count at header+0x14), one 32-byte entry per apiType the
// console supports, each holding {recordCount:u16@0, startIndex:u16@2,
// fp:u32@4, apiType:u32@8, cfgStrOff:u32@12}. `apiType` is EXACT-matched
// (not bitwise) against sub_52C4's real string table (game=1, vsh=2,
// update=3, pops=4, licensegame=5, app=6, umdemu=7, mlnapp=8) -- the
// matched section's own `fp` field is what actually gets ANDed against
// each record's marker (`(fp & marker) & 0xFFF != 0` => include), not a
// fixed global constant. Dumped this section directory LIVE against this
// project's own real, decrypted flash0:/kd/pspbtcnf_03g.bin (the PSAR's
// exact-generation entry, tag 0x4C9496F0): it holds exactly 7 sections
// (every real apiType except licensegame=5, which this console's firmware
// doesn't ship a section for) --
//   vsh(2): fp=0x01   game(1): fp=0x02   update(3): fp=0x04
//   pops(4): fp=0x08  app(6): fp=0x20    umdemu(7): fp=0x40  mlnapp(8): fp=0x80
// -- confirming the project's prior empirical guess (vsh=0x01, game=0x02)
// exactly, now from the real per-apiType fp field itself rather than from
// cross-checking the old project's hardcoded module-name lists. All 7
// sections share the SAME recordTableOff base with startIndex=0 (just a
// different recordCount each), so this project's existing flat scan over
// the whole shared record table, filtered by a single fixed wantBit, is
// behaviorally equivalent to the real per-section walk for vsh/game (the
// only two boot modes beta drives) and needed no code change -- only this
// comment, which previously undersold how solid these two constants are.
constexpr uint32_t kWantBitVsh = 0x01u;
constexpr uint32_t kWantBitGame = 0x02u;

// CORRECTED (2026-11-03, see docs/PORTING_LOG.md): the PSAR actually
// bundles a per-generation pspbtcnf.bin for EVERY real console generation
// 6.61 shipped on (02g/03g/04g/07g/09g/11g, plus a generic unsuffixed
// copy) -- confirmed live, "flash0:/kd/pspbtcnf_03g.bin" IS a real PSAR
// entry, tag 0x4C9496F0 (a key this project already has), and decrypts to
// real, valid boot-config data (header magic 0x0F803001, 80 real
// records) whose cleartext size fields match this console's own
// undecryptable flash0:/kd/pspbtcnf_03g.bin exactly. So this console's
// real pspbtcnf data IS available via the PSAR, decrypted -- the earlier
// assumption that only a mismatched "_11g" copy existed (and that per-
// module records therefore needed generation-normalizing to match) was
// wrong for the pspbtcnf FILE SELECTION itself; see selectPspbtcnfEntry()
// below, which now prefers the exact "_03g" match directly.
//
// stripGenerationSuffix() stays real and necessary for a DIFFERENT job:
// individual MODULE files inside the PSAR (e.g. "memlmd_11g.prx") are
// still suffixed for the update's own "_11g" packaging generation
// regardless of which pspbtcnf copy named them, so a pspbtcnf_03g.bin
// record name (e.g. "/kd/loadexec_03g.prx" -- the exact-match file's own
// records are ALSO generation-suffixed, per real Sony convention) still
// needs normalizing before it can be matched against PSAR entries.
std::string stripGenerationSuffix(const std::string& path) {
    constexpr const char* ext = ".prx";
    const std::size_t extLen = std::strlen(ext);
    if (path.size() < extLen || path.compare(path.size() - extLen, extLen, ext) != 0) return path;
    const std::string stem = path.substr(0, path.size() - extLen);
    if (stem.empty() || stem.back() != 'g') return path;
    std::size_t digitsEnd = stem.size() - 1; // index just past the last digit, i.e. the 'g'
    std::size_t j = digitsEnd;
    while (j > 0 && std::isdigit(static_cast<unsigned char>(stem[j - 1]))) --j;
    if (j == digitsEnd || j == 0 || stem[j - 1] != '_') return path; // no digits, or no leading '_'
    return stem.substr(0, j - 1) + ext;
}

// True if `entryRealName` (a real PSAR entry path, e.g.
// "flash0:/kd/memlmd_03g.prx") matches `recordName` (a real pspbtcnf
// record name, e.g. "/kd/memlmd_11g.prx") once both are generation-
// normalized -- a tail comparison, same convention the old project's own
// "extraModules" tail-match used (entry.realName ends with "/" + name).
bool matchesPspbtcnfRecord(const std::string& entryRealName, const std::string& normalizedRecordName) {
    const std::string normEntry = stripGenerationSuffix(entryRealName);
    if (normEntry.size() < normalizedRecordName.size()) return false;
    return normEntry.compare(normEntry.size() - normalizedRecordName.size(), normalizedRecordName.size(),
                              normalizedRecordName) == 0;
}

const format::ResolvedPsarEntry* findByNormalizedRecordName(const std::vector<format::ResolvedPsarEntry>& entries,
                                                              const std::string& normalizedRecordName) {
    for (const auto& e : entries) {
        if (matchesPspbtcnfRecord(e.realName, normalizedRecordName)) return &e;
    }
    return nullptr;
}

const format::ResolvedPsarEntry* findBySubstring(const std::vector<format::ResolvedPsarEntry>& entries,
                                                   const char* wantedSubstring) {
    for (const auto& e : entries) {
        if (e.realName.find(wantedSubstring) != std::string::npos) return &e;
    }
    return nullptr;
}

uint32_t rd32(const std::vector<uint8_t>& d, std::size_t off) {
    if (off + 4 > d.size()) return 0u;
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

// Minimal, single-purpose export-table lookup: walks ONE module's real
// .lib.ent table (same layout as format::module_link.cpp's walkExports,
// but a read-only lookup for a handful of wanted NIDs, not a registry
// builder) and returns the real exported address for `wantedNid`, or 0 if
// not found. Deliberately not a diagnostic/scanning tool (no printf, no
// hardcoded candidate-address lists) -- the old project re-walked loadcore's
// whole export table with heavy unconditional diagnostics for this; this is
// the same real data access with the investigation scaffolding removed.
uint32_t findExportAddress(CpuBus& bus, uint32_t moduleInfoAddr, uint32_t wantedNid) {
    const uint32_t libent = bus.read32(moduleInfoAddr + 0x24u);
    const uint32_t libentend = bus.read32(moduleInfoAddr + 0x28u);
    uint32_t pos = libent;
    while (pos < libentend) {
        const uint8_t sizeWords = bus.read8(pos + 0x08u);
        if (sizeWords == 0) { pos += 4; continue; }
        const uint8_t vcount = bus.read8(pos + 0x09u);
        const uint16_t fcount = bus.read16(pos + 0x0Au);
        const uint32_t resident = bus.read32(pos + 0x0Cu);
        for (uint16_t j = 0; j < fcount; ++j) {
            const uint32_t nid = bus.read32(resident + j * 4u);
            if (nid == wantedNid) {
                return bus.read32(resident + (static_cast<uint32_t>(fcount) + vcount) * 4u + j * 4u);
            }
        }
        pos += static_cast<uint32_t>(sizeWords) * 4u;
    }
    return 0u;
}

// One of the small, fixed real module names kept at their own real fixed
// staging address (unchanged from the old project -- a real memory-
// placement concern the pspbtcnf-driven redesign doesn't touch). Keyed by
// GENERATION-NORMALIZED name (e.g. "memlmd.prx", not "memlmd_03g.prx").
// loadexec/lowio's real addresses depend on gameBoot -- a game boot (a
// restarted process carrying a launch request) stages those above
// 0x88800000, since init.prx's own real code clears 0x88600000-0x88800000
// (the area reboot.bin occupies) before loading module #9 onward on that
// path, which would wipe anything staged below.
struct FixedSlot {
    const char* normalizedName;
    uint32_t bufAddr;
    uint32_t bufSize;
};

std::array<FixedSlot, 11> fixedSlots(bool gameBoot) {
    return {{
        {"init.prx", 0x88600000u, 0x00080000u},         // 512KB
        {"exceptionman.prx", 0x88680000u, 0x00020000u},  // 128KB
        {"interruptman.prx", 0x88732000u, 0x00020000u},  // 128KB (the staged copy -- see earlyBootModules for the loaded+linked one)
        {"threadman.prx", 0x886A0000u, 0x00028000u},     // 160KB
        {"dmacman.prx", 0x886C8000u, 0x00008000u},       // 32KB
        {"systimer.prx", 0x886D0000u, 0x00004000u},      // 16KB
        {"iofilemgr.prx", 0x886D4000u, 0x0000A000u},     // 40KB
        {"memlmd.prx", 0x886DE000u, 0x00004000u},        // 16KB
        {"modulemgr.prx", 0x886E2000u, 0x00010000u},     // 64KB
        {"loadexec.prx", gameBoot ? 0x88800000u : 0x886F2000u, 0x00020000u}, // 128KB
        {"lowio.prx", gameBoot ? 0x88810000u : 0x88712000u, 0x00020000u},    // 128KB
    }};
}

const FixedSlot* findFixedSlot(const std::array<FixedSlot, 11>& slots, const std::string& normalizedName) {
    for (const auto& s : slots) {
        if (normalizedName.size() >= std::strlen(s.normalizedName) &&
            normalizedName.compare(normalizedName.size() - std::strlen(s.normalizedName), std::strlen(s.normalizedName), s.normalizedName) == 0) {
            return &s;
        }
    }
    return nullptr;
}

constexpr uint32_t kRebootBinBufAddr = 0x88600000u; // same real address as init.prx's slot -- safe real reuse, init has long finished by the time reboot.bin's own run matters

} // namespace

StagedModules stageEarlyBoot(const std::vector<format::ResolvedPsarEntry>& entries, CpuBus& bus,
                              crypto::KirkEngine& kirk, bool gameBoot,
                              const format::Flash0Fs* nandFs) {
    StagedModules result;
    const uint32_t wantBit = gameBoot ? kWantBitGame : kWantBitVsh;

    // This console's own real flash0: files, keyed by their real path
    // ("flash0:/kd/foo.prx"), preferred over the PSAR's generic copy for
    // every tier-2 module below (see module_stager.h's own doc comment).
    // Built once, not per-module -- Flash0Fs::allFiles() walks the whole
    // real volume.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> nandFiles;
    std::map<std::string, const std::vector<uint8_t>*> nandByPath;
    if (nandFs && nandFs->valid()) {
        nandFiles = nandFs->allFiles();
        // Key by the generation-NORMALIZED path (stripGenerationSuffix),
        // not the real file's own raw path -- confirmed this session that
        // real NAND files keep their own real generation suffix
        // ("flash0:/kd/display_03g.prx"), while the lookup below builds
        // its key from `normalizedName` (generation-stripped, since that's
        // what pspbtcnf's own record names resolve to). Without this, the
        // raw-path map NEVER matches any suffixed real NAND file, so every
        // such module silently fell through to the PSAR's generic (wrong
        // console generation, e.g. "_02g") copy instead -- found live via
        // display.prx (NAND has "display_03g.prx" only, but this driver's
        // own realName was printing "display_02g.prx", the PSAR entry).
        for (const auto& f : nandFiles) nandByPath[stripGenerationSuffix(f.first)] = &f.second;
    }

    // ---- Tier 1: sysmem, loadcore, interruptman -- loaded for real, then linked ----
    // NOT pspbtcnf-driven: real hardware can't read pspbtcnf yet either at
    // this point (nothing can read a real file before sysmem/iofilemgr
    // exist) -- this part of the old project's hardcoded approach was
    // already correct, not a shortcut, and is unchanged here.
    static constexpr const char* kEarlyBootWanted[3] = {"sysmem", "loadcore", "interruptman"};
    uint32_t nextBase = MemoryMap::kRamStart + 0x00100000u;
    nextBase = 0x80000000u | nextBase;
    constexpr uint32_t kAlign = 0x1000u;

    std::vector<format::ModuleToLink> toLinkModules;
    for (int i = 0; i < 3; ++i) {
        EarlyBootModuleResult& mod = result.earlyBootModules[i];
        mod.wantedName = kEarlyBootWanted[i];
        const format::ResolvedPsarEntry* entry = findBySubstring(entries, kEarlyBootWanted[i]);
        if (!entry) continue;

        try {
            std::vector<uint8_t> elf = crypto::decryptModule(kirk, entry->data);
            if (elf.empty()) continue;

            const uint32_t base = nextBase;
            const format::ElfLoadResult loadResult = format::loadElf(elf, bus, base);
            mod.entry = loadResult.entryPoint;
            mod.base = base;
            mod.loaded = true;

            uint32_t moduleInfoAddr = 0;
            if (format::findModuleInfo(elf, base, moduleInfoAddr)) {
                mod.moduleInfoAddr = moduleInfoAddr;
                mod.gp = bus.read32(moduleInfoAddr + 0x20u);
                mod.libstub = bus.read32(moduleInfoAddr + 0x2Cu);
                mod.libstubend = bus.read32(moduleInfoAddr + 0x30u);

                if (elf.size() >= 0x30u) {
                    const uint32_t ePhoff = elf[0x1C] | (elf[0x1D] << 8) | (elf[0x1E] << 16) | (elf[0x1F] << 24);
                    const uint16_t ePhentsize = static_cast<uint16_t>(elf[0x2A] | (elf[0x2B] << 8));
                    const uint16_t ePhnum = static_cast<uint16_t>(elf[0x2C] | (elf[0x2D] << 8));
                    for (uint16_t s = 0; s < ePhnum && mod.numSegments < 4; ++s) {
                        const std::size_t phBase = static_cast<std::size_t>(ePhoff) + static_cast<std::size_t>(s) * ePhentsize;
                        if (phBase + 0x18u > elf.size()) break;
                        const uint32_t pType = elf[phBase] | (elf[phBase + 1] << 8) | (elf[phBase + 2] << 16) | (elf[phBase + 3] << 24);
                        if (pType != 1u) continue; // PT_LOAD
                        const uint32_t pVaddr = elf[phBase + 8] | (elf[phBase + 9] << 8) | (elf[phBase + 10] << 16) | (elf[phBase + 11] << 24);
                        const uint32_t pFilesz = elf[phBase + 0x10] | (elf[phBase + 0x11] << 8) | (elf[phBase + 0x12] << 16) | (elf[phBase + 0x13] << 24);
                        const uint32_t pMemsz = elf[phBase + 0x14] | (elf[phBase + 0x15] << 8) | (elf[phBase + 0x16] << 16) | (elf[phBase + 0x17] << 24);
                        mod.segmentAddr[mod.numSegments] = base + pVaddr;
                        mod.segmentSize[mod.numSegments] = std::max(pFilesz, pMemsz);
                        ++mod.numSegments;
                    }
                }
            }

            const uint32_t span = (loadResult.totalSize + kAlign - 1) & ~(kAlign - 1);
            nextBase += span > 0 ? span : kAlign;
            toLinkModules.push_back(format::ModuleToLink{std::move(elf), base});
        } catch (const std::exception&) {
            // This module's decrypt/load/relocate failed; leave mod.loaded == false and continue.
        }
    }

    result.linkStats = format::linkModules(toLinkModules, bus);

    result.sysmemRealEntry = result.earlyBootModules[0].entry;
    result.sysmemRealGp = result.earlyBootModules[0].gp;

    if (result.earlyBootModules[1].loaded) { // loadcore
        const uint32_t loadcoreInfo = result.earlyBootModules[1].moduleInfoAddr;
        result.linkLibraryEntriesAddr = findExportAddress(bus, loadcoreInfo, 0x8EAE9534u); // sceKernelLinkLibraryEntries
        result.checkExecFileAddr = findExportAddress(bus, loadcoreInfo, 0xD3353EC4u);      // sceKernelCheckExecFile
    }

    // ---- Tier 2: every other real pspbtcnf-listed module, driven by the real record table ----
    // Prefer an EXACT match for this console's own real generation
    // ("pspbtcnf_03g.bin") -- confirmed live to exist in the PSAR as a
    // real, separately-tagged (0x4C9496F0, decryptable) entry whose
    // cleartext size fields match the console's own undecryptable
    // flash0:/kd/pspbtcnf_03g.bin exactly (see docs/PORTING_LOG.md).
    // Falls back to the first "pspbtcnf"-named entry only if a future
    // firmware/console target doesn't ship an exact-generation copy.
    std::vector<PspbtcnfRecord> records;
    {
        const format::ResolvedPsarEntry* exact = nullptr;
        const format::ResolvedPsarEntry* fallback = nullptr;
        for (const auto& e : entries) {
            if (e.realName.find("pspbtcnf") == std::string::npos) continue;
            if (!fallback) fallback = &e;
            if (e.realName.find("pspbtcnf_03g.bin") != std::string::npos) { exact = &e; break; }
        }
        const format::ResolvedPsarEntry* chosen = exact ? exact : fallback;
        if (chosen) records = parsePspbtcnf(kirk, chosen->data);
    }
    result.moduleAttributes = groupPspbtcnfByName([&] {
        std::vector<PspbtcnfRecord> normalized = records;
        for (auto& r : normalized) r.name = stripGenerationSuffix(r.name);
        return normalized;
    }());

    const uint32_t extraModulesBufBase = gameBoot ? 0x88820000u : 0x88752000u;
    uint32_t nextExtraModuleBufAddr = extraModulesBufBase;
    const std::array<FixedSlot, 11> slots = fixedSlots(gameBoot);
    std::set<std::string> stagedNormalizedNames;
    StagedModule* lastIncludedModule = nullptr; // real last-in-boot-order module gets attr/bootData below

    for (const PspbtcnfRecord& record : records) {
        const std::string normalizedName = stripGenerationSuffix(record.name);

        // Real hardware's own non-file-backed "main executable" anchor
        // slot (real Sony convention, confirmed by the old project) --
        // not a real PSAR file, nothing to stage.
        if (normalizedName.find("dummy_anchor") != std::string::npos) continue;

        // sysmem/loadcore are tier 1's job (pspbtcnf lists them too, but
        // real hardware loads them before it can even read pspbtcnf).
        if (normalizedName == "/kd/sysmem.prx" || normalizedName == "/kd/loadcore.prx") continue;

        // A module can have more than one real pspbtcnf record (distinct
        // real boot-mode masks, e.g. isofs.prx); stage it at most once,
        // at its first INCLUDED record's position in real file order.
        if (stagedNormalizedNames.count(normalizedName)) continue;

        const bool included = (record.marker & wantBit) != 0u;
        const FixedSlot* slot = findFixedSlot(slots, normalizedName);
        std::vector<StagedModule>& dest = slot ? result.handPlacedModules : result.extraModules;

        StagedModule mod;
        mod.name = normalizedName;
        mod.marker = record.marker;
        // Real uofw init.c (~line 826): `(mod->attr >> 16) & 0xFF` is the
        // real mpIdText/mpIdData partition ID sceKernelLoadModuleBuffer
        // BootInitBtcnf's own LMOption uses to actually load/relocate the
        // module. For hand-placed modules, leaving this 0 (this field's
        // otherwise-correct real default, SCE_KERNEL_UNKNOWN_PARTITION per
        // uofw sysmem_user.h) was a genuine, fixable bug confirmed by the
        // old project's own documented finding and fix -- those modules'
        // own real call site apparently needs an explicit partition ID.
        // extraModules: tried copying the old project's exact approach
        // (real marker, bits 1-2 stripped, HIGH 16 bits -- signcheck|
        // partition-id per the old project's own notes -- left intact) and
        // it REGRESSED ge.prx straight back to SCE_ERROR_KERNEL_
        // PARTITION_MISMATCH (0x80020132), confirmed live: ge.prx's own
        // real marker (0x800100EF) encodes partition id 1 in its high
        // bits (`(0x800100EF>>16)&0xFF == 1`) same as the old flat
        // hardcoded guess, so it hits the exact same explicit
        // `_CheckXXXModulePartition` branch this session already found
        // broken for extraModules. Keep the LOW 16 bits (the real
        // mode-mask -- InitThreadEntry's own `curMod`/backup-area logic
        // reads bit0 of this, uofw init.c:742) since those are genuine
        // real per-module data with no known problem, but zero the HIGH
        // 16 bits (partition id) so extraModules still get the real
        // _PartitionCheck default-from-isKernelMod behavior instead of
        // the broken explicit check -- bits 1-2 (0x6) are still stripped
        // (program-module/is-a-filename aliasing, same as the old
        // project's own reasoning; `lastIncludedModule->attr |= 0x2u`
        // below sets bit1 deliberately for the real last module only).
        mod.attr = slot ? 0x00010000u : ((record.marker & 0xFFFFu) & ~0x6u);
        if (slot) mod.bufAddr = slot->bufAddr;

        if (included) {
            // Prefer this console's own real NAND-resident copy over the
            // PSAR's generic update-package copy -- but ONLY for extraModules
            // (slot==nullptr). handPlacedModules (threadman/iofilemgr/memlmd/
            // modulemgr/init/loadexec/lowio/exceptionman/dmacman/systimer)
            // already work from the PSAR; tried widening this to them too and
            // it regressed real decryption (`decryptPrx: no known type/tag
            // combination matched tag 0xE7312FEA`) -- the NAND's own copies of
            // at least some of these use a real KIRK tag this project's
            // crypto layer doesn't have a mapping for yet. extraModules
            // (lfatfs.prx, vshmain.prx, confirmed this session) are the ones
            // that actually need the real NAND content; leave the hand-placed
            // set on its already-proven-working PSAR source.
            const std::vector<uint8_t>* nandBytes = nullptr;
            std::string nandRealName;
            if (!slot && !nandByPath.empty()) {
                const std::string nandPath = "flash0:" + normalizedName;
                auto nandIt = nandByPath.find(nandPath);
                if (nandIt != nandByPath.end()) {
                    nandBytes = nandIt->second;
                    nandRealName = nandPath;
                }
            }
            const format::ResolvedPsarEntry* entry = nandBytes ? nullptr : findByNormalizedRecordName(entries, normalizedName);
            if (nandBytes || entry) {
                mod.realName = nandBytes ? nandRealName : entry->realName;
                try {
                    // Stage the RAW, still-encrypted file as-is -- real
                    // loadcore's own sceKernelCheckExecFile receives exactly
                    // this (modBuf points at the whole on-disk file,
                    // SceLoadCoreBootModuleInfo.modBuf is documented in uofw
                    // as "the buffer with the entire file content"). It
                    // parses the plaintext "~PSP" header fields directly
                    // (genuinely unencrypted, no decrypt needed to read
                    // them), then calls through g_getLengthFunc (real NID
                    // memlmd_EF73E85B, our func_0x8811c83c/pcRam8811d3d0 hook
                    // in psp_boot_driver.cpp) to decrypt the body IN PLACE --
                    // uofw's own comment: "the ~PSP header will be
                    // overwritten by the plain ELF data when the module is
                    // successfully decrypted". Pre-decrypting host-side here
                    // (an earlier approach, since superseded) produced a
                    // buffer no real call site ever asks for, and silently
                    // relied on the trampoline-restore fix turning that real
                    // decrypt call into a no-op -- confirmed live this
                    // session (raw disassembly of FUN_8811a6b8/FUN_8811A8BC
                    // against uofw's real loadelf.c source) that this call
                    // is genuinely load-bearing, not an unregistered no-op.
                    const std::vector<uint8_t>& src = nandBytes ? *nandBytes : entry->data;
                    if (src.size() < 0x150) throw std::runtime_error("module smaller than the real 0x150-byte header");
                    std::vector<uint8_t> raw = src;
                    if (!raw.empty()) {
                        if (slot) {
                            if (raw.size() <= slot->bufSize) {
                                mod.size = static_cast<uint32_t>(raw.size());
                                mod.bytes = std::move(raw);
                            }
                            // Oversize for its reserved fixed slot: leave bytes empty, same as the old project's own skip-and-continue behavior.
                        } else {
                            mod.size = static_cast<uint32_t>(raw.size());
                            mod.bufAddr = nextExtraModuleBufAddr;
                            nextExtraModuleBufAddr = (nextExtraModuleBufAddr + mod.size + 0xFFFu) & ~0xFFFu;
                            mod.bytes = std::move(raw);
                        }
                    }
                } catch (const std::exception&) {
                    // Leave mod.bytes empty.
                }
            }
        }
        // Excluded by real run-level, or no matching real PSAR entry: mod.bytes stays empty, but the
        // record is still reported (matches the old project's own "NOT FOUND -- skipping" visibility).

        const bool hasBytes = !mod.bytes.empty();
        stagedNormalizedNames.insert(normalizedName);
        dest.push_back(std::move(mod));
        if (included && hasBytes) lastIncludedModule = &dest.back();
    }

    // Real init.c's own InitThreadEntry loop identifies "the program module"
    // positionally -- always the real last entry in bootInfo->modules[] (its
    // `if (mod->attr & 2) break;` only makes sense if that's guaranteed) --
    // not by name. Every other module keeps just the real partition-ID bits
    // set above: the pspbtcnf marker is a real boot-mode section mask, not
    // this field, so there's no real marker-derived value to carry over.
    if (lastIncludedModule) {
        lastIncludedModule->attr |= 0x2u;
        lastIncludedModule->bootData = 0x200u; // SCE_EXEC_FILE_APITYPE_KERNEL_1, real cold-VSH-boot apiType (uofw init.c:673-687)
    }

    // ---- reboot.bin: embedded inside loadexec's own decrypted bytes (not itself in pspbtcnf) ----
    // Real uofw loadexec.c's runReboot() decompresses reboot.bin to this
    // same fixed real address before jumping there. Found by scanning the
    // ALREADY-DECRYPTED, ALREADY-DECOMPRESSED loadexec ELF for a second
    // "~PSP"/"PSPs" tag (a raw-bytes-first scan gives a false negative here
    // -- confirmed by the old project against this exact real file). This
    // is a host-side scan for a known embedded blob, not a model of real
    // hardware behavior, so it needs its own fully-decrypted-and-decompressed
    // copy -- NOT result.handPlacedModules's own loadexec entry, which (as
    // of the header-preserving-splice change above) holds real loadcore's
    // genuine input (intact "~PSP" header + still-KL4E-compressed body),
    // not something byte-scannable for an embedded tag.
    {
        result.rebootBin.name = "reboot.bin";
        result.rebootBin.bufAddr = kRebootBinBufAddr;
        const format::ResolvedPsarEntry* loadexecEntry = findByNormalizedRecordName(entries, "/kd/loadexec.prx");
        std::vector<uint8_t> decryptedLoadexec;
        if (loadexecEntry) {
            try {
                decryptedLoadexec = crypto::decryptModule(kirk, loadexecEntry->data);
            } catch (const std::exception&) {
                // Leave decryptedLoadexec empty.
            }
        }
        if (!decryptedLoadexec.empty()) {
            for (std::size_t off = 4; off + 4 <= decryptedLoadexec.size(); ++off) {
                if (std::memcmp(&decryptedLoadexec[off], "~PSP", 4) != 0 &&
                    std::memcmp(&decryptedLoadexec[off], "PSPs", 4) != 0) {
                    continue;
                }
                uint32_t rebootLength = 0;
                if (off + 0x30 <= decryptedLoadexec.size()) {
                    rebootLength = rd32(decryptedLoadexec, off + 0x2C);
                }
                const std::size_t embeddedEnd = (rebootLength > 0 && off + rebootLength <= decryptedLoadexec.size())
                                                     ? off + rebootLength
                                                     : decryptedLoadexec.size();
                std::vector<uint8_t> embedded(decryptedLoadexec.begin() + static_cast<std::ptrdiff_t>(off),
                                               decryptedLoadexec.begin() + static_cast<std::ptrdiff_t>(embeddedEnd));
                try {
                    result.rebootBin.bytes = crypto::decryptModule(kirk, embedded);
                    result.rebootBin.size = static_cast<uint32_t>(result.rebootBin.bytes.size());
                } catch (const std::exception&) {
                    // Leave rebootBin.bytes empty.
                }
                break;
            }
        }
    }

    result.earlyBootModulesEndMirror = nextBase;
    return result;
}

void writeExtraModulesToRam(const std::vector<StagedModule>& extraModules, CpuBus& bus) {
    for (const auto& m : extraModules) {
        if (m.bufAddr == 0 || m.bytes.empty()) continue;
        for (std::size_t i = 0; i < m.bytes.size(); ++i) {
            bus.write8(m.bufAddr + static_cast<uint32_t>(i), m.bytes[i]);
        }
    }
}

} // namespace beta::firmware
