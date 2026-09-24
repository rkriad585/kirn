// kirnparse: lex + parse a Kirn source file.
//   kirnparse <file>          parse; print "OK" or diagnostics, exit 0/1
//   kirnparse --ast <file>    also dump the AST
#include "lex/lexer.h"
#include "parser/parser.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "kirnparse: cannot open '" << path << "'\n";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void printDiags(const kirn::DiagEngine& diags, const std::string& filename) {
    for (const auto& d : diags.diags())
        std::cout << filename << ":" << d.line << ":" << d.col
                  << ": error: " << d.message << "\n";
}

} // namespace

int main(int argc, char** argv) {
    bool wantAst = false;
    std::string file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ast") wantAst = true;
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: kirnparse [--ast] <file.co>\n";
            return 0;
        } else file = a;
    }
    if (file.empty()) {
        std::cerr << "usage: kirnparse [--ast] <file.co>\n";
        return 2;
    }

    std::string src;
    if (!readFile(file, src)) return 2;

    kirn::DiagEngine diags;
    auto toks = kirn::Lexer(src, file, diags).lexAll();

    size_t lexErrors = diags.count();
    size_t parseErrors = 0;
    if (lexErrors == 0) {
        auto prog = kirn::Parser(toks, diags).parseProgram();
        parseErrors = diags.count() - lexErrors;
        if (wantAst)
            for (const auto& st : prog) kirn::ast::dump(*st);
        if (parseErrors == 0)
            std::cout << file << ": OK (" << prog.size() << " top-level statements)\n";
    }

    printDiags(diags, file);
    size_t total = diags.count();
    if (total == 0) return 0;
    std::cout << file << ": " << total << " error(s) ("
              << lexErrors << " lex, " << parseErrors << " parse)\n";
    return 1;
}
