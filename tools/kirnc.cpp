// kirnc — the compiler driver proper (phase 1 skeleton).
//
//   kirnc <verb> [inputs] [flags]
//
//   check   lex + parse + type-check only          exit 1 on error
//   lex     lex only; print tokens with --emit-tokens
//   parse   lex + parse; print AST with --emit-ast
//   run     execute the program with the bundled runtime
//   build   build an executable (default verb)
//
// `kirnc` is the rustc/gcc-style half of the split: `kirn` stays the
// user-facing orchestrator (packages, `new`, `test`, `doc`), and `kirnc` owns
// the compilation verbs and the stage grammar. This phase is deliberately a
// skeleton: the stage verbs are the real thing (they share one implementation
// with kirnlex/kirnparse/kirncheck), while the stop-at-stage flags are declared
// and rejected with a pointer to the phase that lands them, rather than
// half-implemented.
//
// Exit codes follow the plan: 1 = "check found errors" (what gcc, clang, rustc
// and nim all use; note cargo check is 101), 2 = bad usage. The library's own
// sysexits codes (64 usage, 65 compile, 70 internal) stay internal to `kirn` so
// that the gate scripts, which assert on them, keep working.
#include "driver/driver.h"

#include <fstream>
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#ifdef KIRN_HAS_LLVM
#include "backends/llvm/emit_llvm.h"
#endif

