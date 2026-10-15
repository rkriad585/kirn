// HIR structural verification and dumping.
//
// Why a verifier and not a differential test: BUILDER-PLAN 4.5 keeps MIR a
// separate AOT feeder and never introduces a MIR interpreter in any phase, so
// the phase's verify criterion ("preserves results ... run after lower-verify vs
// direct run") cannot be satisfied by execution -- and the term `lower-verify`
// is defined nowhere in the repository. A structural verifier is the honest
// substitute, and it is what mature compilers actually use at this stage:
// Swift's SILVerifier, LLVM's Verifier, rustc's hir_id_validator.
//
// The scope is LLVM's, and deliberately so. LangRef: "The violations pointed
// out by the verifier pass indicate bugs in transformation passes or input to
// the parser." This finds compiler bugs. It does not validate user programs --
// sema::Checker already did that.
//
// Every check below is mechanical, names the node that broke it, and is
// runnable without executing anything.

#include "hir/hir.h"

#include <algorithm>
#include <ostream>
#include <sstream>
#include <string>
#include <variant>

namespace kirn::hir {
namespace {

struct Verifier {
    const Module& m;
    const Context& ctx;
    std::vector<Violation> out;
    int depth = 0;
    size_t nodeCount = 0;

    Verifier(const Module& mod, const Context& c) : m(mod), ctx(c) {}

    void bad(const ast::Span& sp, std::string msg) { out.push_back(Violation{sp, std::move(msg)}); }

    // --- ids -------------------------------------------------------------
    bool declOk(DeclId id, const ast::Span& sp, const char* what) {
        if (id < ctx.decls.size()) return true;
        bad(sp, std::string(what) + ": decl id " + std::to_string(id) +
                    " out of range (decls.size()=" + std::to_string(ctx.decls.size()) + ")");
        return false;
    }
    bool scopeOk(ScopeId id, const ast::Span& sp, const char* what) {
        if (id < ctx.scopes.size()) return true;
        bad(sp, std::string(what) + ": scope id " + std::to_string(id) +
                    " out of range (scopes.size()=" + std::to_string(ctx.scopes.size()) + ")");
        return false;
    }

    // --- the discriminator check ----------------------------------------
    // The point of the per-kind payload union: a node cannot hold a payload that
    // is invalid for its kind, because there is nowhere to put it. The one way
    // tag and payload can still disagree is a lowering bug that constructs the
    // wrong alternative, and that is what this asserts.
    //
    // Deliberately NOT a table of hand-written variant indices. The first
    // version of this file had one, it was wrong in nine places the moment it
    // ran, and it would have needed editing every time a payload is inserted.
    // The direction used instead -- "the active payload implies this kind" --
    // cannot drift, because the payload type names the kind in the same
    // `if constexpr` arm that has to check it.
    //
    // In the old fat-node IR the equivalent check is "every field not valid for
    // this kind is at its default", which is exactly the bug class that made
    // `ast::Expr` (20 members, 9 vectors, for any one of 24 kinds) hard to
    // reason about.
    void wantKind(const ast::Span& sp, ExprKind have, ExprKind want, const char* what) {
        if (have != want)
            bad(sp, std::string(what) + ": " + ek(want) + " payload on a " + ek(have) + " node");
    }
    void wantKind(const ast::Span& sp, StmtKind have, StmtKind want, const char* what) {
        if (have != want)
            bad(sp, std::string(what) + ": " + sk(want) + " payload on a " + sk(have) + " node");
    }
    // Kept local rather than shared with Dumper: a verification message should
    // not be able to change because the dump's spelling changed.
    static const char* ek(ExprKind k) {
        switch (k) {
            case ExprKind::Int: return "int";         case ExprKind::Float: return "float";
            case ExprKind::CharLit: return "char";   case ExprKind::Str: return "str";
            case ExprKind::FString: return "fstring";case ExprKind::Name: return "name";
            case ExprKind::Unary: return "unary";     case ExprKind::Binary: return "binary";
            case ExprKind::Call: return "call";       case ExprKind::Index: return "index";
            case ExprKind::Slice: return "slice";     case ExprKind::Member: return "member";
            case ExprKind::Try: return "try";         case ExprKind::Lambda: return "lambda";
            case ExprKind::Cond: return "cond";       case ExprKind::ListComp: return "listcomp";
            case ExprKind::SetComp: return "setcomp"; case ExprKind::DictComp: return "dictcomp";
            case ExprKind::Generator: return "generator"; case ExprKind::List: return "list";
            case ExprKind::Dict: return "dict";       case ExprKind::Set: return "set";
            case ExprKind::Tuple: return "tuple";     case ExprKind::New: return "new";
            case ExprKind::Cast: return "cast";       case ExprKind::Match: return "match";
            case ExprKind::Let: return "let";         case ExprKind::Error: return "error";
        }
        return "?";
    }
    static const char* sk(StmtKind k) {
        switch (k) {
            case StmtKind::Nop: return "nop";        case StmtKind::Block: return "block";
            case StmtKind::VarDecl: return "var";    case StmtKind::ExprStmt: return "expr";
            case StmtKind::Assign: return "assign";  case StmtKind::AugAssign: return "augassign";
            case StmtKind::Return: return "return";  case StmtKind::Raise: return "raise";
            case StmtKind::Break: return "break";    case StmtKind::Continue: return "continue";
            case StmtKind::Defer: return "defer";    case StmtKind::If: return "if";
            case StmtKind::While: return "while";    case StmtKind::For: return "for";
            case StmtKind::Match: return "match";    case StmtKind::Select: return "select";
            case StmtKind::Try: return "try";        case StmtKind::Unsafe: return "unsafe";
            case StmtKind::Spawn: return "spawn";    case StmtKind::DoWhile: return "dowhile";
            case StmtKind::Goto: return "goto";      case StmtKind::Label: return "label";
            case StmtKind::Yield: return "yield";    case StmtKind::Del: return "del";
            case StmtKind::GatherItem: return "gather-item";
            case StmtKind::GatherLoop: return "gather-loop";
        }
        return "?";
    }

