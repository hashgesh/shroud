#pragma once
// Tiny check helper for the unit tests. Deliberately not assert(): the project
// builds Release (NDEBUG defined), which turns assert() into a no-op, so a
// unit test built that way would silently "pass" even on a real failure.
#include <cstdio>

inline int g_unitFailures = 0;

inline void unitCheck(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++g_unitFailures;
    }
}

#define CHECK(cond) unitCheck((cond), #cond, __FILE__, __LINE__)
