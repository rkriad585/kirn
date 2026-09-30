// kirnlex — Kirn lexer test driver.
//
//   kirnlex <path>...            lex each file/dir (recursively collects *.kn)
//   kirnlex --dump <file.kn>     print token stream for one file
//
// Phase 1: the body moved into kirn::driver::cmdLex so `kirnc lex` and this
// binary are the same code. This file stays a real executable rather than a
// CMake ALIAS or a shell shim: an ALIAS is not a file on disk (scripts that
// check `[ -x build/kirnlex ]` would fail) and a shell script breaks argv[0],
// quoting, and Windows. Only the argv split remains here.
#include "driver/driver.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return kirn::driver::cmdLex(args, "kirnlex");
}
