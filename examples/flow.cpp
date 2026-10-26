// Demo 3: control flow duplication.
// shroud obfuscate examples/flow.cpp --no-rename --no-strings
#include <cstdio>

static int classify(int n) {
    if (n % 2 == 0) {
        std::printf("even %d\n", n);
    } else {
        std::printf("odd %d\n", n);
    }

    int total = 0;
    for (int i = 0; i < 3; ++i) {
        total += i * n;
    }
    return total;
}

int main() {
    std::printf("result %d\n", classify(4));
    std::printf("result %d\n", classify(5));
    return 0;
}
