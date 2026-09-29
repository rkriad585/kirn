// kirnrun: lex + parse + semantic-check + interpret a Kirn source file.
//   kirnrun <file.kn> [args...]
//                                 run program; main()'s Int return is the
//                                 process exit code (default 0)
#include "interp/runtime.h"
#include "lex/lexer.h"
#include "parser/parser.h"
#include "sema/checker.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "kirnrun: cannot open '" << path << "'\n";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Module search paths, in priority order:
//   1. $KIRN_PETS
//   2. <script dir>/pets      (per-project packages + ad-hoc modules)
//   3. ~/.kirn/pets            (global installs)
//   4. <script dir>/../stdlib       (repo checkout layout)
//   5. ./stdlib
//   6. $KIRN_STDLIB
void addModuleDirs(kirn::interp::Interpreter& interp,
                   const std::string& script) {
    size_t p = script.find_last_of("/\\");
    std::string dir = p == std::string::npos ? "." : script.substr(0, p);
    if (const char* env = std::getenv("KIRN_PETS"))
        interp.addStdlibDir(env);
    interp.addStdlibDir(dir + "/pets");
    if (const char* home = std::getenv("USERPROFILE")) {
        std::string g = std::string(home) + "/.kirn/pets";
        interp.addStdlibDir(g);
    } else if (const char* home2 = std::getenv("HOME")) {
        std::string g = std::string(home2) + "/.kirn/pets";
        interp.addStdlibDir(g);
    }
    interp.addStdlibDir(dir + "/../stdlib");
    interp.addStdlibDir("stdlib");
    if (const char* env = std::getenv("KIRN_STDLIB")) interp.addStdlibDir(env);
}

// Phase 8: the COCO_* env vars became KIRN_* and are a hard cut - never read.
// If a user/script still exports the old names, say so once instead of silently
// dropping the value (a stale var quietly losing effect is how module paths
// silently stop applying).
void warnLegacyEnv() {
    std::string found;
    if (std::getenv("COCO_LIBS")) found = "COCO_LIBS -> KIRN_PETS";
    if (std::getenv("COCO_STDLIB")) {
        if (!found.empty()) found += ", ";
        found += "COCO_STDLIB -> KIRN_STDLIB";
    }
    if (!found.empty())
        std::cerr << "warning: " << found
                  << " are no longer supported; use the KIRN_* names\n";
}

} // namespace

// shared pipeline: front-end + interpret, with panic handling
// The bytecode VM is now the DEFAULT runner: it is verified-correct against the
// tree-walker (32/32 differential, 33/33 corpus, ASan-clean) and substantially
// faster (see scripts/bench.ps1). Use --no-vm to force the tree-walker.
static bool g_useVm = true;
static int runSources(const std::string& label, const std::string& src,
                      const std::map<std::string, std::string>* embedded,
                      const std::vector<std::string>& progArgs) {
    kirn::DiagEngine diags;
    auto toks = kirn::Lexer(src, label, diags).lexAll();
    int ret = 0;
    if (diags.errorCount() == 0) {
        auto body = kirn::Parser(toks, diags).parseProgram();
        if (diags.errorCount() == 0) {
            kirn::sema::Checker checker(diags);
            checker.checkModule(body);
            if (diags.errorCount() == 0) {
                kirn::ast::Stmt module;
                module.kind = kirn::ast::StKind::Pass;
                module.body = std::move(body);
                try {
                    kirn::interp::Interpreter interp(module);
                    addModuleDirs(interp, label);
                    if (embedded)
                        for (const auto& [name, esrc] : *embedded)
                            interp.addEmbeddedSource(name, esrc);
                    interp.setProgramArgs(progArgs);
                    if (g_useVm) interp.enableVm();   // PLAN Phase 4 bytecode VM
                    kirn::interp::Value r = interp.run();
                    ret = r.k == kirn::interp::VK::Int ? (int)r.i : 0;
                } catch (const kirn::interp::PanicSignal& p) {
                    fflush(stdout);
                    fputs(("panic: " + p.msg + "\n").c_str(), stderr);
                    for (const auto& f : p.frames)
                        fputs(("  " + f + "\n").c_str(), stderr);
                    ret = 70;
                } catch (const kirn::interp::SignalRaise&) {
                    fflush(stdout);
                    fputs("panic: uncaught raise escaped main\n", stderr);
                    ret = 70;
                }
            }
        }
    }
    if (diags.errorCount() != 0) {
        for (const auto& d : diags.diags())
            if (d.sev == kirn::Sev::Error || d.sev == kirn::Sev::InternalError)
                std::cout << label << ":" << d.line << ":" << d.col
                          << ": error: " << d.message << "\n";
        std::cout << label << ": " << diags.errorCount() << " error(s)\n";
        return 1;
    }
    if (diags.warningCount()) {
        kirn::SourceMap sm(src);
        std::string out;
        kirn::renderDiags(label, sm, diags.diags(), /*color*/ false,
                          /*plain*/ false, out);
        std::cout << out;
    }
    return ret;
}

int main(int argc, char** argv) {
    warnLegacyEnv();
    std::string file;
    bool afterFile = false;
    std::vector<std::string> progArgs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (afterFile) {
            progArgs.push_back(a);
            continue;
        }
        if (a == "-h" || a == "--help") {
            std::cout << "usage: kirnrun [--no-vm|--vm] <file.kn> [args...]\n"
                         "  (the bytecode-VM accelerator is the default; --no-vm\n"
                         "   forces the tree-walker interpreter, --vm re-enables)\n"
                         "  arguments after <file> are passed to the program as os.args()\n";
            return 0;
        }
        if (a == "--vm") { g_useVm = true; continue; }
        if (a == "--no-vm") { g_useVm = false; continue; }
        file = a;
        afterFile = true;
    }
    if (file.empty()) {
        std::cerr << "usage: kirnrun <file.kn> [args...]\n";
        return 2;
    }

    std::string src;
    if (!readFile(file, src)) return 2;
    return runSources(file, src, nullptr, progArgs);
}
