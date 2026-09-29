// KIRNBUNDLE runner: the prebuilt standalone runtime for bundled apps.
//
// `kirn build` (host) and app installs produce a real executable with NO C++
// toolchain at user time by copying this prebuilt runner and appending the
// app's sources as a KIRNBUNDLE/1 payload (tools/kirn.cpp:appendBundle).
// This main() locates its own executable, splits off the trailing payload,
// feeds the embedded sources to the interpreter and runs the entry program.
//
// Built once per target (lazily by the driver, then cached next to it) with
// the full src/** runtime, mirroring how Go ships its prebuilt toolchain.
#include "interp/runtime.h"
#include "lex/lexer.h"
#include "parser/parser.h"
#include "sema/checker.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

std::string selfExe() {
#if defined(_WIN32)
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n && n < sizeof buf ? std::string(buf, n) : "";
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, n) : "";
#endif
}

// read a whole file as binary bytes
bool slurp(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

struct Bundle {
    std::string name, version, target, entry;
    std::vector<std::pair<std::string, std::string>> sections;  // key, body
};

// split the KIRNBUNDLE/1 payload off the tail of our own binary
bool parseBundle(const std::string& data, Bundle& b) {
    const char magic[] = "\nKIRNBUNDLE/1\n";
    size_t pos = data.rfind(magic);
    if (pos == std::string::npos) return false;
    pos += strlen(magic);
    std::istringstream in(data.substr(pos));
    std::string line;
    std::string curKey;
    std::string cur;
    while (std::getline(in, line)) {
        if (line.rfind("@@FILE ", 0) == 0) {
            curKey = line.substr(7);
            cur.clear();
        } else if (line == "@@END") {
            if (curKey == "@meta") {
                std::istringstream m(cur);
                std::getline(m, b.name);
                std::getline(m, b.version);
                std::getline(m, b.target);
                std::getline(m, b.entry);
            } else if (!curKey.empty()) {
                b.sections.push_back({curKey, cur});
            }
            curKey.clear();
        } else {
            cur += line + "\n";
        }
    }
    return !b.entry.empty();
}

}  // namespace

int main(int argc, char** argv) {
    std::string exe = selfExe();
    std::string data;
    if (!slurp(exe, data)) {
        std::fputs("kirnrt: cannot read own executable\n", stderr);
        return 65;
    }
    Bundle b;
    if (!parseBundle(data, b)) {
        std::fputs("kirnrt: no KIRNBUNDLE payload attached\n", stderr);
        return 65;
    }

    if (argc > 1 && (std::strcmp(argv[1], "--version") == 0 ||
                     std::strcmp(argv[1], "-V") == 0)) {
        std::printf("%s v%s (%s)\n", b.name.c_str(), b.version.c_str(),
                    b.target.c_str());
        return 0;
    }

    // the entry program occupies its own section; the rest are modules
    std::string mainSrc;
    for (const auto& s : b.sections)
        if (s.first == b.entry) mainSrc = s.second;
    if (mainSrc.empty()) {
        std::fprintf(stderr, "kirnrt: entry '%s' missing from bundle\n",
                     b.entry.c_str());
        return 65;
    }

    kirn::DiagEngine diags;
    auto toks = kirn::Lexer(mainSrc, b.entry, diags).lexAll();
    if (diags.errorCount()) {
        std::fputs("embedded source error\n", stderr);
        return 65;
    }
    auto body = kirn::Parser(toks, diags).parseProgram();
    if (diags.errorCount()) return 65;
    { kirn::sema::Checker chk(diags); chk.checkModule(body); }
    if (diags.errorCount()) return 65;

    kirn::ast::Stmt root;
    root.kind = kirn::ast::StKind::Pass;
    root.body = std::move(body);
    try {
        kirn::interp::Interpreter interp(root);
        for (const auto& s : b.sections)
            if (s.first != b.entry)
                interp.addEmbeddedSource(s.first, s.second);
        interp.enableVm();   // bytecode VM is the default runner
        auto r = interp.run();
        return r.k == kirn::interp::VK::Int ? static_cast<int>(r.i) : 0;
    } catch (const kirn::interp::PanicSignal& p) {
        fflush(stdout);
        fputs(("panic: " + p.msg + "\n").c_str(), stderr);
        return 70;
    } catch (const kirn::interp::SignalRaise&) {
        fputs("panic: uncaught raise\n", stderr);
        return 70;
    }
}