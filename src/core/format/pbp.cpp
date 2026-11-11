#include "pbp.h"

#include <stdexcept>

namespace beta::format {

namespace {
uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

PbpFile::PbpFile(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.size() < kHeaderSize) {
        throw std::runtime_error("PBP file too small for header");
    }
    if (!(data_[0] == 0x00 && data_[1] == 'P' && data_[2] == 'B' && data_[3] == 'P')) {
        throw std::runtime_error("not a PBP file (bad magic)");
    }

    version_ = readU32LE(&data_[4]);
    for (int i = 0; i < kPartCount; ++i) {
        offsets_[i] = readU32LE(&data_[8 + i * 4]);
    }
}

ByteSpan PbpFile::section(PbpPart part) const {
    const int index = static_cast<int>(part);
    const uint32_t start = offsets_[index];
    const uint32_t end = (index + 1 < kPartCount) ? offsets_[index + 1] : static_cast<uint32_t>(data_.size());

    if (start > data_.size() || end > data_.size() || end < start) {
        throw std::runtime_error("PBP section offset out of range");
    }

    return ByteSpan{data_.data() + start, end - start};
}

} // namespace beta::format
