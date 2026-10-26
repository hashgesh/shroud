// Unit tests for the pure logic in verify.cpp: the boundary checks that
// decide whether a byte match is a real leak or a coincidence (a longer
// identifier, a section name, a substring of another word).
#include "check.hpp"

#include "../../src/verify.cpp"

using namespace shroud;
using namespace std::string_literals;

void test_verify() {
    // identByte: the character classes a symbol/identifier is made of.
    {
        CHECK(identByte('a'));
        CHECK(identByte('Z'));
        CHECK(identByte('9'));
        CHECK(identByte('_'));
        CHECK(identByte('$'));
        CHECK(!identByte(' '));
        CHECK(!identByte('.'));
        CHECK(!identByte('/'));
    }

    // textHit: a literal is a hit unless it is glued to more identifier/word
    // text on either side (i.e. it is a substring of a longer word).
    {
        const std::string image = "xxbinaryxx \0binary\0 binary.cpp"s;
        // "binary" at offset 2 is glued on both sides -> not a hit.
        CHECK(!textHit(image, 2, "binary"));
        // "binary" at the NUL-bounded position -> a real hit.
        const size_t standalone = image.find("binary", 3);
        CHECK(textHit(image, standalone, "binary"));
        // "binary" immediately followed by ".cpp" -> glued by the '.', not a hit
        // (wordByte treats '.' as part of the word, matching a file name).
        const size_t withExt = image.rfind("binary");
        CHECK(!textHit(image, withExt, "binary"));
    }

    // findSymbol: "name\0" only counts when it is not the tail of a longer
    // identifier and not a section name (preceded by '.'). A symbol table
    // entry is itself NUL-delimited, so put a real entry before it, like a
    // real .strtab/.dynstr would have ("x\0filter\0...").
    {
        const std::string nameNul = std::string("filter") + '\0';
        const std::string prevEntry = std::string("x") + '\0';
        // Standalone: its own NUL-delimited entry.
        CHECK(findSymbol(prevEntry + nameNul, nameNul) != std::string::npos);
        // Glued to a longer identifier ("myfilter\0"): not a match for "filter\0".
        CHECK(findSymbol(prevEntry + "my" + nameNul, nameNul) == std::string::npos);
        // Section name (".filter\0", e.g. ".data"-style): not a symbol.
        CHECK(findSymbol(prevEntry + "." + nameNul, nameNul) == std::string::npos);
    }

    // humanSize: the three size bands and their rounding.
    {
        CHECK(humanSize(500) == "500 B");
        CHECK(humanSize(2048) == "2.0 KiB");
        CHECK(humanSize(1u << 20) == "1.0 MiB");
    }

    // preview: non-printable bytes become '.', hard-truncated at 48 + "...".
    {
        CHECK(preview("abc\x01\x02") == "abc..");
        const std::string longStr(60, 'y');
        const std::string p = preview(longStr);
        CHECK(p.size() == 51);  // 48 'y' + "..."
    }

    // readUInt: little-endian, and out-of-range reads return 0 instead of
    // reading past the buffer.
    {
        const std::string s = std::string("\x01\x02\x03\x04", 4);
        CHECK(readUInt(s, 0, 2) == 0x0201);
        CHECK(readUInt(s, 0, 4) == 0x04030201u);
        CHECK(readUInt(s, 2, 4) == 0);  // off+bytes > s.size()
    }
}
