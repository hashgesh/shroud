#pragma once
#include <string>
#include <vector>

namespace shroud {

struct Options {
    std::string input;
    std::string output;                  // empty => "<stem>.shroud<ext>" next to the input
    std::vector<std::string> functions;  // empty => the whole file; else names only
    bool inPlace = false;                // rewrite the input, keeping a backup
    std::string backupSuffix = ".orig";  // backup name for --in-place
    std::vector<std::string> args;       // exact compiler flags handed to libclang
    std::string map = "shroud.map";
    unsigned seed = 0;
    bool rename = true;
    bool strings = true;
    bool flow = true;
    unsigned flowPerFunction = 2;
    bool force = false;                  // continue despite parse errors, backup, or markers
};

// One entry of the side-car report the tool leaves behind: the evidence that a
// build is expected to be free of a name or a literal.
struct MapEntry {
    enum Kind { Name, String } kind;
    std::string label;                   // human readable form
    std::string bytes;                   // exact bytes to look for
};

struct Map {
    unsigned seed = 0;
    std::vector<MapEntry> entries;

    void addName(const std::string& name);
    void addString(const std::string& bytes);
    bool save(const std::string& path) const;
    bool load(const std::string& path);
};

int obfuscate(const Options& opt);
int verify(const std::string& binary, const std::string& mapPath, const std::string& baseline);

} // namespace shroud
