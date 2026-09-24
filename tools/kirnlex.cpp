// kirnlex — Kirn lexer test driver.
//   kirnlex <path>...            lex each file/dir (recursively collects *.co)
//   kirnlex --dump <file.co>     print token stream for one file
#include "lex/lexer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

static void collect(const fs::path& p, std::vector<fs::path>& files) {
    if (fs::is_directory(p)) {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(p, ec)) {
            if (ec) break;
            if (e.is_regular_file() && e.path().extension() == ".co")
                files.push_back(e.path());
        }
    } else {
        files.push_back(p);
    }
}

int main(int argc, char** argv) {
    bool dump = false;
    std::vector<fs::path> paths;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dump") dump = true;
        else paths.push_back(a);
    }
    if (paths.empty()) {
        std::cerr << "usage: kirnlex [--dump] <file.co | dir>...\n";
        return 2;
    }

    std::vector<fs::path> files;
    for (auto& p : paths) collect(p, files);
    std::sort(files.begin(), files.end());

    size_t totalErrors = 0, okFiles = 0;
    for (auto& f : files) {
        std::string src;
        if (!readFile(f, src)) {
            std::cerr << f.string() << ": cannot read file\n";
            ++totalErrors;
            continue;
        }

        kirn::DiagEngine diags;
        kirn::Lexer lexer(src, f.string(), diags);
        std::vector<kirn::Token> toks = lexer.lexAll();

        if (dump && files.size() == 1) {
            for (const auto& t : toks)
                std::cout << kirn::tokName(t.kind) << '\t' << t.line << ':'
                          << t.col << '\t' << t.text << '\n';
        }

        if (diags.ok()) {
            ++okFiles;
            std::cout << f.string() << ": OK (" << (toks.size() - 1)
                      << " tokens)\n";
        } else {
            for (const auto& d : diags.diags())
                std::cout << f.string() << ':' << d.line << ':' << d.col
                          << ": error: " << d.message << '\n';
            totalErrors += diags.count();
        }
    }

    std::cout << "---\n" << okFiles << "/" << files.size()
              << " files clean, " << totalErrors << " diagnostic(s)\n";
    return totalErrors ? 1 : 0;
}
