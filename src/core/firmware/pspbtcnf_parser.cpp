#include "pspbtcnf_parser.h"

#include <exception>

#include "../crypto/prx_decrypt.h"

namespace beta::firmware {

namespace {

uint32_t rd32(const std::vector<uint8_t>& d, std::size_t off) {
    if (off + 4 > d.size()) return 0u;
    return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

} // namespace

std::vector<PspbtcnfRecord> parsePspbtcnf(crypto::KirkEngine& kirk, const std::vector<uint8_t>& rawPspbtcnf) {
    std::vector<PspbtcnfRecord> result;

    std::vector<uint8_t> decoded;
    try {
        decoded = crypto::decryptModule(kirk, rawPspbtcnf);
    } catch (const std::exception&) {
        return result;
    }
    if (decoded.size() < 0x38) return result;

    const uint32_t recordTableOff = rd32(decoded, 0x20);
    const uint32_t recordCount = rd32(decoded, 0x24);
    const uint32_t stringTableOff = rd32(decoded, 0x30);

    for (uint32_t i = 0; i < recordCount; ++i) {
        const std::size_t recOff = static_cast<std::size_t>(recordTableOff) + static_cast<std::size_t>(i) * 32u;
        if (recOff + 32 > decoded.size()) break;
        const uint32_t stringOff = rd32(decoded, recOff);
        const uint32_t marker = rd32(decoded, recOff + 8);
        const std::size_t strAddr = static_cast<std::size_t>(stringTableOff) + stringOff;
        if (strAddr >= decoded.size()) continue;
        std::string name;
        for (std::size_t p = strAddr; p < decoded.size() && decoded[p] != 0; ++p) {
            name.push_back(static_cast<char>(decoded[p]));
        }
        if (name.empty()) continue;
        result.push_back(PspbtcnfRecord{std::move(name), marker});
    }
    return result;
}

std::map<std::string, std::vector<uint32_t>> groupPspbtcnfByName(const std::vector<PspbtcnfRecord>& records) {
    std::map<std::string, std::vector<uint32_t>> grouped;
    for (const auto& r : records) grouped[r.name].push_back(r.marker);
    return grouped;
}

} // namespace beta::firmware
