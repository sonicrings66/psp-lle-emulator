#include "test_framework.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

#include "bus/system_bus.h"
#include "crypto/kirk.h"
#include "firmware/module_stager.h"
#include "format/pbp.h"
#include "format/psar.h"

// Real end-to-end module-staging test, same real-file pattern as
// test_firmware_extraction.cpp: proves firmware::stageEarlyBoot (ported
// from stageEarlyBoot(), tests/test_ipl_realfuse.cpp:1170-2716, then
// redesigned to be genuinely pspbtcnf-driven -- see docs/PORTING_LOG.md)
// actually stages the user's real, decrypted boot modules, with the real
// pspbtcnf.bin record table driving which modules load, in what order, and
// with what VSH/GAME inclusion -- not synthetic data, and not a hardcoded
// module list.

namespace {

constexpr const char* kFirmwarePbp = "E:/psp project/661.PBP";

std::vector<uint8_t> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<beta::format::ResolvedPsarEntry> loadRealEntries(beta::crypto::KirkEngine& kirk) {
    const std::vector<uint8_t> pbpBytes = readWholeFile(kFirmwarePbp);
    if (pbpBytes.empty()) return {};
    beta::format::PbpFile pbp(pbpBytes);
    const beta::format::ByteSpan psar = pbp.section(beta::format::PbpPart::PsarData);
    return beta::format::resolvePsarEntries(psar, kirk);
}

// Finds a staged module (in either handPlacedModules or extraModules) whose
// generation-normalized real pspbtcnf name ends with "/" + tailName (e.g.
// "threadman.prx" -> matches ".../threadman.prx"). The leading "/" is
// required, not cosmetic: without it, "mgr.prx" would also match
// "iofilemgr.prx" (both literally end in the substring "mgr.prx") -- the
// exact real ambiguity the old project's own "extraModules" tail-match
// comment explicitly calls out ("'audio.prx' must not match
// 'libaudiocodec2.prx'"), and the same convention module_stager.cpp's own
// production matchesPspbtcnfRecord() already uses internally (it matches
// full real paths like "/kd/mgr.prx", not bare filenames).
const beta::firmware::StagedModule* findStaged(const beta::firmware::StagedModules& staged, const char* tailName) {
    const std::string needle = std::string("/") + tailName;
    auto matches = [&](const beta::firmware::StagedModule& m) {
        return m.name.size() >= needle.size() && m.name.compare(m.name.size() - needle.size(), needle.size(), needle) == 0;
    };
    for (const auto& m : staged.handPlacedModules) if (matches(m)) return &m;
    for (const auto& m : staged.extraModules) if (matches(m)) return &m;
    return nullptr;
}

void testRealVshBootStagesSysmemLoadcoreAndLinksRealImports(TestStats& stats) {
    beta::crypto::KirkEngine kirk;
    const std::vector<beta::format::ResolvedPsarEntry> entries = loadRealEntries(kirk);
    CHECK_TRUE(stats, !entries.empty());
    if (entries.empty()) return;

    beta::SystemBus bus;
    const beta::firmware::StagedModules staged = beta::firmware::stageEarlyBoot(entries, bus, kirk, /*gameBoot=*/false);

    // Tier 1: sysmem, loadcore, interruptman actually loaded into `bus`.
    CHECK_EQ(stats, staged.earlyBootModules.size(), 3u);
    CHECK_TRUE(stats, staged.earlyBootModules[0].wantedName == "sysmem");
    CHECK_TRUE(stats, staged.earlyBootModules[0].loaded);
    CHECK_TRUE(stats, staged.earlyBootModules[1].wantedName == "loadcore");
    CHECK_TRUE(stats, staged.earlyBootModules[1].loaded);
    CHECK_TRUE(stats, staged.earlyBootModules[2].wantedName == "interruptman");
    CHECK_TRUE(stats, staged.earlyBootModules[2].loaded);

    CHECK_TRUE(stats, staged.earlyBootModules[0].base >= 0x88000000u && staged.earlyBootModules[0].base < 0x89000000u);
    CHECK_TRUE(stats, staged.earlyBootModules[0].entry != 0);
    CHECK_TRUE(stats, staged.earlyBootModules[0].gp != 0);
    CHECK_TRUE(stats, staged.sysmemRealEntry == staged.earlyBootModules[0].entry);
    CHECK_TRUE(stats, staged.sysmemRealGp == staged.earlyBootModules[0].gp);
    CHECK_TRUE(stats, staged.earlyBootModules[1].base > staged.earlyBootModules[0].base);

    CHECK_TRUE(stats, staged.linkStats.modulesWithInfo >= 3);
    CHECK_TRUE(stats, staged.linkStats.exportsFound > 0);
    CHECK_TRUE(stats, staged.linkStats.importsResolved > 0);
    CHECK_TRUE(stats, staged.checkExecFileAddr != 0);
    CHECK_TRUE(stats, staged.checkExecFileAddr >= staged.earlyBootModules[1].base);

    // Tier 2, now pspbtcnf-driven: the same 11 real fixed-address modules
    // are still present (the real record table lists them too, in the
    // same real order the old hardcoded list used), at the same real
    // cold-VSH-boot addresses.
    const beta::firmware::StagedModule* loadexec = findStaged(staged, "loadexec.prx");
    CHECK_TRUE(stats, loadexec != nullptr);
    if (loadexec) { CHECK_EQ(stats, loadexec->bufAddr, 0x886F2000u); CHECK_TRUE(stats, !loadexec->bytes.empty()); }

    const beta::firmware::StagedModule* lowio = findStaged(staged, "lowio.prx");
    CHECK_TRUE(stats, lowio != nullptr);
    if (lowio) { CHECK_EQ(stats, lowio->bufAddr, 0x88712000u); CHECK_TRUE(stats, !lowio->bytes.empty()); }

    const beta::firmware::StagedModule* threadman = findStaged(staged, "threadman.prx");
    CHECK_TRUE(stats, threadman != nullptr);
    if (threadman) { CHECK_EQ(stats, threadman->bufAddr, 0x886A0000u); CHECK_TRUE(stats, !threadman->bytes.empty()); }

    // reboot.bin found embedded inside loadexec's decrypted bytes.
    CHECK_TRUE(stats, !staged.rebootBin.bytes.empty());
    CHECK_EQ(stats, staged.rebootBin.bufAddr, 0x88600000u);

    // Tier 3, now driven by the real pspbtcnf run-level bit (VSH=0x01):
    // ge.prx is a shared module (real marker has both bits set), umdcache
    // is real GAME-only (excluded here), vshmain is real VSH-only (kept).
    const beta::firmware::StagedModule* ge = findStaged(staged, "ge.prx");
    CHECK_TRUE(stats, ge != nullptr);
    if (ge) {
        CHECK_TRUE(stats, !ge->bytes.empty());
        CHECK_TRUE(stats, (ge->marker & 0x01u) != 0u); // real VSH bit set
        CHECK_TRUE(stats, ge->bufAddr >= 0x88752000u && ge->bufAddr < 0x88820000u);
    }
    const beta::firmware::StagedModule* umdcache = findStaged(staged, "umdcache.prx");
    CHECK_TRUE(stats, umdcache != nullptr);
    if (umdcache) {
        CHECK_TRUE(stats, umdcache->bytes.empty()); // real marker 0x80010002 has only the GAME bit set
        CHECK_EQ(stats, umdcache->marker, 0x80010002u);
    }
    const beta::firmware::StagedModule* vshmain = findStaged(staged, "vshmain.prx");
    CHECK_TRUE(stats, vshmain != nullptr);
    if (vshmain) {
        CHECK_TRUE(stats, !vshmain->bytes.empty()); // real marker 0x80040001 has the VSH bit set
        CHECK_EQ(stats, vshmain->marker, 0x80040001u);
    }

    CHECK_TRUE(stats, !staged.moduleAttributes.empty());
}

void testRealGameBootSkipsVshOnlyModulesAndUsesGameAddresses(TestStats& stats) {
    beta::crypto::KirkEngine kirk;
    const std::vector<beta::format::ResolvedPsarEntry> entries = loadRealEntries(kirk);
    CHECK_TRUE(stats, !entries.empty());
    if (entries.empty()) return;

    beta::SystemBus bus;
    const beta::firmware::StagedModules staged = beta::firmware::stageEarlyBoot(entries, bus, kirk, /*gameBoot=*/true);

    const beta::firmware::StagedModule* loadexec = findStaged(staged, "loadexec.prx");
    if (loadexec) CHECK_EQ(stats, loadexec->bufAddr, 0x88800000u);
    const beta::firmware::StagedModule* lowio = findStaged(staged, "lowio.prx");
    if (lowio) CHECK_EQ(stats, lowio->bufAddr, 0x88810000u);

    const beta::firmware::StagedModule* umdcache = findStaged(staged, "umdcache.prx");
    CHECK_TRUE(stats, umdcache != nullptr);
    if (umdcache) { CHECK_TRUE(stats, !umdcache->bytes.empty()); CHECK_TRUE(stats, umdcache->bufAddr >= 0x88820000u); }

    const beta::firmware::StagedModule* vshmain = findStaged(staged, "vshmain.prx");
    CHECK_TRUE(stats, vshmain != nullptr);
    if (vshmain) CHECK_TRUE(stats, vshmain->bytes.empty());
}

// The strongest evidence the pspbtcnf-driven redesign is behaviorally
// correct, not just architecturally different: the real STAGED MODULE SET
// for each boot mode must match what the OLD hardcoded-list approach's own
// already-merged-and-passing verification established (the 14-name
// kVshOnly list from test_ipl_realfuse.cpp, plus the umdcache.prx special
// case) -- checked here by name, for every one of those 15 real modules,
// in both boot modes.
void testRealModuleSetMatchesOldHardcodedListBehaviorExactly(TestStats& stats) {
    beta::crypto::KirkEngine kirk;
    const std::vector<beta::format::ResolvedPsarEntry> entries = loadRealEntries(kirk);
    CHECK_TRUE(stats, !entries.empty());
    if (entries.empty()) return;

    // The old project's own kVshOnly list (test_ipl_realfuse.cpp:2550-2551
    // etc.) -- real modules that must be present on a VSH boot and absent
    // on a GAME boot.
    static const char* const kOldVshOnly[] = {
        "mgr.prx", "msaudio.prx", "chkreg.prx", "semawm.prx", "npdrm.prx", "chnnlsv.prx", "vaudio.prx",
        "avcodec.prx", "vshbridge.prx", "libatrac3plus.prx", "paf.prx", "common_gui.prx", "common_util.prx",
        "vshmain.prx"};

    beta::SystemBus vshBus, gameBus;
    const beta::firmware::StagedModules vshStaged = beta::firmware::stageEarlyBoot(entries, vshBus, kirk, false);
    const beta::firmware::StagedModules gameStaged = beta::firmware::stageEarlyBoot(entries, gameBus, kirk, true);

    for (const char* name : kOldVshOnly) {
        const beta::firmware::StagedModule* onVsh = findStaged(vshStaged, name);
        const beta::firmware::StagedModule* onGame = findStaged(gameStaged, name);
        CHECK_TRUE(stats, onVsh != nullptr);
        CHECK_TRUE(stats, onGame != nullptr);
        if (onVsh) CHECK_TRUE(stats, !onVsh->bytes.empty());   // present on VSH, matching the old list
        if (onGame) CHECK_TRUE(stats, onGame->bytes.empty());  // absent on GAME, matching the old list
    }

    // umdcache.prx: the old project's one GAME-only special case (opposite direction).
    const beta::firmware::StagedModule* umdcacheVsh = findStaged(vshStaged, "umdcache.prx");
    const beta::firmware::StagedModule* umdcacheGame = findStaged(gameStaged, "umdcache.prx");
    CHECK_TRUE(stats, umdcacheVsh != nullptr && umdcacheVsh->bytes.empty());
    CHECK_TRUE(stats, umdcacheGame != nullptr && !umdcacheGame->bytes.empty());

    // And a handful of "runs everywhere" modules (real marker has both
    // bits set) stay present in BOTH modes, same as the old hardcoded
    // lists (which simply never excluded them).
    for (const char* name : {"ge.prx", "threadman.prx", "iofilemgr.prx"}) {
        const beta::firmware::StagedModule* onVsh = findStaged(vshStaged, name);
        const beta::firmware::StagedModule* onGame = findStaged(gameStaged, name);
        CHECK_TRUE(stats, onVsh != nullptr && !onVsh->bytes.empty());
        CHECK_TRUE(stats, onGame != nullptr && !onGame->bytes.empty());
    }
}

} // namespace

void runAllModuleStagerTests(TestStats& stats) {
    runTest(stats, "ModuleStager.RealVshBootStagesSysmemLoadcoreAndLinksRealImports", testRealVshBootStagesSysmemLoadcoreAndLinksRealImports);
    runTest(stats, "ModuleStager.RealGameBootSkipsVshOnlyModulesAndUsesGameAddresses", testRealGameBootSkipsVshOnlyModulesAndUsesGameAddresses);
    runTest(stats, "ModuleStager.RealModuleSetMatchesOldHardcodedListBehaviorExactly", testRealModuleSetMatchesOldHardcodedListBehaviorExactly);
}
