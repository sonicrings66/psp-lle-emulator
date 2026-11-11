#include <cstdio>
#include "psar_names.h"

#include <cstring>
#include <stdexcept>
#include <string_view>

#include "../crypto/des.h"
#include "../crypto/prx_decrypt.h"

namespace beta::format {

namespace {

struct TableKey {
    uint32_t keyLow;
    uint32_t keyHigh;
    uint8_t iv[8];
};

// One DES key and IV per firmware series. Ported from PPSSPP's
// kTableKeys (Core/Util/PSARUnpack.cpp).
constexpr TableKey kTableKeys[] = {
    {0xB730E5C7, 0x95620B49, {0x9E, 0xA4, 0x33, 0x81, 0x86, 0x0C, 0x52, 0x85}},
    {0x45C9DC95, 0x5A7B3D9D, {0xB2, 0xFE, 0xD9, 0x79, 0x8A, 0x02, 0xB1, 0x87}},
    {0x6F20585A, 0x4CCE495B, {0x81, 0x08, 0xC1, 0xF2, 0x35, 0x98, 0x69, 0xB0}},
    {0x620BF15A, 0x73F45262, {0x6D, 0x52, 0x1B, 0xA3, 0xC2, 0x36, 0xF9, 0x2B}},
    {0xFD9D4498, 0xA664C8F8, {0xDB, 0x4E, 0x79, 0x41, 0xF5, 0x97, 0x30, 0xAD}},
    {0x3D6426E7, 0xD7BD7481, {0xA6, 0x83, 0x0C, 0x2F, 0x63, 0x0B, 0x96, 0x29}},
};

} // namespace

int tableKeyIndexForVersion(const std::string& version) {
    if (version.rfind("3.8", 0) == 0 || version.rfind("3.9", 0) == 0) {
        return 1;
    }
    if (version.rfind("4.", 0) == 0) {
        return 2;
    }
    if (version.rfind("5.", 0) == 0) {
        return 3;
    }
    if (version.rfind("6.", 0) == 0) {
        return 4;
    }
    return 0;
}

void decryptNameTableDes(std::vector<uint8_t>& table, int keyIndex) {
    constexpr int kNumTableKeys = static_cast<int>(sizeof(kTableKeys) / sizeof(kTableKeys[0]));
    if (keyIndex < 0 || keyIndex >= kNumTableKeys || table.size() < 8) {
        throw std::runtime_error("decryptNameTableDes: bad key index or table too small");
    }
    const TableKey& tableKey = kTableKeys[keyIndex];

    uint8_t key[8];
    const uint64_t combined = (static_cast<uint64_t>(tableKey.keyHigh) << 32) | tableKey.keyLow;
    for (int i = 0; i < 8; ++i) {
        key[i] = static_cast<uint8_t>(combined >> (56 - i * 8));
    }
    const crypto::Des des(key);

    uint8_t previous[8];
    std::memcpy(previous, tableKey.iv, sizeof(previous));

    for (std::size_t offset = 0; offset + 8 <= table.size(); offset += 8) {
        uint8_t* at = table.data() + offset;
        uint8_t cipher[8];
        uint8_t plain[8];
        std::memcpy(cipher, at, sizeof(cipher));
        des.decryptBlock(cipher, plain);
        for (int i = 0; i < 8; ++i) {
            at[i] = plain[i] ^ previous[i];
        }
        std::memcpy(previous, cipher, sizeof(previous));
    }
}

NameTable decodeNameTable(std::vector<uint8_t> tableData, int keyIndex, crypto::KirkEngine& kirk) {
    NameTable names;

    try {
        decryptNameTableDes(tableData, keyIndex);

        // Later generations (02g+) wrap their name tables with a different tag than the 0x0E000000 type-0
        // wrapper 661.PBP's generation-1 table uses; PPSSPP decrypts tables with the generic pspDecryptPRX
        // (all types/tags), so do the same via decryptModule.
        int32_t textLen = 0;
        try {
            textLen = crypto::decryptPrxType0(kirk, tableData.data(), tableData.data(),
                                              static_cast<uint32_t>(tableData.size()));
        } catch (const std::exception&) {
            textLen = 0;
        }
        if (textLen <= 0 || static_cast<std::size_t>(textLen) > tableData.size()) {
            std::vector<uint8_t> generic;
            try { generic = crypto::decryptModule(kirk, tableData); } catch (const std::exception&) {}
            if (generic.empty()) {
                return names;
            }
            tableData = std::move(generic);
            textLen = static_cast<int32_t>(tableData.size());
        }

        const char* text = reinterpret_cast<const char*>(tableData.data());
        const std::size_t length = static_cast<std::size_t>(textLen);
        std::size_t start = 0;
        while (start < length) {
            std::size_t end = start;
            while (end < length && text[end] != '\r' && text[end] != '\n') {
                ++end;
            }
            const std::string_view line(text + start, end - start);
            const std::size_t separator = line.find_first_of(",|");
            if (separator != std::string_view::npos && separator > 0) {
                names.emplace(std::string(line.substr(0, separator)), std::string(line.substr(separator + 1)));
            }
            while (end < length && (text[end] == '\r' || text[end] == '\n')) {
                ++end;
            }
            start = end;
        }
    } catch (const std::exception&) {
        // Not actually a name table (or it failed to decrypt) -- the
        // empty table this returns tells the caller to treat the entry
        // as an ordinary file instead.
    }

    return names;
}

} // namespace beta::format
