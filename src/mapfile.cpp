#include "shroud.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace shroud {
namespace {

std::string hex(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string unhex(const std::string& s) {
    std::string out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '"') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ' || c == '\t') {
            out.push_back(' ');
        } else {
            out.push_back('.');
        }
        if (out.size() >= 60) {
            out += "...";
            break;
        }
    }
    return out;
}

} // namespace

void Map::addName(const std::string& name) {
    if (name.empty()) return;
    entries.push_back({MapEntry::Name, name, name + '\0'});
}

void Map::addString(const std::string& bytes) {
    if (bytes.empty()) return;
    entries.push_back({MapEntry::String, printable(bytes), bytes});
}

bool Map::save(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "shroud-map 1\n";
    char seed[32];
    std::snprintf(seed, sizeof seed, "seed 0x%08x\n", this->seed);
    out << seed;
    std::set<std::string> written;
    for (const auto& e : entries) {
        const char* tag = e.kind == MapEntry::Name ? "sym" : "str";
        const std::string key = std::string(tag) + hex(e.bytes);
        if (!written.insert(key).second) continue;
        out << tag << ' ' << hex(e.bytes) << "  # " << e.label << '\n';
    }
    return out.good();
}

bool Map::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    entries.clear();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tag, payload;
        ls >> tag >> payload;
        if (tag == "seed") {
            seed = static_cast<unsigned>(std::strtoul(payload.c_str(), nullptr, 0));
        } else if (tag == "sym" || tag == "str") {
            MapEntry e;
            e.kind = tag == "sym" ? MapEntry::Name : MapEntry::String;
            e.bytes = unhex(payload);
            if (e.bytes.empty()) continue;
            std::string rest;
            std::getline(ls, rest);
            const size_t hash = rest.find('#');
            e.label = hash == std::string::npos ? printable(e.bytes) : rest.substr(hash + 1);
            const size_t first = e.label.find_first_not_of(" \t");
            e.label = first == std::string::npos ? printable(e.bytes) : e.label.substr(first);
            entries.push_back(std::move(e));
        }
    }
    return true;
}

} // namespace shroud
