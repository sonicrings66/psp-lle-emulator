#include "golden_trace.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace beta::testsupport {

bool GoldenTrace::loadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    entries_.clear();
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t marker = line.find("[STATE] instr");
        if (marker == std::string::npos) continue;

        long long instr = 0;
        unsigned long long cpuHash = 0, ramHash = 0;
        unsigned int pc = 0, count = 0;
        const int matched = std::sscanf(line.c_str() + marker,
                                         "[STATE] instr %lld cpu=%llX ram=%llX pc=%X count=%X",
                                         &instr, &cpuHash, &ramHash, &pc, &count);
        if (matched != 5) continue; // malformed/truncated line -- skip, don't abort the whole load

        GoldenEntry e;
        e.instr = static_cast<int64_t>(instr);
        e.hash.cpu = static_cast<uint64_t>(cpuHash);
        e.hash.ram = static_cast<uint64_t>(ramHash);
        e.pc = static_cast<uint32_t>(pc);
        e.count = static_cast<uint32_t>(count);
        entries_.push_back(e);
    }
    return true;
}

int64_t GoldenTrace::firstMismatch(const GoldenTrace& other) const {
    const std::size_t n = entries_.size() < other.entries_.size() ? entries_.size() : other.entries_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const GoldenEntry& a = entries_[i];
        const GoldenEntry& b = other.entries_[i];
        if (a.hash != b.hash || a.pc != b.pc || a.count != b.count) return static_cast<int64_t>(i);
    }
    if (entries_.size() != other.entries_.size()) return static_cast<int64_t>(n);
    return -1;
}

} // namespace beta::testsupport
