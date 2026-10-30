#!/usr/bin/env bash
# Prove a packaged release is self-contained: with no system LLVM on the loader
# path it must resolve libclang and the clang builtin-header resource-dir
# relative to the binary, then obfuscate, build and run. This is what breaks a
# release when the build-machine's absolute paths are baked in (the RPATH and
# the resource-dir); the checks below fail loudly if that regresses.
#
#   tests/pkg-smoke.sh <package-root> [sample.cpp]
#
# The package root is an installed/extracted tree (bin/, lib/, include/, ...).
# Extra clang flags for the obfuscate step can be passed in SHROUD_OBF_FLAGS
# (e.g. -isysroot on macOS); the compiler for the build step is $CXX.
set -euo pipefail

pkg_in=${1:?usage: pkg-smoke.sh <package-root> [sample.cpp]}
root=$(cd "$(dirname "$0")/.." && pwd)
sample=${2:-$root/tests/sample/main.cpp}
cxx=${CXX:-c++}
obf_flags=${SHROUD_OBF_FLAGS:-}
os=$(uname -s)

fail() { echo "FAIL: $*" >&2; exit 1; }

# Relocate the package to a fresh prefix so nothing can depend on its build-time
# or install-time location. Everything below runs against this moved copy.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pkg="$work/shroud-pkg"
cp -a "$(cd "$pkg_in" && pwd)" "$pkg"

bin="$pkg/bin/shroud"
[ -x "$bin" ] || fail "no executable at $bin"

echo "[1/6] packaged libclang and resource-dir are present"
ls "$pkg"/lib/clang/*/include/stddef.h >/dev/null 2>&1 || fail "no packaged clang resource-dir"
case "$os" in
    Darwin) ls "$pkg"/lib/libclang*.dylib >/dev/null 2>&1 || fail "libclang not bundled" ;;
    MINGW*|MSYS*|CYGWIN*) ls "$pkg"/bin/libclang.dll >/dev/null 2>&1 || fail "libclang.dll not bundled" ;;
    *)      ls "$pkg"/lib/libclang*.so* >/dev/null 2>&1 || fail "libclang not bundled" ;;
esac

echo "[2/6] binary references libclang relatively, not by an absolute build path"
case "$os" in
    Linux)
        rp=$(readelf -d "$bin" | awk '/R(UN)?PATH/{print $NF}' | tr -d '[]')
        echo "   runpath: ${rp:-<none>}"
        case "$rp" in
            *'$ORIGIN'*) ;;
            *) fail "runpath is not \$ORIGIN-relative: ${rp:-<none>}" ;;
        esac
        ;;
    Darwin)
        if otool -L "$bin" | grep -E 'libclang' | grep -vq '@rpath'; then
            otool -L "$bin" | grep libclang >&2
            fail "binary references libclang by a non-@rpath path"
        fi
        ;;
    *) echo "   (Windows: libclang.dll sits beside the .exe, no RPATH to check)" ;;
esac

echo "[3/6] libclang resolves inside the package with the environment stripped"
case "$os" in
    Linux)
        deps=$(env -i LD_LIBRARY_PATH= ldd "$bin" 2>&1 || true)
        echo "$deps" | grep -q 'not found' && { echo "$deps" | grep 'not found' >&2; fail "unresolved dependency"; }
        echo "$deps" | grep -E 'libclang' | grep -q "$pkg/" \
            || { echo "$deps" | grep -i clang >&2; fail "libclang did not resolve inside the package"; }
        # A clean machine has no system libLLVM, and ldd above can be satisfied by
        # one. So check by name: every libLLVM soname the bundled libclang needs
        # must exist by that exact filename in lib/, or it won't resolve off-box.
        for cl in "$pkg"/lib/libclang*.so*; do
            [ -f "$cl" ] || continue
            for need in $(readelf -d "$cl" | awk '/NEEDED/ && /libLLVM/ {gsub(/[][]/,"",$NF); print $NF}'); do
                [ -e "$pkg/lib/$need" ] \
                    || { ls -la "$pkg"/lib >&2; fail "libclang needs $need but no file of that name is in lib/ (soname mismatch)"; }
                echo "   libLLVM dependency present in-package: $need"
            done
        done
        ;;
    *) echo "   (skipped on $os; step 4 exercises the real loader)" ;;
esac

echo "[4/6] obfuscate the sample with no LLVM in the environment"
cp "$sample" "$work/sample.cpp"
# argv survives env -i (only the environment is cleared): no LLVM_ROOT, no
# LD_LIBRARY_PATH / DYLD_*, and a PATH with no LLVM bindir. The binary must find
# everything it needs through its own relative layout.
( cd "$work" && env -i HOME="${HOME:-/tmp}" PATH="/usr/bin:/bin" \
    "$bin" obfuscate sample.cpp --out sample.obf.cpp --map sample.map -- $obf_flags )
[ -s "$work/sample.obf.cpp" ] || fail "no obfuscated output produced"
grep -q '#include <shroud/runtime.hpp>' "$work/sample.obf.cpp" || fail "output missing runtime include"

echo "[5/6] obfuscated sample builds against the packaged header and runs"
"$cxx" -std=c++20 -O1 -I"$pkg/include" $obf_flags "$work/sample.obf.cpp" -o "$work/sample"
"$work/sample" >/dev/null

echo "[6/6] resource-dir is taken from the package, not a build-time absolute path"
if [ "$os" = "Linux" ]; then
    # Decisive: break only the packaged builtin stddef.h. A source that pulls it
    # must now fail with an error citing the packaged path -- proving the
    # exe-relative resource-dir is authoritative over any baked fallback.
    hdr=$(ls "$pkg"/lib/clang/*/include/stddef.h | head -n1)
    printf '#error shroud_pkg_smoke_poisoned_builtin_header\n' > "$hdr"
    printf '#include <cstddef>\nint main(){ return (int)sizeof(std::size_t) - (int)sizeof(std::size_t); }\n' \
        > "$work/probe.cpp"
    out=$( cd "$work" && env -i HOME="${HOME:-/tmp}" PATH="/usr/bin:/bin" \
        "$bin" obfuscate probe.cpp --out probe.obf.cpp --map probe.map -- $obf_flags 2>&1 ) && rc=0 || rc=$?
    [ "$rc" -ne 0 ] || { echo "$out" >&2; fail "obfuscate ignored the poisoned packaged header (relative resource-dir not used)"; }
    echo "$out" | grep -q "$hdr" || { echo "$out" >&2; fail "failure did not cite the packaged header $hdr"; }
    echo "   confirmed: clang read the packaged builtin header at $hdr"
else
    echo "   (decisive check is Linux-only; steps 1-2 already tie the resource-dir to the package)"
fi

echo "PASS: packaged shroud is self-contained and relocatable"
