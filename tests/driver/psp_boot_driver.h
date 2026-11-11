#pragma once

#include <cstdint>
#include <string>

#include "options.h"

// Thin boot driver (see the plan's "tests/driver/psp_boot_driver.cpp: thin
// app driver... no business logic"): wires together the real pieces
// src/core/firmware already provides (NAND-based IPL extraction, pspbtcnf-
// driven module staging) and a real Cpu, then runs the exact two-stage real
// boot sequence this project's own old emulator uses (see
// docs/PORTING_LOG.md's "Milestone A boot-sequence" entry for the uofw/
// test_ipl_realfuse.cpp citations this was traced from):
//
//   Stage 1 (real cIPL execution): cpu.reset() to the real IPL's own entry
//   point (extractAndLoadRealIplCached's return value) and let the CPU
//   genuinely execute real cIPL/exploit-trampoline code until it reaches a
//   real, confirmed-stable halt at pc=0x00010108 (uofw's syscon/power.S).
//
//   Stage 2 (this project's own documented shortcut, not real hardware
//   behavior -- real SYSCON would re-run the whole pre-IPL->cIPL chain
//   again): once that halt is confirmed, splice directly to the already-
//   staged sysmem.prx entry point instead, preserving cp0State/cp0Ctrl/
//   countRegister/timerInterruptPending across the reset (a real CPU stays
//   continuous across a SYSCON reset -- only PC/SP/GP actually change).
//
// Emits periodic "[STATE] instr N cpu=... ram=... pc=... count=..." lines
// in the exact format testsupport::GoldenTrace parses, so a run of this
// driver is directly diffable against an old-project golden trace.
namespace beta::driver {

struct BootDriverOptions {
    // E:\PSP-LLE-beta\ -- this project's own portable folder (nand\, firmware\, config\),
    // structured like psp-lle-emu's E:\PSP-LLE\ at the user's request (2026-11-11); the
    // launcher's own Install tab writes new NAND dumps/updates here when targeting this build.
    std::string firmwarePbpPath = "E:/PSP-LLE-beta/firmware/661.PBP";
    std::string nandDumpPath = "E:/PSP-LLE-beta/nand/UNKNOWN_db40916a_64M.bin";
    std::string iplCacheDir; // empty = no cache
    bool gameBoot = false;
    // Basically unlimited (2026-11-11): the window loop has no other exit, so a small
    // budget here used to freeze an otherwise-live XMB session after a couple of real
    // seconds once the interpreter caught up to it. This exhausts at roughly the
    // multi-trillion-instruction mark instead -- years of real run time -- so closing
    // the window (or killing the process) is the only practical way this ends now.
    uint64_t instructionBudget = 1'000'000'000'000'000ull;
    int64_t stateHashInterval = 1'000'000; // 0 disables periodic [STATE] printing
    // The library's own config surface (window/GE/ME toggles etc.) -- main() builds this
    // via config::parseOptionsFromEnvironment()/applyCommandLineArgs() so this driver,
    // like every other file, never calls std::getenv()/reads argv itself (see options_cli.h).
    config::EmuOptions emu;
};

struct BootDriverResult {
    bool ok = false;
    std::string failureReason;
    uint32_t iplEntry = 0;
    uint32_t sysmemRealEntry = 0;
    uint64_t haltConfirmedAtInstr = 0; // 0 if the stage-1 halt was never confirmed
    uint64_t totalInstructionsExecuted = 0;
};

// Runs the full real boot sequence once. Prints progress/[STATE] lines to
// stdout as it goes (same real-time-visible-progress convention the old
// project's own driver used) -- this is a driver, not a silent library call.
BootDriverResult runBoot(const BootDriverOptions& opts);

} // namespace beta::driver
