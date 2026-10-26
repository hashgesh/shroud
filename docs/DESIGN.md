# Design notes

Full edge-case reference and real-project validation history for shroud. The
short version — what to run, what it reports — lives in the main
[README](../README.md); this doc is what to read before touching transform or
verify behavior, or when a specific skip/warning needs explaining.

## What it does

### 1. Identifier renaming

Every declaration with internal or no linkage whose canonical definition lives
in the file being transformed: file-local `static` functions and variables,
anonymous-namespace members, locals, parameters. Each gets
`shd_<hash of seed, position and old name>`.

Never touched: anything with external linkage (your public API stays intact),
class members, templates and their instantiations, names produced by a macro,
and anything declared in a header (the header is shared, the `.cpp` is not).

Two safety rules learned on real code:

* A variable that is referenced from inside a macro expansion (ns-3's
  `NS_LOG_FUNCTION(args)`, `assert(x)`) is not renamed. The macro body lives in
  another file, so the reference cannot be rewritten and the build would break.
* A variable captured by a lambda (`[&width]`) is not renamed either: libclang
  has no cursor for a capture, so only the body would change.
* A name that also appears in an `#if` group keeps its spelling. libclang only
  sees the active branch; renaming a name that is declared again in the other
  branch (`#ifdef _WIN32`) breaks the platform you are not building right now.

`#include <shroud/runtime.hpp>` is inserted after the leading `#include` block,
but never inside an `#if` group and never inside a multi-line `#define`.

### 2. String literal encryption

