// Demo 1: identifier renaming.
// shroud obfuscate examples/rename.cpp --no-strings --no-flow
#include <cstdio>

namespace {

int counter = 41;                          // internal linkage -> renamed
int bump(int by) { return counter += by; } // internal linkage -> renamed

} // namespace

static int helper(int value) { return value * 2; } // internal linkage -> renamed

int public_value = 7; // external linkage -> keeps its name

int main() {
    const int local = bump(1); // local -> renamed
    std::printf("%d %d %d\n", local, helper(local), public_value);
    return 0;
}