namespace {

const char* const kUsage =
    "usage: kirnc [verb] <file.kn> [flags]\n"
    "       verbs: check|lex|parse|run|hir|rt|build|llvm (default: check)\n"
    "\n"
    "verbs:\n"
    "  check   lex + parse + type-check only; exit 1 on error\n"
    "  lex     lex only (--emit-tokens to print the token stream)\n"
    "  parse   lex + parse (--emit-ast to print the AST)\n"
    "  hir     lower to HIR, verify structurally, print the dump\n"
    "  rt      lower through HIR to MIR, verify the CFG, print the dump\n"
    "  llvm    lower through HIR+MIR to a verified LLVM module, print the IR\n"
    "  run     run a file or kirn.toml project through the Kirn runtime\n"
    "  build   build an AOT executable (default verb)\n"
    "\n"
    "flags:\n"
    "  --emit-tokens   print the token stream (lex)\n"
    "  --emit-ast      print the AST (parse)\n"
    "  --emit-hir      lower to HIR, verify it, print the dump (check)\n"
    "  --emit-mir      lower through HIR to a verified MIR CFG and print it (rt)\n"
    "  -emit-llvm      lower through HIR+MIR to LLVM IR (same as the `llvm` verb)\n"
    "  -E              preprocess only; print the expanded token stream\n"
    "  -D name[=val]   define a macro (with -E)\n"
    "  -U name         undefine a macro (with -E)\n"
    "  -o <path>       output path (build)\n"
    "  -fbackend=<b>   backend selector: cpp (default), runtime, or llvm\n"
    "                  (`runtime` keeps the prebuilt interpreter runner)\n"
    "  --plain         no source/caret lines in diagnostics (check)\n"
    "  --color         force ANSI colour in diagnostics (check)\n"
    "  -h, --help      this message\n"
    "\n"
    "stop-at-stage flags use the system C++ toolchain; -### prints without executing.\n";

bool isHelp(const std::string& a) { return a == "-h" || a == "--help"; }

// Stage flags the plan schedules for later phases. Rejecting them loudly beats
// silently treating "-c" as an input filename, which is what the old tools did
// with any unrecognised argument.
//
// -E is NOT here any more: it landed with the preprocessor in phase 2, and
// kirnc dispatches it below.
// --emit-hir was here and has landed (phase 3). --emit-mir stays, and lands
// with the MIR CFG. Both dash spellings are listed for each because the plan
// uses zig-style single dash in one place and gcc-style in another; dropping
// either would make it a filename.
// --emit-mir landed with the MIR CFG (phase 3); -emit-sasm lands with
// codegen in phase 5. Both dash spellings are listed for each because the plan
// uses zig-style single dash in one place and gcc-style in another; dropping
// either would make it a filename. -emit-llvm landed with the LLVM backend
// (phase 4) but is NOT in this list: it is a real verb below (`llvm`).
const char* const kLaterFlags[] = {"-S", "-s", "-c", "-###", "-v", "--verbose",
                                   "-emit-sasm", "--emit-sasm"};

bool isLaterFlag(const std::string& a) {
    for (const char* f : kLaterFlags)
        if (a == f) return true;
    return false;
}

// The `-fbackend=llvm|cpp` selector from the flags table (BUILDER-PLAN 3.4).
// Both dash spellings are accepted, matching the convention that a flag
// spelled either way must never be a filename. The selector is validated and
// removed wherever it appears; verbs below `run`/`build` either do no codegen
// or are the LLVM backend themselves, so there the selection is a compatible
// no-op. Only a codegen verb (run/build) cannot honour `llvm` yet -- that is
// the phase-5 assembler+linker -- so main() answers it before dispatch.
// Returns the selected backend ("llvm", "cpp", or "" when absent).
std::string takeBackendSelector(std::vector<std::string>& rest) {
    std::string sel;
    std::vector<std::string> out;
    for (const auto& a : rest) {
        size_t eq = a.find('=');
        if (eq != std::string::npos &&
            (a.compare(0, eq, "-fbackend") == 0 ||
             a.compare(0, eq, "--fbackend") == 0)) {
            sel = a.substr(eq + 1);
            continue;
        }
        out.push_back(a);
    }
    rest = out;
    return sel;
}

// `kirnc llvm`: the phase-4 LLVM backend. Runs the same front-end pipeline as
// `rt`, then lowers the verified MIR to LLVM IR (rejecting anything outside the
// scalar subset with a diagnostic). With no -o the IR goes to stdout; with -o a
// file. Exit code: 0 functionally OK, 1 source/HIR errors, 65 verification or
// scalar-subset rejection or unwritable output -- matching the other stage
// verbs in driver.cpp.
int cmdLlvm(const std::vector<std::string>& args, const char* toolName) {
#ifdef KIRN_HAS_LLVM
    std::vector<std::string> files;
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help") {
            std::cout << "usage: " << toolName << " llvm <file.kn> [-o out.ll]\n"
                      << "  lower through HIR to a verified MIR CFG, then to LLVM\n"
                      << "  IR; --emit-llvm/-emit-llvm is an alias of this verb\n";
            return 0;
        }
        if (a == "-o" && i + 1 < args.size()) { out = args[++i]; continue; }
        if (!a.empty() && a[0] == '-') continue;
        files.push_back(a);
    }
    if (files.size() != 1) {
        std::cerr << "usage: " << toolName << " llvm <file.kn> [-o out.ll]\n";
        return 2;
    }
    kirn::driver::MirOutcome m = kirn::driver::produceMir(files[0], toolName);
    if (m.rc == 66) { std::cerr << m.err; return m.rc; }
    if (m.rc != 0) { std::cout << m.err; return m.rc; }

    kirn::backends::llvm::EmitOptions opts;
    std::vector<kirn::backends::llvm::EmitDiag> diags;
    std::string ll;
    if (!kirn::backends::llvm::emitModule(*m.prog, opts, diags, ll, nullptr)) {
        for (const auto& d : diags)
            std::cout << files[0] << ":" << d.line << ":" << d.col
                      << ": error: " << d.message << "\n";
        return 65;
    }
    if (out.empty()) {
        std::cout << ll;
    } else {
        std::ofstream f(out, std::ios::binary);
        if (!f.good()) {
            std::cerr << toolName << " llvm: cannot write '" << out << "'\n";
            return 66;
        }
        f << ll;
    }
    return 0;
#else
    (void)args;
    (void)toolName;
    std::cerr << "kirnc llvm: this build of kirn has no LLVM backend\n"
              << "  re-configure with -DKIRN_WITH_LLVM=ON to enable it\n";
    return 2;
#endif
}

