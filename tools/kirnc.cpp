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

#include <iostream>
#include <string>
#include <vector>

namespace {

const char* const kUsage =
    "usage: kirnc <check|lex|parse|run|build> <file.kn> [flags]\n"
    "\n"
    "verbs:\n"
    "  check   lex + parse + type-check only; exit 1 on error\n"
    "  lex     lex only (--emit-tokens to print the token stream)\n"
    "  parse   lex + parse (--emit-ast to print the AST)\n"
    "  hir     lower to HIR, verify structurally, print the dump\n"
    "  run     not implemented yet (see the note below)\n"
    "  build   build an executable (default verb)\n"
    "\n"
    "flags:\n"
    "  --emit-tokens   print the token stream (lex)\n"
    "  --emit-ast      print the AST (parse)\n"
    "  --emit-hir      lower to HIR, verify it, print the dump (check)\n"
    "  -E              preprocess only; print the expanded token stream\n"
    "  -D name[=val]   define a macro (with -E)\n"
    "  -U name         undefine a macro (with -E)\n"
    "  -o <path>       output path (build)\n"
    "  --plain         no source/caret lines in diagnostics (check)\n"
    "  --color         force ANSI colour in diagnostics (check)\n"
    "  -h, --help      this message\n"
    "\n"
    "stop-at-stage (-S, -c, -###, -v) lands in phase 5.\n";

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
const char* const kLaterFlags[] = {"-c", "-###", "-v", "--verbose",
                                   "-emit-mir", "--emit-mir",
                                   "-emit-llvm", "--emit-llvm",
                                   "-emit-sasm", "--emit-sasm"};

bool isLaterFlag(const std::string& a) {
    for (const char* f : kLaterFlags)
        if (a == f) return true;
    return false;
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
            a == "-emit-hir")
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

    const std::string verb = args.front();
    std::vector<std::string> rest(args.begin() + 1, args.end());

    // `kirnc -E file.kn` -- the gcc spelling, where -E acts as the verb. This is
    // checked before verb validation because "-E" is not a verb name, and a user
    // who types it first means "preprocess", not "unknown verb".
    if (verb == "-E") {
        return kirn::driver::cmdPreprocess(rest, "kirnc");
    }

    if (verb != "check" && verb != "lex" && verb != "parse" && verb != "run" &&
        verb != "hir" && verb != "build") {
        std::cerr << "kirnc: unknown verb '" << verb << "'\n";
        std::cerr << kUsage;
        return 2;
    }

    for (const auto& a : rest) {
        if (!isLaterFlag(a)) continue;
        std::cerr << "kirnc " << verb << ": '" << a
                  << "' is not implemented yet\n"
                  << "  --emit-mir lands with the MIR CFG, and -S/-c/-###/-v with "
                     "codegen; see BUILDER-PLAN.md section 6\n";
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
    if (verb == "check") {
        // --plain/--color/--no-color are cmdCheck's own flags; pass them
        // through rather than stripping them.
        return kirn::driver::cmdCheck(stripFor(rest, true), "kirnc");
    }

    if (verb == "run") {
        // Deliberately not implemented yet, and this is a real design decision
        // rather than an omission. `run` has THREE incompatible existing
        // behaviours and the plan does not say which one kirnc must match:
        //   kirnrun   -> type errors on STDOUT, exit 1, "N error(s)" summary,
        //                panic stack frames printed
        //   kirn run  -> type errors on STDERR, exit 65, no summary, no frames
        //   bundled   -> exit 65 with no diagnostic printed at all
        // Picking one silently would bake a guess into the driver, and the
        // verify criterion ("kirnc X == old tool output") is unanswerable until
        // it is chosen. BUILDER-PLAN.md open question #1 is the same question.
        std::cerr << "kirnc run: not implemented yet\n"
                  << "  the plan gives `run` three incompatible existing "
                     "behaviours (kirnrun, kirn run, bundled) and does not say "
                     "which one kirnc must match\n"
                  << "  see BUILDER-PLAN.md open question #1; use `kirn run` "
                     "meanwhile\n";
        return 2;
    }

    // `build` goes through the existing build driver, so there is exactly one
    // implementation of the pipeline. cmdBuild's `from` skips argv[0] (the
    // program name), so the verb occupies slot 0 and the real arguments begin
    // at index 1.
    std::vector<std::string> forwarded;
    forwarded.push_back(verb);
    for (const auto& a : rest) forwarded.push_back(a);
    return kirn::driver::cmdBuild(forwarded, 1);
}