    // --- patterns ---------------------------------------------------------
    void pat(const Pattern& p, const ast::Span& owner) {
        if (++depth > 512) { depth--; bad(owner, "pattern nesting exceeds the depth limit"); return; }
        ++nodeCount;
        if (p.kind == Pattern::Kind::Bind && p.binding == NO_DECL)
            bad(p.span, "pattern binds '" + p.name + "' but has no decl");
        else if (p.binding != NO_DECL)
            declOk(p.binding, p.span, "pattern binding");
        if (p.lo) pat(*p.lo, owner);
        if (p.hi) pat(*p.hi, owner);
        if (p.sub) pat(*p.sub, owner);
        for (const auto& e : p.elems) if (e) pat(*e, owner);
        for (const auto& f : p.fields) if (f.pat) pat(*f.pat, owner);
        --depth;
    }

    // --- expressions ------------------------------------------------------
    void expr(const Expr& e) {
        if (++depth > 512) { depth--; bad(e.span, "expression nesting exceeds the depth limit"); return; }
        ++nodeCount;
        std::visit([&](auto& p) { exprPayload(e, p); }, e.p);
        --depth;
    }

    template <class P>
    void exprPayload(const Expr& e, P& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            // Legal only on an explicit error node; a silent monostate is how a
            // lowering bug produces a node that verifies and means nothing.
            if (e.kind != ExprKind::Error)
                bad(e.span, "expression has no payload but is not an error node");
        } else if constexpr (std::is_same_v<T, payload::Error>) {
            // always fine
        } else if constexpr (std::is_same_v<T, payload::Lit>) {
            // This arm was MISSING from the chain, so a Lit payload fell through
            // every branch and was never checked against its kind at all. Nothing
            // in the corpus could find it, because the corpus has no kind/payload
            // mismatch -- only the in-process self-test builds one on purpose.
            if (e.kind != ExprKind::Int && e.kind != ExprKind::Float &&
                e.kind != ExprKind::CharLit && e.kind != ExprKind::Str)
                bad(e.span, std::string("literal payload on a ") + ek(e.kind) + " node");
            // The flavour flags must agree with the kind too: a "float" whose
            // payload says isStr is the same class of disagreement one level in.
            if (e.kind == ExprKind::Str && !p.isStr)
                bad(e.span, "str node with a non-string literal payload");
            if (e.kind == ExprKind::CharLit && !p.isChar)
                bad(e.span, "char node with a non-char literal payload");
            if (e.kind == ExprKind::Float && !p.isFloat)
                bad(e.span, "float node with a non-float literal payload");
            if (e.kind == ExprKind::Int && (p.isStr || p.isChar || p.isFloat))
                bad(e.span, "int node with a mismatched literal payload");
        } else if constexpr (std::is_same_v<T, payload::Name>) {
            wantKind(e.span, e.kind, ExprKind::Name, "Name");
            if (p.decl == NO_DECL)
                bad(e.span, "name '" + p.spelling + "' did not resolve to a declaration");
            else
                declOk(p.decl, e.span, "name reference");
        } else if constexpr (std::is_same_v<T, payload::Call>) {
            wantKind(e.span, e.kind, ExprKind::Call, "Call");
            if (!p.callee) bad(e.span, "call has no callee");
            else expr(*p.callee);
            for (const auto& a : p.args)
                if (a) expr(*a); else bad(e.span, "call has a null argument");
        } else if constexpr (std::is_same_v<T, payload::Binary>) {
            wantKind(e.span, e.kind, ExprKind::Binary, "Binary");
            if (!p.lhs) bad(e.span, "binary has no lhs");
            else expr(*p.lhs);
            if (!p.rhs) {
                // An open-ended range is `a..` / `a..=` with no upper bound, and
                // the parser produces it as a Binary with a null rhs. That is
                // real syntax -- `text.split(" ")[8..]` in examples/30 -- and the
                // VM has a dedicated opcode for it (OP_UNARANGE). Flagging every
                // null rhs rejected a legal program.
                if (p.op != ".." && p.op != "..=")
                    bad(e.span, "binary '" + p.op + "' has no rhs");
            } else {
                expr(*p.rhs);
            }
        } else if constexpr (std::is_same_v<T, payload::Unary>) {
            wantKind(e.span, e.kind, ExprKind::Unary, "Unary");
            if (!p.operand) bad(e.span, "unary has no operand");
            else expr(*p.operand);
        } else if constexpr (std::is_same_v<T, payload::Index>) {
            wantKind(e.span, e.kind, ExprKind::Index, "Index");
            if (p.obj) expr(*p.obj);
            if (p.idx) expr(*p.idx);
        } else if constexpr (std::is_same_v<T, payload::Member>) {
            wantKind(e.span, e.kind, ExprKind::Member, "Member");
            if (p.obj) expr(*p.obj);
            if (p.field != NO_DECL) declOk(p.field, e.span, "member field");
        } else if constexpr (std::is_same_v<T, payload::Composite>) {
            if (e.kind != ExprKind::List && e.kind != ExprKind::Set && e.kind != ExprKind::Tuple)
                bad(e.span, "composite payload on a non-composite node");
            for (const auto& el : p.elems)
                if (el) expr(*el); else bad(e.span, "composite has a null element");
        } else if constexpr (std::is_same_v<T, payload::DictLit>) {
            wantKind(e.span, e.kind, ExprKind::Dict, "Dict");
            if (p.keys.size() != p.values.size()) bad(e.span, "dict key/value arity mismatch");
            for (size_t i = 0; i < p.keys.size(); ++i) {
                if (p.keys[i]) expr(*p.keys[i]);
                if (i < p.values.size() && p.values[i]) expr(*p.values[i]);
            }
        } else if constexpr (std::is_same_v<T, payload::FString>) {
            wantKind(e.span, e.kind, ExprKind::FString, "FString");
            for (const auto& part : p.parts)
                if (part.isExpr && part.expr) expr(*part.expr);
        } else if constexpr (std::is_same_v<T, payload::Cond>) {
            wantKind(e.span, e.kind, ExprKind::Cond, "Cond");
            if (!p.cond) bad(e.span, "ternary has no condition");
            else expr(*p.cond);
            if (p.then) expr(*p.then);
            if (p.otherwise) expr(*p.otherwise);
        } else if constexpr (std::is_same_v<T, payload::Comp>) {
            if (e.kind != ExprKind::ListComp && e.kind != ExprKind::Generator &&
                e.kind != ExprKind::SetComp && e.kind != ExprKind::DictComp)
                bad(e.span, "comprehension payload on a non-comprehension node");
            for (const auto& c : p.clauses) {
                pat(c.pat, e.span);
                if (c.iterable) expr(*c.iterable);
                if (c.cond) expr(*c.cond);
            }
            if (p.element) expr(*p.element);
            if (p.value) expr(*p.value);
            if (e.kind == ExprKind::DictComp && !p.value)
                bad(e.span, "dict comprehension has no value expression");
            if (!p.isGenerator && !p.element)
                bad(e.span, "comprehension has no element expression");
        } else if constexpr (std::is_same_v<T, payload::MatchExpr>) {
            wantKind(e.span, e.kind, ExprKind::Match, "Match");
            if (p.subject) expr(*p.subject);
            if (p.arms.empty()) bad(e.span, "match expression has no arms");
            for (const auto& a : p.arms) {
                pat(a.pat, e.span);
                for (const auto& s : a.body) if (s) stmt(*s);
                if (a.value) expr(*a.value);
            }
        } else if constexpr (std::is_same_v<T, payload::Lambda>) {
            wantKind(e.span, e.kind, ExprKind::Lambda, "Lambda");
            for (DeclId id : p.params) declOk(id, e.span, "lambda parameter");
            for (const auto& s : p.body) if (s) stmt(*s);
            if (p.arrow) expr(*p.arrow);
        } else if constexpr (std::is_same_v<T, payload::NewExpr>) {
            wantKind(e.span, e.kind, ExprKind::New, "New");
            for (const auto& a : p.args) if (a) expr(*a);
        } else if constexpr (std::is_same_v<T, payload::CastExpr>) {
            wantKind(e.span, e.kind, ExprKind::Cast, "Cast");
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::TryExpr>) {
            wantKind(e.span, e.kind, ExprKind::Try, "Try");
            if (p.operand) expr(*p.operand);
        } else if constexpr (std::is_same_v<T, payload::Let>) {
            wantKind(e.span, e.kind, ExprKind::Let, "Let");
            if (p.decl != NO_DECL) declOk(p.decl, e.span, "let binding");
            if (p.init) expr(*p.init);
            if (p.body) expr(*p.body);
        } else if constexpr (std::is_same_v<T, payload::Slice>) {
            wantKind(e.span, e.kind, ExprKind::Slice, "Slice");
            if (p.obj) expr(*p.obj);
            if (p.lo) expr(*p.lo);
            if (p.hi) expr(*p.hi);
            if (p.step) expr(*p.step);
        }
    }

