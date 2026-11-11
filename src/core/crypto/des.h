#pragma once

#include <cstdint>

namespace beta::crypto {

// Textbook DES, single-block decrypt only (no encrypt) -- this project
// only needs it to decrypt PSAR's per-model name tables, which use plain
// DES-CBC (see format/psar_names.h). Ported from PPSSPP's DESContext
// (Core/Util/PSARUnpack.cpp).
class Des {
public:
    explicit Des(const uint8_t key[8]);

    void decryptBlock(const uint8_t in[8], uint8_t out[8]) const;

private:
    uint8_t subkeys_[16][48];
};

} // namespace beta::crypto
