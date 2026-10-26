#include "shroud.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void usage() {
    std::printf(
        "shroud %s - source level obfuscator for C++20 (compiler agnostic)\n"
        "\n"
        "usage:\n"
        "  shroud obfuscate <file.cpp> [options] [-- <compiler flags>]\n"
        "  shroud verify <binary> [options]\n"
        "\n"
        "obfuscate options:\n"
        "  --out <file>            output file (default: <stem>.shroud<ext> next to the input)\n"
        "  --in-place              rewrite the input file; a backup is written first\n"
        "  --backup <.suffix>      backup suffix for --in-place (default: .orig)\n"
        "  --functions <a,b>       only transform inside these functions (default: whole file)\n"
        "  --map <file>            evidence file for `verify` (default: shroud.map)\n"
        "  --seed <n>              naming/keystream seed; use a random value per release\n"
        "  --no-rename             keep identifiers\n"
        "  --no-strings            keep string literals\n"
        "  --no-flow               keep control flow untouched\n"
        "  --flow-per-function <n> statements duplicated per function (default 2)\n"
        "  --force                 proceed despite parse errors, an existing backup,\n"
        "                          or a file that already looks obfuscated\n"
        "\n"
        "verify options:\n"
        "  --map <file>            evidence file written by obfuscate (default: shroud.map)\n"
        "  --baseline <binary>     report the size change against a reference build\n"
        "\n"
        "everything after `--` is passed to libclang, e.g.\n"
        "  shroud obfuscate app.cpp -- -std=c++20 -Iinclude -DNDEBUG\n",
        "0.1");
}

std::string need(const std::vector<std::string>& v, size_t& i, const char* flag) {
    if (i + 1 >= v.size()) {
        std::fprintf(stderr, "shroud: %s needs a value\n", flag);
        std::exit(2);
    }
    return v[++i];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }

    std::vector<std::string> rest, clangArgs;
    bool afterSeparator = false;
    for (int i = 2; i < argc; ++i) {
        if (!afterSeparator && std::strcmp(argv[i], "--") == 0) {
            afterSeparator = true;
            continue;
        }
        (afterSeparator ? clangArgs : rest).push_back(argv[i]);
    }

    if (cmd == "obfuscate") {
        shroud::Options opt;
        opt.args = clangArgs;
        bool backupGiven = false;
        for (size_t i = 0; i < rest.size(); ++i) {
            const std::string& a = rest[i];
            if (a == "--out") {
                opt.output = need(rest, i, "--out");
            } else if (a == "--in-place") {
                opt.inPlace = true;
            } else if (a == "--backup") {
                opt.backupSuffix = need(rest, i, "--backup");
                backupGiven = true;
            } else if (a == "--functions") {
                const std::string list = need(rest, i, "--functions");
                size_t start = 0;
                for (;;) {
                    const size_t comma = list.find(',', start);
                    const std::string name = list.substr(start, comma == std::string::npos ? comma : comma - start);
                    if (!name.empty()) opt.functions.push_back(name);
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            } else if (a == "--map") {
                opt.map = need(rest, i, "--map");
            } else if (a == "--seed") {
                opt.seed = static_cast<unsigned>(std::strtoul(need(rest, i, "--seed").c_str(), nullptr, 0));
            } else if (a == "--flow-per-function") {
                opt.flowPerFunction =
                    static_cast<unsigned>(std::strtoul(need(rest, i, "--flow-per-function").c_str(), nullptr, 0));
            } else if (a == "--no-rename") {
                opt.rename = false;
            } else if (a == "--no-strings") {
                opt.strings = false;
            } else if (a == "--no-flow") {
                opt.flow = false;
            } else if (a == "--force") {
                opt.force = true;
            } else if (!a.empty() && a[0] == '-') {
                std::fprintf(stderr, "shroud: unknown option %s\n", a.c_str());
                return 2;
            } else if (opt.input.empty()) {
                opt.input = a;
            } else {
                std::fprintf(stderr, "shroud: one input file per run (got %s and %s)\n", opt.input.c_str(), a.c_str());
                return 2;
            }
        }
        if (opt.input.empty()) {
            usage();
            return 2;
        }
        if (opt.inPlace && !opt.output.empty()) {
            std::fprintf(stderr, "shroud: --in-place and --out are mutually exclusive\n");
            return 2;
        }
        if (backupGiven && !opt.inPlace) {
            std::fprintf(stderr, "shroud: --backup only means something with --in-place\n");
            return 2;
        }
        return shroud::obfuscate(opt);
    }

    if (cmd == "verify") {
        std::string binary, mapPath = "shroud.map", baseline;
        for (size_t i = 0; i < rest.size(); ++i) {
            const std::string& a = rest[i];
            if (a == "--map") {
                mapPath = need(rest, i, "--map");
            } else if (a == "--baseline") {
                baseline = need(rest, i, "--baseline");
            } else if (!a.empty() && a[0] == '-') {
                std::fprintf(stderr, "shroud: unknown option %s\n", a.c_str());
                return 2;
            } else if (binary.empty()) {
                binary = a;
            } else {
                std::fprintf(stderr, "shroud: one binary per run\n");
                return 2;
            }
        }
        if (binary.empty()) {
            usage();
            return 2;
        }
        return shroud::verify(binary, mapPath, baseline);
    }

    std::fprintf(stderr, "shroud: unknown command %s\n", cmd.c_str());
    usage();
    return 2;
}