    // --- statements -------------------------------------------------------
    void block(const payload::Block& b, const ast::Span& owner) {
        if (b.scope == NO_SCOPE) bad(owner, "block has no scope: the frame it binds in is unnamed");
        else scopeOk(b.scope, owner, "block");
        for (const auto& s : b.stmts)
            if (s) stmt(*s); else bad(owner, "block has a null statement");
    }

    void stmtList(const std::vector<StmtP>& v, const ast::Span& owner) {
        for (const auto& s : v)
            if (s) stmt(*s); else bad(owner, "statement list has a null entry");
    }

    void stmt(const Stmt& s) {
        if (++depth > 512) { depth--; bad(s.span, "statement nesting exceeds the depth limit"); return; }
        ++nodeCount;
        std::visit([&](auto& p) { stmtPayload(s, p); }, s.p);
        --depth;
    }

    template <class P>
    void stmtPayload(const Stmt& s, P& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            bad(s.span, "statement has no payload");
        } else if constexpr (std::is_same_v<T, payload::Nop>) {
            if (s.kind != StmtKind::Nop)
                bad(s.span, "Nop payload on a non-Nop statement");
            // A dropped statement is a hole in the program, not a no-op. This is
            // the check that turns "no HIR mapping for X" into a hard error at
            // the point of lowering instead of a warning nobody reads.
            if (p.dropped)
                bad(s.span, "lowering DROPPED a statement (no HIR mapping for " +
                                p.reason + ")");
        } else if constexpr (std::is_same_v<T, payload::Block>) {
            wantKind(s.span, s.kind, StmtKind::Block, "Block");
            block(p, s.span);
        } else if constexpr (std::is_same_v<T, payload::VarDecl>) {
            wantKind(s.span, s.kind, StmtKind::VarDecl, "VarDecl");
            if (p.decl == NO_DECL) bad(s.span, "declaration has no decl");
            else declOk(p.decl, s.span, "declaration");
            if (p.init) expr(*p.init);
        } else if constexpr (std::is_same_v<T, payload::ExprStmt>) {
            wantKind(s.span, s.kind, StmtKind::ExprStmt, "ExprStmt");
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::Assign>) {
            wantKind(s.span, s.kind, StmtKind::Assign, "Assign");
            if (p.target) expr(*p.target);
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::AugAssign>) {
            wantKind(s.span, s.kind, StmtKind::AugAssign, "AugAssign");
            if (p.target) expr(*p.target);
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::If>) {
            wantKind(s.span, s.kind, StmtKind::If, "If");
            if (!p.cond) bad(s.span, "if has no condition");
            else expr(*p.cond);
            block(p.then_, s.span);
            // The lowerer pushes a cond and a body together, so a mismatch here
            // would desync every later elif onto the wrong body.
            if (p.elifs.size() != p.elifConds.size())
                bad(s.span, "if has " + std::to_string(p.elifs.size()) + " elif bodies but " +
                            std::to_string(p.elifConds.size()) + " elif conditions");
            for (size_t i = 0; i < p.elifConds.size(); ++i) {
                if (p.elifConds[i]) expr(*p.elifConds[i]);
                else bad(s.span, "elif condition is null");
                if (i < p.elifs.size()) block(p.elifs[i], s.span);
            }
            if (p.hasElse) block(p.otherwise, s.span);
        } else if constexpr (std::is_same_v<T, payload::While>) {
            wantKind(s.span, s.kind, StmtKind::While, "While");
            if (!p.cond) bad(s.span, "while has no condition");
            else expr(*p.cond);
            block(p.body, s.span);
        } else if constexpr (std::is_same_v<T, payload::DoWhile>) {
            wantKind(s.span, s.kind, StmtKind::DoWhile, "DoWhile");
            block(p.body, s.span);
            if (!p.cond) bad(s.span, "do-while has no condition");
            else expr(*p.cond);
        } else if constexpr (std::is_same_v<T, payload::For>) {
            wantKind(s.span, s.kind, StmtKind::For, "For");
            pat(p.pat, s.span);
            if (!p.iterable) bad(s.span, "for has no iterable");
            else expr(*p.iterable);
            block(p.body, s.span);
            if (p.binding != NO_DECL) declOk(p.binding, s.span, "for binding");
        } else if constexpr (std::is_same_v<T, payload::Return>) {
            wantKind(s.span, s.kind, StmtKind::Return, "Return");
            for (const auto& v : p.values) if (v) expr(*v);
        } else if constexpr (std::is_same_v<T, payload::MatchStmt>) {
            wantKind(s.span, s.kind, StmtKind::Match, "Match");
            if (p.subject) expr(*p.subject);
            if (p.arms.empty()) bad(s.span, "match statement has no arms");
            for (const auto& a : p.arms) {
                pat(a.pat, s.span);
                stmtList(a.body, s.span);
            }
        } else if constexpr (std::is_same_v<T, payload::Select>) {
            wantKind(s.span, s.kind, StmtKind::Select, "Select");
            if (p.subject) expr(*p.subject);
            if (p.arms.empty()) bad(s.span, "select has no arms");
            for (const auto& a : p.arms) {
                if (a.chanOp) expr(*a.chanOp);
                stmtList(a.body, s.span);
            }
        } else if constexpr (std::is_same_v<T, payload::TryStmt>) {
            wantKind(s.span, s.kind, StmtKind::Try, "Try");
            stmtList(p.body, s.span);
            stmtList(p.handler, s.span);
            if (p.catchDecl != NO_DECL) declOk(p.catchDecl, s.span, "catch binding");
        } else if constexpr (std::is_same_v<T, payload::UnsafeBlock>) {
            wantKind(s.span, s.kind, StmtKind::Unsafe, "Unsafe");
            block(p.body, s.span);
        } else if constexpr (std::is_same_v<T, payload::Label>) {
            wantKind(s.span, s.kind, StmtKind::Label, "Label");
            block(p.body, s.span);
        } else if constexpr (std::is_same_v<T, payload::Defer>) {
            wantKind(s.span, s.kind, StmtKind::Defer, "Defer");
            stmtList(p.body, s.span);
        } else if constexpr (std::is_same_v<T, payload::Raise>) {
            wantKind(s.span, s.kind, StmtKind::Raise, "Raise");
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::Spawn>) {
            wantKind(s.span, s.kind, StmtKind::Spawn, "Spawn");
            if (p.expr) expr(*p.expr);
        } else if constexpr (std::is_same_v<T, payload::Yield>) {
            wantKind(s.span, s.kind, StmtKind::Yield, "Yield");
            if (p.value) expr(*p.value);
        } else if constexpr (std::is_same_v<T, payload::GatherItem>) {
            wantKind(s.span, s.kind, StmtKind::GatherItem, "GatherItem");
            if (p.item) expr(*p.item);
        } else if constexpr (std::is_same_v<T, payload::GatherLoop>) {
            wantKind(s.span, s.kind, StmtKind::GatherLoop, "GatherLoop");
            for (const auto& seed : p.seeds)
                if (seed) expr(*seed);
            block(p.body, s.span);
        } else if constexpr (std::is_same_v<T, payload::Del>) {
            wantKind(s.span, s.kind, StmtKind::Del, "Del");
            for (const auto& t : p.targets) if (t) expr(*t);
        } else {
            // Break / Continue / Goto carry only a label; nothing to check.
        }
    }

