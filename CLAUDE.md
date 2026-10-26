# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

shroud is a source-level C++ obfuscator. It rewrites `.cpp` source (not compiler IR) so the same output builds under MSVC/GCC/Clang, then proves the plaintext is absent from the built binary. `README.md` is the design document and is kept current — read it before changing transform or verify behavior; the "Tested on real projects" and "Limits, honestly" sections record why specific edge cases are handled the way they are.

## Build & test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release    # LLVM_ROOT=/opt/llvm if libclang isn't standard
cmake --build build
ctest --test-dir build --output-on-failure        # "smoke" (tests/run.sh) + "unit" (tests/unit/)
```

libclang (any LLVM >= 16) is the only dependency, used only to parse. The tool needs C++20; the *generated* code must also compile as C++11 (enforced by run.sh step 11).

Two test binaries, both run by `ctest`:

- `tests/run.sh` (ctest `smoke`) — 18 numbered end-to-end checks, each building and running real binaries, one regression step per bug found on a real project (see README). To iterate on one check, run the script against a built binary and read to the failing step:

  ```sh
  tests/run.sh build/shroud          # or: SHROUD=build/shroud tests/run.sh
  ```

  When fixing a transform or verify bug that an end-to-end build can reproduce, add a numbered step here rather than relying on manual checks.

- `tests/unit/` (ctest `unit`, binary `build/unit_tests`) — plain `CHECK()`-based checks (not assert(): the project builds Release, which turns assert() into a no-op) for pure logic that doesn't need a real libclang parse: escape/UDL decoding, the `#if` scan, edit assembly (overlap drop, wrap offset shifting), map save/load, and verify's boundary checks. Each `test_*.cpp` `#include`s its `src/*.cpp` counterpart directly to reach its anonymous-namespace helpers — safe because an unnamed namespace has internal linkage, so merging two such files' symbols into different test TUs cannot collide. Prefer adding a check here over an end-to-end step when the bug is reachable without parsing real source (a hash collision, an off-by-one in a string scan); it runs in milliseconds and pins the exact function.

## Architecture

Two subcommands dispatched in `src/main.cpp`; shared declarations in `src/shroud.hpp`.

- **`obfuscate`** (`src/obfuscate.cpp`, ~1200 lines, the bulk of the project) walks the translation unit with libclang and applies three independent, individually-disableable transforms: identifier renaming, string-literal XOR encryption, and control-flow duplication behind opaque predicates. It emits an edit list over the original byte offsets and applies them, then writes a sibling `*.shroud.cpp` (or rewrites in place with a backup). It also appends to the **map** file.
- **`verify`** (`src/verify.cpp`) scans the built binary byte-by-byte — no ELF/PE parser, so it is cross-platform — and checks every map entry is absent. It exits non-zero on a surviving literal or name, so it works as a CI gate.
- **The map** (`src/mapfile.cpp`, `Map`/`MapEntry` in `shroud.hpp`) is the contract between the two commands: it records what each run changed so `verify` knows what must not appear. It **accumulates** across runs (never overwritten) because one linked binary covers many translation units.
- **`include/shroud/runtime.hpp`** is header-only (`shd::lit`, `shd::opaque`, keystream) and is `#include`d into obfuscated output. It must stay valid C++11.
- **`cmake/Shroud.cmake`** runs the obfuscator at configure time over a file list for downstream CMake projects; one tool run handles one TU, so it loops.

Key invariant: the obfuscator operates on raw source byte offsets from libclang cursors. Most of the non-obvious code exists because libclang's reported extents don't match source reality (macro expansions, lambda captures, inactive `#if` branches, qualified references, `switch` case constant-contexts, canonical declarations in headers sharing an offset with a local). Before touching offset handling, read the corresponding "found and fixed" note in the README — each guard is there for a reproduced failure.

## Conventions

- `shortcut: <limit>, <when to upgrade>` comments mark deliberate shortcuts with known limits (e.g. the XOR-stream cipher, the never-pruned map). Honor the form when adding one.
- `build/`, `build-w/` are build trees (gitignored); `build/examples`, `build/test` etc. hold generated transform output, not source.
