#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "cpu_bus.h"

namespace beta {

// A flat, byte-addressable, little-endian block of storage. Used both
// directly (unit tests -- a bare Memory is a complete, if minimal, CpuBus:
// Cpu can run against one with no SystemBus/MMIO involved at all, exactly
// like the old project's own test_cpu.cpp) and as the backing store for
// RAM/scratchpad/IPL regions inside SystemBus. Implements JitHints for
// real (code-write tracking); TimingSource and SyscallResolver come along
// for free via CpuBus's safe defaults -- plain memory has no device timing
// and participates in no syscall table, so there's nothing to override.
class Memory : public CpuBus {
public:
    explicit Memory(std::size_t sizeBytes);

    uint8_t  read8(uint32_t addr) const override;
    uint16_t read16(uint32_t addr) const override;
    uint32_t read32(uint32_t addr) const override;

    void write8(uint32_t addr, uint8_t value) override;
    void write16(uint32_t addr, uint16_t value) override;
    void write32(uint32_t addr, uint32_t value) override;

    void loadBinary(uint32_t addr, const uint8_t* data, std::size_t size);

    // Code-write tracking (see JitHints::watchCode). Every write path
    // funnels through the four write methods above, so this is exact.
    bool tracksCodeWrites() const override { return true; }
    void watchCode(uint32_t addr, uint32_t size) override;
    uint64_t codeWriteEpoch() const override { return epoch_; }
    bool storeNeedsInterpreter(uint32_t addr) const override { return isWatched(addr, 4); }
    bool isWatched(uint32_t addr, uint32_t size) const;
    // For callers that write data() directly (SystemBus's RAM fast path): same effect as the write itself having gone through write8/16/32.
    void noteStore(uint32_t addr, std::size_t size) { noteWrite(addr, size); }
    bool anyWatched() const { return anyWatched_; }
    const uint8_t* watchedMap() const { return watched_.data(); } // stable for the Memory's lifetime (allocated in the constructor)

    std::size_t size() const { return data_.size(); }
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }

private:
    void checkBounds(uint32_t addr, std::size_t accessSize) const;
    // Called by every write after the bounds check: if any watched 32-byte line is touched, unwatch it and bump the epoch.
    void noteWrite(uint32_t addr, std::size_t size) {
        if (!anyWatched_) return;
        const std::size_t last = (static_cast<std::size_t>(addr) + size - 1) >> 5;
        for (std::size_t l = addr >> 5; l <= last; ++l) {
            if (watched_[l]) { watched_[l] = 0; ++epoch_; }
        }
    }

    std::vector<uint8_t> data_;
    std::vector<uint8_t> watched_; // one byte per 32-byte line, allocated on the first watchCode()
    bool anyWatched_ = false;
    uint64_t epoch_ = 0;
};

} // namespace beta
