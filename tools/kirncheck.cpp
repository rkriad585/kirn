// kirncheck: lex + parse + semantic analysis for a Kirn source file.
//   kirncheck <file>        check; print "OK" or diagnostics, exit 0/1
//
// Phase 1: the body moved into kirn::driver::cmdCheck so `kirnc check` and
// this binary are the same code. See kirnlex.cpp for why this stays a real
// executable instead of a CMake ALIAS.
#include "driver/driver.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return kirn::driver::cmdCheck(args, "kirncheck");
}
