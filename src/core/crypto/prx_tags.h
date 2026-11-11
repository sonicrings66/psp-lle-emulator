#pragma once

#include <cstdint>

namespace beta::crypto {

// Per-tag key tables for the general PRX decrypter (prx_decrypt.h). Ported
// verbatim from PPSSPP's Core/ELF/PrxDecrypter.cpp (g_tagInfo/g_tagInfo2 and
// the raw key byte arrays behind them) -- real Sony modules across many
// firmware versions each carry one of these tags at block offset 0xD0, and
// the tag alone selects which fixed key (never a per-console secret) and
// which of the ~6 decode "types" applies.

// 144-byte ("old method") keys, used by decryptPrxType0.
struct TagInfo {
    uint32_t tag;
    const uint32_t* key; // 36 x u32 = 144 bytes
    uint8_t code;
    bool scrambleKey; // true if `key` is stored raw and needs a kirk7 pass before use
};

const TagInfo* getTagInfo(uint32_t tag);

// 16-byte ("new method") keys, used by decryptPrxType1/2/5/6/9.
struct TagInfo2 {
    uint32_t tag;
    const uint8_t* key; // 16 bytes
    uint8_t code;
    uint8_t type = 0;         // extra dispatch hint some entries carry; unused by the types ported so far
    const uint8_t* seed = nullptr; // optional bonus seed (type 5 only)
};

const TagInfo2* getTagInfo2(uint32_t tag);

} // namespace beta::crypto
