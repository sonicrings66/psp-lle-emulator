#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "crypto/kirk.h"
#include "pbp.h"

namespace beta::format {

// PSAR ("PSP ARchive") is the container inside a firmware updater's
// PbpPart::PsarData section holding the actual OS files: a flat sequence
// of records, each an encrypted header ("demangle" + PRX type-0 decrypt,
// see decodePsarBlock) followed by an entry description and, for files,
// zlib-compressed contents. There is no index -- entry n+1 is found only
// by decoding entry n and adding up its sizes (PsarReader below does
// this). Resolving an entry's cryptic short name (e.g. "00042") to its
// real path needs a further, separate DES-based table cipher that hasn't
// been ported yet -- entries come back under their short name for now.
struct PsarPreamble {
    bool magicValid = false;
    uint8_t version = 0;    // jpcsp: psarVersion, byte at offset+4. 1 = "oldschool" layout.
    bool decrypted = false; // jpcsp: true if the u32 at offset+0x20 == 0x2C333333
};

PsarPreamble readPsarPreamble(ByteSpan psarSection);

// Decrypts the PSAR's very first table block using the "demangle" step
// (jpcsp: PSAR.java's demangle() -- just a KIRK CMD7 AES-128-CBC decrypt
// with keyId 0x55, nothing more exotic) and returns the 4-byte tag at
// offset 0xD0 of the decrypted block. That tag selects which further
// per-entry decode routine (jpcsp: PRX.java's DecryptPRX, ~500 lines with
// 11 decode "types" and a 100+ entry per-firmware-version key table) real
// table/file entries need. Used to discover empirically which of those
// paths a given firmware version's PSAR actually exercises before porting
// DecryptPRX itself -- ported so far only covers this far, not DecryptPRX.
uint32_t peekFirstBlockTag(ByteSpan psarSection, crypto::KirkEngine& kirk);

// Decrypts one PSAR record's block (demangle, then the type-0 PRX decrypt
// -- see crypto/prx_decrypt.h) and returns just the useful decrypted
// payload, trimmed to its own declared size. `offset` and `cbIn` are
// psar-relative, matching jpcsp/ppsspp's DecodeBlock. Ported from
// PPSSPP's PSARReader::DecodeBlock; the "give it 16 bytes of slack" read-
// past-the-end quirk it documents is preserved here.
std::vector<uint8_t> decodePsarBlock(ByteSpan psarSection, uint32_t offset, uint32_t cbIn,
                                      bool oldschool, crypto::KirkEngine& kirk);

// Decrypts the PSAR's very first record (a text block, not a file) and
// returns the version string from it, e.g. "6.61". This is the first real,
// checkable proof that decodePsarBlock/decryptPrxType0 work end to end
// against a genuine encrypted firmware archive.
std::string readFirmwareVersion(ByteSpan psarSection, crypto::KirkEngine& kirk);

struct PsarEntry {
    std::string shortName;      // e.g. "00042" -- the cryptic name PSAR itself uses, not a real path
    bool isDirectory = false;
    std::vector<uint8_t> data;  // decompressed file contents; empty for directories
};

// Walks a PSAR archive's flat record list one entry at a time. Ported
// from PPSSPP's PSARReader (Core/Util/PSARUnpack.cpp): Init() decodes the
// firmware-version record and the second (fixed-size-guess) record every
// archive has, and next() decodes each subsequent file/directory entry in
// turn. Every zlib-compressed entry is inflated (util/inflate.h); other
// compression schemes (KL4E, KL3E, LZR -- see the format doc in PPSSPP's
// repo) are detected but left undecoded, with an empty `data`.
class PsarReader {
public:
    // Throws if `psarSection` doesn't parse as a PSAR archive, or if
    // either of its first two records fails to decrypt.
    PsarReader(ByteSpan psarSection, crypto::KirkEngine& kirk);

    // Returns the next entry, or nullopt once the archive is exhausted.
    // Throws if an entry fails to decrypt or the archive's internal
    // bookkeeping (the always-zero sanity field at each entry's +0x100)
    // doesn't check out -- both mean position tracking has desynced from
    // the real record boundaries, and every entry after would be garbage.
    std::optional<PsarEntry> next();

    const std::string& firmwareVersion() const { return firmwareVersion_; }

private:
    ByteSpan psar_;
    crypto::KirkEngine& kirk_;
    bool oldschool_ = false;
    uint32_t overhead_ = 0x150;
    uint32_t pos_ = 0;
    uint32_t limit_ = 0;
    std::string firmwareVersion_;
};

struct ResolvedPsarEntry {
    std::string shortName;  // PSAR's own cryptic name, e.g. "00104"
    std::string realName;   // e.g. "flash0:/font/ltn0.pgf" -- empty if no name table claimed it
    bool isDirectory = false;
    bool isNameTable = false; // consumed into the resolver's name map, not a real file -- data is empty
    std::vector<uint8_t> data;
};

// Walks an entire PSAR archive and resolves each entry's short name to a
// real path where possible, using the archive's own per-model name-table
// entries (format/psar_names.h) -- the same short numeric names ("00001"
// through "00012") that PPSSPP's docs say hold 6.x/5.x's file lists.
// Grouped 3.x-style names ("com:00123") are not handled, only the flat
// 6.x/5.x form this project's actual test firmware (661.PBP) uses.
std::vector<ResolvedPsarEntry> resolvePsarEntries(ByteSpan psarSection, crypto::KirkEngine& kirk);

struct PsarInstallStats {
    int entries = 0;
    int written = 0;
    int unnamed = 0;   // written under their short numeric name -- no name table claimed them
    int failed = 0;    // had a real/short name but no decoded content to write
};

// "Installs" a firmware updater the way real hardware does and RPCS3
// installs a PS3 PUP: writes every real file the archive names into a
// plain directory tree rooted at `outputDir`, one subdirectory per
// device ("flash0/font/ltn0.pgf", "flash1/registry", "ipl/psp_nandipl.bin"
// -- the colon becomes a path separator, matching PPSSPP's
// RelativePathFromEntryName). This is the "virtual memory stick" a later
// phase's file-I/O HLE syscalls can read flash0:/flash1: from, once that
// layer exists -- it doesn't hook into SystemBus itself yet.
PsarInstallStats installPsar(ByteSpan psarSection, crypto::KirkEngine& kirk, const std::string& outputDir);

// The reverse of installPsar: rebuilds the entry list from a folder it wrote, so the emulator can boot from "firmware/" instead of
// decrypting 661.PBP on every start. A file "flash0/kd/loadcore.prx" becomes realName "flash0:/kd/loadcore.prx" (shortName is not kept for
// named entries); a file directly in the folder whose name is all digits is an entry no name table claimed (shortName only, empty
// realName); anything else directly in the folder (661.PBP itself) is ignored. Sorted by path so the result is deterministic. Empty if the
// folder does not exist or holds no flash0/flash1/ipl folder.
std::vector<ResolvedPsarEntry> loadUnpackedEntries(const std::string& dir);

} // namespace beta::format
