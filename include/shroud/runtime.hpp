#pragma once
// Runtime support for code transformed by shroud. Header-only, no dependencies.
// Generated code refers to ::shd::lit<> and ::shd::opaque(), so this header must
// stay on the include path of the obfuscated translation units.

#include <atomic>

#if defined(_MSC_VER)
#define SHD_NOINLINE __declspec(noinline)
#else
#define SHD_NOINLINE __attribute__((noinline))
#endif

namespace shd {

// Keystream. The generator includes this same header, so there is exactly one
// definition of the cipher. Not cryptographic: it only stops strings(1)/grep.
// shortcut: XOR stream with a 32-bit mix, upgrade to ChaCha20 if literal secrecy
// must survive a determined reverse engineer.
// Not constexpr: C++11 only allows a constexpr body that is a single return, and
// the generator needs C++11 targets (yaml-cpp builds with -std=gnu++11).
inline unsigned char ks(unsigned key, unsigned i) {
    unsigned x = key + i * 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return static_cast<unsigned char>(x);
}

// Rebuilds one literal per call site into a static buffer. Returns a reference
// to an array, so the result is still a string literal as far as the language is
// concerned: it binds to `const char (&)[N]` parameters such as
// template<size_t N> f(const char (&)[N]) and keeps sizeof() correct.
// Tag is a per-site value (the generator passes a 64-bit hash of the plaintext
// and its position): it makes each call site a distinct instantiation, so two
// different literals that happen to collide on Key cannot share one buffer and
// return each other's text.
template <unsigned Key, unsigned N, unsigned long long Tag>
const char (&lit(const char (&ct)[N]))[N] {
    static char buf[N];
    static const bool ready = [ct] {
        // A volatile read stops the optimizer from folding the decode into the
        // plaintext (GCC does it at -O3, leaving the literal in .rodata).
        const volatile char* src = ct;
        for (unsigned i = 0; i + 1 < N; ++i)
            buf[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ ks(Key, i));
        buf[N - 1] = '\0';
        return true;
    }();
    (void)ready;
    return buf;
}

// Opaque at compile time: the value comes from storage the optimizer cannot
// reason about. Callers only ever use it to pick between equivalent branches.
SHD_NOINLINE inline bool opaque() {
    // Atomic, not volatile: this is inline so there is one shared `state` across
    // the program, and a plain volatile read-modify-write is a data race once two
    // threads call opaque() at once (UB, flagged by ThreadSanitizer). Relaxed is
    // enough; callers only pick between equivalent branches, order does not matter.
    static std::atomic<unsigned char> state{0x5B};
    unsigned char v = state.load(std::memory_order_relaxed);
    v = static_cast<unsigned char>(v * 37u + 11u);
    state.store(v, std::memory_order_relaxed);
    return (v & 1u) != 0;
}

} // namespace shd
