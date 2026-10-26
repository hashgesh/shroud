#include "shroud.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace shroud {
namespace {

std::string readAll(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = true;
    return s;
}

std::vector<size_t> findAll(const std::string& hay, const std::string& needle, size_t limit = 3) {
    std::vector<size_t> hits;
    if (needle.empty()) return hits;
    for (size_t pos = hay.find(needle); pos != std::string::npos && hits.size() < limit;
         pos = hay.find(needle, pos + 1)) {
        hits.push_back(pos);
    }
    return hits;
}

bool identByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

// A symbol table entry is `name\0`, but prose strings ("the name\0") match too.
// ELF/MSVC symbol names never contain whitespace, quotes or slashes, so use that
// to tell a symbol from the tail of a message.
bool symbolLike(const std::string& image, size_t pos, size_t len) {
    size_t b = pos;
    while (b > 0 && image[b - 1] != '\0') --b;
    for (size_t i = b; i < pos + len; ++i) {
        const unsigned char c = static_cast<unsigned char>(image[i]);
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '"' || c == '\'') return false;
    }
    return true;
}

// A symbol table entry is `name\0`, but `name\0` also matches the tail of a
// longer identifier ("__libc_start_main" contains "in"), so require a boundary.
size_t findSymbol(const std::string& image, const std::string& nameNul) {
    for (size_t pos = image.find(nameNul); pos != std::string::npos; pos = image.find(nameNul, pos + 1)) {
        // '.' rules out ELF section names (".data", ".comment"), which are not
        // symbols and cannot be renamed by any source transform.
        if (pos > 0 && (identByte(static_cast<unsigned char>(image[pos - 1])) || image[pos - 1] == '.')) continue;
        if (symbolLike(image, pos, nameNul.size() - 1)) return pos;
    }
    return std::string::npos;
}

// A plain-text literal survives only if it is not a substring of a longer word:
// "binary" also matches the file name "binary.cpp" stored in the object.
bool wordByte(unsigned char c) {
    return identByte(c) || c == '.';
}

bool textHit(const std::string& image, size_t pos, const std::string& s) {
    if (pos > 0 && identByte(static_cast<unsigned char>(s.front())) &&
        wordByte(static_cast<unsigned char>(image[pos - 1])))
        return false;
    const size_t after = pos + s.size();
    if (after < image.size() && identByte(static_cast<unsigned char>(s.back())) &&
        wordByte(static_cast<unsigned char>(image[after])))
        return false;
    return true;
}

std::string humanSize(size_t n) {
    char buf[32];
    if (n >= (1u << 20)) std::snprintf(buf, sizeof buf, "%.1f MiB", double(n) / (1u << 20));
    else if (n >= (1u << 10)) std::snprintf(buf, sizeof buf, "%.1f KiB", double(n) / (1u << 10));
    else std::snprintf(buf, sizeof buf, "%zu B", n);
    return buf;
}

std::string preview(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= 48) {
            out += "...";
            break;
        }
        out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.');
    }
    return out;
}

uint64_t readUInt(const std::string& s, size_t off, unsigned bytes) {
    if (off + bytes > s.size()) return 0;
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; ++i) v |= uint64_t(static_cast<unsigned char>(s[off + i])) << (8 * i);
    return v;
}

// Real DWARF sections, not the string ".debug_info" that backtrace code embeds.
// ELF section table is parsed; Mach-O and PE are recognised by their markers.
bool hasDebugInfo(const std::string& image) {
    if (image.find("__debug_info") != std::string::npos) return true;
    if (image.find(".pdb") != std::string::npos || image.find("RSDS") != std::string::npos) return true;
    if (image.size() < 64 || image.compare(0, 4, "\x7f" "ELF", 4) != 0) return false;

    const bool wide = static_cast<unsigned char>(image[4]) == 2;
    if (!wide && static_cast<unsigned char>(image[4]) != 1) return false;
    if (static_cast<unsigned char>(image[5]) != 1) return false; // little endian only

    const uint64_t shoff = readUInt(image, wide ? 0x28 : 0x20, wide ? 8 : 4);
    const uint64_t shentsize = readUInt(image, wide ? 0x3A : 0x2E, 2);
    const uint64_t shnum = readUInt(image, wide ? 0x3C : 0x30, 2);
    const uint64_t shstrndx = readUInt(image, wide ? 0x3E : 0x32, 2);
    if (!shoff || !shentsize || !shnum || shstrndx >= shnum) return false;

    const size_t sec = static_cast<size_t>(shoff + shstrndx * shentsize);
    const uint64_t strOff = readUInt(image, sec + (wide ? 0x18 : 0x10), wide ? 8 : 4);
    const uint64_t strSize = readUInt(image, sec + (wide ? 0x20 : 0x14), wide ? 8 : 4);
    if (!strOff || !strSize || strOff + strSize > image.size()) return false;

    for (uint64_t i = 0; i < shnum; ++i) {
        const size_t h = static_cast<size_t>(shoff + i * shentsize);
        const uint64_t nameOff = readUInt(image, h, 4);
        const uint64_t size = readUInt(image, h + (wide ? 0x20 : 0x14), wide ? 8 : 4);
        if (!size || nameOff >= strSize) continue;
        const size_t at = static_cast<size_t>(strOff + nameOff);
        const size_t end = image.find('\0', at);
        if (end == std::string::npos) continue;
        if (image.compare(at, 7, ".debug_") == 0) return true;
    }
    return false;
}

} // namespace

