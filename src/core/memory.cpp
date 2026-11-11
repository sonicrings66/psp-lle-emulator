#include "memory.h"

#include <stdexcept>
#include <string>

namespace beta {

Memory::Memory(std::size_t sizeBytes) : data_(sizeBytes, 0), watched_((sizeBytes >> 5) + 1, 0) {}

void Memory::checkBounds(uint32_t addr, std::size_t accessSize) const {
    if (static_cast<uint64_t>(addr) + accessSize > data_.size()) {
        throw std::out_of_range(
            "memory access out of bounds: addr=0x" + std::to_string(addr) +
            " size=" + std::to_string(accessSize) +
            " memSize=" + std::to_string(data_.size()));
    }
}

uint8_t Memory::read8(uint32_t addr) const {
    checkBounds(addr, sizeof(uint8_t));
    return data_[addr];
}

uint16_t Memory::read16(uint32_t addr) const {
    checkBounds(addr, sizeof(uint16_t));
    return static_cast<uint16_t>(data_[addr]) |
           (static_cast<uint16_t>(data_[addr + 1]) << 8);
}

uint32_t Memory::read32(uint32_t addr) const {
    checkBounds(addr, sizeof(uint32_t));
    return static_cast<uint32_t>(data_[addr]) |
           (static_cast<uint32_t>(data_[addr + 1]) << 8) |
           (static_cast<uint32_t>(data_[addr + 2]) << 16) |
           (static_cast<uint32_t>(data_[addr + 3]) << 24);
}

void Memory::watchCode(uint32_t addr, uint32_t size) {
    if (size == 0 || static_cast<uint64_t>(addr) + size > data_.size()) return;
    for (std::size_t l = addr >> 5; l <= (static_cast<std::size_t>(addr) + size - 1) >> 5; ++l) watched_[l] = 1;
    anyWatched_ = true;
}

bool Memory::isWatched(uint32_t addr, uint32_t size) const {
    if (!anyWatched_ || static_cast<uint64_t>(addr) + size > data_.size()) return false;
    for (std::size_t l = addr >> 5; l <= (static_cast<std::size_t>(addr) + size - 1) >> 5; ++l) if (watched_[l]) return true;
    return false;
}

void Memory::write8(uint32_t addr, uint8_t value) {
    checkBounds(addr, sizeof(uint8_t));
    noteWrite(addr, 1);
    data_[addr] = value;
}

void Memory::write16(uint32_t addr, uint16_t value) {
    checkBounds(addr, sizeof(uint16_t));
    noteWrite(addr, 2);
    data_[addr] = static_cast<uint8_t>(value & 0xFF);
    data_[addr + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void Memory::write32(uint32_t addr, uint32_t value) {
    checkBounds(addr, sizeof(uint32_t));
    noteWrite(addr, 4);
    data_[addr] = static_cast<uint8_t>(value & 0xFF);
    data_[addr + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data_[addr + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data_[addr + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void Memory::loadBinary(uint32_t addr, const uint8_t* data, std::size_t size) {
    checkBounds(addr, size);
    if (size) noteWrite(addr, size);
    for (std::size_t i = 0; i < size; ++i) {
        data_[addr + i] = data[i];
    }
}

} // namespace beta
