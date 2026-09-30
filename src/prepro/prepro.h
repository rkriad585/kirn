#pragma once

// The Kirn preprocessor (BUILDER-PLAN.md phase 2, section 4.0).
//
// Unit: the token stream BEFORE parsing, so a macro can synthesize syntax. The
// contract is one line: consume `std::vector<Token>`, return the expanded
// `std::vector<Token>`. Nothing here re-lexes source text, and nothing here
// knows about the AST, the parser, or the checker.
//
// ## Why `{. ... .}` and not `#def`
//
// The plan proposed C-style `#def` / `#if` / `#undef`, and justified the sigil
// with "`#` is unused today except in comments -- verify in lexer". That check
// came back the other way: `#` is the *active, normative* line-comment sigil
// (`grammar/kirn.ebnf:43`, `src/lex/lexer.cpp:186-189`), so the plan's syntax
// is not an error today, it is a SILENT NO-OP -- a file full of `#ifdef` still
// type-checks clean at exit 0, which is the worst possible failure mode for a
// preprocessor. The syntax here is Nim's `{. ... .}`, which is what Nim itself
// uses for annotations in a language whose comment sigil is also `#`.
//
// The decisive practical consequence: `{.` already lexes as two ordinary
// `Punct` tokens (`{` and `.` are both in `kPunct1`), so this whole layer is
// pure token pattern-matching. No lexer change, no new `Tok` enumerator, and no
// edit to the normative COMMENT production in the grammar -- which in turn means
// the 53 recorded lex goldens whose sources begin with a `#` comment are
// untouched rather than re-recorded.
//
// ## Hygiene
//
// Capture-free, following the plan and Nim's `gensym` default for data
// bindings:
//   - parameters bind to the CALL SITE, so a macro can act on caller values;
//   - every other identifier written in a macro body is alpha-renamed into a
//     private per-macro namespace at DEFINITION time, so a macro can neither
//     capture a caller local of the same name nor leak a binding of its own.
// The verify criterion for this phase is "macros cannot capture caller locals",
// and this is the mechanism that makes it true.

#include "lex/token.h"
#include "support/diag.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace kirn {
namespace prepro {

// A macro definition. The body is stored as TOKENS, captured before parsing,
// which is what lets a macro expand to arbitrary syntax rather than to an
// expression.
struct Macro {
    std::vector<std::string> params;
    std::vector<Token> body;
    // Distinguishes "no parameter list was written" (object-like) from "an empty
    // parameter list was written" (function-like, called as `F()`). Without this
    // the two are indistinguishable and `{.def F() = ...}` is treated as
    // object-like, so the `()` at the call site is never consumed and the
    // expansion is spliced in beside a dangling empty argument list.
    bool functionLike = false;
    bool builtin = false;  // __FILE__ / __LINE__ / __KIRN__ / __VERSION__
    uint32_t defLine = 1;
};

// True if the token stream contains at least one `{.` NAME ... `.}` directive.
//
// Exposed separately from Preprocessor because the driver needs it for a
// question the preprocessor itself cannot answer: whether a source USED
// macros, as opposed to merely passing through unchanged. A build path that
// hands raw source to a runtime which will re-lex it (the KIRNBUNDLE/1 launcher)
// has to know this, or it ships a binary that cannot run: `kirn run` preprocesses,
// the bundled `kirnrt` does not, so the same program would be exit 0 from the
// driver and exit 65 from the artefact it produced.
bool hasDirective(const std::vector<Token>& toks);

class Preprocessor {
public:
    // `filename` is threaded in explicitly because Token carries no file id:
    // the lexer takes a filename and discards it (Lexer::filename_ is written
    // and never read), so __FILE__ cannot be recovered from the token stream.
    Preprocessor(std::string filename, DiagEngine& diags);

    // Command-line seeding. `-D name[=value]` defines, `-U name` undefines.
    // Both are applied before any directive in the file, so a file can
    // override or undefine a -D.
    void define(const std::string& name, const std::string& value);
    void undefine(const std::string& name);

    // Directive scan followed by expansion to fixpoint. Returns an empty vector
    // if the directives were malformed.
    std::vector<Token> run(std::vector<Token> toks);

    // Macro table, for tests and for a future `-dM`-style state dump.
    const std::map<std::string, Macro>& macros() const { return macros_; }

private:
    // --- stage 1: directives -------------------------------------------------
    // Splits the stream into kept tokens and directives, applying def/undef
    // and evaluating conditionals. Conditional branches are tracked with a
    // stack so that a dead branch still has its {.if.}/{.endif.} nesting
    // consumed -- otherwise a file with #if 0 around a {.if.} would confuse
    // the nesting and error out.
    std::vector<Token> scanDirectives(const std::vector<Token>& in);

    // Recognises `{.` NAME ... `.}` starting at `i`. Returns true and advances
    // `i` past the closing `.}` when a directive was found.
    bool atDirective(const std::vector<Token>& in, size_t& i, std::string& name,
                     std::vector<Token>& payload);

    // --- stage 2: expansion --------------------------------------------------
    // Fixpoint worklist: tokens produced by an expansion are pushed back onto
    // the front of the pending queue and rescanned, so a macro that expands to
    // another macro invocation converges. Bounded by a step limit so that
    // mutually recursive macros are a diagnostic rather than a hang; the direct
    // self-reference case is caught earlier, with a better message, at
    // definition time.
    std::vector<Token> expand(const std::vector<Token>& in);

    // Splits `(` ... `)` on top-level commas into one token vector per
    // argument. Depth-aware so that `f(g(1, 2), 3)` is two arguments.
    static bool collectArgs(std::deque<Token>& q, size_t& i,
                            std::vector<std::vector<Token>>& args);

    // Substitutes parameters into a macro body. The body's free identifiers
    // were already alpha-renamed at definition time, so this is a pure
    // substitution; every result is re-spanned to the call site. `mname` is
    // only used to make an arity error name the macro rather than a parameter.
    std::vector<Token> instantiate(const Macro& m,
                                   const std::vector<std::vector<Token>>& args,
                                   const Token& useSite,
                                   const std::string& mname);

    // --- {.if EXPR.} evaluation --------------------------------------------
    // Small integer/bool expression language over the defined set: no strings,
    // no user calls. Deliberately tiny, matching the Rust `cfg` precedent of a
    // closed predicate language rather than C's full constant expressions.
    bool evalCondition(const std::vector<Token>& expr, uint32_t line,
                       uint32_t col, bool& result);
    // Recursive-descent over expr; `pos` advances. `ok` is cleared on error.
    bool evalOr(const std::vector<Token>& expr, size_t& pos, int64_t& out,
                bool& ok);
    bool evalAnd(const std::vector<Token>& expr, size_t& pos, int64_t& out,
                 bool& ok);
    bool evalCmp(const std::vector<Token>& expr, size_t& pos, int64_t& out,
                 bool& ok);
    bool evalAdd(const std::vector<Token>& expr, size_t& pos, int64_t& out,
                 bool& ok);
    bool evalUnary(const std::vector<Token>& expr, size_t& pos, int64_t& out,
                   bool& ok);

    void err(uint32_t line, uint32_t col, const std::string& msg) {
        diags_.report(line, col, msg);
    }

    std::string filename_;
    DiagEngine& diags_;
    std::map<std::string, Macro> macros_;
    uint32_t nextMacroId_ = 1;
};

}  // namespace prepro
}  // namespace kirn