int cmdLlvmTarget(const std::vector<std::string>& args, const char* toolName,
                  bool assembly, bool executable) {
#ifdef KIRN_HAS_LLVM
    const auto llvmHostTriple = []() {
#if defined(_WIN32)
        return std::string("x86_64-pc-windows-gnu");
#elif defined(__APPLE__)
        return std::string("x86_64-apple-darwin");
#elif defined(__aarch64__)
        return std::string("aarch64-linux-gnu");
#else
        return std::string("x86_64-pc-linux-gnu");
#endif
    };
    std::vector<std::string> files;
    std::string out;
    std::string targetTriple = llvmHostTriple();
    bool dryRun = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if ((a == "-o" || a == "--output") && i + 1 < args.size()) {
            out = args[++i];
            continue;
        }
        if (a == "--target" && i + 1 < args.size()) {
            targetTriple = args[++i];
            continue;
        }
        if (a.rfind("--target=", 0) == 0) {
            targetTriple = a.substr(9);
            continue;
        }
        if (a == "-S" || a == "-s" || a == "--asm") {
            assembly = true;
            continue;
        }
        if (a == "-c") {
            assembly = false;
            continue;
        }
        if (a == "-###") {
            dryRun = true;
            continue;
        }
        if (!a.empty() && a[0] == '-') continue;
        files.push_back(a);
    }
    if (files.size() != 1) {
        std::cerr << "usage: " << toolName
                  << " -fbackend=llvm " << (assembly ? "-S" : "-c")
                  << " <file.kn> -o <output>\n";
        return 2;
    }
    if (targetTriple == "linux-amd64") targetTriple = "x86_64-pc-linux-gnu";
    else if (targetTriple == "linux-arm64") targetTriple = "aarch64-linux-gnu";
    else if (targetTriple == "windows-amd64") targetTriple = "x86_64-pc-windows-gnu";
    else if (targetTriple == "windows-arm64") targetTriple = "aarch64-w64-windows-gnu";
    else if (targetTriple == "darwin-amd64") targetTriple = "x86_64-apple-darwin";
    else if (targetTriple == "darwin-arm64") targetTriple = "aarch64-apple-darwin";
    if (executable && targetTriple != llvmHostTriple()) {
        std::cerr << toolName << ": LLVM executable linking for target '"
                  << targetTriple << "' requires a matching C runtime/linker; "
                     "use -c or -S for cross-target output\n";
        return 2;
    }
    if (out.empty()) {
        const std::string stem = files[0].substr(0, files[0].find_last_of('.'));
        out = stem + (assembly ? ".s" : (executable ? "" : ".o"));
    }
    kirn::driver::MirOutcome m = kirn::driver::produceMir(files[0], toolName);
    if (m.rc == 66) { std::cerr << m.err; return m.rc; }
    if (m.rc != 0) { std::cout << m.err; return m.rc; }

    kirn::backends::llvm::EmitOptions opts;
    std::vector<kirn::backends::llvm::EmitDiag> diags;
    std::string objectPath = out;
    if (executable) objectPath += ".kirn-llvm.o";
    const char* linker = std::getenv("KIRN_LLVM_LINKER");
    const std::string linkerCmd = linker && *linker ? linker : "clang++";
    const std::string linkCommand = "\"" + linkerCmd +
                                    "\" -no-pie -o \"" + out + "\" \"" +
                                    objectPath + "\"";
    if (dryRun) {
        if (executable)
            std::cout << linkCommand << "\n";
        else
            std::cout << "<LLVM TargetMachine> -> " << objectPath << "\n";
        return 0;
    }
    if (!kirn::backends::llvm::emitTargetFile(
            *m.prog, opts, targetTriple, objectPath, assembly, diags)) {
        for (const auto& d : diags)
            std::cerr << files[0] << ":" << d.line << ":" << d.col
                      << ": error: " << d.message << "\n";
        return 65;
    }
    if (executable) {
        const int rc = std::system(linkCommand.c_str());
        std::remove(objectPath.c_str());
        if (rc != 0) {
            std::cerr << toolName << ": LLVM linker failed (" << linkerCmd
                      << ")\n";
            return 65;
        }
    }
    return 0;
#else
    (void)args;
    (void)toolName;
    (void)assembly;
    (void)executable;
    std::cerr << "kirnc: '-fbackend=llvm' requires a build with LLVM enabled\n";
    return 2;
#endif
}

