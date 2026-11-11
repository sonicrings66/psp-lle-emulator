// Reads the console's real 48-bit fuse ID from <project root>/config/psp-lle.ini
// ([console] fuse_id = 0x... ), the same file and format psp-lle-emu's own
// pspcfg::requireFuse() uses -- never hardcoded in source, never logged.
// KirkEngine's own default constructor uses a placeholder fuse value; any
// real KIRK command that derives a per-console key (e.g.
// KIRK_CMD_ENCRYPT_IV_FUSE/DECRYPT_IV_FUSE) needs the real one instead.
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace beta::fuseconfig {

namespace detail {
inline std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}
inline bool parseHex(const std::string& s, uint64_t& out) {
    std::string t = s;
    if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) t = t.substr(2);
    if (t.empty() || t.size() > 16) return false;
    uint64_t v = 0;
    for (char c : t) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint64_t>(c - 'A' + 10);
        else return false;
    }
    out = v;
    return true;
}
} // namespace detail

// Returns false (leaving fuse90/fuse94 untouched) if the file or the
// fuse_id key is missing -- callers should fall back to KirkEngine's own
// placeholder default in that case, same as today.
inline bool loadRealFuse(const std::string& iniPath, uint32_t& fuse90, uint32_t& fuse94) {
    std::ifstream f(iniPath);
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        std::string t = detail::trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t[0] == '[') {
            const std::size_t e = t.find(']');
            section = detail::lower(detail::trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
            continue;
        }
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = detail::lower(detail::trim(t.substr(0, eq)));
        std::string val = t.substr(eq + 1);
        for (const char* mark : {" ;", "\t;", " #", "\t#"}) {
            const std::size_t c = val.find(mark);
            if (c != std::string::npos) val = val.substr(0, c);
        }
        val = detail::trim(val);
        if (section == "console" && key == "fuse_id") {
            uint64_t v = 0;
            if (!detail::parseHex(val, v)) return false;
            v &= 0x0000FFFFFFFFFFFFull;
            fuse90 = static_cast<uint32_t>(v & 0xFFFFFFFFull);
            fuse94 = static_cast<uint32_t>(v >> 32);
            return true;
        }
    }
    return false;
}

} // namespace beta::fuseconfig
