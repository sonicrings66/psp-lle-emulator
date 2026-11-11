#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace beta {

// What the Memory Stick controller engine needs from a card: 512-byte sectors, read and write. Two implementations: MsImage (a raw image file, below) and MsFolderCard
// (util/ms_folder_card.h: a FAT32 volume synthesised from a host folder).
class MsCard {
public:
    static constexpr uint32_t kSectorSize = 512;
    virtual ~MsCard() = default;
    virtual uint64_t sectorCount() const = 0;
    virtual bool readSector(uint64_t lba, uint8_t* out) = 0;   // false (and `out` zeroed) for a sector past the end
    virtual bool writeSector(uint64_t lba, const uint8_t* in) = 0;
    virtual const std::string& path() const = 0;              // what to show the user (an image file or a folder)
    uint64_t sizeBytes() const { return sectorCount() * kSectorSize; }
};

// A Memory Stick card as a raw image file: 512-byte sectors, sector 0 = the MBR, exactly what a PC sees when it reads the card. Pure LLE: the firmware's own
// driver and FAT code talk to the controller (mmio/ms_engine.h), and the controller reads and writes sectors here. Writes go straight through to the file
// (flushed after every sector), so what the firmware writes survives a restart and a killed process loses at most the sector in flight.
class MsImage : public MsCard {
public:

    bool open(const std::string& path) {
        close();
        f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f_) return false;
        f_.seekg(0, std::ios::end);
        const std::streamoff size = f_.tellg();
        if (size <= 0 || size % kSectorSize != 0) { f_.close(); return false; }
        sectors_ = static_cast<uint64_t>(size) / kSectorSize;
        path_ = path;
        return true;
    }
    void close() { if (f_.is_open()) f_.close(); sectors_ = 0; }
    bool isOpen() const { return f_.is_open(); }
    const std::string& path() const override { return path_; }
    uint64_t sectorCount() const override { return sectors_; }

    // false (and `out` zeroed) for a sector past the end of the card
    bool readSector(uint64_t lba, uint8_t* out) override {
        if (!f_.is_open() || lba >= sectors_) { std::memset(out, 0, kSectorSize); ++outOfRange; return false; }
        f_.clear(); f_.seekg(static_cast<std::streamoff>(lba * kSectorSize));
        f_.read(reinterpret_cast<char*>(out), kSectorSize);
        if (!f_) { f_.clear(); std::memset(out, 0, kSectorSize); return false; }
        ++reads;
        return true;
    }
    bool writeSector(uint64_t lba, const uint8_t* in) override {
        if (!f_.is_open() || lba >= sectors_) { ++outOfRange; return false; }
        f_.clear(); f_.seekp(static_cast<std::streamoff>(lba * kSectorSize));
        f_.write(reinterpret_cast<const char*>(in), kSectorSize);
        f_.flush();
        if (!f_) { f_.clear(); return false; }
        ++writes;
        return true;
    }

    uint64_t reads = 0, writes = 0, outOfRange = 0;

private:
    std::fstream f_;
    uint64_t sectors_ = 0;
    std::string path_;
};

} // namespace beta
