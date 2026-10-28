#include "shroud.hpp"

#include "shroud/runtime.hpp" // shd::ks, so generator and runtime cannot drift

#include <clang-c/Index.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Resolving the packaged resource-dir relative to the running binary needs the
// executable's own path, which has no portable API.
#if defined(_WIN32)
// windows.h defines min()/max() macros that break std::min/std::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace shroud {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- utilities

std::string toStd(CXString s) {
    std::string r;
    if (const char* p = clang_getCString(s)) r = p;
    clang_disposeString(s);
    return r;
}

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    auto p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t h = a ^ (b + 0x9e3779b97f4a7c15ull);
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

std::string readFile(const fs::path& p, bool& ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = true;
    return s;
}

bool writeFile(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool isMainFile(CXCursor c) {
    return clang_Location_isFromMainFile(clang_getCursorLocation(c)) != 0;
}

// True when the token literally lives in this file, i.e. it is not the
// expansion of a macro. Editing macro expansions would rewrite the wrong text.
bool isPlain(CXCursor c) {
    CXSourceLocation loc = clang_getCursorLocation(c);
    CXFile exp = nullptr, spell = nullptr;
    unsigned l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    unsigned o1 = 0, o2 = 0;
    clang_getExpansionLocation(loc, &exp, &l1, &c1, &o1);
    clang_getSpellingLocation(loc, &spell, &l2, &c2, &o2);
    if (!exp || !spell || o1 != o2) return false;
    return clang_File_isEqual(exp, spell) != 0;
}

bool extentOffsets(CXCursor c, size_t& b, size_t& e) {
    CXSourceRange r = clang_getCursorExtent(c);
    CXFile f = nullptr;
    unsigned line = 0, col = 0, begin = 0, end = 0;
    clang_getExpansionLocation(clang_getRangeStart(r), &f, &line, &col, &begin);
    clang_getExpansionLocation(clang_getRangeEnd(r), &f, &line, &col, &end);
    b = begin;
    e = end;
    return e >= b;
}

size_t cursorOffset(CXCursor c) {
    CXFile f = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getExpansionLocation(clang_getCursorLocation(c), &f, &line, &col, &off);
    return off;
}

bool isTemplate(CXCursor c) {
    switch (clang_getCursorKind(c)) {
    case CXCursor_FunctionTemplate:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
        return true;
    default:
        return false;
    }
}

bool isFunctionDefinitionKind(CXCursorKind k) {
    switch (k) {
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
    case CXCursor_ConversionFunction:
        return true;
    default:
        return false;
    }
}

// Walks up the nesting chain. Only the semantic parent is usable here: the
// lexical parent returns an invalid cursor for expressions.
template <typename Fn>
void forEnclosing(CXCursor c, Fn fn) {
    for (CXCursor p = clang_getCursorSemanticParent(c);; p = clang_getCursorSemanticParent(p)) {
        const CXCursorKind k = clang_getCursorKind(p);
        if (clang_Cursor_isNull(p) || k == CXCursor_TranslationUnit || k == CXCursor_InvalidFile) return;
        if (fn(p)) return;
    }
}

// ---------------------------------------------------------------- context

struct Edit {
    size_t begin;
    size_t len;
    std::string text;
};

struct Wrap {
    size_t begin; // original offset of the statement
    size_t end;
    size_t id;
};

struct Ctx {
    CXTranslationUnit tu = nullptr;
    const std::string* src = nullptr;
    unsigned seed = 0;
    unsigned flowPerFunction = 2;
    bool doRename = false;
    bool doStrings = false;
    bool doFlow = false;
    std::vector<std::string> functions;    // --functions; empty means the whole file
    std::set<std::string> usedFunctions;   // names that matched a real body, for typo warnings

    std::vector<CXCursor> stack;                               // ancestors of the node being handled
    std::map<size_t, std::string> names;                       // canonical decl offset -> new spelling
    std::vector<std::pair<std::string, std::string>> nameLog;  // (old, new)
    std::set<size_t> macroUsed;                                // decls referenced from a macro expansion
    std::set<std::string> lambdaCaptured;                      // names spelled only in a capture list
    std::vector<Edit> edits;
    std::vector<Wrap> wraps;
    std::vector<std::string> plaintexts;
    unsigned encrypted = 0;
    unsigned wrapped = 0;
};

void addEdit(Ctx& ctx, size_t begin, size_t len, std::string text) {
    if (begin + len > ctx.src->size()) return;
    ctx.edits.push_back({begin, len, std::move(text)});
}

// With --functions, nothing outside the listed function bodies may be touched.
// The ancestor stack is authoritative for the recursive walk; the semantic
// parent chain is the fallback for passes that visit the translation unit with
// clang_visitChildren, where libclang does not report expression parents.
bool selectedFunction(const Ctx& ctx, CXCursor c) {
    if (ctx.functions.empty()) return true;
    auto named = [&](CXCursor p) {
        if (!isFunctionDefinitionKind(clang_getCursorKind(p))) return false;
        const std::string name = toStd(clang_getCursorSpelling(p));
        return std::find(ctx.functions.begin(), ctx.functions.end(), name) != ctx.functions.end();
    };
    if (named(c)) return true;
    for (auto it = ctx.stack.rbegin(); it != ctx.stack.rend(); ++it)
        if (named(*it)) return true;
    for (CXCursor p = clang_getCursorSemanticParent(c); !clang_Cursor_isNull(p);
         p = clang_getCursorSemanticParent(p)) {
        if (clang_getCursorKind(p) == CXCursor_TranslationUnit) break;
        if (named(p)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- pass 1: names

bool renameable(CXCursor c) {
    if (!isMainFile(c) || !isPlain(c)) return false;
    CXLinkageKind lk = clang_getCursorLinkage(c);
    if (lk == CXLinkage_External || lk == CXLinkage_UniqueExternal) return false;
    const std::string name = toStd(clang_getCursorSpelling(c));
    if (name.empty() || name == "main") return false;
    if (name.rfind("operator", 0) == 0) return false;
    if (name.rfind("~", 0) == 0) return false;
    // Members of a class or a template have to stay identical wherever the
    // class is seen, and both may be textually replaced per instantiation.
    if (clang_Cursor_getNumTemplateArguments(c) >= 0) return false;
    bool inTemplate = false;
    forEnclosing(c, [&](CXCursor p) {
        if (isTemplate(p)) {
            inTemplate = true;
            return true;
        }
        return false;
    });
    return !inTemplate;
}

std::string makeName(unsigned seed, size_t offset, const std::string& old) {
    uint64_t h = mix(fnv1a(&offset, sizeof offset, seed), fnv1a(old.data(), old.size()));
    char buf[32];
    std::snprintf(buf, sizeof buf, "shd_%012llx", static_cast<unsigned long long>(h & 0xFFFFFFFFFFFFull));
    return buf;
}

CXChildVisitResult collectNames(CXCursor c, CXCursor, CXClientData d) {
    Ctx& ctx = *static_cast<Ctx*>(d);
    const CXCursorKind k = clang_getCursorKind(c);
    if (k != CXCursor_VarDecl && k != CXCursor_ParmDecl && k != CXCursor_FunctionDecl) {
        return CXChildVisit_Recurse;
    }
    // A function name is also spelled at every call site, and those may sit
    // outside the selected functions, so a scoped run leaves function names alone.
    if (k == CXCursor_FunctionDecl && !ctx.functions.empty()) return CXChildVisit_Recurse;
    if (!selectedFunction(ctx, c) || !renameable(c)) return CXChildVisit_Recurse;

    CXCursor canon = clang_getCanonicalCursor(c);
    if (!clang_Cursor_isNull(canon) && isMainFile(canon) && isPlain(canon)) {
        const size_t off = cursorOffset(canon);
        if (ctx.names.find(off) == ctx.names.end()) {
            const std::string old = toStd(clang_getCursorSpelling(canon));
            if (!old.empty()) {
                ctx.names[off] = makeName(ctx.seed, off, old);
                ctx.nameLog.emplace_back(old, ctx.names[off]);
            }
        }
    }
    return CXChildVisit_Recurse;
}

// ---------------------------------------------------------------- pass 2: edits

// A reference that lives inside a macro expansion cannot be renamed (the macro
// body is not this file), so the declaration has to keep its name too. Logging
// macros such as NS_LOG_FUNCTION(foo) spell their arguments this way.
CXChildVisitResult findMacroUses(CXCursor c, CXCursor, CXClientData d) {
    Ctx& ctx = *static_cast<Ctx*>(d);
    if (clang_getCursorKind(c) != CXCursor_DeclRefExpr || isPlain(c)) return CXChildVisit_Recurse;
    CXCursor ref = clang_getCanonicalCursor(clang_getCursorReferenced(c));
    if (clang_Cursor_isNull(ref)) return CXChildVisit_Recurse;
    const size_t off = cursorOffset(ref);
    if (ctx.names.find(off) != ctx.names.end()) ctx.macroUsed.insert(off);
    return CXChildVisit_Recurse;
}

void dropMacroUsedNames(Ctx& ctx) {
    clang_visitChildren(clang_getTranslationUnitCursor(ctx.tu), findMacroUses, &ctx);
    for (size_t off : ctx.macroUsed) ctx.names.erase(off);

    std::set<std::string> live;
    for (const auto& [off, name] : ctx.names) live.insert(name);
    ctx.nameLog.erase(std::remove_if(ctx.nameLog.begin(), ctx.nameLog.end(),
                                     [&](const auto& e) { return !live.count(e.second); }),
                      ctx.nameLog.end());
}

bool identChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

// libclang exposes no cursor for a lambda capture, so the names in `[...]`
// cannot be rewritten. A captured variable therefore keeps its spelling.
CXChildVisitResult findLambdaCaptures(CXCursor c, CXCursor, CXClientData d) {
    Ctx& ctx = *static_cast<Ctx*>(d);
    if (clang_getCursorKind(c) != CXCursor_LambdaExpr || !isMainFile(c) || !isPlain(c))
        return CXChildVisit_Recurse;
    size_t b = 0, e = 0;
    if (!extentOffsets(c, b, e)) return CXChildVisit_Recurse;
    const size_t open = ctx.src->find('[', b);
    if (open == std::string::npos || open >= e) return CXChildVisit_Recurse;
    size_t depth = 0, close = std::string::npos;
    for (size_t i = open; i < e; ++i) {
        if ((*ctx.src)[i] == '[') {
            ++depth;
        } else if ((*ctx.src)[i] == ']' && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos) return CXChildVisit_Recurse;
    for (size_t i = open + 1; i < close;) {
        if (!identChar((*ctx.src)[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < close && identChar((*ctx.src)[j])) ++j;
        ctx.lambdaCaptured.insert(ctx.src->substr(i, j - i));
        i = j;
    }
    return CXChildVisit_Recurse;
}

void dropLambdaCapturedNames(Ctx& ctx) {
    clang_visitChildren(clang_getTranslationUnitCursor(ctx.tu), findLambdaCaptures, &ctx);
    std::set<std::string> drop;
    for (const auto& [old, fresh] : ctx.nameLog)
        if (ctx.lambdaCaptured.count(old)) drop.insert(fresh);
    if (drop.empty()) return;
    for (auto it = ctx.names.begin(); it != ctx.names.end();) {
        if (drop.count(it->second)) it = ctx.names.erase(it); else ++it;
    }
    std::set<std::string> live;
    for (const auto& [off, name] : ctx.names) live.insert(name);
    ctx.nameLog.erase(std::remove_if(ctx.nameLog.begin(), ctx.nameLog.end(),
                                     [&](const auto& e) { return !live.count(e.second); }),
                      ctx.nameLog.end());
    std::fprintf(stderr, "shroud: note: %zu name(s) kept because a lambda captures them\n", drop.size());
}

struct FunctionScopes {
    std::map<size_t, std::string> keyOfOffset;  // FunctionDecl offset -> "scope::name"
    std::map<std::string, unsigned> count;      // "scope::name" -> how many declarations share it
};

CXChildVisitResult collectFunctionScopes(CXCursor c, CXCursor, CXClientData d) {
    auto& out = *static_cast<FunctionScopes*>(d);
    const CXCursorKind k = clang_getCursorKind(c);
    // A function *template* overload (e.g. the variadic-recursion case paired
    // with a plain non-template base case, as in benchmark's `size_impl()` /
    // `size_impl(head, tail...)`) counts too: it is never itself renamed, but it
    // still occupies the name in that scope's overload set.
    if (k != CXCursor_FunctionDecl && k != CXCursor_FunctionTemplate) return CXChildVisit_Recurse;
    CXCursor canon = clang_getCanonicalCursor(c);
    if (clang_Cursor_isNull(canon)) return CXChildVisit_Recurse;
    const std::string name = toStd(clang_getCursorSpelling(canon));
    if (name.empty()) return CXChildVisit_Recurse;
    const std::string key = toStd(clang_getCursorUSR(clang_getCursorSemanticParent(canon))) + "::" + name;
    ++out.count[key];
    if (k == CXCursor_FunctionDecl) out.keyOfOffset[cursorOffset(canon)] = key;
    return CXChildVisit_Recurse;
}

// A free function that is overloaded (same name, same scope, several
// declarations, a sibling function template included) cannot be safely
// renamed: a call to it from inside another function *template* is a
// dependent lookup, resolved again at instantiation time against the plain
// spelling. libclang's reference for such a call can point at only one
// overload (or at a same-named function template, which is never renamed), so
// after one overload is renamed the call site still says the old name, finds
// no matching overload, and silently binds to whatever candidate remains
// instead (found obfuscating googletest's templated IsSubstringImpl<StringType>,
// which calls the overloaded IsSubstringPred: renaming only the wchar_t
// overload left the call falling through to the generic template,
// `haystack.find()` on a `const wchar_t*` does not compile; and benchmark's
// `size_impl()`/`join_impl()`, whose non-template base case lost its name the
// same way against its own variadic-template recursive case).
void dropOverloadedFunctionNames(Ctx& ctx) {
    FunctionScopes scopes;
    clang_visitChildren(clang_getTranslationUnitCursor(ctx.tu), collectFunctionScopes, &scopes);

    std::set<std::string> drop;
    for (const auto& [off, fresh] : ctx.names) {
        auto it = scopes.keyOfOffset.find(off);
        if (it != scopes.keyOfOffset.end() && scopes.count[it->second] > 1) drop.insert(fresh);
    }
    if (drop.empty()) return;
    for (auto it = ctx.names.begin(); it != ctx.names.end();) {
        if (drop.count(it->second)) it = ctx.names.erase(it); else ++it;
    }
    std::set<std::string> live;
    for (const auto& [off, name] : ctx.names) live.insert(name);
    ctx.nameLog.erase(std::remove_if(ctx.nameLog.begin(), ctx.nameLog.end(),
                                     [&](const auto& e) { return !live.count(e.second); }),
                      ctx.nameLog.end());
    std::fprintf(stderr, "shroud: note: %zu name(s) kept because the function is overloaded\n", drop.size());
}

// Copies the source with comments and literal bodies blanked, so a `#if` or an
// identifier inside a string does not confuse the scans below.
std::string maskedSource(const std::string& s) {
    std::string m = s;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') m[i++] = ' ';
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            m[i++] = ' ';
            m[i++] = ' ';
            while (i < s.size() && !(s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/')) {
                if (s[i] != '\n') m[i] = ' ';
                ++i;
            }
            if (i < s.size()) { m[i++] = ' '; if (i < s.size()) m[i++] = ' '; }
        } else if (c == 'R' && i + 1 < s.size() && s[i + 1] == '"') {
            const size_t open = s.find('(', i + 2);
            if (open == std::string::npos) {
                ++i;
                continue;
            }
            const std::string close = ")" + s.substr(i + 2, open - i - 2) + "\"";
            const size_t end = s.find(close, open + 1);
            const size_t stop = end == std::string::npos ? s.size() : end + close.size();
            for (size_t k = i; k < stop; ++k)
                if (s[k] != '\n') m[k] = ' ';
            i = stop;
        } else if (c == '\'' && i > 0 && i + 1 < s.size() && s[i - 1] >= '0' && s[i - 1] <= '9' &&
                   s[i + 1] >= '0' && s[i + 1] <= '9') {
            ++i; // C++14 digit separator, not a character literal
        } else if (c == '"' || c == '\'') {
            const char q = c;
            m[i++] = ' ';
            while (i < s.size() && s[i] != q) {
                if (s[i] == '\\' && i + 1 < s.size()) m[i++] = ' ';
                if (s[i] != '\n') m[i] = ' ';
                ++i;
            }
            if (i < s.size()) m[i++] = ' ';
        } else {
            ++i;
        }
    }
    return m;
}

// Byte ranges covered by any #if/#ifdef/#ifndef group. Declarations in a group
// that is inactive on this platform are invisible to libclang, so a name that
// also appears there must keep its spelling or the other platform stops
// compiling (ns-3 keeps two SYSTEM_PATH_SEP declarations, for example).
std::vector<std::pair<size_t, size_t>> conditionalRanges(const std::string& m) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t depth = 0, start = 0, i = 0;
    while (i < m.size()) {
        const size_t eol = std::min(m.find('\n', i), m.size());
        size_t k = i;
        while (k < eol && (m[k] == ' ' || m[k] == '\t' || m[k] == '\r')) ++k;
        if (k < eol && m[k] == '#') {
            ++k;
            while (k < eol && (m[k] == ' ' || m[k] == '\t')) ++k;
            const std::string_view d(m.data() + k, std::min<size_t>(2, eol - k));
            if (d == "if") {
                if (depth++ == 0) start = i;
            } else if (d == "en" && depth > 0) {
                if (--depth == 0) out.emplace_back(start, eol);
            }
        }
        i = eol + 1;
    }
    if (depth > 0) out.emplace_back(start, m.size());
    return out;
}

void dropConditionalNames(Ctx& ctx) {
    const std::string& src = *ctx.src;
    const std::string masked = maskedSource(src);
    const auto ranges = conditionalRanges(masked);
    if (ranges.empty()) return;

    std::set<std::string> drop;
    for (const auto& [off, fresh] : ctx.names) {
        size_t e = off;
        while (e < src.size() && identChar(src[e])) ++e;
        const std::string old = src.substr(off, e - off);
        if (old.empty() || drop.count(fresh)) continue;
        for (size_t pos = masked.find(old); pos != std::string::npos; pos = masked.find(old, pos + 1)) {
            const bool boundary = (pos == 0 || !identChar(masked[pos - 1])) &&
                                  (pos + old.size() >= masked.size() || !identChar(masked[pos + old.size()]));
            if (!boundary) continue;
            const bool conditional = std::any_of(ranges.begin(), ranges.end(), [&](const auto& r) {
                return pos >= r.first && pos < r.second;
            });
            if (!conditional) continue;
            const bool covered = std::any_of(ctx.edits.begin(), ctx.edits.end(), [&](const Edit& ed) {
                return ed.text == fresh && pos >= ed.begin && pos < ed.begin + ed.len;
            });
            if (!covered) {
                drop.insert(fresh);
                break;
            }
        }
    }

    if (drop.empty()) return;
    for (auto it = ctx.names.begin(); it != ctx.names.end();) {
        if (drop.count(it->second)) it = ctx.names.erase(it); else ++it;
    }
    // The name edits were already queued; drop them together with the names.
    ctx.edits.erase(std::remove_if(ctx.edits.begin(), ctx.edits.end(),
                                   [&](const Edit& ed) { return drop.count(ed.text) != 0; }),
                    ctx.edits.end());
    std::set<std::string> live;
    for (const auto& [off, name] : ctx.names) live.insert(name);
    ctx.nameLog.erase(std::remove_if(ctx.nameLog.begin(), ctx.nameLog.end(),
                                     [&](const auto& e) { return !live.count(e.second); }),
                      ctx.nameLog.end());
    std::fprintf(stderr, "shroud: note: %zu name(s) kept because they also appear in #if branches\n", drop.size());
}

void declEdit(Ctx& ctx, CXCursor c) {
    CXCursor canon = clang_getCanonicalCursor(c);
    if (clang_Cursor_isNull(canon)) return;
    // Offsets are per file: a header declaration (leveldb::BuildTable) can share
    // an offset with a renamed local (db/builder.cc `file`) and must never pick
    // up its new name. External declarations stay in headers, so they land here.
    if (!isMainFile(canon) || !isPlain(canon)) return;
    auto it = ctx.names.find(cursorOffset(canon));
    if (it == ctx.names.end()) return;
    const std::string name = toStd(clang_getCursorSpelling(c));
    if (name.empty()) return;
    if (!isPlain(c) || !isMainFile(c)) return;
    const size_t off = cursorOffset(c);
    if (off + name.size() > ctx.src->size()) return;
    if (ctx.src->compare(off, name.size(), name) != 0) return;
    addEdit(ctx, off, name.size(), it->second);
}

void refEdit(Ctx& ctx, CXCursor c) {
    CXCursor ref = clang_getCursorReferenced(c);
    if (clang_Cursor_isNull(ref)) return;
    CXCursor canon = clang_getCanonicalCursor(ref);
    if (clang_Cursor_isNull(canon)) return;
    // Offsets are per file: a header declaration (std::flush) can share an
    // offset with a renamed local and must never pick up its new name.
    if (!isMainFile(canon)) return;
    auto it = ctx.names.find(cursorOffset(canon));
    if (it == ctx.names.end()) return;
    if (!isPlain(c) || !isMainFile(c)) return;
    size_t b = 0, e = 0;
    if (!extentOffsets(c, b, e)) return;
    // The extent of a qualified reference ("Detail::outlier_variance") starts at
    // the qualifier, so take the name token from the cursor location instead.
    const size_t nb = cursorOffset(c);
    if (nb < b || nb > e) return;
    const std::string name = toStd(clang_getCursorSpelling(ref));
    if (name.empty() || nb + name.size() > ctx.src->size()) return;
    if (ctx.src->compare(nb, name.size(), name) != 0) return;
    addEdit(ctx, nb, name.size(), it->second);
}

// Text from the start of a declaration up to its body or initializer, used to
// spot constexpr/consteval without a real semantic query in the C API.
std::string declPrefix(const Ctx& ctx, CXCursor c) {
    size_t b = 0, e = 0;
    if (!extentOffsets(c, b, e)) return {};
    const size_t lim = std::min(e, b + 320);
    std::string_view s(ctx.src->data() + b, lim - b);
    const size_t stop = s.find_first_of("{;=");
    return std::string(s.substr(0, stop));
}

bool hasWord(const std::string& s, const char* w) {
    return s.find(w) != std::string::npos;
}

bool constantEvaluated(Ctx& ctx) {
    // Once the literal is wrapped in an argument/aggregate context (a call, a
    // constructor, `new X{...}`, a braced-init-list), it is in pointer position
    // for whatever consumes it there, no matter what encloses *that* further out.
    // Without this, `static const auto& x = *new std::set<String>{"a", "b"};`
    // skipped every literal in the set: the walk kept going past the
    // CXXNewExpr/InitListExpr and hit the reference-typed VarDecl `x`, which the
    // array/reference rule below treats as "not a pointer" — wrongly, since the
    // literals are std::string constructor arguments, not that VarDecl's own
    // initializer (found obfuscating jsoncpp's StreamWriterBuilder::validate).
    bool sawArgumentContext = false;
    for (auto it = ctx.stack.rbegin(); it != ctx.stack.rend(); ++it) {
        const CXCursor p = *it;
        switch (clang_getCursorKind(p)) {
        case CXCursor_FunctionDecl:
        case CXCursor_CXXMethod:
        case CXCursor_Constructor:
        case CXCursor_Destructor:
        case CXCursor_ConversionFunction:
        case CXCursor_FunctionTemplate:
        case CXCursor_ClassTemplate:
        case CXCursor_ClassTemplatePartialSpecialization: {
            const std::string pre = declPrefix(ctx, p);
            if (hasWord(pre, "constexpr") || hasWord(pre, "consteval") || hasWord(pre, "constinit") ||
                isTemplate(p)) {
                return true;
            }
            break;
        }
        case CXCursor_VarDecl: {
            const std::string pre = declPrefix(ctx, p);
            if (hasWord(pre, "constexpr") || hasWord(pre, "constinit") || hasWord(pre, "consteval")) {
                return true;
            }
            if (!sawArgumentContext) {
                CXType t = clang_getCursorType(p);
                if (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray ||
                    t.kind == CXType_LValueReference || t.kind == CXType_RValueReference) {
                    return true;
                }
            }
            break;
        }
        case CXCursor_InitListExpr:
        case CXCursor_CallExpr: // libclang reports constructor calls as CallExpr too
        case CXCursor_CXXNewExpr:
            sawArgumentContext = true;
            break;
        case CXCursor_StaticAssert:
        case CXCursor_AsmStmt:
        case CXCursor_UnexposedAttr:
        case CXCursor_AnnotateAttr:
            return true;
        case CXCursor_CaseStmt:
            // Only the label ("case 'a':") is constant evaluated. Body statements
            // are children of the same cursor, so do not skip them wholesale.
            return it == ctx.stack.rbegin();
        case CXCursor_UnaryExpr: {
            // sizeof / alignof / noexcept operands are constant contexts.
            if (clang_getCursorUnaryOperatorKind(p) != CXUnaryOperator_AddrOf) return true;
            break;
        }
        default:
            break;
        }
    }
    return false;
}

bool hexval(char c, unsigned& v) {
    if (c >= '0' && c <= '9') { v = unsigned(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { v = unsigned(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { v = unsigned(c - 'A' + 10); return true; }
    return false;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// Turns the source text of one or more adjacent string literals ("a" "b",
// R"(raw)") into the bytes they denote. Anything not fully understood, including
// L/u/U/u8 prefixes and user defined suffixes, makes it fail so the caller keeps
// the literal untouched.
bool literalBytes(std::string_view s, std::string& out) {
    size_t i = 0;
    auto trivia = [&] {
        while (i < s.size()) {
            const char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
                const size_t nl = s.find('\n', i);
                i = nl == std::string_view::npos ? s.size() : nl + 1;
            } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                const size_t close = s.find("*/", i + 2);
                if (close == std::string_view::npos) return;
                i = close + 2;
            } else {
                return;
            }
        }
    };

    trivia();
    bool any = false;
    while (i < s.size()) {
        if (s[i] == 'R' && i + 1 < s.size() && s[i + 1] == '"') {
            i += 2;
            const size_t open = s.find('(', i);
            if (open == std::string_view::npos || open - i > 16) return false;
            const std::string close = ")" + std::string(s.substr(i, open - i)) + "\"";
            const size_t body = s.find(close, open + 1);
            if (body == std::string_view::npos) return false;
            out.append(s.substr(open + 1, body - open - 1));
            i = body + close.size();
        } else if (s[i] == '"') {
            ++i;
            bool closed = false;
            while (i < s.size()) {
                const char c = s[i++];
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (i >= s.size()) return false;
                const char e = s[i++];
                switch (e) {
                case '\'': case '"': case '?': case '\\': out.push_back(e); break;
                case 'a': out.push_back('\a'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'v': out.push_back('\v'); break;
                case '\n': break; // line continuation
                case 'x': {
                    unsigned v = 0;
                    size_t digits = 0;
                    while (i < s.size()) {
                        unsigned d = 0;
                        if (!hexval(s[i], d)) break;
                        v = v * 16 + d;
                        ++i;
                        ++digits;
                    }
                    if (!digits) return false;
                    out.push_back(char(v & 0xFF));
                    break;
                }
                case 'u':
                case 'U': {
                    const size_t want = e == 'u' ? 4 : 8;
                    unsigned cp = 0;
                    for (size_t k = 0; k < want; ++k, ++i) {
                        unsigned d = 0;
                        if (i >= s.size() || !hexval(s[i], d)) return false;
                        cp = cp * 16 + d;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    if (e < '0' || e > '7') return false;
                    {
                        unsigned v = unsigned(e - '0');
                        for (size_t k = 0; k < 2 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++k, ++i)
                            v = v * 8 + unsigned(s[i] - '0');
                        out.push_back(char(v & 0xFF));
                    }
                    break;
                }
            }
            if (!closed) return false;
        } else {
            return false; // prefix or user defined suffix
        }
        any = true;
        const size_t before = i;
        trivia();
        if (i == before) break; // next token is not another literal
    }
    // A trailing suffix ("x"s) or anything else means the extent is not a plain
    // literal; leave it alone rather than guessing what the suffix means.
    return any && i >= s.size();
}

// libclang's extent for `"x"_suffix` covers the suffix too, and a user defined
// literal cannot be applied to a ::shd::lit<> expression. Split the suffix off so
// the quoted part can be parsed and the operator called explicitly.
bool udlSplit(std::string_view s, std::string& bytes, std::string& suffix) {
    size_t k = s.size();
    while (k > 0 && identChar(s[k - 1])) --k;
    if (k == s.size() || k == 0 || s[k] != '_') return false; // only user defined names start with '_'
    std::string_view lit = s.substr(0, k);
    while (!lit.empty() && (lit.back() == ' ' || lit.back() == '\t' || lit.back() == '\r' ||
                            lit.back() == '\n'))
        lit.remove_suffix(1);
    std::string parsed; // literalBytes appends, and the caller may have tried it already
    if (!literalBytes(lit, parsed)) return false;
    bytes = std::move(parsed);
    suffix = std::string(s.substr(k));
    return true;
}

// ::shd::lit<key,N>("ciphertext") for one literal. `at` is the source offset so
// two identical literals in different places still get different keystreams.
std::string encCall(Ctx& ctx, const std::string& bytes, size_t at) {
    // `at` must go through mix(), not serve as the FNV-1a seed directly: FNV-1a
    // with a varying seed is linear enough that two nearby literals differing in
    // one byte can cancel out a matching offset delta and hash identically.
    // Real case: benchmark's `{"Ki", "Mi", "Gi", ...}` has "Ki" and "Gi" 12 bytes
    // apart (the width of `"Mi", `) and their first bytes differ by exactly
    // 0x4B ^ 0x47 = 0x0C = 12, so seeding FNV-1a with the raw offset made both
    // literals hash to the same key *and* the same Tag, so `lit<>()` returned
    // "Ki" at both call sites (not caught by the earlier Key-only collision fix,
    // since Tag collided right along with it).
    const uint64_t h = mix(fnv1a(&ctx.seed, sizeof ctx.seed), mix(fnv1a(bytes.data(), bytes.size()), at));
    const unsigned key = static_cast<unsigned>(h ^ (h >> 32)) | 1u;

    // Octal escapes keep the generated source plain ASCII on every compiler and
    // cannot merge with a following literal character like \x would.
    // `h` is the per-site Tag: it makes each call site a distinct lit<> instance
    // so a Key collision between two literals cannot make them share one buffer.
    std::string call = "::shd::lit<" + std::to_string(key) + "u," + std::to_string(bytes.size() + 1) +
                       "u," + std::to_string(h) + "ull>(\"";
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char ct = static_cast<unsigned char>(bytes[i]) ^ shd::ks(key, static_cast<unsigned>(i));
        char tmp[8];
        std::snprintf(tmp, sizeof tmp, "\\%03o", unsigned(ct));
        call += tmp;
    }
    call += "\")";
    return call;
}

void stringEdit(Ctx& ctx, CXCursor c) {
    if (!isMainFile(c) || !isPlain(c)) return;
    if (!selectedFunction(ctx, c)) return;
    const CXType t = clang_getCursorType(c);
    if (t.kind != CXType_ConstantArray) return;
    const long long n = clang_getArraySize(t);
    const CXType et = clang_getElementType(t);
    if (et.kind != CXType_Char_S && et.kind != CXType_Char_U) return; // wide/u8 literals stay
    if (n < 1) return;
    if (constantEvaluated(ctx)) return;

    size_t b = 0, e = 0;
    if (!extentOffsets(c, b, e)) return;
    const std::string_view text(ctx.src->data() + b, e - b);
    std::string bytes;
    std::string suffix;
    if (!literalBytes(text, bytes) && !udlSplit(text, bytes, suffix)) return;
    if (static_cast<long long>(bytes.size()) != n - 1 || bytes.empty()) return;

    std::string call = encCall(ctx, bytes, b);
    if (!suffix.empty())
        call = "operator\"\"" + suffix + "(" + call + "," + std::to_string(bytes.size()) + "u)";
    addEdit(ctx, b, e - b, std::move(call));
    ctx.plaintexts.push_back(bytes);
    ++ctx.encrypted;
}

// ---------------------------------------------------------------- flow

bool hasLineCommentOrDirective(const Ctx& ctx, size_t b, size_t e) {
    size_t i = b;
    bool lineStart = true;
    while (i < e) {
        const char ch = (*ctx.src)[i];
        if (lineStart) {
            size_t k = i;
            while (k < e && ((*ctx.src)[k] == ' ' || (*ctx.src)[k] == '\t' || (*ctx.src)[k] == '\r')) ++k;
            if (k < e && (*ctx.src)[k] == '#') return true;
        }
        lineStart = (ch == '\n');
        ++i;
    }
    return false;
}

struct LabelScan {
    bool found;
};

CXChildVisitResult scanLabel(CXCursor c, CXCursor, CXClientData d) {
    switch (clang_getCursorKind(c)) {
    case CXCursor_LabelStmt:
    case CXCursor_CaseStmt:
    case CXCursor_DefaultStmt:
        static_cast<LabelScan*>(d)->found = true;
        return CXChildVisit_Break;
    default:
        return CXChildVisit_Recurse;
    }
}

struct BodyScan {
    Ctx* ctx;
    std::vector<std::pair<size_t, size_t>> out;
};

// libclang's extent of a statement stops before its terminating semicolon.
size_t statementEnd(const Ctx& ctx, size_t e) {
    size_t i = e;
    while (i < ctx.src->size() && ((*ctx.src)[i] == ' ' || (*ctx.src)[i] == '\t' || (*ctx.src)[i] == '\r' ||
                                   (*ctx.src)[i] == '\n'))
        ++i;
    if (i < ctx.src->size() && (*ctx.src)[i] == ';') return i + 1;
    return e;
}

CXChildVisitResult scanBody(CXCursor c, CXCursor, CXClientData d) {
    BodyScan& s = *static_cast<BodyScan*>(d);
    switch (clang_getCursorKind(c)) {
    case CXCursor_DeclStmt:      // moving a declaration into a branch changes its scope
    case CXCursor_NullStmt:
    case CXCursor_LabelStmt:
    case CXCursor_CaseStmt:
    case CXCursor_DefaultStmt:
        return CXChildVisit_Continue;
    default:
        break;
    }
    size_t b = 0, e = 0;
    if (!extentOffsets(c, b, e)) return CXChildVisit_Continue;
    if (e <= b || e - b > 1200) return CXChildVisit_Continue;
    if (!isPlain(c)) return CXChildVisit_Continue;
    // The extent of a statement stops before its ';'. If the next token is not
    // ';' and the text does not end a block, a comment or directive sits between
    // ("return c == '-'" / "#ifdef WIN32" / "..."): wrapping would split it.
    size_t end = statementEnd(*s.ctx, e);
    if (end == e && (*s.ctx->src)[e - 1] != '}') return CXChildVisit_Continue;
    if (hasLineCommentOrDirective(*s.ctx, b, end)) return CXChildVisit_Continue;
    LabelScan ls{false};
    clang_visitChildren(c, scanLabel, &ls);
    if (ls.found) return CXChildVisit_Continue;
    s.out.emplace_back(b, end);
    return CXChildVisit_Continue;
}

CXChildVisitResult findBody(CXCursor c, CXCursor, CXClientData d) {
    if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
        *static_cast<CXCursor*>(d) = c;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

void flowFor(Ctx& ctx, CXCursor fn) {
    if (!isMainFile(fn) || !isPlain(fn)) return;
    if (!selectedFunction(ctx, fn)) return;
    if (!clang_isCursorDefinition(fn)) return;
    if (isTemplate(fn)) return;
    bool constant = false;
    for (CXCursor p : ctx.stack) {
        const std::string pre = declPrefix(ctx, p);
        if (hasWord(pre, "constexpr") || hasWord(pre, "consteval") || isTemplate(p)) {
            constant = true;
            break;
        }
    }
    const std::string own = declPrefix(ctx, fn);
    if (hasWord(own, "constexpr") || hasWord(own, "consteval")) constant = true;
    if (constant) return;

    CXCursor body = clang_getNullCursor();
    clang_visitChildren(fn, findBody, &body);
    if (clang_Cursor_isNull(body)) return;

    BodyScan scan{&ctx, {}};
    clang_visitChildren(body, scanBody, &scan);
    if (scan.out.empty()) return;

    // Biggest statements first: they carry the most code to duplicate.
    std::sort(scan.out.begin(), scan.out.end(),
              [](auto& a, auto& b) { return (a.second - a.first) > (b.second - b.first); });
    const size_t take = std::min<size_t>(scan.out.size(), ctx.flowPerFunction);
    for (size_t i = 0; i < take; ++i) {
        const size_t b = scan.out[i].first;
        // libclang can report the same cursor under two parents (anonymous
        // namespace members); wrapping a position twice would corrupt the text.
        const bool seen = std::any_of(ctx.wraps.begin(), ctx.wraps.end(),
                                      [&](const Wrap& w) { return w.begin == b; });
        if (!seen) ctx.wraps.push_back({b, scan.out[i].second, ctx.wrapped++});
    }
}

void visitNode(Ctx& ctx, CXCursor c) {
    const CXCursorKind k = clang_getCursorKind(c);
    if (!ctx.functions.empty() && isFunctionDefinitionKind(k) && isMainFile(c)) {
        const std::string name = toStd(clang_getCursorSpelling(c));
        if (std::find(ctx.functions.begin(), ctx.functions.end(), name) != ctx.functions.end()) {
            ctx.usedFunctions.insert(name);
        }
    }
    if (ctx.doStrings && k == CXCursor_StringLiteral) stringEdit(ctx, c);
    if (ctx.doRename) {
        if (k == CXCursor_DeclRefExpr) {
            refEdit(ctx, c);
        } else if (k == CXCursor_VarDecl || k == CXCursor_ParmDecl || k == CXCursor_FunctionDecl) {
            declEdit(ctx, c);
        }
    }
    if (ctx.doFlow && isFunctionDefinitionKind(k)) flowFor(ctx, c);

    // The C API cannot report the parent of an expression, so keep the chain.
    ctx.stack.push_back(c);
    clang_visitChildren(
        c, [](CXCursor child, CXCursor, CXClientData d) {
            visitNode(*static_cast<Ctx*>(d), child);
            return CXChildVisit_Continue;
        },
        &ctx);
    ctx.stack.pop_back();
}

// ---------------------------------------------------------------- text assembly

// Offset just past the last leading preprocessor line, so a MSVC precompiled
// header include stays first as it must.
size_t includePosition(const std::string& s) {
    size_t last = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t eol = s.find('\n', i);
        if (eol == std::string::npos) eol = s.size();
        const std::string_view line(s.data() + i, eol - i);
        const size_t k = line.find_first_not_of(" \t\r");
        if (k == std::string_view::npos) {
            i = eol + 1;
            continue;
        }
        const std::string_view t = line.substr(k);
        if (t.rfind("//", 0) == 0 || t.rfind("/*", 0) == 0 || t[0] == '*') {
            i = eol + 1;
            continue;
        }
        if (t[0] == '#') {
            // Never split a conditional group: on the platform where the group
            // is inactive the runtime include would not be compiled at all.
            size_t d = 1;
            while (d < t.size() && (t[d] == ' ' || t[d] == '\t')) ++d;
            const std::string_view dir = t.substr(d, 2);
            if (dir == "if" || dir == "el" || dir == "en") break; // if/ifdef/ifndef, elif/else, endif
            // A trailing backslash keeps the directive open (multi-line macro);
            // the include must go after the last line of that macro.
            while (eol < s.size()) {
                size_t k2 = eol;
                while (k2 > i && (s[k2 - 1] == ' ' || s[k2 - 1] == '\t' || s[k2 - 1] == '\r')) --k2;
                if (k2 == i || s[k2 - 1] != '\\') break;
                eol = s.find('\n', eol + 1);
                if (eol == std::string::npos) {
                    eol = s.size();
                    break;
                }
            }
            last = std::min(eol + 1, s.size());
            i = last;
            continue;
        }
        break;
    }
    return last;
}

std::string assemble(Ctx& ctx) {
    std::string out = *ctx.src;

    std::sort(ctx.edits.begin(), ctx.edits.end(), [](const Edit& a, const Edit& b) {
        if (a.begin != b.begin) return a.begin > b.begin;
        return a.len > b.len; // replacements before insertions at the same offset
    });
    size_t floor = out.size();
    std::vector<Edit> applied; // only these may move positions; a dropped edit did not change the text
    for (const Edit& e : ctx.edits) {
        if (e.begin + e.len > floor) continue; // overlapping edit: drop it rather than corrupt text
        out.replace(e.begin, e.len, e.text);
        floor = e.begin;
        applied.push_back(e);
    }

    if (ctx.wraps.empty()) return out;

    // Translate original offsets through the edits already applied: an edit that
    // starts at or before a position moves that position, one that starts after
    // it does not. `pos` itself maps to the start of a replacement at `pos`.
    auto shift = [&](size_t pos) {
        long long d = 0;
        for (const Edit& e : applied)
            if (e.begin < pos) d += static_cast<long long>(e.text.size()) - static_cast<long long>(e.len);
        return static_cast<long long>(pos) + d;
    };

    std::sort(ctx.wraps.begin(), ctx.wraps.end(), [](const Wrap& a, const Wrap& b) { return a.begin > b.begin; });
    for (const Wrap& w : ctx.wraps) {
        const size_t b = static_cast<size_t>(shift(w.begin));
        const size_t e = static_cast<size_t>(shift(w.end));
        if (b >= e || e > out.size()) continue;
        const std::string body = out.substr(b, e - b);
        const std::string junk = std::to_string(w.id);
        // The volatile write differs per branch so the optimizer cannot fold the
        // two copies into one. It may still merge them; see README.
        const std::string mid = "\n} else {\nvolatile unsigned shd_j" + junk + " = 0x" +
                                std::to_string(0x1000u + (w.begin & 0xfffu)) + "u; (void)shd_j" + junk + ";\n";
        out.insert(e, mid + body + "\n}\n");
        out.insert(b, "\nif (::shd::opaque()) {\n");
    }
    return out;
}

// ---------------------------------------------------------------- driver

std::string defaultOutput(const std::string& input) {
    fs::path p(input);
    return (p.parent_path() / (p.stem().string() + ".shroud" + p.extension().string())).string();
}

void parseDiagnostics(CXTranslationUnit tu, unsigned& errors) {
    errors = 0;
    const unsigned n = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
        if (sev >= CXDiagnostic_Error) {
            if (errors < 3) {
                const std::string msg = toStd(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions()));
                std::fprintf(stderr, "shroud: parse: %s\n", msg.c_str());
            }
            ++errors;
        }
        clang_disposeDiagnostic(d);
    }
}

// Absolute path of the running executable, or an empty path if it can't be
// determined. Used to find builtin headers shipped next to a release binary.
fs::path executablePath() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf);
        }
        buf.resize(buf.size() * 2); // path longer than the buffer: grow and retry
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // first call reports the size it needs
    std::string buf(size, '\0');
    if (size == 0 || _NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    const fs::path real = fs::canonical(buf, ec); // _NSGetExecutablePath may return a symlink/./..
    return ec ? fs::path(buf) : real;
#elif defined(__linux__)
    std::error_code ec;
    const fs::path real = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : real;
#else
    return {};
#endif
}

// Look for the clang builtin headers (stddef.h, ...) packaged next to the
// binary, as the install layout ships them: <exe>/../lib/clang/<ver>/include.
// Returns the directory to hand clang via -resource-dir (the one that *contains*
// include/), or an empty path if nothing packaged is found. This keeps a release
// working on a machine that has no LLVM at the build-time absolute path.
fs::path packagedResourceDir() {
    const fs::path exe = executablePath();
    if (exe.empty()) return {};
    std::error_code ec;
    const fs::path clangRoot = exe.parent_path().parent_path() / "lib" / "clang";
    if (!fs::is_directory(clangRoot, ec)) return {};
    fs::path best;
    for (fs::directory_iterator it(clangRoot, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path cand = it->path();
        if (!fs::exists(cand / "include" / "stddef.h", ec)) continue;
        // Prefer the highest version if several clang resource dirs are shipped.
        if (best.empty() || cand.filename().string() > best.filename().string()) best = cand;
    }
    return best;
}

} // namespace

int obfuscate(const Options& opt) {
    bool ok = false;
    std::string src = readFile(opt.input, ok);
    if (!ok) {
        std::fprintf(stderr, "shroud: cannot read %s\n", opt.input.c_str());
        return 1;
    }

    const std::string backup = opt.inPlace ? opt.input + opt.backupSuffix : std::string();
    if (opt.inPlace) {
        std::error_code ec;
        if (fs::exists(backup, ec) && !opt.force) {
            std::fprintf(stderr, "shroud: backup %s already exists and may hold the original; remove it or pass --force\n",
                         backup.c_str());
            return 1;
        }
        // The transform is not idempotent: a second run would encrypt the
        // ciphertext again and rename the generated names.
        if (src.find("#include <shroud/runtime.hpp>") != std::string::npos && !opt.force) {
            std::fprintf(stderr,
                         "shroud: %s already includes <shroud/runtime.hpp>, so it looks transformed already; "
                         "restore the backup or pass --force\n",
                         opt.input.c_str());
            return 1;
        }
    }

    std::vector<const char*> args;
    for (const auto& a : opt.args) args.push_back(a.c_str());
    bool hasStd = false;
    bool hasResourceDir = false;
    for (const auto& a : opt.args) {
        if (a.rfind("-std=", 0) == 0 || a.rfind("/std:", 0) == 0) hasStd = true;
        if (a.rfind("-resource-dir", 0) == 0) hasResourceDir = true;
    }
    if (!hasStd) args.push_back("-std=c++20");
    // A user-supplied -resource-dir always wins. Otherwise prefer the builtin
    // headers packaged next to this binary (so a release is relocatable), and
    // only then fall back to the absolute path baked in at build time. The
    // string must outlive the parse below, since args keeps its c_str().
    std::string resourceDir;
    if (!hasResourceDir) {
        fs::path rd = packagedResourceDir();
#ifdef SHROUD_RESOURCE_DIR
        std::error_code ec;
        if (rd.empty() && fs::is_directory(SHROUD_RESOURCE_DIR, ec)) rd = SHROUD_RESOURCE_DIR;
#endif
        if (!rd.empty()) {
            resourceDir = "-resource-dir=" + rd.string();
            args.push_back(resourceDir.c_str());
        }
    }

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;
    const CXErrorCode err = clang_parseTranslationUnit2(index, opt.input.c_str(), args.data(),
                                                        static_cast<int>(args.size()), nullptr, 0,
                                                        CXTranslationUnit_KeepGoing, &tu);
    if (err != CXError_Success || !tu) {
        std::fprintf(stderr, "shroud: cannot parse %s (error %d)\n", opt.input.c_str(), static_cast<int>(err));
        if (tu) clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return 1;
    }

    unsigned errors = 0;
    parseDiagnostics(tu, errors);
    if (errors > 0 && !opt.force) {
        std::fprintf(stderr,
                     "shroud: %u parse error(s); refusing to transform. Pass the real compiler flags after `--` "
                     "(e.g. -I, -D, -std=c++20) or use --force.\n",
                     errors);
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return 1;
    }

    Ctx ctx;
    ctx.tu = tu;
    ctx.src = &src;
    ctx.seed = opt.seed;
    ctx.flowPerFunction = opt.flowPerFunction;
    ctx.doRename = opt.rename;
    ctx.doStrings = opt.strings;
    ctx.doFlow = opt.flow;
    ctx.functions = opt.functions;

    CXCursor root = clang_getTranslationUnitCursor(tu);
    if (ctx.doRename) {
        clang_visitChildren(root, collectNames, &ctx);
        dropMacroUsedNames(ctx);
        dropLambdaCapturedNames(ctx);
        dropOverloadedFunctionNames(ctx);
    }
    visitNode(ctx, root);
    if (ctx.doRename) dropConditionalNames(ctx);

    if ((ctx.doStrings || ctx.doFlow) && (!ctx.plaintexts.empty() || !ctx.wraps.empty())) {
        addEdit(ctx, includePosition(src), 0, "#include <shroud/runtime.hpp>\n");
    }
    const std::string out = assemble(ctx);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);

    const std::string target = opt.inPlace ? std::string() : (opt.output.empty() ? defaultOutput(opt.input) : opt.output);
    if (opt.inPlace) {
        if (!writeFile(backup, src)) {
            std::fprintf(stderr, "shroud: cannot write backup %s\n", backup.c_str());
            return 1;
        }
        // Write beside the target and rename, so a failed write cannot leave a
        // half-rewritten source file behind.
        const fs::path tmp = opt.input + ".shroud.tmp";
        std::error_code ec;
        if (!writeFile(tmp, out)) {
            std::fprintf(stderr, "shroud: cannot write %s\n", tmp.string().c_str());
            return 1;
        }
        fs::rename(tmp, opt.input, ec);
        if (ec) {
            fs::remove(tmp, ec);
            std::fprintf(stderr, "shroud: cannot replace %s\n", opt.input.c_str());
            return 1;
        }
    } else if (!writeFile(target, out)) {
        std::fprintf(stderr, "shroud: cannot write %s\n", target.c_str());
        return 1;
    }

    // CMake hands the same map to every source file, so merge instead of
    // truncating: the union is what `verify` needs for the linked binary.
    // shortcut: stale entries from earlier runs can only cause a loud failure,
    // never a silent pass; delete the map after large edits.
    Map map;
    map.load(opt.map);
    map.seed = opt.seed;
    std::map<std::string, bool> seenNames;
    for (const auto& [old, fresh] : ctx.nameLog) {
        if (seenNames.emplace(old, true).second) map.addName(old);
    }
    for (const auto& s : ctx.plaintexts) map.addString(s);
    if (!map.save(opt.map)) {
        std::fprintf(stderr, "shroud: cannot write map %s\n", opt.map.c_str());
        return 1;
    }

    if (opt.inPlace) {
        std::printf("%s  (in place, backup %s)  (%zu names, %u strings, %zu branches)\n", opt.input.c_str(),
                    backup.c_str(), ctx.names.size(), ctx.encrypted, ctx.wraps.size());
    } else {
        std::printf("%s -> %s  (%zu names, %u strings, %zu branches)\n", opt.input.c_str(), target.c_str(),
                    ctx.names.size(), ctx.encrypted, ctx.wraps.size());
    }
    for (const auto& f : opt.functions) {
        if (!ctx.usedFunctions.count(f)) {
            std::fprintf(stderr, "shroud: warning: --functions %s matched no function body in this file\n", f.c_str());
        }
    }
    return 0;
}

} // namespace shroud
