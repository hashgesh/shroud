// Demo 2: string literal encryption.
// shroud obfuscate examples/strings.cpp --no-rename --no-flow
#include <cstdio>
#include <string>
#include <string_view>

namespace {

constexpr const char* kConst = "constexpr stays"; // constant context -> kept

char kArray[] = "array stays"; // array initialiser -> kept

} // namespace

#define MACRO_TEXT "macro text stays" // macro body -> kept

int main() {
    std::puts("plain literal becomes ciphertext");   // encrypted
    std::string name = "std::string works too";      // encrypted
    std::string_view view = "string_view works too"; // encrypted
    static_assert(sizeof("sizeof stays") == 13, ""); // sizeof operand -> kept

    std::printf("%s\n", name.c_str());
    std::printf("%s\n", view.data());
    std::printf("%s\n", kArray);
    std::printf("%s\n", kConst);
    std::printf("%s\n", MACRO_TEXT);
    return 0;
}
