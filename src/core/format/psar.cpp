#include <cstdio>
#include "psar.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "crypto/prx_decrypt.h"
#include "psar_names.h"
#include "util/inflate.h"

namespace beta::format {

namespace {
uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
} // namespace

uint32_t peekFirstBlockTag(ByteSpan psarSection, crypto::KirkEngine& kirk) {
    // jpcsp: pspPSARInit calls decodeBlock(dataPSAR, dataPSAROffset+0x10, ...),
    // and decodeBlock's demangle() call decrypts a fixed 0x130-byte window
    // starting 0x20 bytes into that chunk -- i.e. at psar-relative offset
    // 0x10+0x20 = 0x30. The tag lives at absolute offset 0xD0 within the
    // decoded block, which is offset 0xD0-0x20 = 0xB0 into the 0x130-byte
    // plaintext this decrypt produces.
    constexpr std::size_t kChunkStart = 0x10;
    constexpr std::size_t kCipherOffset = kChunkStart + 0x20;
    constexpr std::size_t kCipherSize = 0x130;
    constexpr std::size_t kTagOffsetInPlain = 0xD0 - 0x20;
    constexpr int kDemangleKeyId = 0x55;

    if (psarSection.size < kCipherOffset + kCipherSize) {
        throw std::runtime_error("PSAR section too small to demangle the first block");
    }

    const std::vector<uint8_t> ciphertext(psarSection.data + kCipherOffset,
                                           psarSection.data + kCipherOffset + kCipherSize);
    const std::vector<uint8_t> plain = kirk.decryptCbc(ciphertext, kDemangleKeyId);

    return readU32LE(&plain[kTagOffsetInPlain]);
}

PsarPreamble readPsarPreamble(ByteSpan psarSection) {
    if (psarSection.size < 0x24) {
        throw std::runtime_error("PSAR section too small for preamble");
    }

    PsarPreamble preamble;
    const uint8_t* d = psarSection.data;

    preamble.magicValid = (d[0] == 'P' && d[1] == 'S' && d[2] == 'A' && d[3] == 'R');
    preamble.version = d[4];
    preamble.decrypted = readU32LE(&d[0x20]) == 0x2C333333u;

    return preamble;
}

std::vector<uint8_t> decodePsarBlock(ByteSpan psarSection, uint32_t offset, uint32_t cbIn,
                                      bool oldschool, crypto::KirkEngine& kirk) {
    if (cbIn == 0 || offset >= psarSection.size) {
        throw std::runtime_error("decodePsarBlock: bad offset/size");
    }
    const uint8_t* in = psarSection.data + offset;
    const std::size_t avail = psarSection.size - offset;
    if (cbIn > avail) {
        throw std::runtime_error("decodePsarBlock: block runs past the end of the PSAR section");
    }

    // PPSSPP's docs: "the decrypter reads a little past the end of the
    // block and uses what it finds there, so a decoder should hand it
    // about 16 bytes of slack" -- give it real trailing bytes when the
    // archive has them, zero-fill only the rest.
    const std::size_t slack = std::min<std::size_t>(0x10, avail - cbIn);
    std::vector<uint8_t> out(in, in + cbIn + slack);
    out.resize(static_cast<std::size_t>(cbIn) + 0x10, 0);

    if (!oldschool) {
        if (cbIn < 0x150) {
            throw std::runtime_error("decodePsarBlock: block too small to demangle");
        }
        // "Demangle": the 0x130 bytes at +0x20 are AES-128-CBC encrypted on
        // top of everything else, hiding the PRX tag at +0xD0 that says how
        // to decrypt the rest.
        kirk7(out.data() + 0x20, in + 0x20, 0x130, 0x55);
    }

    // Pass the real available size (cbIn + slack), not just cbIn -- CMD1's
    // own read-bounds check needs to see that the extra slack bytes above
    // are genuine, intentionally-provided data (real archive bytes, or an
    // explicit zero-fill), not treat them as absent.
    const int32_t decoded = crypto::decryptPrxType0(kirk, out.data(), out.data(), static_cast<uint32_t>(cbIn) + 0x10);
    if (decoded <= 0 || static_cast<std::size_t>(decoded) > out.size()) {
        throw std::runtime_error("decodePsarBlock: decrypted size out of range");
    }

    out.resize(static_cast<std::size_t>(decoded));
    return out;
}

std::string readFirmwareVersion(ByteSpan psarSection, crypto::KirkEngine& kirk) {
    const PsarPreamble preamble = readPsarPreamble(psarSection);
    if (!preamble.magicValid) {
        throw std::runtime_error("readFirmwareVersion: not a PSAR section");
    }

    const uint32_t overhead = preamble.decrypted ? 0 : 0x150;
    constexpr uint32_t kEntrySize = 0x110;
    const bool oldschool = preamble.version == 1;

    const std::vector<uint8_t> block = decodePsarBlock(psarSection, 0x10, overhead + kEntrySize, oldschool, kirk);
    if (block.size() != kEntrySize) {
        throw std::runtime_error("readFirmwareVersion: unexpected decoded size for the version block");
    }

    // jpcsp/ppsspp: this first record holds a text string at +0x10 whose
    // last comma-separated field is the firmware version, e.g. "...,6.61".
    const char* text = reinterpret_cast<const char*>(block.data() + 0x10);
    const std::size_t maxLen = kEntrySize - 0x10;
    std::size_t len = 0;
    while (len < maxLen && text[len] != '\0') {
        ++len;
    }

    const std::string line(text, len);
    const std::size_t comma = line.rfind(',');
    return comma == std::string::npos ? line : line.substr(comma + 1);
}

PsarReader::PsarReader(ByteSpan psarSection, crypto::KirkEngine& kirk)
    : psar_(psarSection), kirk_(kirk) {
    const PsarPreamble preamble = readPsarPreamble(psar_);
    if (!preamble.magicValid) {
        throw std::runtime_error("PsarReader: not a PSAR archive");
    }

    oldschool_ = preamble.version == 1;
    overhead_ = preamble.decrypted ? 0 : 0x150;

    // The header says how long the archive's records really are; both
    // real archives checked have a few bytes of padding after that which
    // don't decode as a record, so trust this over the raw file size.
    limit_ = static_cast<uint32_t>(psar_.size);
    if (psar_.size >= 0x0C) {
        const uint32_t declaredSize = readU32LE(psar_.data + 8);
        if (declaredSize >= 0x40 && declaredSize <= psar_.size) {
            limit_ = declaredSize;
        }
    }

    constexpr uint32_t kEntrySize = 0x110;
    const std::vector<uint8_t> block = decodePsarBlock(psar_, 0x10, overhead_ + kEntrySize, oldschool_, kirk_);
    if (block.size() != kEntrySize) {
        throw std::runtime_error("PsarReader: couldn't decrypt the PSAR's first block");
    }

    const char* text = reinterpret_cast<const char*>(block.data() + 0x10);
    std::size_t textLen = 0;
    while (textLen < kEntrySize - 0x10 && text[textLen] != '\0') {
        ++textLen;
    }
    const std::string firstLine(text, textLen);
    const std::size_t comma = firstLine.rfind(',');
    firmwareVersion_ = comma == std::string::npos ? firstLine : firstLine.substr(comma + 1);

    pos_ = 0x10 + overhead_ + kEntrySize;

    if (preamble.decrypted) {
        const uint32_t secondSize = readU32LE(block.data() + 0x90);
        const std::vector<uint8_t> block2 = decodePsarBlock(psar_, pos_, secondSize, oldschool_, kirk_);
        pos_ += overhead_ + static_cast<uint32_t>(block2.size());
        return;
    }

    if (!oldschool_) {
        // The second record's size isn't recorded anywhere, so try the
        // ones real updaters use: 100 covers most, 2.7x is bigger, and
        // some firmwares store it in the first record.
        const uint16_t sizeFromFirstBlock = static_cast<uint16_t>(block[0x90] | (block[0x91] << 8));
        const uint32_t candidates[] = {100, 144, sizeFromFirstBlock};
        bool ok = false;
        for (uint32_t candidate : candidates) {
            if (candidate == 0) {
                continue;
            }
            try {
                const std::vector<uint8_t> block2 = decodePsarBlock(psar_, pos_, overhead_ + candidate, oldschool_, kirk_);
                pos_ += overhead_ + ((static_cast<uint32_t>(block2.size()) + 15) & ~15u);
                ok = true;
                break;
            } catch (const std::exception&) {
                // Wrong guess for this candidate size; try the next one.
            }
        }
        if (!ok) {
            throw std::runtime_error("PsarReader: couldn't read the PSAR's second block");
        }
    }
}

std::optional<PsarEntry> PsarReader::next() {
    constexpr uint32_t kEntrySize = 0x110;

    if (static_cast<uint64_t>(pos_) + overhead_ + kEntrySize > limit_) {
        return std::nullopt;
    }

    const std::vector<uint8_t> block = decodePsarBlock(psar_, pos_, overhead_ + kEntrySize, oldschool_, kirk_);
    if (block.size() != kEntrySize) {
        throw std::runtime_error("PsarReader: couldn't decrypt the entry at " + std::to_string(pos_));
    }

    // A well-formed entry has a zero here; if it doesn't, position
    // tracking has desynced from the real record boundaries.
    if (readU32LE(block.data() + 0x100) != 0) {
        throw std::runtime_error("PsarReader: malformed entry at " + std::to_string(pos_) + " (lost sync)");
    }

    PsarEntry entry;
    const char* name = reinterpret_cast<const char*>(block.data() + 4);
    std::size_t nameLen = 0;
    while (nameLen < kEntrySize - 4 && name[nameLen] != '\0') {
        ++nameLen;
    }
    entry.shortName.assign(name, nameLen);

    pos_ += overhead_ + kEntrySize;

    const uint32_t chunkSize = readU32LE(block.data() + 0x104);
    const uint32_t expandedSize = readU32LE(block.data() + 0x108);

    if (expandedSize == 0) {
        entry.isDirectory = true;
    } else {
        const std::vector<uint8_t> content = decodePsarBlock(psar_, pos_, chunkSize, oldschool_, kirk_);
        if (content.size() >= 2 && content[0] == 0x78 && content[1] == 0x9C) {
            entry.data = util::inflateZlib(content.data(), content.size(), expandedSize);
        }
        // Other compression schemes (KL4E/KL3E/LZR) are detected but left
        // undecoded -- entry.data stays empty for those.
    }

    pos_ += chunkSize;
    return entry;
}

namespace {

// 6.x/5.x name-list entries are a bare 5-digit number from 1 to 12 --
// "00001" = 01g, "00002" = 02g, and so on (PPSSPP's docs). Directories
// and grouped 3.x-style names ("com:00123") never match this.
bool isNameListEntry(const std::string& shortName, int& generationOut) {
    if (shortName.size() != 5) {
        return false;
    }
    int value = 0;
    for (char c : shortName) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > 12) {
        return false;
    }
    generationOut = value;
    return true;
}

} // namespace