`"hello"` becomes `::shd::lit<key, 6>("\\315\\022...")`. The ciphertext is
stored as an octal-escaped string so the generated source stays pure ASCII on
every compiler and no escape can swallow the next byte. Decryption happens once
per call site into a function-local static buffer, and `lit` returns a reference
to an array (`const char (&)[N]`), so the call still behaves like a literal:
`sizeof` is unchanged and it binds to `template <size_t N> f(const char (&)[N])`
parameters (yaml-cpp's `same(str, size, "null")`). The decode loop reads the
ciphertext through a `volatile` pointer: without it GCC at `-O3` folds the XOR
back to the plaintext and stores *that* in `.rodata`, which defeats the whole
exercise.

A string literal with a user defined suffix (`"version"_sr`, common in Catch2's
reporters) cannot take `lit` in front of it — a suffix only applies to a literal
token. It becomes an explicit operator call instead:
`operator""_sr(::shd::lit<key, 8>("..."), 7)`. This assumes the operator takes
`(const char*, size_t)`, which every string literal operator does. If the result
is needed in a constant expression the ancestor scan does not recognise (a
template argument, say), the generated call is not constant and the build stops
loudly; it is not silent breakage.

Left alone on purpose:

| context | why |
| --- | --- |
| `constexpr` / `consteval` / `constinit` | the call is not a constant expression |
| `char buf[] = "..."`, array or reference initialisation | the initialiser is not a pointer |
| `sizeof("...")`, `alignof`, `static_assert` | result must stay a compile-time constant |
| `case "..."`, attributes, `asm` | not valid in those positions |
| numeric user defined literals (`123_km`) | not a string |
| literals produced by a `#define` | editing the expansion site rewrites the wrong text |
| wide literals (`L`, `u`, `U`, `u8`) | only `char` literals are handled |

Skipped literals stay in the binary; `verify` lists them separately, so a gap is
visible instead of silent.

### 3. Control flow duplication

The largest statements of each function get wrapped:

```cpp
if (::shd::opaque()) {
    std::puts("...");
} else {
    volatile unsigned shd_j3 = 0x4390u; (void)shd_j3;
    std::puts("...");
}
```

Exactly one branch runs, so behaviour is unchanged. The volatile write differs
per branch, which stops the optimizer from proving both branches identical.
`opaque()` reads mutable storage through `volatile`, so it cannot be folded to a
constant.

Statements that declare a name used later, contain labels, or contain
preprocessor directives are skipped, because moving them into a block changes
their meaning.

`shortcut: source-level duplication is best effort. A compiler may still merge
the two copies; real control flow flattening needs an IR pass. This raises the
cost of reading the disassembly, it does not make it unreadable.`

## Verification

`shroud verify` scans the built binary byte by byte. No ELF/PE parser, no
external tools, so it works the same on every platform.

* A string literal that is still present is **fatal**. That is the strongest
  signal you can get: the exact bytes are gone or they are not.
* Short literals (under six bytes, e.g. `"--"` or `" for "`) are not checked:
  they occur inside unrelated strings and padding of any real binary. A literal
  that is entirely NUL bytes is treated the same way: `"\0\0\0\0\0\0"` cannot be
  told apart from alignment padding, so a hit proves nothing.
* A name still followed by a NUL byte is **fatal**. The byte run must also look
  like a symbol (no spaces, quotes or slashes), so a message ending in `...name`
  is not mistaken for the identifier `name`. A leading `.` is ignored too: the
  section names `.data` and `.comment` are not symbols and cannot be renamed.
* A literal only counts when it is its own NUL terminated entry. A hit inside a
  longer message is not fatal: a header string (`"... format of Automake .trs
  files"s`) legitimately contains the same word as a transformed `.cpp` literal,
  and a word boundary alone cannot tell the two apart.
* A name that appears inside a longer string is reported, not fatal. Mangled
  C++ symbols (`_ZL10accumulatei`) are compiler specific, so a hit there is a
  hint, not proof.
* Names shorter than four bytes are not checked: `i\0` occurs by chance in any
  binary.
* With real debug info the name check is reported as *not conclusive*, because
  DWARF keeps every identifier of every inlined header function, not only the
  names this run renamed. Strip the binary (or use a separate debug file) and
  run `verify` again; the result is then a hard PASS/FAIL.
* `--baseline` reports the size change. Compare like with like: a stripped
  binary against a debug build only measures the debug info.

What this does not prove: that the code is hard to reverse engineer. It proves
the plaintext is not sitting in the image.

## Limits, honestly

* The scan looks for contiguous bytes. A compiler may materialise a short
  literal as immediates inside instructions instead of a `.rodata` string, and
  then no byte scan can see it. This only hurts the *negative* control: encrypted
  literals never take that path, because the plaintext is rebuilt at runtime.
* Obfuscation is not security. A determined reverse engineer runs the binary,
  reads the keystream out of the code and recovers every string. The cipher is an
  XOR stream over a 32-bit key, not ChaCha: `shortcut: it defeats strings(1) and
  casual grep, upgrade to ChaCha20 if literals guard anything valuable.`
* One translation unit per run: there is no directory walk, so a project with
  200 files needs 200 invocations (the CMake module does that loop). Only names
  with internal linkage are touched, so nothing has to stay consistent across
  files. Exported symbols and anything declared in a header keep their names.
* `__LINE__`, `__func__` and stringised macro arguments are not adjusted. A name
  that is stringified by a macro is not detected and renaming it changes the
  string. Rare, but it happens.
* A literal or name passed as a macro *argument* is skipped too, even though it
  is physically written in this file: `isPlain()` compares the expansion
  location against the spelling location, and for an argument substituted into
  a macro body those differ (the expansion location collapses to the macro's
  own call site), so the check reads as "generated by the macro" even for a
  forwarded argument. One function-like macro around a literal (`CALL("x")` for
  `#define CALL(x) foo(x)`) is enough; test suites and flag-registration macros
  (gtest's `GTEST_DEFINE_*_`, benchmark's `BM_DEFINE_*`) lean on this pattern
  heavily, so files built around them keep most of their literals as text.
* A string literal that is also the name of a logged function (ns-3's
  `NS_LOG_FUNCTION(this)` inside a method literally named `PrintHelp`) or of an
  external symbol this TU references (`fflush(stdout)`) stays in the binary
  regardless of encryption: the compiler or linker writes that exact text
  itself, for an unrelated reason. `verify` reports it, correctly — the bytes
  really are there — but it is not a sign the encryption failed.
* A template that takes a string as `const char(&)[N]` is fine now: `lit` has the
  same array type. Before that change such a call was a compile error, never
  silent breakage.
* The map records *what shroud changed*, not where else the same bytes exist.
  After a `--functions` run, `verify` on the whole binary can still find a
  literal or a short name in another translation unit or header that was never
  transformed. Verify the object file of the transformed TU when you need a
  clean scoped signal.
* Inactive `#if` branches are invisible to libclang. Names that also occur there
  are kept (see above), so a cross-platform file gets less renaming than a
  single-platform one. Build one configuration at a time; do not expect one run
  to cover `_WIN32` and `__linux__` together.
* The generated code is bigger: about +56% on the test program at `-O1`, largely
  because of the duplicated branches. Real projects see less.
* A format string passed to `printf`-family functions is no longer a literal as
  far as the compiler is concerned, so GCC/Clang report `-Wformat-security` when
  the call has no format arguments. The warning points at the transformed line;
  a `-Werror` build needs `-Wno-format-security` or `--no-strings` for that file.
  Catch2 builds with `-Wall -Wextra` and saw none, because it does not format
  through a plain literal.

### Tested on real projects

ns-3-dev (GitLab, C++23, CMake, `-Os`, g++ 15): whole-file transforms on 15
core and network sources — 674 names, 156 literals, 610 branch wrappers — then
`56/56` test suites pass (also after a `--functions` run on `system-path.cc`).
The transformed TUs compile with clang++ too. `system-path.cc.o` grew from
80,278 to 84,631 bytes of `.text` (+5.4%); the project's 15 files gained 8,261
lines and lost 2,095.

That run found and fixed five real bugs: strings were skipped in `--functions`
mode, variables used by logging macros were renamed, lambda captures were left
behind, the runtime include landed inside `#if` groups and inside multi-line
macros, and a cursor reported under two parents was wrapped twice.

A per-TU `verify` on the built object is clean. Verifying the whole `libns3` with
a partial map fails, correctly: the same literal also lives in a test TU, and
`/proc/self/exe` comes from statically linked libbacktrace, not from ns-3.

yaml-cpp (GitHub, 5.2k stars, C++11, CMake, gcc 15): all 30 `src/*.cpp` files
transformed in place — 655 names, 101 literals, 489 branch wrappers. The library
rebuilds, `ctest` passes (`1068/1068` gtest cases in the `yaml-cpp-tests` binary),
and a `--functions HandleNextDocument` run on `parser.cpp` also builds and
passes. Objects grew from 1,139 to 1,396 KiB (+22.6%); the statically linked test
binary grew 1.0%. `verify` on the 30 object files against their baseline objects:
`30/30 PASS`.

That run found and fixed three more: the runtime header needed C++11 (a C++14
`constexpr` body does not compile in a `-std=gnu++11` project), `lit` returned a
pointer so it could not bind to `const char (&)[N]` parameters, and `verify`
reported ELF section names (`.data`, `.comment`) and file names (`binary.cpp`) as
surviving names/literals. The generated code now compiles as C++11 as well, which
the test suite checks in step 11.

Catch2 (GitHub, 18k stars, C++17, CMake, gcc 15): all 108 `src/**/*.cpp` files
transformed in place — 1,586 names, 803 literals, 1,195 branch wrappers. The
library and SelfTest rebuild with no warnings from the transformed code, `ctest`
passes `81/81`, and the SelfTest binary reports the same `527` cases (504 passed,
7 skipped, 16 failed as expected) with `84,893` assertions. A `--functions
run,runInternal` run on `catch_session.cpp` (6 names, 7 literals, 4 wrappers)
also builds and passes all 81 tests. Objects grew from 2,453 to 4,280 KiB
(+74.5%), `libCatch2.a` from 2.55 to 4.49 MiB, and the statically linked SelfTest
binary by 14.7%. `verify` on each of the 107 object files against its baseline
object, using that TU's own map: `107/107 PASS`.

That run found seven real bugs. The first four were in the rename and flow
passes: a qualified reference (`Detail::outlier_variance`) was not renamed
because libclang's extent starts at the qualifier; a header declaration
(`std::flush`) shared an offset with a renamed local and was renamed; a
statement split by an inner `#if` was wrapped and broken; and dropped
(overlapping) edits still shifted the flow wrap offsets. The last three came out
of `verify`. String literals in `switch` bodies were skipped because the ancestor
walk treated every `case`/`default` statement as a constant context (only the
label itself is one). GCC at `-O3` folded the decode loop and left the plaintext
in `.rodata` — the volatile read above stops it. `"x"_sr` literals were left
untouched because libclang's extent for the string literal includes the suffix;
they are now rewritten as explicit operator calls. `verify` also stopped counting
a literal that survives only inside a longer header message (the Catch2 reporter
headers end their descriptions in `"..."s`, which never enters a `.cpp` map).

The test suite checks the literal fixes in step 12: switch body literals, a
`_sr`-style user defined literal, and a `-O3` object whose plaintext must be
gone.

leveldb (GitHub, 37k stars, C++11/C++17, CMake, gcc 15): all 38 library TUs
transformed in place — 1,866 names, 217 literals, 910 branch wrappers — with the
C API (`db/c.cc`) mixed into the same library as `leveldb::`. The library and
tests rebuild, `ctest` passes `3/3`, and a `--functions WriteLevel0Table` run on
`db_impl.cc` (11 names, 2 literals, 2 wrappers) also builds and passes. Objects
grew from 1,177,000 to 2,597,000 bytes (+120.6%), `libleveldb.a` from 750,910 to
1,426,766 bytes, and `leveldb_tests` by 10.5%. `verify` on each of the 38 object
files against its baseline object: `38/38 PASS`.

That run found two more bugs. `declEdit` looked up the canonical declaration's
offset in the per-file name map without checking the file it came from, so
`leveldb::BuildTable` (declared in `db/builder.h` at offset 769) took the name of
the local `file` at offset 769 of `db/builder.cc`; the C API definitions in
`db/c.cc` broke the same way, and the link failed with `undefined reference`.
`refEdit` already had that guard since Catch2, `declEdit` did not. The other one
was in `verify`: the six-NUL padding literal in `db/log_writer.cc` was encrypted,
but six zero bytes occur in every object's alignment padding, so the scan
reported a leak. Both have a regression step now (13 and 14).

### Re-verified after the audit fixes (2026-10-25)

A later audit changed three things that touch generated code and the gate: the
`verify` string scan now checks *every* occurrence of a literal instead of the
first three (it could otherwise pass a leak), `::shd::lit` gained a per-site
`Tag` template argument so two literals that collide on the key cannot share one
decode buffer, and `::shd::opaque()` uses `std::atomic` instead of a plain
`volatile` read-modify-write (a data race once two threads call it). All four
projects above were re-run end to end against the rebuilt tool to confirm the
generated code still compiles and behaves, across gnu++11, C++17 and C++23/`-Os`:

| project | std | transform | build | tests | per-TU `verify` |
| --- | --- | --- | --- | --- | --- |
| leveldb (multithreaded) | C++11/17 | 1,866 / 217 / 910 | clean | `ctest 3/3` | `38/38 PASS` |
| Catch2 | C++17 | 1,586 / 803 / 1,195 | clean | SelfTest 527 cases, 84,893 assertions | `107/107 PASS` |
| yaml-cpp | gnu++11 | 685 / 165 / 509 | clean | gtest `1068` pass | `32/32 PASS` |
| ns-3-dev (core,network,stats) | C++23 `-Os` | 674 / 168 / 610 | clean | `test.py 56/56` | `13/15 PASS` |

(Counts are names / literals / branch wrappers; yaml-cpp and ns-3 differ slightly
from the original runs above because the upstream checkouts have moved on.) The
new `std::atomic` opaque predicate and the three-argument `lit` compile under all
three standards with no warnings beyond the documented `-Wformat-security`.

The two ns-3 objects that do not pass per-TU `verify` are the `__func__` limit,
not a regression: `command-line.cc` encrypts the string literals `"PrintHelp"`,
`"PrintGroups"`, `"PrintTypeIds"`, `"PrintGlobals"` and `"PrintGroup"`, but each
is also the name of a method whose body runs `NS_LOG_FUNCTION`, so the compiler
emits the same text as a function-name string in `.rodata` that no source-level
transform can touch (it survives a `--strip-debug`). `type-id.cc` is the same
class (`"SUPPORTED"`/`"DEPRECATED"`/`"OBSOLETE"` enum names and stringised macro
arguments). The stricter `verify` is what surfaces this: the plaintext genuinely
is in the image, just not because string encryption failed. For such a TU, strip
is no help; use `--no-strings` on it, or accept that logged symbol names are
visible. The 13 TUs without a literal/name collision pass clean.

### Three more real projects (2026-10-26)

jsoncpp (3 library TUs, C++17), googletest (9 TUs including gtest-death-test's
`fork()` paths, C++17) and google/benchmark (19 TUs, C++17) were each
obfuscated whole-file, rebuilt against the fixed tool and runtime, and run:
jsoncpp's 131 assertions, googletest's own 434-test self-check, and
benchmark's 87 ctest suites all pass; every object verifies clean except the
macro-argument-literal and symbol-name-collision cases already documented
above (gtest's `GTEST_DEFINE_*_` and benchmark's `BM_DEFINE_*` flag macros).

That round found and fixed three real, previously-unexercised bugs, each now a
regression step (16-18):

1. **A literal nested inside another expression could inherit an unrelated
   ancestor's "this is not a pointer" skip.** `constantEvaluated()` walked the
   whole ancestor chain looking for a reference- or array-typed `VarDecl` and
   stopped renaming/encrypting the instant it found one, however far away. In
   jsoncpp's `StreamWriterBuilder::validate()`, `static const auto& x = *new
   std::set<String>{"indentation", "precision", ...};` left every literal in
   that set unencrypted, because the walk crossed the `CXXNewExpr`/
   `InitListExpr` and found the *outer* reference-typed `x` several levels up.
   Fixed by stopping that specific rule at the first call/constructor/
   new-expression/init-list boundary: once a literal is an *argument* to
   something, nothing further out can un-pointer it.
2. **An overloaded free function could lose its name to a dependent call it
   couldn't see.** A call to an overloaded function from *inside another
   function template* is resolved by plain spelling again at instantiation
   time; libclang's reported reference for such a call is unreliable (it can
   point at only one overload, or at a sibling function template). Renaming
   just one overload left the old spelling unresolved at that call site,
   which then silently bound to whatever remained — in googletest's
   `IsSubstringImpl<StringType>` calling the overloaded `IsSubstringPred`,
   renaming only the `wchar_t*` overload left the call falling through to the
   generic `template <typename StringType> IsSubstringPred`, and
   `haystack.find()` doesn't compile for a pointer; in benchmark's
   `size_impl()`/`join_impl()`, the non-template base case lost the same way
   to its own variadic-template recursive case. Fixed by never renaming a free
   function whose name is shared by another declaration — a plain overload or
   a function template — in the same scope.
3. **Two literals could land on the same keystream.** `encCall` seeded the
   ciphertext's FNV-1a hash with the literal's raw source offset instead of
   mixing it in through an avalanching function; FNV-1a with a linear seed is
   invertible enough that a byte difference between two literals can cancel
   out a matching offset difference. benchmark's `kBigIECUnits[] = {"Ki",
   "Mi", "Gi", "Ti", ...}` hit this for real: `"Ki"` and `"Gi"` sit 12 bytes
   apart (the width of `"Mi", `) and their first bytes differ by exactly
   `0x4B ^ 0x47 = 12`, so both got the same key *and* the same per-site tag,
   and `lit<>()` returned `"Ki"` at the `"Gi"` call site too —
   `HumanReadableNumber(1 << 30, 1024)` printed `"1Ki"` instead of `"1Gi"`.
   This is the same failure mode the per-site `Tag` in the 2026-10-25 fix was
   meant to rule out; that fix added the right *shape* of defence but the
   `Tag` itself still went through the same vulnerable seeding. Fixed by
   mixing the offset in after hashing the bytes, not using it as their seed.
