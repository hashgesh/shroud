// Smoke target: exercises the three transforms plus the contexts shroud must
// leave alone. The obfuscated build has to print exactly the same bytes.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <cstddef>

// The runtime include shroud inserts must land after this continued directive,
// not between its lines.
#define SHD_SUM(a, b) \
    ((a) + (b))

// ...and it must not land inside this group, which is inactive.
#ifdef SHD_NEVER_DEFINED
constexpr int kPlatform = 0;
#else
constexpr int kPlatform = 1;
#endif

namespace {

int table[] = {3, 5, 7};

int helper(int v) {
    return v * 2 + table[v % 3];
}

constexpr const char* kConst = "constexpr stays";

} // namespace

#define MACRO_TEXT "macro text stays"

// A macro that spells its parameter: renaming the variable would rewrite only
// the declaration and break this use, so shroud must leave it alone.
#define PRINT_INT(v) std::printf("macro arg=%d\n", (v))

static int accumulate(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) {
            total += helper(i);
        } else {
            total -= helper(i) / 2;
        }
    }
    return total;
}

static int macro_user(int n) {
    int v = n * 3;
    PRINT_INT(v);
    return v;
}

static int lambda_user(int n) {
    int base = n + 2;
    auto add = [&base](int v) { return base + v; };
    return add(5);
}

// A literal bound to a `const char (&)[N]` parameter is used as an array, so
// the encrypted form must keep the array type (yaml-cpp's same(str, size, "null")).
// A directive in the middle of a statement: wrapping it would split the
// expression (Catch2: return c == '-' / #ifdef WIN32 / || c == '/').
static bool is_opt_prefix(char c) {
    return c == '-'
#ifdef SHD_NEVER_DEFINED
           || c == '/'
#endif
        ;
}

template <std::size_t N>
static bool same_text(const char* s, std::size_t len, const char (&lit)[N]) {
    return len == N - 1 && std::strncmp(s, lit, N - 1) == 0;
}

int main() {
    std::printf("platform=%d sum=%d\n", kPlatform, SHD_SUM(2, 3));
    std::printf("greeting: %s\n", "hello from shroud");

    std::string name = "world";
    std::printf("name=%s size=%zu\n", name.c_str(), name.size());

    std::string_view view = "view literal";
    std::printf("view=%s len=%zu\n", view.data(), view.size());

    const char* ptr = "pointer literal";
    std::printf("ptr=%s\n", ptr);

    char buf[] = "array stays";
    std::printf("buf=%s sizeof=%zu\n", buf, sizeof buf);

    std::printf("constexpr=%s\n", kConst);
    std::printf("macro=%s\n", MACRO_TEXT);

    static_assert(sizeof("static assert") == 14, "sizeof of a literal");

    const int total = accumulate(9);
    std::printf("total=%d\n", total);

    std::printf("macro_user=%d\n", macro_user(4));
    std::printf("lambda_user=%d\n", lambda_user(6));

    std::printf("cmp=%s\n", std::strcmp(ptr, "pointer literal") == 0 ? "equal" : "diff");

    std::printf("same=%d%d\n", same_text(name.data(), name.size(), "world") ? 1 : 0,
                same_text("nope", 4, "null") ? 1 : 0);

    std::printf("opt=%d%d\n", is_opt_prefix('-') ? 1 : 0, is_opt_prefix('x') ? 1 : 0);

    switch (total % 3) {
    case 0:
        std::printf("mod=0\n");
        break;
    case 1:
        std::printf("mod=1\n");
        break;
    default:
        std::printf("mod=2\n");
        break;
    }

    using namespace std::string_literals;
    const std::string udl = "suffix literal"s;
    std::printf("udl=%s\n", udl.c_str());
    return 0;
}