    // --- whole-module -----------------------------------------------------
    void run() {
        // V1: decl ids must be dense. addDecl never skips a slot, so this holds
        // by construction -- which is exactly why it is worth asserting, since
        // a future addDecl that reuses ids would break it silently.
        for (size_t i = 0; i < ctx.decls.size(); ++i) {
            const Decl& d = ctx.decls[i];
            if (d.parent != NO_DECL) declOk(d.parent, d.span, "decl parent");
            if (d.isLocal && d.parent == NO_DECL)
                bad(d.span, "local '" + d.name + "' has no enclosing function");
        }
        // V2: scope chains terminate and do not cycle.
        for (size_t i = 0; i < ctx.scopes.size(); ++i) {
            size_t steps = 0;
            ScopeId cur = static_cast<ScopeId>(i);
            while (cur != NO_SCOPE) {
                if (cur >= ctx.scopes.size()) {
                    bad(ast::Span{}, "scope chain from " + std::to_string(i) + " leaves the table");
                    break;
                }
                if (++steps > ctx.scopes.size()) {
                    bad(ast::Span{}, "scope chain from " + std::to_string(i) + " is cyclic");
                    break;
                }
                cur = ctx.scopes[cur].parent;
            }
            for (DeclId b : ctx.scopes[i].bindings) declOk(b, ast::Span{}, "scope binding");
        }
        // V3: items.
        for (const auto& it : m.items) {
            if (it.decl != NO_DECL) {
                if (!declOk(it.decl, it.span, "item decl")) continue;
                const Decl& d = ctx.decls[it.decl];
                bool kindOk = true;
                switch (it.kind) {
                    case Item::Kind::Func:    kindOk = d.kind == Decl::Kind::Func; break;
                    case Item::Kind::Struct:  kindOk = d.kind == Decl::Kind::Struct; break;
                    case Item::Kind::Enum:    kindOk = d.kind == Decl::Kind::Enum; break;
                    case Item::Kind::Trait:   kindOk = d.kind == Decl::Kind::Trait; break;
                    case Item::Kind::Import:  kindOk = d.kind == Decl::Kind::Import; break;
                    case Item::Kind::GlobalVar: kindOk = d.kind == Decl::Kind::GlobalVar; break;
                    default: kindOk = true; break;
                }
                if (!kindOk)
                    bad(it.span, "item '" + it.name + "' points at a decl of the wrong kind");
            }
            if (it.kind == Item::Kind::Unhandled && !it.unhandledNote.empty())
                bad(it.span, "item '" + it.name + "' has no HIR mapping: " + it.unhandledNote);
            if (it.kind == Item::Kind::Func) {
                if (!it.func) { bad(it.span, "func item has no Func"); continue; }
                func(*it.func);
            } else if (it.kind == Item::Kind::GlobalVar) {
                if (it.global && it.global->init) expr(*it.global->init);
                if (it.global && it.global->decl != NO_DECL)
                    declOk(it.global->decl, it.span, "global var");
            }
        }
        // V4: every function in m.funcs is the func of exactly one item, and
        // vice versa. A duplicate here would mean MIR builds the same body twice.
        std::vector<Func*> modFuncs = m.funcs;
        std::vector<Func*> itemFuncs;
        for (const auto& it : m.items)
            if (it.func) itemFuncs.push_back(it.func.get());
        std::sort(modFuncs.begin(), modFuncs.end());
        std::sort(itemFuncs.begin(), itemFuncs.end());
        if (modFuncs != itemFuncs)
            bad(ast::Span{}, "module function list and item list disagree");
    }

