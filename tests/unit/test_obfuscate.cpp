// Unit tests for the pure logic in obfuscate.cpp that does not need a real
// libclang parse: escape-sequence decoding, the comment/string masking used
// to scan for #if groups, and the edit-assembly step (overlap drop, wrap
// offset shifting) that turns edits into the final source text.
#include "check.hpp"

#include "../../src/obfuscate.cpp"

using namespace shroud;

void test_obfuscate() {
    // literalBytes: escapes, raw strings, concatenation, and rejecting
    // anything it does not fully understand (a suffix, an unterminated quote).
    {
        std::string bytes;
        CHECK(literalBytes("\"hello\"", bytes) && bytes == "hello");

        bytes.clear();
        CHECK(literalBytes("\"a\" \"b\"", bytes) && bytes == "ab");  // adjacent concatenation

        bytes.clear();
        CHECK(literalBytes("\"\\n\\t\\\\\\\"\"", bytes) && bytes == "\n\t\\\"");

        bytes.clear();
        CHECK(literalBytes("\"\\x41\\x42\"", bytes) && bytes == "AB");  // hex escape

        bytes.clear();
        CHECK(literalBytes("\"\\101\\102\"", bytes) && bytes == "AB");  // octal escape

        bytes.clear();
        CHECK(literalBytes("\"\\u00e9\"", bytes) && bytes == "\xc3\xa9");  // UTF-8 encoding of e-acute

        bytes.clear();
        CHECK(literalBytes("R\"(raw\\nstring)\"", bytes) && bytes == "raw\\nstring");  // literal backslash-n

        bytes.clear();
        CHECK(!literalBytes("\"x\"s", bytes));    // trailing suffix: not a plain literal
        CHECK(!literalBytes("\"abc", bytes));     // unterminated
        CHECK(!literalBytes("x", bytes));         // not a literal at all
    }

    // udlSplit: only a token ending in a '_'-prefixed suffix identifier counts.
    {
        std::string bytes, suffix;
        CHECK(udlSplit("\"x\"_sr", bytes, suffix) && bytes == "x" && suffix == "_sr");
        bytes.clear();
        suffix.clear();
        CHECK(!udlSplit("\"x\"", bytes, suffix));  // no suffix at all
    }

    // maskedSource: comments and literal bodies become spaces (same length,
    // same line count), but preprocessor text and newlines survive untouched
    // for the #if scan that runs on the masked copy.
    {
        const std::string src =
            "#if X\n"
            "int a = 1; // comment with #if inside, must not confuse the scan\n"
            "char c = 'q';\n"
            "const char* s = \"text with #endif inside\";\n"
            "#endif\n";
        const std::string m = maskedSource(src);
        CHECK(m.size() == src.size());
        // Same number of lines: masking must not eat newlines.
        CHECK(std::count(m.begin(), m.end(), '\n') == std::count(src.begin(), src.end(), '\n'));
        // The directives themselves are untouched...
        CHECK(m.find("#if X") != std::string::npos);
        CHECK(m.find("#endif") != std::string::npos);
        // ...but the fake "#if"/"#endif" text inside the comment and the
        // string literal must be gone (blanked), or conditionalRanges() would
        // see four markers instead of two and mis-pair them.
        CHECK(m.find("#if", m.find("#if X") + 1) == std::string::npos);
        size_t afterFirstEndif = m.find("#endif") + 6;
        CHECK(m.find("#endif", afterFirstEndif) == std::string::npos);
    }

    // conditionalRanges: a simple group, and a nested group collapsed into one
    // outer range (the active/inactive branch distinction isn't visible here,
    // only the whole #if...#endif span matters to the caller).
    {
        const std::string m = "a\n#if X\nb\n#endif\nc\n";
        const auto ranges = conditionalRanges(m);
        CHECK(ranges.size() == 1);
        if (ranges.size() == 1) {
            CHECK(m.compare(ranges[0].first, 6, "#if X\n") == 0);
        }

        const std::string nested = "#if A\n#if B\nx\n#endif\n#endif\n";
        const auto nestedRanges = conditionalRanges(nested);
        CHECK(nestedRanges.size() == 1);  // one merged range, not two
    }

    // assemble: non-overlapping edits apply in the right place...
    {
        Ctx ctx;
        std::string src = "0123456789";
        ctx.src = &src;
        ctx.edits.push_back({0, 2, "ab"});
        ctx.edits.push_back({4, 2, "XYZ"});
        CHECK(assemble(ctx) == "ab23XYZ6789");
    }
    // ...an edit that overlaps a higher (already-applied) one is dropped
    // rather than corrupting the text, since edits apply high-offset first.
    {
        Ctx ctx;
        std::string src = "0123456789";
        ctx.src = &src;
        ctx.edits.push_back({4, 2, "XYZ"});  // applied first (higher offset)
        ctx.edits.push_back({3, 3, "bad"});  // overlaps [4,6) -> must be dropped
        // The second edit's range [3,6) runs into the floor left by the first
        // edit (4), so it is dropped rather than corrupting the text.
        CHECK(assemble(ctx) == "0123XYZ6789");
    }
    // ...a wrap around a region that itself contains an edit: the wrapped
    // text must be the *edited* text, and the branch shape must be present.
    {
        Ctx ctx;
        std::string src = "pre; mid; post;";
        ctx.src = &src;
        ctx.edits.push_back({5, 3, "XYZ"});     // "mid" -> "XYZ", inside the wrap
        ctx.wraps.push_back({5, 9, 0});          // wraps "mid;" (end includes ';')
        const std::string out = assemble(ctx);
        CHECK(out.find("::shd::opaque()") != std::string::npos);
        CHECK(out.find("XYZ;") != std::string::npos);  // the edited text, wrapped
        CHECK(out.find("mid;") == std::string::npos);  // not the original text
    }
}
