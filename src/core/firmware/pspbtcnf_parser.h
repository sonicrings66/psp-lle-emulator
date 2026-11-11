#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../crypto/kirk.h"

// Real pspbtcnf.bin parser -- ported from the record-table-parsing logic
// embedded in psp-lle-emu's stageEarlyBoot() lambda
// (tests/test_ipl_realfuse.cpp:1361-1389; see docs/PORTING_LOG.md). Everything
// AROUND that parsing in the old code (an ASCII-string scan, a raw hex dump
// to a scratch file, an unconditional "first 16 raw bytes" printf) was pure
// one-off investigation scaffolding from the session that reverse-engineered
// this real layout -- dropped here; only the parse itself is real, load-
// bearing logic.
//
// pspbtcnf.bin is the real, authoritative boot-module config real hardware's
// own SysBoot()/InitInit() read to decide WHICH modules load, in WHAT ORDER,
// and WITH WHAT run-level inclusion (uofw's reboot/main.c, sub_707C/sub_71A8 --
// confirmed by direct decompile, not guessed). This is the REAL driver of
// module staging -- see module_stager.h, which iterates parsePspbtcnf()'s
// result in real record order rather than a hardcoded module list.
namespace beta::firmware {

// One real pspbtcnf.bin record, in real file order. `name` is the real,
// full path string the record's own stringOff resolves to (e.g.
// "/kd/threadman.prx", "/vsh/module/vshmain.prx") -- note this may carry a
// different real hardware-generation suffix than this console's own actual
// files use (see module_stager.cpp's stripGenerationSuffix: the PSAR's
// bundled pspbtcnf.bin is this update's "_11g"-generation copy, since this
// specific console is 03g and its own real per-console pspbtcnf_03g.bin
// lives in flash0 under per-file tags this project can't decrypt -- the SET
// of real modules is the same, just under each console generation's own
// real filename suffix). `marker` is the real per-record attribute/boot-
// mode field (bytes 8-11 of the 32-byte record).
struct PspbtcnfRecord {
    std::string name;
    uint32_t marker = 0;
};

// Real layout (confirmed live against this project's own real 661.PBP, and
// cross-checked structurally against uofw's reboot/main.c): header field
// recordTableOff at +0x20, recordCount at +0x24, stringTableOff at +0x30;
// each of recordCount 32-byte records holds a cumulative byte offset from
// stringTableOff to a NUL-terminated real module path string (+0x00) and
// the real marker (+0x08). Returns records in real file order -- a module
// can legitimately appear under more than one real record (distinct real
// boot-mode masks in the marker's low bits -- e.g. this project's own
// 661.PBP has two distinct real records for "/kd/isofs.prx": one for
// VSH+GAME+UPDATER, 0x80010007, and a separate one for UMDEMU+MLNAPP only,
// 0x800100C0), so every record is kept, not deduplicated by name.
//
// Returns an empty vector if `rawPspbtcnf` doesn't decrypt
// (crypto::decryptModule throws) or is too small to hold the real header.
std::vector<PspbtcnfRecord> parsePspbtcnf(crypto::KirkEngine& kirk, const std::vector<uint8_t>& rawPspbtcnf);

// Convenience: groups parsePspbtcnf()'s ordered records by name (order
// within each name's own vector is preserved, but the across-name order is
// lost -- use parsePspbtcnf() directly, not this, when real staging ORDER
// matters). Kept for callers that only want "every real marker seen for a
// given module name" (e.g. a diagnostic attribute report), not a staging
// driver.
std::map<std::string, std::vector<uint32_t>> groupPspbtcnfByName(const std::vector<PspbtcnfRecord>& records);

} // namespace beta::firmware
