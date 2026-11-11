#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace beta::format {

struct ByteSpan {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
};

enum class PbpPart {
    Sfo = 0,
    Icon0,
    Icon1,
    Pic0,
    Pic1,
    Snd0,
    PspData,
    PsarData,
};

// Parses the PBP ("PSP Bootable Package") container: a small fixed header
// (magic + version + 8 section offsets) followed by the sections
// themselves, back to back. Real Sony firmware updaters store the actual
// firmware payload in the last section, PsarData, which runs to
// end-of-file and is itself a further proprietary container -- see
// format/psar.h, not decoded here.
//
// Layout confirmed both against jpcsp's PBP.java and, byte for byte, by
// hand against this project's own 661.PBP (a legitimately owned 6.61
// firmware updater): its psar_data offset (0x577635) lands exactly on a
// literal "PSAR" magic in the real file.
class PbpFile {
public:
    // Takes ownership of `data` (kept alive for the lifetime of this
    // object; every section() span points into it).
    explicit PbpFile(std::vector<uint8_t> data);

    uint32_t version() const { return version_; }
    ByteSpan section(PbpPart part) const;

private:
    static constexpr std::size_t kHeaderSize = 0x28; // magic(4) + version(4) + 8 offsets(4 each)
    static constexpr int kPartCount = 8;

    std::vector<uint8_t> data_;
    uint32_t version_ = 0;
    uint32_t offsets_[kPartCount] = {};
};

} // namespace beta::format
