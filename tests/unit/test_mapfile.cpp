// Unit tests for the pure logic in mapfile.cpp: hex/unhex round-trip, the
// printable() truncation/escaping rules, and the Map save/load round-trip
// that is the actual contract between `obfuscate` and `verify`.
#include "check.hpp"

#include "../../src/mapfile.cpp"

#include <filesystem>

using namespace shroud;

void test_mapfile() {
    // hex/unhex round-trip, including a NUL and a high-bit byte.
    {
        const std::string s = std::string("ab") + '\0' + "\xff\x01";
        CHECK(unhex(hex(s)) == s);
        CHECK(hex(std::string()).empty());
    }

    // nibble: valid digits in all three cases, invalid -> -1.
    {
        CHECK(nibble('0') == 0);
        CHECK(nibble('9') == 9);
        CHECK(nibble('a') == 10);
        CHECK(nibble('f') == 15);
        CHECK(nibble('A') == 10);
        CHECK(nibble('F') == 15);
        CHECK(nibble('g') == -1);
        CHECK(nibble(' ') == -1);
    }

    // unhex: odd length or a non-hex character makes the whole result empty,
    // not a partial decode (the map line would silently point at wrong bytes).
    {
        CHECK(unhex("41").size() == 1 && unhex("41")[0] == 'A');
        CHECK(unhex("4").empty());
        CHECK(unhex("4g").empty());
    }

    // printable: control bytes and quotes become '.', tab/space stay space,
    // and it hard-truncates at 60 visible characters with "...".
    {
        // Split after \x01: a hex escape consumes every following hex digit,
        // and 'd' is one, so "\x01d" alone would decode as \x1d, not \x01+'d'.
        CHECK(printable("a\"b\tc\x01" "d") == "a.b c.d");
        const std::string longStr(100, 'x');
        const std::string p = printable(longStr);
        CHECK(p.size() == 63);  // 60 'x' + "..."
        CHECK(p.substr(60) == "...");
    }

    // Map::addName/addString ignore empty input (nothing to look for).
    {
        Map m;
        m.addName("");
        m.addString("");
        CHECK(m.entries.empty());
    }

    // Map save/load round-trip: this file format is the contract between
    // `obfuscate` (writer) and `verify` (reader); a drift here is silent until
    // someone's CI gate stops catching real leaks.
    {
        Map m;
        m.seed = 0x1234abcd;
        m.addName("myFunction");
        m.addString(std::string("secret\0tail", 11));  // bytes containing a NUL

        const auto path = std::filesystem::temp_directory_path() / "shroud_unit_mapfile_test.map";
        CHECK(m.save(path.string()));

        Map loaded;
        CHECK(loaded.load(path.string()));
        CHECK(loaded.seed == m.seed);
        CHECK(loaded.entries.size() == m.entries.size());
        bool sawName = false, sawString = false;
        for (const auto& e : loaded.entries) {
            if (e.kind == MapEntry::Name && e.bytes == std::string("myFunction\0", 11)) sawName = true;
            if (e.kind == MapEntry::String && e.bytes == std::string("secret\0tail", 11)) sawString = true;
        }
        CHECK(sawName);
        CHECK(sawString);

        std::filesystem::remove(path);
    }

    // load() on a missing file fails cleanly (false), not a crash or a silent
    // empty-but-"ok" map that would make `verify` pass with nothing checked.
    {
        Map m;
        CHECK(!m.load("/this/path/does/not/exist/shroud.map"));
    }
}
