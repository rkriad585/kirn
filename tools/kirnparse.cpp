// kirnparse: lex + parse a Kirn source file.
//   kirnparse <file>          parse; print "OK" or diagnostics, exit 0/1
//   kirnparse --ast <file>    also dump the AST
//
// Phase 1: the body moved into kirn::driver::cmdParse so `kirnc parse` and
// this binary are the same code. See kirnlex.cpp for why this stays a real
// executable instead of a CMake ALIAS.
#include "driver/driver.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return kirn::driver::cmdParse(args, "kirnparse");
}
