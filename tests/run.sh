#!/usr/bin/env bash
# End to end check: obfuscated code must behave exactly like the original, the
# verifier must pass on the obfuscated binary and fail on the plain one.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
shroud=${1:-${SHROUD:-$root/build/shroud}}
out=${BUILD_DIR:-$root/build/test}
cxx=${CXX:-g++}
src="$root/tests/sample/main.cpp"

mkdir -p "$out"
rm -f "$out"/*.cpp "$out"/*.txt "$out"/*.map "$out"/*.orig

echo "[1/18] baseline build ($cxx)"
"$cxx" -std=c++20 -O1 "$src" -o "$out/app.orig"
"$out/app.orig" > "$out/expected.txt"

echo "[2/18] obfuscate"
"$shroud" obfuscate "$src" --out "$out/main.shroud.cpp" --map "$out/shroud.map" --seed 0x1234

# The include must follow the continued #define above it, not split it, and it
# must stay outside the inactive #if group.
inc=$(grep -n '#include <shroud/runtime.hpp>' "$out/main.shroud.cpp" | cut -d: -f1)
grp=$(grep -n '#ifdef SHD_NEVER_DEFINED' "$out/main.shroud.cpp" | cut -d: -f1)
if [ -z "$inc" ] || [ "$inc" -ge "$grp" ]; then
    echo "FAIL: runtime include was placed at line ${inc:-?}, group starts at $grp"
    exit 1
fi

echo "[3/18] obfuscated build"
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/main.shroud.cpp" -o "$out/app.obf"

echo "[4/18] behaviour must be identical"
"$out/app.obf" > "$out/actual.txt"
diff -u "$out/expected.txt" "$out/actual.txt"

echo "[5/18] verifier must pass on the obfuscated binary"
"$shroud" verify "$out/app.obf" --map "$out/shroud.map" --baseline "$out/app.orig"

echo "[6/18] verifier must fail on the plain binary"
if "$shroud" verify "$out/app.orig" --map "$out/shroud.map" > "$out/negative.txt" 2>&1; then
    echo "FAIL: verifier accepted the unobfuscated binary"
    cat "$out/negative.txt"
    exit 1
fi

echo "[7/18] --functions must leave the rest of the file alone"
"$shroud" obfuscate "$src" --out "$out/scoped.cpp" --map "$out/scoped.map" --seed 0x1234 \
    --functions accumulate --no-strings --no-flow > /dev/null
sed -n '/^int main/,$p' "$src" > "$out/main.before"
sed -n '/^int main/,$p' "$out/scoped.cpp" > "$out/main.after"
diff -u "$out/main.before" "$out/main.after"
grep -q 'shd_' "$out/scoped.cpp"

echo "[8/18] --in-place must back up and refuse a second run"
cp "$src" "$out/inplace.cpp"
"$shroud" obfuscate "$out/inplace.cpp" --in-place --map "$out/inplace.map" --seed 0x1234 > /dev/null
cmp -s "$src" "$out/inplace.cpp.orig"
if cmp -s "$src" "$out/inplace.cpp"; then
    echo "FAIL: --in-place did not change the file"
    exit 1
fi
if "$shroud" obfuscate "$out/inplace.cpp" --in-place --map "$out/inplace.map" --seed 0x1234 > /dev/null 2>&1; then
    echo "FAIL: second --in-place run was allowed to overwrite the backup"
    exit 1
fi
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/inplace.cpp" -o "$out/app.inplace"
"$out/app.inplace" > "$out/inplace.txt"
diff -u "$out/expected.txt" "$out/inplace.txt"

echo "[9/18] the shared map must keep entries from earlier files"
"$shroud" obfuscate "$src" --out "$out/a.cpp" --map "$out/union.map" --seed 0x1234 --no-strings --no-flow > /dev/null
"$shroud" obfuscate "$root/examples/rename.cpp" --out "$out/b.cpp" --map "$out/union.map" --seed 0x1234 \
    --no-strings --no-flow > /dev/null
grep -q '# accumulate' "$out/union.map"
grep -q '# bump' "$out/union.map"

echo "[10/18] --functions must still encrypt strings inside the selected function"
"$shroud" obfuscate "$src" --out "$out/scoped-strings.cpp" --map "$out/scoped-strings.map" --functions main \
    --no-rename --no-flow --seed 0x1234 > /dev/null
grep -q 'shd::lit' "$out/scoped-strings.cpp"
if sed -n '1,/^int main/p' "$out/scoped-strings.cpp" | grep -q 'shd::lit'; then
    echo "FAIL: --functions main rewrote text outside main"
    exit 1
fi
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/scoped-strings.cpp" -o "$out/app.scoped"
"$out/app.scoped" > "$out/scoped.txt"
diff -u "$out/expected.txt" "$out/scoped.txt"

echo "[11/18] generated code must also build as C++11"
"$shroud" obfuscate "$root/tests/sample/cxx11.cpp" --out "$out/cxx11.cpp" --map "$out/cxx11.map" --seed 0x1234 \
    > /dev/null
"$cxx" -std=c++11 -Wall -Wextra -pedantic -O1 -I"$root/include" "$out/cxx11.cpp" -o "$out/cxx11"
"$out/cxx11" > "$out/cxx11.txt"
echo "cxx11=5" | diff -u - "$out/cxx11.txt"

echo "[12/18] switch bodies and user defined literals are encrypted, -O3 must not fold them"
cat > "$out/switch.cpp" <<'EOF'
#include <cstdio>
struct S { const char* p; unsigned long n; };
constexpr S operator""_sr(const char* p, unsigned long n) { return S{p, n}; }
static const char* pick(int x) {
    switch (x) {
    case 1: return "alpha";
    default: return "omega";
    }
}
int main(int argc, char**) {
    std::printf("%s %s\n", pick(argc), pick(argc - 1));
    std::printf("%s\n", "tail");
    S s = "suffix"_sr;
    std::printf("%s %lu\n", s.p, s.n);
}
EOF
"$shroud" obfuscate "$out/switch.cpp" --out "$out/switch.obf.cpp" --map "$out/switch.map" --seed 0x1234 \
    > /dev/null
if grep -q '"alpha"\|"omega"\|"tail"\|"suffix"' "$out/switch.obf.cpp"; then
    echo "FAIL: a literal in a switch body was left as text"
    exit 1
fi
"$cxx" -std=c++20 -O3 -I"$root/include" -c "$out/switch.obf.cpp" -o "$out/switch.o"
for lit in alpha omega tail suffix; do
    if strings "$out/switch.o" | grep -qx "$lit"; then
        echo "FAIL: $lit survived -O3 in the object"
        exit 1
    fi
done
"$cxx" -std=c++20 -O3 -I"$root/include" "$out/switch.obf.cpp" -o "$out/switch"
printf 'alpha omega\ntail\nsuffix 6\n' | diff -u - <("$out/switch")

echo "[13/18] a header declaration must not borrow the name of a local at the same offset"
cat > "$out/collide.cpp" <<'EOF'
#include "collide.h"
int api(int arg) { int spot = arg; return spot + 1; }
int main() { return api(0) - 1; }
EOF
# Put `api` in the header at the exact byte offset of `spot` in the .cpp, which is
# what libclang reports for the canonical cursor of the definition.
spot=$(grep -bo 'spot = arg' "$out/collide.cpp" | cut -d: -f1)
{ printf "%$((spot - 4))s" ''; echo 'int api(int);'; } > "$out/collide.h"
"$shroud" obfuscate "$out/collide.cpp" --out "$out/collide.obf.cpp" --map "$out/collide.map" \
    --seed 0x1234 --no-strings --no-flow > /dev/null
if ! grep -q 'int api' "$out/collide.obf.cpp"; then
    echo "FAIL: external api() was renamed through a header offset collision"
    exit 1
fi
"$cxx" -std=c++20 -O1 -I"$out" -I"$root/include" "$out/collide.obf.cpp" -o "$out/collide"
"$out/collide"

echo "[14/18] an all-NUL literal cannot be proven leaked by zero padding"
cat > "$out/zero.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("%d\n", "\0\0\0\0\0\0"[5]); }
EOF
"$shroud" obfuscate "$out/zero.cpp" --out "$out/zero.obf.cpp" --map "$out/zero.map" --seed 0x1234 > /dev/null
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/zero.obf.cpp" -o "$out/zero"
"$out/zero" | diff -u - <(echo 0)
"$shroud" verify "$out/zero" --map "$out/zero.map" > /dev/null

echo "[15/18] lit returns the right string per call site; opaque() race-free under threads"
cat > "$out/threads.cpp" <<'EOF'
#include <cstdio>
#include <string>
#include <thread>
// Two distinct literals at two call sites (lit must not mix them), inside a
// flow-wrappable loop so opaque() runs; two threads call it at once.
static std::string tag(int n) {
    const char* a = "left-marker-string";
    const char* b = "right-marker-string";
    std::string r;
    for (int i = 0; i < n; ++i) { r += a; r += b; }
    return r;
}
int main() {
    std::string e1, e2;
    std::thread t1([&] { for (int i = 0; i < 5000; ++i) e1 = tag(1); });
    std::thread t2([&] { for (int i = 0; i < 5000; ++i) e2 = tag(1); });
    t1.join();
    t2.join();
    std::printf("%s|%s\n", e1.c_str(), e2.c_str());
}
EOF
"$shroud" obfuscate "$out/threads.cpp" --out "$out/threads.obf.cpp" --map "$out/threads.map" --seed 0x1234 \
    > /dev/null
"$cxx" -std=c++20 -O1 -I"$root/include" -pthread "$out/threads.obf.cpp" -o "$out/threads"
printf 'left-marker-stringright-marker-string|left-marker-stringright-marker-string\n' \
    | diff -u - <("$out/threads")
if printf 'int main(){}\n' | "$cxx" -std=c++20 -fsanitize=thread -x c++ - -o "$out/tsan-probe" 2>/dev/null; then
    "$cxx" -std=c++20 -O1 -fsanitize=thread -I"$root/include" -pthread "$out/threads.obf.cpp" -o "$out/threads.tsan"
    TSAN_OPTIONS=halt_on_error=1 "$out/threads.tsan" > /dev/null
else
    echo "   (tsan unavailable, skipped the race check)"
fi

echo "[16/18] a literal nested in an InitListExpr/CXXNewExpr must not inherit a reference-typed ancestor's skip"
cat > "$out/refinit.cpp" <<'EOF'
#include <cstdio>
#include <set>
#include <string>
// A literal used as a std::string constructor argument *inside* a container's
// initializer list, where the whole thing initializes a reference-typed local.
// The reference-typed VarDecl must not make the literal "not a pointer": it is
// an argument to std::string's converting constructor, not that var's own
// initializer (found obfuscating jsoncpp's StreamWriterBuilder::validate()).
static bool hasKey(const std::string& x) {
    static const auto& keys = *new std::set<std::string>{"abcdef", "ghijkl"};
    return keys.count(x) > 0;
}
int main() { std::printf("%d %d\n", hasKey("abcdef") ? 1 : 0, hasKey("zzzzzz") ? 1 : 0); }
EOF
"$shroud" obfuscate "$out/refinit.cpp" --out "$out/refinit.obf.cpp" --map "$out/refinit.map" --seed 0x1234 \
    > /dev/null
if grep -q '"abcdef"\|"ghijkl"' "$out/refinit.obf.cpp"; then
    echo "FAIL: a literal inside the set<string>{...} initializer was left as text"
    exit 1
fi
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/refinit.obf.cpp" -o "$out/refinit"
printf '1 0\n' | diff -u - <("$out/refinit")

echo "[17/18] an overloaded free function used from inside another template must keep its name"
cat > "$out/overload.cpp" <<'EOF'
#include <cstdio>
namespace {
// A non-template overload and a function template sharing a spelling. A call
// to it from inside *another* template is a dependent lookup resolved again,
// by plain spelling, at instantiation time: renaming only the non-template
// overload makes that lookup fail to find it and silently fall through to the
// template instead (found obfuscating googletest's IsSubstringPred, and
// benchmark's size_impl()/join_impl() base case vs. variadic-template case).
bool eq(const char* a, const char* b) { return a[0] == b[0]; }
template <typename T>
bool eq(const T& a, const T& b) { return a == b; }
template <typename T>
bool same(const T& a, const T& b) { return eq(a, b); }
}
int main() { std::printf("%d %d\n", same("abc", "abd") ? 1 : 0, same(5, 5) ? 1 : 0); }
EOF
"$shroud" obfuscate "$out/overload.cpp" --out "$out/overload.obf.cpp" --map "$out/overload.map" --seed 0x1234 \
    > /dev/null
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/overload.obf.cpp" -o "$out/overload"
printf '1 1\n' | diff -u - <("$out/overload")

echo "[18/18] two literals at offsets whose delta cancels their byte delta must not share a keystream"
cat > "$out/collide2.cpp" <<'EOF'
#include <cstdio>
// "Ki" and "Gi" sit 12 bytes apart (the width of `"Mi", `) and their first
// bytes differ by exactly 0x4B ^ 0x47 = 12: seeding the ciphertext's FNV-1a
// hash with the raw source offset let that delta cancel out, so both got the
// same key *and* the same per-site Tag, and lit<>() returned "Ki" for both
// (found obfuscating benchmark's kBigIECUnits[], a real latent bug, not
// hypothetical: this exact array is shipped in google/benchmark).
static const char* const kUnits[] = {"Ki", "Mi", "Gi", "Ti"};
int main() { std::printf("%s %s\n", kUnits[0], kUnits[2]); }
EOF
"$shroud" obfuscate "$out/collide2.cpp" --out "$out/collide2.obf.cpp" --map "$out/collide2.map" --seed 0x1234 \
    > /dev/null
"$cxx" -std=c++20 -O1 -I"$root/include" "$out/collide2.obf.cpp" -o "$out/collide2"
printf 'Ki Gi\n' | diff -u - <("$out/collide2")

echo "OK"