    void func(const Func& f) {
        if (f.bodyScope == NO_SCOPE)
            bad(f.span, "function '" + f.name + "' has no body scope");
        else if (scopeOk(f.bodyScope, f.span, "function body scope")) {
            if (!ctx.scopes[f.bodyScope].isFunctionTop)
                bad(f.span, "function '" + f.name + "' body scope is not marked as a function top");
        }
        for (DeclId id : f.params) {
            if (!declOk(id, f.span, "function parameter")) continue;
            const Decl& d = ctx.decls[id];
            if (d.kind != Decl::Kind::Param)
                bad(f.span, "function '" + f.name + "' has a non-param in its parameter list");
            if (d.slot >= f.paramNames.size())
                bad(f.span, "parameter '" + d.name + "' has slot " + std::to_string(d.slot) +
                                " but the function has " + std::to_string(f.paramNames.size()) + " slots");
        }
        // Slots must be dense: every index 0..paramNames.size()-1 is taken.
        // This is the check that makes "slot < paramNames.size()" a real bound
        // rather than a tautology, and it is rustc's ItemLocalId density
        // invariant applied to frame slots.
        if (!f.paramNames.empty()) {
            std::vector<int> seen(f.paramNames.size(), 0);
            for (DeclId id : f.params)
                if (id < ctx.decls.size()) seen[ctx.decls[id].slot]++;
            for (size_t i = 0; i < seen.size(); ++i)
                if (seen[i] == 0)
                    bad(f.span, "function '" + f.name + "' slot " + std::to_string(i) +
                                    " is unassigned: slots must be dense");
        }
        stmtList(f.body, f.span);
    }
};

}  // namespace

