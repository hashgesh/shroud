// The generated code has to keep building under C++11: yaml-cpp builds with
// -std=gnu++11, so the runtime header may not use C++14 constexpr bodies.
#include <cstdio>
#include <cstring>

static int classify(const char* name) {
    int score = 0;
    if (std::strcmp(name, "alpha") == 0) {
        score += 1;
    } else if (std::strcmp(name, "beta") == 0) {
        score += 2;
    } else {
        score += 4;
    }
    return score;
}

int main() {
    int total = 0;
    for (int i = 0; i < 2; ++i) {
        total += classify(i == 0 ? "alpha" : "gamma");
    }
    std::printf("cxx11=%d\n", total);
    return total == 5 ? 0 : 1;
}