std::vector<ResolvedPsarEntry> resolvePsarEntries(ByteSpan psarSection, crypto::KirkEngine& kirk) {
    PsarReader reader(psarSection, kirk);
    const int keyIndex = tableKeyIndexForVersion(reader.firmwareVersion());

    std::map<int, NameTable> namesByModel;
    std::vector<ResolvedPsarEntry> results;

    while (std::optional<PsarEntry> entry = reader.next()) {
        ResolvedPsarEntry resolved;
        resolved.shortName = entry->shortName;
        resolved.isDirectory = entry->isDirectory;

        int generation = 0;
        if (!entry->isDirectory && !entry->data.empty() && isNameListEntry(entry->shortName, generation)) {
            NameTable table = decodeNameTable(entry->data, keyIndex, kirk);
            if (!table.empty()) {
                namesByModel[generation] = std::move(table);
                resolved.isNameTable = true;
            } else {
                // Didn't decode as a name table after all -- treat it as
                // an ordinary file, per PPSSPP's docs ("a list always
                // decrypts successfully... failing here means this is
                // just a file").
                resolved.data = std::move(entry->data);
            }
        } else {
            resolved.data = std::move(entry->data);
            if (!entry->isDirectory) {
                for (const auto& [generationKey, table] : namesByModel) {
                    (void)generationKey;
                    const auto found = table.find(entry->shortName);
                    if (found != table.end()) {
                        resolved.realName = found->second;
                        break;
                    }
                }
            }
        }

        results.push_back(std::move(resolved));
    }

    return results;
}