int verify(const std::string& binary, const std::string& mapPath, const std::string& baseline) {
    Map map;
    if (!map.load(mapPath)) {
        std::fprintf(stderr, "shroud: cannot read map %s\n", mapPath.c_str());
        return 2;
    }
    bool ok = false;
    const std::string image = readAll(binary, ok);
    if (!ok) {
        std::fprintf(stderr, "shroud: cannot read %s\n", binary.c_str());
        return 2;
    }

    size_t names = 0, strings = 0, leakedNames = 0, leakedStrings = 0, suspicious = 0, tooShort = 0;
    size_t weakStrings = 0, debugNames = 0;
    const bool debug = hasDebugInfo(image);
    std::vector<std::string> leaks;
    for (const auto& e : map.entries) {
        if (e.kind == MapEntry::Name) {
            // One or two bytes followed by a NUL occur by chance in any binary,
            // so only longer names give a usable signal.
            if (e.label.size() < 4) {
                ++tooShort;
                continue;
            }
            ++names;
            // Debug info keeps every identifier of every inlined header
            // function, including names this run never could rename.
            if (debug) {
                ++debugNames;
                continue;
            }
            if (findSymbol(image, e.bytes) != std::string::npos) {
                ++leakedNames;
                if (leaks.size() < 5) leaks.push_back("name survives: " + e.label);
            }
            if (e.label.size() >= 6 && !findAll(image, e.label, 1).empty()) ++suspicious;
        } else {
            // Short byte runs ("--", " for ") occur in unrelated literals and
            // padding of any real binary, so they prove nothing either way.
            // An all-NUL literal is the same: alignment padding is full of them.
            if (e.bytes.size() < 6 || e.bytes.find_first_not_of('\0') == std::string::npos) {
                ++weakStrings;
                continue;
            }
            ++strings;
            // Scan every occurrence, not just the first few: a literal can appear
            // many times inside longer messages (not a leak) and still have one
            // standalone NUL terminated copy that is. Capping the scan could pass a
            // real leak. Stop at the first fatal hit.
            for (size_t h = image.find(e.bytes); h != std::string::npos; h = image.find(e.bytes, h + 1)) {
                if (!textHit(image, h, e.bytes)) continue;
                // A transformed literal is either gone or present as its own NUL
                // terminated entry. A hit inside a longer message (a header string
                // that happens to contain the same words) is not a leak.
                if ((h > 0 && image[h - 1] != '\0') ||
                    (h + e.bytes.size() < image.size() && image[h + e.bytes.size()] != '\0'))
                    continue;
                ++leakedStrings;
                if (leaks.size() < 5) {
                    char where[32];
                    std::snprintf(where, sizeof where, "0x%zx", h);
                    leaks.push_back(std::string("literal survives at ") + where + ": \"" + preview(e.bytes) + "\"");
                }
                break;
            }
        }
    }

    const bool hasBaseline = !baseline.empty();
    size_t baseSize = 0;
    if (hasBaseline) {
        bool bok = false;
        const std::string base = readAll(baseline, bok);
        if (bok) baseSize = base.size();
    }

    std::printf("shroud verify\n");
    std::printf("  binary   : %s (%s)\n", binary.c_str(), humanSize(image.size()).c_str());
    std::printf("  map      : %s (seed 0x%08x, %zu names, %zu strings)\n", mapPath.c_str(), map.seed, names,
                strings);
    std::printf("  names    : ");
    if (debug) {
        std::printf("not conclusive (%zu skipped because debug info is present)\n", debugNames);
    } else {
        std::printf("%zu/%zu absent", names - leakedNames, names);
        if (tooShort) std::printf(" (%zu too short to check)", tooShort);
        if (suspicious) std::printf(" (%zu still appear inside longer strings, check them)", suspicious);
        std::printf("\n");
    }
    std::printf("  strings  : %zu/%zu absent", strings - leakedStrings, strings);
    if (weakStrings) std::printf(" (%zu too short or all-NUL to check)", weakStrings);
    std::printf("\n");
    std::printf("  debug    : %s\n", debug ? "present (removes names from a stripped build)" : "absent");
    if (baseSize)
        std::printf("  baseline : %s -> %+.1f%%\n", humanSize(baseSize).c_str(),
                    100.0 * (double(image.size()) - double(baseSize)) / double(baseSize));
    for (const auto& l : leaks) std::printf("  leak     : %s\n", l.c_str());

    if (leakedStrings || leakedNames) {
        std::printf("  result   : FAIL (%zu literal(s), %zu name(s) still recoverable)\n", leakedStrings,
                    leakedNames);
        return 1;
    }
    if (debug) std::printf("  result   : PASS, but strip debug info before shipping\n");
    else std::printf("  result   : PASS\n");
    return 0;
}

} // namespace shroud
