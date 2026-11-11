#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../crypto/kirk.h"

namespace beta::format {

// Which of the DES key/IV sets (see psar_names.cpp) a firmware version
// uses to encrypt its PSAR name tables. Ported from PPSSPP's
// TableKeyIndexForVersion (Core/Util/PSARUnpack.cpp).
int tableKeyIndexForVersion(const std::string& version);

// Decrypts a PSAR name-table entry's contents in place: plain DES-CBC,
// keyed by tableKeyIndexForVersion's result. This is the layer PPSSPP's
// docs describe as being "on top of everything else" for a name table.
// What's left afterward is an ordinary PRX blob (not decoded here -- see
// crypto/prx_decrypt.h).
void decryptNameTableDes(std::vector<uint8_t>& table, int keyIndex);

// shortname -> realpath, e.g. "00104" -> "flash0:/font/ltn0.pgf".
using NameTable = std::map<std::string, std::string>;

// Decrypts and parses one PSAR name-table entry's (already zlib-inflated)
// contents into a NameTable. Layers, innermost first: DES-CBC, then an
// ordinary PRX type-0 decrypt (confirmed empirically: 661.PBP's name
// tables carry the same 0x0E000000 tag as the PSAR blocks themselves),
// then plain text -- one "shortname<sep>realpath" line per entry, comma-
// or bar-separated depending on firmware generation. Returns an empty
// table on any failure; a real name table always decrypts successfully
// in practice, so this doubles as "is this actually a name table".
NameTable decodeNameTable(std::vector<uint8_t> tableData, int keyIndex, crypto::KirkEngine& kirk);

} // namespace beta::format