namespace {

// Turns a name out of the archive into a path relative to the output
// directory: "flash0:/font/ltn0.pgf" becomes "flash0/font/ltn0.pgf".
// Returns false for anything that could escape the output directory --
// these names come from a file we didn't write. Ported from PPSSPP's
// RelativePathFromEntryName.
bool relativePathFromEntryName(const std::string& name, std::string& out) {
    if (name.empty()) {
        return false;
    }
    std::string path = name;
    for (char& c : path) {
        if (c == ':' || c == '\\') {
            c = '/';
        }
    }
    std::string cleaned;
    cleaned.reserve(path.size());
    for (char c : path) {
        if (c == '/' && (cleaned.empty() || cleaned.back() == '/')) {
            continue;
        }
        cleaned.push_back(c);
    }
    if (cleaned.empty() || cleaned.find("..") != std::string::npos) {
        return false;
    }
    out = cleaned;
    return true;
}

} // namespace

std::vector<ResolvedPsarEntry> loadUnpackedEntries(const std::string& dir) {
    std::vector<ResolvedPsarEntry> out;
    namespace fs = std::filesystem;
    const fs::path root(dir);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        std::string rel = fs::relative(f, root, ec).generic_string();
        const std::size_t slash = rel.find('/');
        ResolvedPsarEntry e;
        if (slash == std::string::npos) {
            if (rel.empty() || !std::all_of(rel.begin(), rel.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
            e.shortName = rel;
        } else {
            e.realName = rel.substr(0, slash) + ":/" + rel.substr(slash + 1);
        }
        std::ifstream in(f, std::ios::binary);
        e.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (e.data.empty()) continue;
        out.push_back(std::move(e));
    }
    return out;
}

PsarInstallStats installPsar(ByteSpan psarSection, crypto::KirkEngine& kirk, const std::string& outputDir) {
    PsarInstallStats stats;
    const std::vector<ResolvedPsarEntry> entries = resolvePsarEntries(psarSection, kirk);
    const std::filesystem::path root(outputDir);

    for (const ResolvedPsarEntry& entry : entries) {
        ++stats.entries;

        if (entry.isDirectory || entry.isNameTable) {
            continue; // no content of its own to write
        }
        if (entry.data.empty()) {
            ++stats.failed; // had a real slot in the archive but no usable content
            continue;
        }

        std::string name = entry.realName;
        if (name.empty()) {
            ++stats.unnamed;
            name = entry.shortName;
        }

        std::string relative;
        if (!relativePathFromEntryName(name, relative)) {
            ++stats.failed;
            continue;
        }

        const std::filesystem::path destination = root / relative;
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            ++stats.failed;
            continue;
        }

        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out) {
            ++stats.failed;
            continue;
        }
        out.write(reinterpret_cast<const char*>(entry.data.data()), static_cast<std::streamsize>(entry.data.size()));
        if (!out) {
            ++stats.failed;
            continue;
        }

        ++stats.written;
    }

    return stats;
}

} // namespace beta::format
