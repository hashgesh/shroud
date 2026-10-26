// Unit test driver: pure-logic checks that don't need a real libclang parse
// (see tests/run.sh for the end-to-end checks that do). Not gtest/Catch2: a
// handful of CHECK()s does not need a dependency.
#include "check.hpp"

#include <cstdio>

void test_mapfile();
void test_verify();
void test_obfuscate();

int main() {
    test_mapfile();
    test_verify();
    test_obfuscate();
    if (g_unitFailures) {
        std::fprintf(stderr, "%d unit check(s) failed\n", g_unitFailures);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
