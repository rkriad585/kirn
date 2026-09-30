// The Kirn preprocessor -- see prepro.h for the design rationale.

#include "prepro/prepro.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <set>

namespace kirn {
namespace prepro {

namespace {

// Bounded so that a runaway expansion is a diagnostic rather than a hang.
//
// In practice this is defence in depth rather than a reachable path: a macro
// body's free identifiers are alpha-renamed at definition time, and a name that
// already names a macro is left alone for composability, but only if that macro
// was defined EARLIER. So a body can only ever refer to a macro that already
// existed, which means recursion would have to pass through a definition and
// the table lookup always moves strictly backwards. Mutual recursion is
// therefore structurally impossible, and `{.def A. A.}` is caught outright at
// definition time with a better message. The limit stays because that argument
// is about the current implementation, and a hang is a much worse failure than
// a spurious diagnostic.
constexpr size_t kMaxSteps = 200000;

// Gensym prefix for alpha-renamed macro-body identifiers. `_`-leading
// identifiers are ordinary Ident tokens in this lexer (verified), so this needs
// no lexer change. The `__kirnpre` stem is reserved: library code must not
// write it, or a macro could capture a renamed binding.
constexpr const char* kGensymStem = "__kirnpre";

bool isPunct(const Token& t, const char* s) {
    return t.kind == Tok::Punct && t.text == s;
}

bool isOp(const Token& t, const char* s) {
    return t.kind == Tok::Op && t.text == s;
}

// Synthesized tokens carry the position of the thing that caused them. A macro
// body's token has no meaningful position of its own -- it is not on any line
// the user wrote -- so the call site is the only honest choice, and it is what
// makes a diagnostic after expansion point somewhere useful.
Token synth(Tok kind, const std::string& text, const Token& at) {
    Token t;
    t.kind = kind;
    t.text = text;
    t.line = at.line;
    t.col = at.col;
    t.endLine = 0;
    t.endCol = 0;
    return t;
}

const std::set<std::string>& directiveNames() {
    static const std::set<std::string> kDirectives = {
        "def", "undef", "if", "ifdef", "ifndef", "else", "elif", "endif"};
    return kDirectives;
}

}  // namespace

bool hasDirective(const std::vector<Token>& toks) {
    for (size_t i = 0; i + 2 < toks.size(); ++i) {
        if (!isPunct(toks[i], "{") || !isPunct(toks[i + 1], ".")) continue;
        if (toks[i + 2].kind != Tok::Ident) continue;
        if (directiveNames().count(toks[i + 2].text)) return true;
    }
    return false;
}

Preprocessor::Preprocessor(std::string filename, DiagEngine& diags)
    : filename_(std::move(filename)), diags_(diags) {
    // Built-ins. __TARGET__ is deliberately NOT seeded here: the target is a
    // property of the host build, and inventing a value at this layer would let
    // a macro observe a lie. It is left to the driver to define through -D,
    // which is the same channel the cross build already uses.
    for (const char* name : {"__FILE__", "__LINE__", "__KIRN__", "__VERSION__"}) {
        Macro m;
        m.builtin = true;
        macros_[name] = m;
    }
}

void Preprocessor::define(const std::string& name, const std::string& value) {
    if (name.empty()) return;
    Macro m;
    if (!value.empty()) {
        // A -D value becomes exactly one token, chosen by shape. Keeping it to a
        // single token is the point: it makes it impossible for a -D to smuggle
        // in unbalanced punctuation that only fails much later.
        Tok kind = Tok::StrNormal;
        if (value.find_first_not_of("0123456789") == std::string::npos)
            kind = Tok::Int;
        else if (value == "true" || value == "false")
            kind = Tok::Ident;
        m.body.push_back(synth(kind, value, Token{}));
    }
    macros_[name] = m;
}

void Preprocessor::undefine(const std::string& name) {
    // A built-in is not a user symbol: __FILE__ is a property of the token
    // stream. Letting it vanish would turn a useful diagnostic into a confusing
    // "undefined name" from the checker much further downstream.
    if (name == "__FILE__" || name == "__LINE__" || name == "__KIRN__" ||
        name == "__VERSION__")
        return;
    macros_.erase(name);
}

// ---------------------------------------------------------------------------
// Stage 1: directives
// ---------------------------------------------------------------------------

// Recognises `{.` NAME ... `.}`. The directive is line-scoped (Nim's rule, and
// necessary here because the lexer emits no Newline token, so "end of line" is
// observable only as a change in Token::line). The terminator is a `.`
// IMMEDIATELY followed by `}`; requiring the adjacency is what lets a body
// contain both `.` and `}` -- `1.5` and `{ return 1; }` both round-trip.
bool Preprocessor::atDirective(const std::vector<Token>& in, size_t& i,
                               std::string& name,
                               std::vector<Token>& payload) {
    if (i + 3 >= in.size()) return false;
    if (!isPunct(in[i], "{") || !isPunct(in[i + 1], ".")) return false;
    if (in[i + 2].kind != Tok::Ident) return false;

    // `{.` followed by an identifier that is not a directive is ordinary code
    // (an empty set literal, or a block whose first statement starts with a
    // dot), so the name set is a whitelist rather than a catch-all.
    const std::string word = in[i + 2].text;
    if (!directiveNames().count(word)) return false;

    const uint32_t line = in[i].line;
    size_t j = i + 3;
    while (j < in.size() && in[j].line == line) {
        if (isPunct(in[j], ".") && j + 1 < in.size() && isPunct(in[j + 1], "}")) {
            name = word;
            payload.assign(in.begin() + i + 3, in.begin() + j);
            i = j + 2;
            return true;
        }
        ++j;
    }
    // Unterminated. Report at the `{.` and swallow the rest of the line so the
    // user gets one error rather than a cascade.
    err(in[i].line, in[i].col,
        "unterminated '" + word + "' directive (expected '.}')");
    i = j;
    name.clear();
    payload.clear();
    return true;
}

std::vector<Token> Preprocessor::scanDirectives(const std::vector<Token>& in) {
    // `enclosingLive` is stored per frame because {.elif.} in a DEAD parent
    // branch must stay dead. Recomputing liveness from a bare "have we matched
    // yet" flag would let a dead parent's elif turn back on.
    struct Frame {
        bool enclosingLive;
        bool taken;
        bool seenElse;
    };
    std::vector<Frame> stack;
    bool baseLive = true;

    auto live = [&] {
        if (!baseLive) return false;
        for (const auto& f : stack)
            if (!(f.enclosingLive && f.taken)) return false;
        return true;
    };

    std::vector<Token> out;
    size_t i = 0;
    while (i < in.size()) {
        std::string name;
        std::vector<Token> payload;
        size_t probe = i;
        if (!atDirective(in, probe, name, payload)) {
            if (live()) out.push_back(in[i]);
            ++i;
            continue;
        }
        const Token at = in[i];
        i = probe;

        if (name == "def") {
            if (!live()) continue;
            if (payload.empty() || payload[0].kind != Tok::Ident) {
                err(at.line, at.col, "{.def.} expects a macro name");
                continue;
            }
            const std::string mname = payload[0].text;
            Macro m;
            m.defLine = at.line;
            size_t p = 1;
            // Function-like when the name is immediately followed by `(`.
            if (p < payload.size() && isPunct(payload[p], "(")) {
                m.functionLike = true;
                ++p;
                bool closed = false;
                while (p < payload.size()) {
                    if (isPunct(payload[p], ")")) {
                        closed = true;
                        ++p;
                        break;
                    }
                    if (payload[p].kind == Tok::Ident &&
                        std::find(m.params.begin(), m.params.end(),
                                  payload[p].text) == m.params.end())
                        m.params.push_back(payload[p].text);
                    ++p;
                }
                if (!closed) {
                    err(at.line, at.col,
                        "unterminated parameter list in {.def " + mname + "(...).}");
                    continue;
                }
                if (p >= payload.size() || !isOp(payload[p], "=")) {
                    err(at.line, at.col,
                        "{.def " + mname + "(...).} expects '=' before the body");
                    continue;
                }
                ++p;
            } else {
                // Object-like. Two spellings, because the dot form is what Nim
                // uses and the `=` form is what the function-like spelling
                // already implies -- and because the dot form is genuinely
                // ambiguous otherwise: `{.def N. 10.}` cannot be told apart from
                // a body that begins with a `.` unless the dot is a separator.
                // `.5` is not a valid Kirn literal (a numeric literal needs a
                // leading digit), so consuming one dot here is unambiguous.
                if (p < payload.size() && isPunct(payload[p], ".")) ++p;
                if (p < payload.size() && isOp(payload[p], "=")) ++p;
            }
            m.body.assign(payload.begin() + p, payload.end());

            // Hygiene: alpha-rename the body's free identifiers into a private
            // per-macro namespace NOW, at definition time, so that
            // instantiation is a pure parameter substitution with no second
            // renaming pass. Two kinds of identifier are left alone:
            //
            //  - one followed by `(`, which is a call and resolves at the call
            //    site, so a macro body can call a function the caller defines;
            //  - one that already names a defined macro, so macro bodies can
            //    COMPOSE. Without this, `{.def OUTER. INNER + 1.}` would rename
            //    INNER to a private name and the inner macro could never fire.
            //
            // Composition is therefore backward-only: INNER must be defined
            // before OUTER refers to it, because the table is consulted at
            // definition time. That ordering rule is also what makes mutual
            // recursion structurally impossible rather than merely detected --
            // see the note on kMaxSteps.
            const uint32_t id = nextMacroId_++;
            std::set<std::string> renamed;
            for (size_t k = 0; k < m.body.size(); ++k) {
                const Token& b = m.body[k];
                if (b.kind != Tok::Ident) continue;
                if (std::find(m.params.begin(), m.params.end(), b.text) !=
                    m.params.end())
                    continue;
                if (k + 1 < m.body.size() && isPunct(m.body[k + 1], "(")) continue;
                if (macros_.count(b.text)) continue;  // a macro: keep composable
                if (b.text.rfind(kGensymStem, 0) == 0) continue;  // already private
                renamed.insert(b.text);
            }
            for (auto& b : m.body)
                if (b.kind == Tok::Ident && renamed.count(b.text))
                    b.text = std::string(kGensymStem) + std::to_string(id) + "_" +
                             b.text;

            // A body mentioning its own name can only ever expand to itself.
            if (renamed.count(mname)) {
                err(at.line, at.col,
                    "macro '" + mname + "' expands to itself (infinite expansion)");
                continue;
            }
            macros_[mname] = std::move(m);
            continue;
        }

        if (name == "undef") {
            if (!live()) continue;
            if (payload.empty() || payload[0].kind != Tok::Ident) {
                err(at.line, at.col, "{.undef.} expects a macro name");
                continue;
            }
            undefine(payload[0].text);
            continue;
        }

        if (name == "if" || name == "ifdef" || name == "ifndef") {
            const bool enclosing = live();
            bool value = false;
            if (enclosing) {
                if (name == "if") {
                    bool ok = false;
                    value = evalCondition(payload, at.line, at.col, ok);
                } else if (payload.empty() || payload[0].kind != Tok::Ident) {
                    err(at.line, at.col, "{." + name + "} expects a macro name");
                } else {
                    const bool def = macros_.count(payload[0].text) != 0;
                    value = (name == "ifdef") ? def : !def;
                }
            }
            stack.push_back({enclosing, enclosing && value, false});
            continue;
        }

        if (name == "elif") {
            if (stack.empty()) {
                err(at.line, at.col, "{.elif.} without a matching {.if.}");
                continue;
            }
            Frame& f = stack.back();
            if (f.seenElse) {
                err(at.line, at.col, "{.elif.} after {.else.}");
                continue;
            }
            bool value = false;
            if (f.enclosingLive && !f.taken) {
                bool ok = false;
                value = evalCondition(payload, at.line, at.col, ok);
            }
            f.taken = f.taken || value;
            continue;
        }

        if (name == "else") {
            if (stack.empty()) {
                err(at.line, at.col, "{.else.} without a matching {.if.}");
                continue;
            }
            Frame& f = stack.back();
            if (f.seenElse) {
                err(at.line, at.col, "duplicate {.else.}");
                continue;
            }
            f.seenElse = true;
            f.taken = f.enclosingLive && !f.taken;
            continue;
        }

        if (stack.empty()) {
            err(at.line, at.col, "{.endif.} without a matching {.if.}");
            continue;
        }
        stack.pop_back();
    }

    if (!stack.empty())
        err(in.empty() ? 1 : in.back().line, in.empty() ? 1 : in.back().col,
            "unterminated {.if.} (missing {.endif.})");
    return out;
}

// ---------------------------------------------------------------------------
// Stage 2: expansion
// ---------------------------------------------------------------------------

// `i` points just past the opening '('. Returns with `i` just past the matching
// ')'. Depth-aware, so `f(g(1, 2), 3)` is two arguments, not four.
bool Preprocessor::collectArgs(std::deque<Token>& q, size_t& i,
                               std::vector<std::vector<Token>>& args) {
    std::vector<Token> cur;
    int depth = 0;
    while (i < q.size()) {
        const Token t = q[i];
        if (t.kind == Tok::Punct) {
            if (t.text == "(" || t.text == "[" || t.text == "{") {
                ++depth;
            } else if (t.text == "]" || t.text == "}") {
                --depth;
            } else if (t.text == ")" && depth == 0) {
                // `f()` yields ZERO arguments, so a zero-parameter macro can be
                // written and called bare. `f(a)` yields one.
                if (!cur.empty() || !args.empty()) args.push_back(cur);
                ++i;
                return true;
            } else if (t.text == "," && depth == 0) {
                args.push_back(cur);
                cur.clear();
                ++i;
                continue;
            }
        }
        cur.push_back(t);
        ++i;
    }
    return false;
}

std::vector<Token> Preprocessor::instantiate(
    const Macro& m, const std::vector<std::vector<Token>>& args,
    const Token& useSite, const std::string& mname) {
    std::vector<Token> out;
    out.reserve(m.body.size());
    for (const Token& b : m.body) {
        if (b.kind == Tok::Ident) {
            auto p = std::find(m.params.begin(), m.params.end(), b.text);
            if (p != m.params.end()) {
                const size_t idx = static_cast<size_t>(p - m.params.begin());
                if (idx < args.size()) {
                    // Parameters bind at the CALL SITE: argument tokens are
                    // spliced in verbatim, so a macro can read and rewrite the
                    // caller's expressions. They are re-spanned to the use site
                    // because their own positions belong to a different line.
                    for (Token a : args[idx]) out.push_back(synth(a.kind, a.text, useSite));
                } else {
                    err(useSite.line, useSite.col,
                        "macro '" + mname + "' expects " +
                            std::to_string(m.params.size()) + " argument(s) but got " +
                            std::to_string(args.size()));
                }
                continue;
            }
        }
        out.push_back(synth(b.kind, b.text, useSite));
    }
    return out;
}

std::vector<Token> Preprocessor::expand(const std::vector<Token>& in) {
    std::vector<Token> out;
    std::deque<Token> pending(in.begin(), in.end());
    size_t steps = 0;

    while (!pending.empty()) {
        if (++steps > kMaxSteps) {
            err(in.empty() ? 1 : in[0].line, in.empty() ? 1 : in[0].col,
                "macro expansion did not terminate (step limit reached)");
            return {};
        }
        Token t = pending.front();
        pending.pop_front();

        if (t.kind != Tok::Ident) {
            out.push_back(std::move(t));
            continue;
        }
        auto it = macros_.find(t.text);
        if (it == macros_.end()) {
            out.push_back(std::move(t));
            continue;
        }
        Macro& m = it->second;

        if (m.builtin) {
            if (t.text == "__FILE__") {
                out.push_back(synth(Tok::StrNormal, filename_, t));
                continue;
            }
            if (t.text == "__LINE__") {
                out.push_back(synth(Tok::Int, std::to_string(t.line), t));
                continue;
            }
            if (t.text == "__KIRN__") {
                out.push_back(synth(Tok::Int, "1", t));
                continue;
            }
            if (t.text == "__VERSION__") {
                out.push_back(synth(Tok::StrNormal, "0.0.1-beta", t));
                continue;
            }
        }

        std::vector<std::vector<Token>> args;
        if (m.functionLike) {
            if (pending.empty() || !isPunct(pending.front(), "(")) {
                // A function-like macro not followed by `(` is just a name, not
                // an invocation. `let f = SQUARE;` must not blow up.
                out.push_back(std::move(t));
                continue;
            }
            pending.pop_front();
            size_t idx = 0;
            if (!collectArgs(pending, idx, args)) {
                err(t.line, t.col, "unterminated argument list in macro call");
                return {};
            }
            // collectArgs only walks the queue; the tokens it walked past are
            // still in it. Drop them, or the original argument text survives
            // alongside the expansion and the stream doubles up.
            pending.erase(pending.begin(), pending.begin() + idx);
        }

        std::vector<Token> body = instantiate(m, args, t, t.text);
        if (diags_.errorCount()) return {};
        // Push the expansion back onto the FRONT so it is rescanned. That is the
        // fixpoint: a macro expanding to another invocation converges, and a
        // body that is not an invocation is simply copied through.
        for (auto rit = body.rbegin(); rit != body.rend(); ++rit)
            pending.push_front(std::move(*rit));
    }
    return out;
}

std::vector<Token> Preprocessor::run(std::vector<Token> toks) {
    // The lexer's trailing Eof is dropped and re-added: the expander's worklist
    // has no reason to carry it, and the parser relies on it being last.
    const bool hadEof = !toks.empty() && toks.back().kind == Tok::Eof;
    if (hadEof) toks.pop_back();

    std::vector<Token> staged = scanDirectives(toks);
    if (diags_.errorCount()) return {};
    std::vector<Token> result = expand(staged);
    if (diags_.errorCount()) return {};
    if (hadEof)
        result.push_back(synth(Tok::Eof, "", toks.empty() ? Token{} : toks.back()));
    return result;
}

// ---------------------------------------------------------------------------
// {.if EXPR.} -- a small closed integer/bool language
// ---------------------------------------------------------------------------

bool Preprocessor::evalUnary(const std::vector<Token>& e, size_t& p, int64_t& out,
                             bool& ok) {
    if (!ok || p >= e.size()) {
        ok = false;
        return false;
    }
    const Token t = e[p];
    if (isOp(t, "!")) {
        ++p;
        int64_t v = 0;
        evalUnary(e, p, v, ok);
        out = v ? 0 : 1;
        return true;
    }
    if (isOp(t, "-")) {
        ++p;
        int64_t v = 0;
        evalUnary(e, p, v, ok);
        out = -v;
        return true;
    }
    if (isOp(t, "+")) {
        ++p;
        return evalUnary(e, p, out, ok);
    }
    if (t.kind == Tok::Int) {
        ++p;
        out = std::strtoll(t.text.c_str(), nullptr, 10);
        return true;
    }
    if (isPunct(t, "(")) {
        ++p;
        if (!evalOr(e, p, out, ok) || !ok) {
            ok = false;
            return false;
        }
        if (p >= e.size() || !isPunct(e[p], ")")) {
            ok = false;
            return false;
        }
        ++p;
        return true;
    }
    if (t.kind == Tok::Ident) {
        if (t.text == "true" || t.text == "false") {
            ++p;
            out = (t.text == "true") ? 1 : 0;
            return true;
        }
        if (t.text == "defined") {
            ++p;
            if (p >= e.size() || !isPunct(e[p], "(")) {
                ok = false;
                return false;
            }
            ++p;
            if (p >= e.size() || e[p].kind != Tok::Ident) {
                ok = false;
                return false;
            }
            out = macros_.count(e[p].text) ? 1 : 0;
            ++p;
            if (p >= e.size() || !isPunct(e[p], ")")) {
                ok = false;
                return false;
            }
            ++p;
            return true;
        }
        // A bare identifier IS its definedness, so `{.if DEBUG.}` reads the way
        // it looks. This is the Rust `cfg` shape, not C's.
        ++p;
        out = macros_.count(t.text) ? 1 : 0;
        return true;
    }
    ok = false;
    return false;
}

bool Preprocessor::evalAdd(const std::vector<Token>& e, size_t& p, int64_t& out,
                           bool& ok) {
    if (!evalUnary(e, p, out, ok)) return false;
    while (ok && p < e.size() &&
           (isOp(e[p], "+") || isOp(e[p], "-") || isOp(e[p], "*") ||
            isOp(e[p], "/") || isOp(e[p], "%"))) {
        const std::string op = e[p].text;
        ++p;
        int64_t rhs = 0;
        if (!evalUnary(e, p, rhs, ok)) return false;
        if ((op == "/" || op == "%") && rhs == 0) {
            ok = false;
            return false;
        }
        if (op == "+") out += rhs;
        else if (op == "-") out -= rhs;
        else if (op == "*") out *= rhs;
        else if (op == "/") out /= rhs;
        else out %= rhs;
    }
    return true;
}

bool Preprocessor::evalCmp(const std::vector<Token>& e, size_t& p, int64_t& out,
                           bool& ok) {
    if (!evalAdd(e, p, out, ok)) return false;
    while (ok && p < e.size() &&
           (isOp(e[p], "==") || isOp(e[p], "!=") || isOp(e[p], "<") ||
            isOp(e[p], ">") || isOp(e[p], "<=") || isOp(e[p], ">="))) {
        const std::string op = e[p].text;
        ++p;
        int64_t rhs = 0;
        if (!evalAdd(e, p, rhs, ok)) return false;
        bool r = false;
        if (op == "==") r = (out == rhs);
        else if (op == "!=") r = (out != rhs);
        else if (op == "<") r = (out < rhs);
        else if (op == ">") r = (out > rhs);
        else if (op == "<=") r = (out <= rhs);
        else r = (out >= rhs);
        out = r ? 1 : 0;
    }
    return true;
}

bool Preprocessor::evalAnd(const std::vector<Token>& e, size_t& p, int64_t& out,
                           bool& ok) {
    if (!evalCmp(e, p, out, ok)) return false;
    while (ok && p < e.size() && isOp(e[p], "&&")) {
        ++p;
        int64_t rhs = 0;
        if (!evalCmp(e, p, rhs, ok)) return false;
        out = (out && rhs) ? 1 : 0;
    }
    return true;
}

bool Preprocessor::evalOr(const std::vector<Token>& e, size_t& p, int64_t& out,
                          bool& ok) {
    if (!evalAnd(e, p, out, ok)) return false;
    while (ok && p < e.size() && isOp(e[p], "||")) {
        ++p;
        int64_t rhs = 0;
        if (!evalAnd(e, p, rhs, ok)) return false;
        out = (out || rhs) ? 1 : 0;
    }
    return true;
}

bool Preprocessor::evalCondition(const std::vector<Token>& expr, uint32_t line,
                                 uint32_t col, bool& result) {
    if (expr.empty()) {
        err(line, col, "{.if.} expects a condition");
        return false;
    }
    bool ok = true;
    size_t pos = 0;
    int64_t v = 0;
    if (!evalOr(expr, pos, v, ok) || !ok) {
        err(line, col, "malformed condition in {.if.}");
        return false;
    }
    if (pos != expr.size()) {
        err(line, col, "trailing tokens after condition in {.if.}");
        return false;
    }
    result = v != 0;
    return true;
}

}  // namespace prepro
}  // namespace kirn