// Remove the flags a stage verb does not understand, so they are never mistaken
// for an input path (the old tools treated ANY unrecognised argument as a
// filename, which is how "kirnlex --dump=1 x.kn" silently lexed a file called
// "--dump=1").
//
// Note the two functions have different grammars, so which flags are stripped
// depends on the verb -- most importantly `--plain`/`--color` belong to check
// and must be handed through untouched, while they are noise for lex and parse.
std::vector<std::string> stripFor(const std::vector<std::string>& in,
                                  bool keepDiagFlags) {
    std::vector<std::string> out;
    for (const auto& a : in) {
        if (a == "--emit-tokens" || a == "--emit-ast" || a == "--emit-hir" ||
            a == "-emit-hir" || a == "--emit-mir" || a == "-emit-mir")
            continue;
        if (!keepDiagFlags &&
            (a == "--plain" || a == "--color" || a == "--no-color"))
            continue;
        out.push_back(a);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        std::cerr << kUsage;
        return 2;
    }
    if (isHelp(args.front())) {
        std::cout << kUsage;
        return 0;
    }

    // verbs are optional: if first arg is not a verb and not -E and not a flag,
    // default to "check"
    const std::string verb0 = args.front();
    auto isFlag = [](const std::string& a) { return !a.empty() && a[0] == '-'; };
    bool verbIsVerb = (verb0 == "check" || verb0 == "lex" || verb0 == "parse" ||
                       verb0 == "run" || verb0 == "hir" || verb0 == "rt" ||
                       verb0 == "build" || verb0 == "llvm" || verb0 == "-E" ||
                       verb0.rfind("-fbackend=", 0) == 0 ||
                       verb0.rfind("--fbackend=", 0) == 0 ||
                       verb0 == "-S" || verb0 == "-s" || verb0 == "--asm" ||
                       verb0 == "-c" || verb0 == "-###" || verb0 == "-v" ||
                       verb0 == "--verbose");
    std::string verb = verbIsVerb ? verb0 : "check";
    std::vector<std::string> rest = verbIsVerb
                                        ? std::vector<std::string>(args.begin() + 1, args.end())
                                        : args;

    // if -S/-c appear as verb position after default? handled

    // also handle -S as first flag when no verb given? already handled above
    if (verb == "-S" || verb == "-s" || verb == "--asm") {
        verb = "build";
        rest.insert(rest.begin(), "-S");
    } else if (verb == "--asm") {
        verb = "build";
        rest.insert(rest.begin(), "-S");
    } else if (verb == "-c") {
        verb = "build";
        rest.insert(rest.begin(), "-c");
    } else if (verb == "-###") {
        verb = "build";
        rest.insert(rest.begin(), "-###");
    } else if (verb == "-v" || verb == "--verbose") {
        verb = "build";
        rest.insert(rest.begin(), "-v");
    } else if (verb == "-E") {
        // `-E` is handled by the preprocessor dispatch below; it is a valid
        // stage flag, not an unknown option.
    } else if (verb.rfind("-fbackend=", 0) == 0 ||
               verb.rfind("--fbackend=", 0) == 0) {
        rest.insert(rest.begin(), verb);
        verb = "check";
        for (const auto& a : rest)
            if (a == "-c" || a == "-S" || a == "-s" || a == "--asm")
                verb = "build";
    } else if (isFlag(verb)) {
        // unknown flag as verb
        std::cerr << "kirnc: unknown option '" << verb << "'\n";
        std::cerr << kUsage;
        return 2;
    }

    if (verb != "check" && verb != "lex" && verb != "parse" && verb != "run" &&
        verb != "hir" && verb != "rt" && verb != "build" && verb != "llvm" && verb != "-E") {
        std::cerr << "kirnc: unknown verb '" << verb << "'\n";
        std::cerr << kUsage;
        return 2;
    }

    // `kirnc -E file.kn` -- the gcc spelling, where -E acts as the verb. This is
    // checked before verb validation because "-E" is not a verb name, and a user
    // who types it first means "preprocess", not "unknown verb".
    if (verb == "-E") {
        return kirn::driver::cmdPreprocess(rest, "kirnc");
    }

    if (verb != "check" && verb != "lex" && verb != "parse" && verb != "run" &&
        verb != "hir" && verb != "rt" && verb != "build" && verb != "llvm") {
        std::cerr << "kirnc: unknown verb '" << verb << "'\n";
        std::cerr << kUsage;
        return 2;
    }

    for (const auto& a : rest) {
        if (!isLaterFlag(a)) continue;
        if (a == "-S" || a == "-s" || a == "--asm" || a == "-emit-sasm" || a == "--emit-sasm") {
            if (verb == "build" || verb == "run" || verb == "check") {
                // allow -S through to build for check? no, handle below
            }
        }
        if (verb != "build" &&
            (a == "-c" || a == "-###" || a == "-v" || a == "--verbose")) {
            std::cerr << "kirnc " << verb << ": '" << a
                      << "' is not implemented yet\n"
                      << "  -c/-###/-v land with the assembler+linker phase; see BUILDER-PLAN.md\n";
            return 2;
        }
    }

    // -E handled earlier; -D/-U only with -E handled.

    // `-fbackend=llvm` is a real selector. Object and assembly emission use
    // LLVM's in-process TargetMachine; executable linking remains the normal
    // host backend until the LLD integration phase.
    std::string backend = takeBackendSelector(rest);
    if (backend == "runtime") {
        if (verb != "run" && verb != "build") {
            std::cerr << "kirnc " << verb
                      << ": '-fbackend=runtime' is only valid for run/build\n";
            return 2;
        }
        rest.push_back("--runtime");
    } else if (backend == "llvm") {
        if (verb == "run" || verb == "build") {
            bool targetFile = false;
            bool assembly = false;
            for (const auto& a : rest) {
                if (a == "-c") targetFile = true;
                if (a == "-S" || a == "-s" || a == "--asm") {
                    targetFile = true;
                    assembly = true;
                }
            }
            if (verb == "build")
                return cmdLlvmTarget(rest, "kirnc", assembly, !targetFile);
            std::cerr << "kirnc " << verb << ": '-fbackend=llvm' is recognised, "
                         "but running or building an executable from LLVM IR "
                         "lands in phase 5\n"
                      << "  emit IR today with `kirnc llvm <file.kn> [-o out.ll]`; "
                         "omit the flag for the default ('cpp') backend\n";
            return 2;
        }
    } else if (!backend.empty() && backend != "cpp") {
        std::cerr << "kirnc " << verb << ": unknown backend '-fbackend="
                  << backend << "'\n"
                  << "  the flags table pins 'llvm' and 'cpp'\n";
        return 2;
    }

    // --emit-tokens / --emit-ast select the dump; without them the verb still
    // runs but prints only its OK/diagnostic summary. This is the rustc
    // --emit=metadata shape: the verb says how far to go, the flag says what
    // to write.
    auto hasFlag = [&rest](const char* f) {
        for (const auto& a : rest)
            if (a == f) return true;
        return false;
    };

    // -E is a stop-at-stage flag, not a verb: it selects the preprocessor as
    // the last stage to run, whatever verb was named. Handled before dispatch
    // because a stage flag must not depend on which verb it follows -- the gcc
    // grammar puts -E/-S/-c at the driver level, not inside a subcommand.
    if (hasFlag("-E")) {
        std::vector<std::string> a;
        for (const auto& s : rest) {
            if (s == "-E") continue;
            a.push_back(s);
        }
        return kirn::driver::cmdPreprocess(a, "kirnc");
    }

    // -D/-U only have an effect on -E for now. The preprocessor does run in
    // front of `check` (see frontEnd), so threading the flags into every verb is
    // real remaining work -- but rejecting them is strictly better than the
    // alternative, which is that `kirnc check -DFOO=1 f.kn` today silently
    // accepts the flag and `kirnc lex -DFOO=1 f.kn` treats it as a FILENAME and
    // reports "cannot read file". An unrecognised flag must never be a path.
    for (const auto& a : rest) {
        if (a.rfind("-D", 0) != 0 && a.rfind("-U", 0) != 0) continue;
        std::cerr << "kirnc " << verb << ": '" << a
                  << "' is only accepted together with -E\n"
                  << "  macro definitions reach the other verbs once -D/-U are "
                     "threaded into the front end; use 'kirnc -E " << a
                  << " ...' to see the expansion\n";
        return 2;
    }

    if (verb == "lex") {
        std::vector<std::string> a = stripFor(rest, false);
        if (hasFlag("--emit-tokens")) a.push_back("--dump");
        return kirn::driver::cmdLex(a, "kirnc");
    }
    if (verb == "parse") {
        std::vector<std::string> a = stripFor(rest, false);
        if (hasFlag("--emit-ast")) a.push_back("--ast");
        return kirn::driver::cmdParse(a, "kirnc");
    }
    if (verb == "hir" || hasFlag("--emit-hir")) {
        // `kirnc hir f.kn` and `kirnc check --emit-hir f.kn` are the same thing,
        // matching how --emit-ast rides on `parse`. The flag form exists so
        // `--emit-hir` composes with a verb, the way rustc's --emit= does.
        std::vector<std::string> a;
        for (const auto& s : rest)
            if (s != "--emit-hir" && s != "-emit-hir") a.push_back(s);
        return kirn::driver::cmdHir(a, "kirnc");
    }
    if (verb == "rt" || hasFlag("--emit-mir")) {
        // `rt` is the MIR dumper for this phase. BUILDER-PLAN section 6 gives it
        // two meanings -- "run through the runtime" and "dump MIR" -- and the
        // phase-3 text resolves it as the dumper; the runner meaning is deferred
        // to the phase that retires the bundle.
        std::vector<std::string> a;
        for (const auto& s : rest)
            if (s != "--emit-mir" && s != "-emit-mir") a.push_back(s);
        return kirn::driver::cmdRt(a, "kirnc");
    }
    // `--emit-llvm` is accepted as an alternate spelling of the `llvm` verb for
    // any stage verb that would need the full pipeline (bcc-style: the flag
    // selects the backend, the verb says how far to stop). `lex/parse/run/build`
    // reject it, because emitting IR is meaningless there.
    if (verb == "llvm" || hasFlag("--emit-llvm") || hasFlag("-emit-llvm")) {
        if (verb == "run" || verb == "build" || verb == "lex" || verb == "parse") {
            std::cerr << "kirnc " << verb << ": '--emit-llvm' does not apply to this verb\n";
            return 2;
        }
        std::vector<std::string> a;
        for (const auto& s : rest)
            if (s != "--emit-llvm" && s != "-emit-llvm") a.push_back(s);
        return cmdLlvm(a, "kirnc");
    }
    if (verb == "check") {
        // --plain/--color/--no-color are cmdCheck's own flags; pass them
        // through rather than stripping them.
        return kirn::driver::cmdCheck(stripFor(rest, true), "kirnc");
    }

    if (verb == "run") {
        return kirn::driver::cmdRun(rest, "kirnc");
    }

    if (verb == "build" || verb == "run") {
        // forward to existing build driver; -S/-s/--asm map to opts.sasm
        std::vector<std::string> forwarded;
        forwarded.push_back(verb == "run" ? std::string("build") : verb);
        for (const auto& a : rest) {
            if (a == "-S" || a == "-s" || a == "--asm") {
                forwarded.push_back("--emit-asm");
                continue;
            }
            if (a == "--emit-sasm") { forwarded.push_back("-S"); continue; }
            if (a == "-emit-sasm") { forwarded.push_back("-S"); continue; }
            if (a == "--verbose" || a == "-v") {
                forwarded.push_back("-v");
                continue;
            }
            forwarded.push_back(a);
        }
        return kirn::driver::cmdBuild(forwarded, 1);
    }
    if (verb == "check" && (hasFlag("-S") || hasFlag("-s") || hasFlag("--asm") || hasFlag("--emit-sasm") || hasFlag("-emit-sasm"))) {
        std::vector<std::string> forwarded;
        forwarded.push_back("build");
        for (const auto& a : rest) {
            if (a == "--emit-sasm") { forwarded.push_back("-S"); continue; }
            if (a == "-emit-sasm") { forwarded.push_back("-S"); continue; }
            forwarded.push_back(a);
        }
        // add a dummy file if needed
        return kirn::driver::cmdBuild(forwarded, 1);
    }
}