std::vector<Violation> verify(const Module& m, const Context& ctx) {
    Verifier v(m, ctx);
    v.run();
    return std::move(v.out);
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

namespace {

// Types print by name, never by address: a dump containing a pointer is a dump
// that cannot be golden-tested.
std::string tyName(const sema::TyP& t) {
    if (!t) return "none";
    // Prints the type NAME, never the address, and never a numeric id, so the
    // dump is stable across runs. Unknown falls back to the nominal name.
    switch (t->k) {
        case sema::TyK::Int:      return t->name.empty() ? "Int" : t->name;
        case sema::TyK::Float:    return t->name.empty() ? "Float" : t->name;
        case sema::TyK::Bool:     return "Bool";
        case sema::TyK::Char:     return t->name.empty() ? "Char" : t->name;
        case sema::TyK::Str:      return "Str";
        case sema::TyK::Bytes:    return "bytes";
        case sema::TyK::None:     return "none";
        case sema::TyK::Unknown:  return "any";
        case sema::TyK::Error:    return "error";
        case sema::TyK::Opt:      return t->inner ? tyName(t->inner) + "?" : "opt";
        case sema::TyK::Ptr:      return t->inner ? "*" + tyName(t->inner) : "ptr";
        case sema::TyK::Ref:      return t->inner ? "&" + tyName(t->inner) : "ref";
        case sema::TyK::List:     return t->args.empty() ? "list" : "list<" + tyName(t->args[0]) + ">";
        case sema::TyK::Dict:     return "dict";
        case sema::TyK::Set:      return t->args.empty() ? "set" : "set<" + tyName(t->args[0]) + ">";
        case sema::TyK::Chan:     return "chan";
        case sema::TyK::Range:    return "range";
        case sema::TyK::EnumVal:  return t->name + "::" + t->variant;
        default: return t->name.empty() ? "?" : t->name;
    }
}

struct Dumper {
    std::ostream& o;
    explicit Dumper(std::ostream& out) : o(out) {}

    static const char* ek(ExprKind k) {
        switch (k) {
            case ExprKind::Int: return "int";      case ExprKind::Float: return "float";
            case ExprKind::CharLit: return "char";case ExprKind::Str: return "str";
            case ExprKind::FString: return "fstring"; case ExprKind::Name: return "name";
            case ExprKind::Unary: return "unary";  case ExprKind::Binary: return "binary";
            case ExprKind::Call: return "call";    case ExprKind::Index: return "index";
            case ExprKind::Slice: return "slice";  case ExprKind::Member: return "member";
            case ExprKind::Try: return "try";      case ExprKind::Lambda: return "lambda";
            case ExprKind::Cond: return "cond";    case ExprKind::ListComp: return "listcomp";
            case ExprKind::SetComp: return "setcomp"; case ExprKind::DictComp: return "dictcomp";
            case ExprKind::Generator: return "generator"; case ExprKind::List: return "list";
            case ExprKind::Dict: return "dict";    case ExprKind::Set: return "set";
            case ExprKind::Tuple: return "tuple";  case ExprKind::New: return "new";
            case ExprKind::Cast: return "cast";    case ExprKind::Match: return "match";
            case ExprKind::Let: return "let";    case ExprKind::Error: return "error";
        }
        return "?";
    }
    static const char* sk(StmtKind k) {
        switch (k) {
            case StmtKind::Nop: return "nop";      case StmtKind::Block: return "block";
            case StmtKind::VarDecl: return "var";  case StmtKind::ExprStmt: return "expr";
            case StmtKind::Assign: return "assign";case StmtKind::AugAssign: return "augassign";
            case StmtKind::Return: return "return";case StmtKind::Raise: return "raise";
            case StmtKind::Break: return "break";  case StmtKind::Continue: return "continue";
            case StmtKind::Defer: return "defer";  case StmtKind::If: return "if";
            case StmtKind::While: return "while";  case StmtKind::For: return "for";
            case StmtKind::Match: return "match";  case StmtKind::Select: return "select";
            case StmtKind::Try: return "try";      case StmtKind::Unsafe: return "unsafe";
            case StmtKind::Spawn: return "spawn";  case StmtKind::DoWhile: return "dowhile";
            case StmtKind::Goto: return "goto";    case StmtKind::Label: return "label";
            case StmtKind::Yield: return "yield";  case StmtKind::Del: return "del";
                case StmtKind::GatherItem: return "gather-item";
            case StmtKind::GatherLoop: return "gather-loop";
    }
        return "?";
    }

    void ind(int n) { for (int i = 0; i < n; ++i) o << "  "; }

    // A range bound, inline. Range bounds are always literals in practice; a
    // non-literal prints as `?` rather than lying about it.
    static std::string boundText(const Pattern& p) {
        if (p.kind != Pattern::Kind::Literal) return "?";
        if (p.lit.isStr) return "\"" + p.lit.s + "\"";
        if (p.lit.isFloat) return std::to_string(p.lit.f);
        return std::to_string(p.lit.i);
    }

    void pat(const Pattern& p, int n) {
        static const char* pk[] = {"wild", "literal", "range", "tuple", "ctor",
                                   "bind", "bindalias", "or", "slice", "rest", "ref"};
        ind(n);
        o << "pat " << pk[static_cast<int>(p.kind)];
        if (p.kind == Pattern::Kind::Bind) o << " " << p.name << " #" << p.binding;
        if (p.kind == Pattern::Kind::Ctor) o << " " << p.ctor;
        if (p.kind == Pattern::Kind::Rest && !p.name.empty()) o << " " << p.name;
        if (p.kind == Pattern::Kind::Literal) {
            if (p.lit.isStr) o << " \"" << p.lit.s << "\"";
            else if (p.lit.isFloat) o << " " << p.lit.f;
            else o << " " << p.lit.i;
        }
        o << "\n";
        for (const auto& e : p.elems) if (e) pat(*e, n + 1);
        for (const auto& f : p.fields) if (f.pat) pat(*f.pat, n + 1);
        if (p.kind == Pattern::Kind::Range) {
            // One line, `lo..hi` or `lo..=hi`, so the distinction is visible in the
            // dump and therefore TESTABLE. Dropping `inclusive` used to turn every
            // `case 200..=299` into `case 200..298` and nothing noticed, because
            // the drop check only catches a lost statement, not a lost field.
            ind(n);
            o << "  range ";
            if (p.lo) o << boundText(*p.lo);
            o << (p.inclusive ? "..=" : "..");
            if (p.hi) o << boundText(*p.hi);
            o << "\n";
            if (p.lo) pat(*p.lo, n + 1);
            if (p.hi) pat(*p.hi, n + 1);
            return;
        }
        if (p.lo) pat(*p.lo, n + 1);
        if (p.hi) pat(*p.hi, n + 1);
        if (p.sub) pat(*p.sub, n + 1);
    }

    void expr(const Expr& e, int n) {
        ind(n);
        o << ek(e.kind) << " : " << tyName(e.type);
        std::visit([&](auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, payload::Lit>) {
                if (p.isStr) o << " \"" << p.s << "\"";
                else if (p.isChar) o << " " << p.i;
                else if (p.isFloat) o << " " << p.f;
                else o << " " << p.i;
            } else if constexpr (std::is_same_v<T, payload::Name>) {
                o << " " << p.spelling << " -> #" << p.decl;
            } else if constexpr (std::is_same_v<T, payload::Unary>) {
                o << " " << p.op;
            } else if constexpr (std::is_same_v<T, payload::Binary>) {
                o << " " << p.op;
            } else if constexpr (std::is_same_v<T, payload::Member>) {
                o << " ." << p.name << (p.nilSafe ? " ?" : "");
            } else if constexpr (std::is_same_v<T, payload::NewExpr>) {
                o << " " << p.typeName;
            } else if constexpr (std::is_same_v<T, payload::CastExpr>) {
                o << " as " << p.typeName;
            } else if constexpr (std::is_same_v<T, payload::Let>) {
                o << " #" << p.decl;
            } else if constexpr (std::is_same_v<T, payload::Error>) {
                o << " " << p.message;
            }
        }, e.p);
        o << "\n";
        std::visit([&](auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, payload::Lit> || std::is_same_v<T, payload::Error>) {
            } else if constexpr (std::is_same_v<T, payload::Name>) {
            } else if constexpr (std::is_same_v<T, payload::Unary>) {
                if (p.operand) expr(*p.operand, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Binary>) {
                if (p.lhs) expr(*p.lhs, n + 1);
                if (p.rhs) expr(*p.rhs, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Call>) {
                if (p.callee) expr(*p.callee, n + 1);
                for (const auto& a : p.args) if (a) expr(*a, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Index>) {
                if (p.obj) expr(*p.obj, n + 1);
                if (p.idx) expr(*p.idx, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Slice>) {
                if (p.obj) expr(*p.obj, n + 1);
                if (p.lo) expr(*p.lo, n + 1);
                if (p.hi) expr(*p.hi, n + 1);
                if (p.step) expr(*p.step, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Member>) {
                if (p.obj) expr(*p.obj, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Composite>) {
                for (const auto& el : p.elems) if (el) expr(*el, n + 1);
            } else if constexpr (std::is_same_v<T, payload::DictLit>) {
                for (size_t i = 0; i < p.keys.size(); ++i) {
                    if (p.keys[i]) expr(*p.keys[i], n + 1);
                    if (i < p.values.size() && p.values[i]) expr(*p.values[i], n + 1);
                }
            } else if constexpr (std::is_same_v<T, payload::FString>) {
                for (const auto& part : p.parts)
                    if (part.isExpr && part.expr) expr(*part.expr, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Cond>) {
                if (p.cond) expr(*p.cond, n + 1);
                if (p.then) expr(*p.then, n + 1);
                if (p.otherwise) expr(*p.otherwise, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Comp>) {
                for (const auto& c : p.clauses) {
                    ind(n + 1); o << c.kind << "\n";
                    pat(c.pat, n + 2);
                    if (c.iterable) expr(*c.iterable, n + 2);
                    if (c.cond) expr(*c.cond, n + 2);
                }
                if (p.element) expr(*p.element, n + 1);
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::MatchExpr>) {
                if (p.subject) expr(*p.subject, n + 1);
                for (const auto& a : p.arms) { pat(a.pat, n + 1); stmtList(a.body, n + 2); }
            } else if constexpr (std::is_same_v<T, payload::Lambda>) {
                for (const auto& s : p.body) if (s) stmt(*s, n + 1);
                if (p.arrow) expr(*p.arrow, n + 1);
            } else if constexpr (std::is_same_v<T, payload::NewExpr>) {
                for (const auto& a : p.args) if (a) expr(*a, n + 1);
            } else if constexpr (std::is_same_v<T, payload::CastExpr>) {
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::TryExpr>) {
                if (p.operand) expr(*p.operand, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Let>) {
                if (p.decl != NO_DECL) { ind(n + 1); o << "decl #" << p.decl << "\n"; }
                if (p.init) expr(*p.init, n + 1);
                if (p.body) expr(*p.body, n + 1);
            }
        }, e.p);
    }

    void stmtList(const std::vector<StmtP>& v, int n) {
        for (const auto& s : v) if (s) stmt(*s, n);
    }

    void stmt(const Stmt& s, int n) {
        ind(n);
        o << sk(s.kind);
        std::visit([&](auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, payload::VarDecl>) {
                o << " #" << p.decl << (p.isConst ? " const" : "") << (p.isLocal ? " local" : "");
            } else if constexpr (std::is_same_v<T, payload::If>) {
                o << " elifs=" << p.elifs.size() << " else=" << (p.hasElse ? 1 : 0);
            } else if constexpr (std::is_same_v<T, payload::AugAssign>) {
                o << " " << p.op;
            } else if constexpr (std::is_same_v<T, payload::Break> ||
                                 std::is_same_v<T, payload::Continue> ||
                                 std::is_same_v<T, payload::Goto> ||
                                 std::is_same_v<T, payload::Label> ||
                                 std::is_same_v<T, payload::While> ||
                                 std::is_same_v<T, payload::DoWhile> ||
                                 std::is_same_v<T, payload::For>) {
                (void)p;
            }
        }, s.p);
        o << "\n";
        std::visit([&](auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, payload::Block>) {
                block(p, n + 1);
            } else if constexpr (std::is_same_v<T, payload::VarDecl>) {
                if (p.init) expr(*p.init, n + 1);
            } else if constexpr (std::is_same_v<T, payload::ExprStmt>) {
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Assign> ||
                                 std::is_same_v<T, payload::AugAssign>) {
                if (p.target) expr(*p.target, n + 1);
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::If>) {
                if (p.cond) expr(*p.cond, n + 1);
                ind(n + 1); o << "then\n"; block(p.then_, n + 2);
                for (size_t i = 0; i < p.elifConds.size(); ++i) {
                    ind(n + 1); o << "elif\n";
                    if (p.elifConds[i]) expr(*p.elifConds[i], n + 2);
                    if (i < p.elifs.size()) block(p.elifs[i], n + 2);
                }
                if (p.hasElse) { ind(n + 1); o << "else\n"; block(p.otherwise, n + 2); }
            } else if constexpr (std::is_same_v<T, payload::While>) {
                if (p.cond) expr(*p.cond, n + 1);
                block(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::DoWhile>) {
                block(p.body, n + 1);
                if (p.cond) expr(*p.cond, n + 1);
            } else if constexpr (std::is_same_v<T, payload::For>) {
                ind(n + 1); o << "iterable\n";
                if (p.iterable) expr(*p.iterable, n + 2);
                ind(n + 1); o << "binding\n"; pat(p.pat, n + 2);
                block(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Return>) {
                for (const auto& v : p.values) if (v) expr(*v, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Raise>) {
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::MatchStmt>) {
                if (p.subject) expr(*p.subject, n + 1);
                for (const auto& a : p.arms) { pat(a.pat, n + 1); stmtList(a.body, n + 2); }
            } else if constexpr (std::is_same_v<T, payload::Select>) {
                if (p.subject) expr(*p.subject, n + 1);
                for (const auto& a : p.arms) {
                    ind(n + 1); o << "arm " << a.bind << "\n";
                    if (a.chanOp) expr(*a.chanOp, n + 2);
                    stmtList(a.body, n + 2);
                }
            } else if constexpr (std::is_same_v<T, payload::TryStmt>) {
                ind(n + 1); o << "try\n"; stmtList(p.body, n + 2);
                ind(n + 1); o << "catch " << (p.var.empty() ? "-" : p.var) << "\n";
                stmtList(p.handler, n + 2);
            } else if constexpr (std::is_same_v<T, payload::UnsafeBlock>) {
                block(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Label>) {
                ind(n + 1); o << p.name << "\n";
                block(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Defer>) {
                stmtList(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Spawn>) {
                if (p.expr) expr(*p.expr, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Yield>) {
                if (p.value) expr(*p.value, n + 1);
            } else if constexpr (std::is_same_v<T, payload::GatherItem>) {
                if (p.item) expr(*p.item, n + 1);
            } else if constexpr (std::is_same_v<T, payload::GatherLoop>) {
                ind(n + 1); o << "seeds\n";
                for (const auto& seed : p.seeds) if (seed) expr(*seed, n + 2);
                block(p.body, n + 1);
            } else if constexpr (std::is_same_v<T, payload::Del>) {
                for (const auto& t : p.targets) if (t) expr(*t, n + 1);
            }
        }, s.p);
    }

    void block(const payload::Block& b, int n) {
        ind(n); o << "scope #" << b.scope << "\n";
        stmtList(b.stmts, n);
    }
};

}  // namespace

void dump(const Module& m, const Context& ctx, std::ostream& out) {
    Dumper d(out);
    out << "hir 1\n";
    out << "module " << m.name << "\n";
    out << "decls " << ctx.decls.size() << "\n";
    out << "scopes " << ctx.scopes.size() << "\n";
    for (const auto& dl : ctx.decls) {
        out << "decl #" << (&dl - ctx.decls.data()) << " ";
        switch (dl.kind) {
            case Decl::Kind::Func: out << "func"; break;
            case Decl::Kind::Param: out << "param"; break;
            case Decl::Kind::Var: out << "var"; break;
            case Decl::Kind::Const: out << "const"; break;
            case Decl::Kind::GlobalVar: out << "global"; break;
            case Decl::Kind::Type: out << "type"; break;
            case Decl::Kind::Struct: out << "struct"; break;
            case Decl::Kind::Enum: out << "enum"; break;
            case Decl::Kind::Trait: out << "trait"; break;
            case Decl::Kind::Impl: out << "impl"; break;
            case Decl::Kind::Import: out << "import"; break;
            case Decl::Kind::Builtin: out << "builtin"; break;
            case Decl::Kind::Field: out << "field"; break;
            case Decl::Kind::Variant: out << "variant"; break;
        }
        out << " " << dl.name;
        if (dl.isLocal) out << " local";
        if (dl.parent != NO_DECL) out << " parent=#" << dl.parent;
        if (dl.type) out << " : " << tyName(dl.type);
        if (!dl.declaredType.empty()) out << " declared=" << dl.declaredType;
        if (dl.isLocal) out << " slot=" << dl.slot;
        out << "\n";
    }
    for (const auto& it : m.items) {
        switch (it.kind) {
            case Item::Kind::Func:
                out << "func " << it.func->name << " #" << it.func->decl
                    << " params=" << it.func->sig.arity()
                    << " required=" << it.func->sig.required
                    << " scope=#" << it.func->bodyScope << "\n";
                for (size_t i = 0; i < it.func->params.size(); ++i) {
                    const Decl* pd = ctx.get(it.func->params[i]);
                    out << "  param " << i << " #" << it.func->params[i];
                    if (pd) out << " " << pd->name;
                    out << "\n";
                }
                d.stmtList(it.func->body, 1);
                break;
            case Item::Kind::Struct:
                out << "struct " << it.strct->name << " #" << it.decl
                    << " fields=" << it.strct->fields.size() << "\n";
                for (DeclId f : it.strct->fields) {
                    const Decl* fd = ctx.get(f);
                    out << "  field #" << f << (fd ? " " + fd->name : std::string()) << "\n";
                }
                break;
            case Item::Kind::Enum:
                out << "enum " << it.enm->name << " #" << it.decl << "\n";
                for (DeclId v : it.enm->variants) {
                    const Decl* vd = ctx.get(v);
                    out << "  variant #" << v << (vd ? " " + vd->name : std::string()) << "\n";
                }
                break;
            case Item::Kind::Trait:
                out << "trait " << it.trait->name << " #" << it.decl
                    << " methods=" << it.trait->methods.size() << "\n";
                break;
            case Item::Kind::Impl:
                out << "impl " << it.impl->typeName
                    << (it.impl->traitName.empty() ? "" : " : " + it.impl->traitName) << "\n";
                d.stmtList(it.impl->methods.empty() ? std::vector<StmtP>() : std::vector<StmtP>(), 1);
                break;
            case Item::Kind::Import:
                out << "import " << it.imp->moduleName
                    << (it.imp->alias.empty() ? "" : " as " + it.imp->alias)
                    << " #" << it.decl << "\n";
                break;
            case Item::Kind::GlobalVar:
                out << "global " << it.name << " #" << it.decl << "\n";
                if (it.global && it.global->init) d.expr(*it.global->init, 1);
                break;
            case Item::Kind::Export:
                out << "export\n";
                break;
            case Item::Kind::Unhandled:
                out << "unhandled " << it.name << " : " << it.unhandledNote << "\n";
                break;
        }
    }
    out << "end\n";
}

}  // namespace kirn::hir
