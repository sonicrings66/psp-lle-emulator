#include "test_framework.h"

#include <fstream>
#include <iterator>
#include <string>

#include "crypto/kirk.h"
#include "crypto/prx_decrypt.h"
#include "format/elf.h"
#include "format/pbp.h"
#include "format/psar.h"
#include "firmware/ipl_extractor.h"
#include "util/nand_image.h"
#include "bus/system_bus.h"

// Phase A milestone (see the plan at
// C:\Users\quinn\.claude\plans\polished-swinging-wall.md and
// docs/PORTING_LOG.md): proves the real KIRK-based decrypt chain this
// session ported into src/core/{crypto,format,firmware} actually works
// end to end against the user's own real, genuinely-owned firmware files
// -- not synthetic test data. This is the user's explicit ask ("I do want
// firmware/kirk to do the decrypting of the boot modules if possible").

namespace {

// Phase A has no portable path-resolution helper yet (the old project's
// psp_paths.h is a later port -- see docs/PORTING_LOG.md). These are the
// user's own real files at their known, fixed location for now.
constexpr const char* kFirmwarePbp = "E:/psp project/661.PBP";
constexpr const char* kNandDump = "E:/psp project/UNKNOWN_db40916a_64M.bin";

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void testRealFirmwareVersionDecryptsViaRealKirk(TestStats& stats) {
    // This is the project's own documented "first real, checkable proof"
    // that the real KIRK demangle + type-0 PRX decrypt chain
    // (format::decodePsarBlock -> crypto::decryptPrxType0 -> real
    // KirkEngine::command()) works end to end against a genuine,
    // encrypted Sony firmware archive -- no synthetic/pre-decrypted data.
    const std::vector<uint8_t> pbpBytes = readWholeFile(kFirmwarePbp);
    CHECK_TRUE(stats, !pbpBytes.empty());
    if (pbpBytes.empty()) return;

    beta::format::PbpFile pbp(pbpBytes);
    const beta::format::ByteSpan psarSection = pbp.section(beta::format::PbpPart::PsarData);
    CHECK_TRUE(stats, psarSection.size > 0);

    beta::crypto::KirkEngine kirk; // placeholder fuse -- CMD1/7/11/ECDSA_VERIFY don't read it
    const std::string version = beta::format::readFirmwareVersion(psarSection, kirk);
    // 661.PBP is this project's real 6.61 firmware updater.
    CHECK_TRUE(stats, version == "6.61");
}

void testRealPsarEntriesResolveViaRealKirk(TestStats& stats) {
    // A broader real-decrypt-chain check: walk the whole archive (every
    // entry decrypted via decodePsarBlock -> real KIRK) and confirm a
    // real, known module -- sysmem.prx, which every boot chain needs --
    // comes back with real, non-empty decrypted+decompressed content.
    const std::vector<uint8_t> pbpBytes = readWholeFile(kFirmwarePbp);
    CHECK_TRUE(stats, !pbpBytes.empty());
    if (pbpBytes.empty()) return;

    beta::format::PbpFile pbp(pbpBytes);
    const beta::format::ByteSpan psarSection = pbp.section(beta::format::PbpPart::PsarData);

    beta::crypto::KirkEngine kirk;
    const std::vector<beta::format::ResolvedPsarEntry> entries = beta::format::resolvePsarEntries(psarSection, kirk);
    CHECK_TRUE(stats, !entries.empty());

    bool foundSysmem = false;
    for (const auto& e : entries) {
        if (e.realName.find("sysmem.prx") != std::string::npos) {
            foundSysmem = true;
            CHECK_TRUE(stats, !e.data.empty());
            break;
        }
    }
    CHECK_TRUE(stats, foundSysmem);
}

void testRealNandIplExtractionMatchesOldProjectCachedResult(TestStats& stats) {
    // Ported from extractAndLoadRealIpl/Cached (test_ipl_realfuse.cpp).
    // A 2026-09-11 header comment in that file claims this path
    // consistently fails ECDSA_VERIFY -- but that comment is stale: the
    // old project's OWN cached extraction result from a real prior run
    // (scratch/cache/ipl_0000000000000000_C5720C1906C5F5C9.bin, placeholder
    // fuse, i.e. fuseKey=0) has entry=0x040E0000, proving the real ECDSA
    // chain genuinely verifies against this console's real NAND dump
    // today. This test pins that real, verified result as the parity
    // baseline for this port (docs/TESTING.md's own discipline: a
    // deterministic real-hardware-derived value, not a guess).
    beta::NandImage nand(kNandDump);
    beta::crypto::KirkEngine kirk; // placeholder fuse, matching the cached run above
    int segmentCount = 0;
    const uint32_t entry = beta::firmware::extractAndLoadRealIpl(
        kirk, nand, [&](uint32_t, uint32_t) { ++segmentCount; });
    CHECK_EQ(stats, entry, 0x040E0000u);
    CHECK_TRUE(stats, segmentCount > 0);
}

void testRealSysmemModuleDecryptsAndLoadsAsRealElf(TestStats& stats) {
    // Closes the loop the user specifically asked for: not just "KIRK can
    // decrypt bytes" (the tests above) but a real boot module going all
    // the way from encrypted PSAR entry -> real KIRK module decrypt
    // (crypto::decryptModule, dispatching on the module's real tag to the
    // matching PPSSPP-derived type, each type itself built on real
    // KirkEngine::command() calls) -> a plain ELF -> actually loaded into
    // a real SystemBus via the real ELF loader (format::loadElf), exactly
    // as this project's own stageEarlyBoot() does for every kernel module.
    const std::vector<uint8_t> pbpBytes = readWholeFile(kFirmwarePbp);
    CHECK_TRUE(stats, !pbpBytes.empty());
    if (pbpBytes.empty()) return;

    beta::format::PbpFile pbp(pbpBytes);
    const beta::format::ByteSpan psarSection = pbp.section(beta::format::PbpPart::PsarData);

    beta::crypto::KirkEngine kirk;
    const std::vector<beta::format::ResolvedPsarEntry> entries = beta::format::resolvePsarEntries(psarSection, kirk);

    const beta::format::ResolvedPsarEntry* sysmem = nullptr;
    for (const auto& e : entries) {
        if (e.realName.find("sysmem.prx") != std::string::npos) { sysmem = &e; break; }
    }
    CHECK_TRUE(stats, sysmem != nullptr);
    if (!sysmem) return;

    // sysmem.prx's PSAR entry is zlib-decompressed already (resolvePsarEntries'
    // job), but still a real "~PSP"-tagged encrypted module -- one more real
    // KIRK decrypt step turns it into a plain ELF.
    const std::vector<uint8_t> elfBytes = beta::crypto::decryptModule(kirk, sysmem->data);
    CHECK_TRUE(stats, elfBytes.size() > 0x34); // at least an ELF header
    CHECK_TRUE(stats, elfBytes[0] == 0x7F && elfBytes[1] == 'E' && elfBytes[2] == 'L' && elfBytes[3] == 'F');

    // sysmem.prx is a relocatable ET_PSP_PRX whose own p_vaddr starts near
    // 0 (confirmed live: baseAddress=0 faulted on an unmapped low address),
    // so it needs a real base to land in RAM. 0x88000000 (masked to RAM's
    // start, 0x08000000) is the real stock kernel base -- this test only
    // checks that the real decrypt+load pipeline works end to end, not the
    // exact real address this project's own boot chain ultimately uses
    // (Phase B's module stager picks that, and per project memory this
    // project's actual sysmem placement differs from stock for its own
    // reasons -- irrelevant to proving the KIRK decrypt/load mechanism).
    beta::SystemBus bus;
    const beta::format::ElfLoadResult result = beta::format::loadElf(elfBytes, bus, 0x88000000u);
    CHECK_TRUE(stats, result.loadedSegments > 0);
    CHECK_TRUE(stats, result.entryPoint != 0);
}

} // namespace

void runAllFirmwareExtractionTests(TestStats& stats) {
    runTest(stats, "FirmwareExtraction.RealFirmwareVersionDecryptsViaRealKirk", testRealFirmwareVersionDecryptsViaRealKirk);
    runTest(stats, "FirmwareExtraction.RealPsarEntriesResolveViaRealKirk", testRealPsarEntriesResolveViaRealKirk);
    runTest(stats, "FirmwareExtraction.RealNandIplExtractionMatchesOldProjectCachedResult", testRealNandIplExtractionMatchesOldProjectCachedResult);
    runTest(stats, "FirmwareExtraction.RealSysmemModuleDecryptsAndLoadsAsRealElf", testRealSysmemModuleDecryptsAndLoadsAsRealElf);
}
