#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace beta {

// A real, raw NAND dump (main data + spare/OOB bytes per page, no filesystem
// or partition parsing) -- e.g. a real PSP-3000's system NAND, dumped page-
// by-page rather than extracted through a filesystem layer (which discards
// exactly the physical spare-area structure real boot-time NAND scans read).
// Confirmed real layout against a real 64MB-NAND dump: 528 bytes/page (512
// main + 16 spare), page/spare interleaved per record, no header. Real page
// size (512) and pages-per-block (32) match jpcsp's `Nand.java` constants.
class NandImage {
public:
    static constexpr std::size_t kPageSize = 512;
    static constexpr std::size_t kSpareSize = 16;
    static constexpr std::size_t kRecordSize = kPageSize + kSpareSize;

    explicit NandImage(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            throw std::runtime_error("NandImage: could not open " + path);
        }
        f.seekg(0, std::ios::end);
        const auto size = static_cast<std::size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        if (size % kRecordSize != 0) {
            throw std::runtime_error("NandImage: file size is not a multiple of the 528-byte page+spare record size");
        }
        data_.resize(size);
        f.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(size));
        if (!f) {
            throw std::runtime_error("NandImage: short read on " + path);
        }
        pageCount_ = size / kRecordSize;
    }

    std::size_t pageCount() const { return pageCount_; }

    // Returns pointers to the real main (512B) and spare (16B) data for a
    // given real physical page number, or nullptr/nullptr if `ppn` is
    // outside the real dump (e.g. the scan requests a page number our
    // approximated seed/cursor logic produces that doesn't correspond to
    // real hardware -- callers should treat that as "no real data here"
    // rather than faulting, since this project's own cursor arithmetic is
    // still an approximation, not a byte-exact real address generator).
    const uint8_t* mainData(uint32_t ppn) const {
        if (ppn >= pageCount_) return nullptr;
        return data_.data() + static_cast<std::size_t>(ppn) * kRecordSize;
    }
    uint8_t* mainDataMut(uint32_t ppn) {
        if (ppn >= pageCount_) return nullptr;
        return data_.data() + static_cast<std::size_t>(ppn) * kRecordSize;
    }
    uint8_t* spareDataMut(uint32_t ppn) {
        if (ppn >= pageCount_) return nullptr;
        return data_.data() + static_cast<std::size_t>(ppn) * kRecordSize + kPageSize;
    }
    const uint8_t* spareData(uint32_t ppn) const {
        if (ppn >= pageCount_) return nullptr;
        return data_.data() + static_cast<std::size_t>(ppn) * kRecordSize + kPageSize;
    }

    // ---- write-back (pure LLE: what the guest programs and erases reaches the dump file) --------------------------------------------------
    // The image lives in memory. With a backing file attached, the pages the guest really changed are remembered (markDirty) and flushToFile()
    // writes them into that file at their own 528-byte record offsets (the file is opened read/write and never truncated or rewritten as a whole).
    void attachBackingFile(const std::string& path) { backingPath_ = path; }
    bool hasBackingFile() const { return !backingPath_.empty(); }
    const std::string& backingFile() const { return backingPath_; }
    void markDirty(uint32_t ppn) {
        if (backingPath_.empty() || ppn >= pageCount_) return;
        if (dirty_.empty()) dirty_.assign((pageCount_ + 63) / 64, 0ull);
        uint64_t& w = dirty_[ppn >> 6]; const uint64_t b = 1ull << (ppn & 63);
        if (!(w & b)) { w |= b; ++dirtyCount_; }
    }
    std::size_t dirtyCount() const { return dirtyCount_; }
    // Writes every dirty page into the backing file, in page order, adjacent pages in one write. Returns false and keeps the pages dirty if the
    // file cannot be opened, has changed size since it was loaded, or a write fails (the in-memory image is unaffected either way).
    bool flushToFile(std::string* error = nullptr) {
        if (dirtyCount_ == 0 || backingPath_.empty()) return true;
        std::fstream f(backingPath_, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) { if (error) *error = "could not open " + backingPath_ + " for writing"; return false; }
        f.seekg(0, std::ios::end);
        if (static_cast<std::size_t>(f.tellg()) != data_.size()) { if (error) *error = backingPath_ + " changed size since it was loaded; not writing"; return false; }
        auto isDirty = [&](std::size_t q) { return (dirty_[q >> 6] >> (q & 63)) & 1ull; };
        std::size_t p = 0;
        while (p < pageCount_) {
            if (!isDirty(p)) { ++p; continue; }
            std::size_t q = p;
            while (q < pageCount_ && isDirty(q)) ++q;
            f.seekp(static_cast<std::streamoff>(p * kRecordSize));
            f.write(reinterpret_cast<const char*>(data_.data() + p * kRecordSize), static_cast<std::streamsize>((q - p) * kRecordSize));
            if (!f) { if (error) *error = "write to " + backingPath_ + " failed"; return false; }
            p = q;
        }
        f.flush();
        if (!f) { if (error) *error = "flush of " + backingPath_ + " failed"; return false; }
        std::fill(dirty_.begin(), dirty_.end(), 0ull); dirtyCount_ = 0;
        return true;
    }

private:
    std::vector<uint8_t> data_;
    std::size_t pageCount_ = 0;
    std::string backingPath_;
    std::vector<uint64_t> dirty_;
    std::size_t dirtyCount_ = 0;
};

} // namespace beta
