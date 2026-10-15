#pragma once
// Semantic analysis for Kirn v1: symbol collection, name resolution,
// type inference/checking over the parsed AST. Pragmatic ruleset aligned
// with docs/KIRN_PLAN.md §5 and the frozen example corpus.
#include "ast/ast.h"
#include "sema/symbols.h"
#include "support/diag.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kirn {
namespace sema {

// Lint configuration (PLAN phase 2.5). `allow` suppresses a lint code;
// `deny` upgrades it to a hard error. Empty sets keep default severity.
struct LintConfig {
    std::set<std::string> allow;    // e.g. {"W0105"}
    std::set<std::string> deny;     // e.g. {"W0101"}
};

class Checker {
public:
    explicit Checker(DiagEngine& diags, const LintConfig* cfg = nullptr)
        : diags_(diags) {
        if (cfg) {
            lintAllow_ = cfg->allow;
            lintDeny_ = cfg->deny;
        }
    }

    // Runs collection (2 passes) + body checking. Reports into diags_.
    void checkModule(const std::vector<ast::StmtP>& prog);

    // `--strict`: escalation policy for advisory diagnostics. Currently the
    // W-narrow smart-cast warning, which becomes a hard error under strict.
    void setStrict(bool on) { strict_ = on; }

    // Resolved type of an expression node, as recorded when that node was
    // type-checked by checkExpr (see typeCache_). Returns unkTy() if the node
    // was never reached / cached. This is the read-only query a code generation
    // backend uses to lower statically-typed expressions (PLAN Phase 8.2).
    TyP typeOf(const ast::Expr& e) const;


    // Every name predeclareBuiltins() put in the root scope, in declaration
    // order. Published rather than re-listed by consumers, because two
    // hand-maintained copies already exist: kBuiltins() in src/vm/compiler.cpp
    // and its twin in enableVm() in src/interp/runtime.cpp. A builtin added to
    // the Checker but not to those lists silently loses its fast path, and one
    // added to the lists but not here fails to resolve. HIR needs the same
    // answer and should not add a third copy.
    const std::vector<std::string>& builtinNames() const { return builtinNames_; }

    // ---- types ----
    // Public because HIR needs the RESOLVED type of a declaration -- a parameter,
    // a `var`/`const` annotation, a struct field -- and expressions already have
    // one through typeOf(). resolveSig additionally builds a whole FuncSig.
    //
    // This is not a second opinion. Both are pure: they read typeVars_ and return
    // interned TyPs, with no AST mutation and no diagnostic state, so calling them
    // after checkModule() returns exactly what the Checker itself computed for the
    // same nodes. Re-resolving types independently elsewhere -- which is what HIR
    // did first, and had to abandon -- risks disagreeing with the pass that
    // already checked the program; re-reading these cannot.
    TyP resolveType(const ast::TypeP& t);
    FuncSig resolveSig(const std::vector<ast::Param>& params, const ast::TypeP& ret,
                       bool stripSelf);

    // The resolved signature of a function definition, or nullptr if this Checker
    // never checked that definition.
    //
    // A CACHE, not a recomputation, and that distinction is the whole point.
    // resolveSig/resolveType read `typeVars_`, which a TvGuard pushes and pops
    // around each generic function: inside `fn f[T](x: T)` the name T resolves
    // to a TypeVar, and after checkModule() returns it does not resolve at all.
    // Calling them from outside therefore yields "unknown type 'T'" for every
    // generic function -- which is exactly what happened when HIR first used them
    // directly, and it looked like a correct answer because it was the same
    // function returning a different thing in a different environment.
    //
    // So the signature is recorded where it is computed, with the type variables
    // still in scope, and read back here.
    // The resolved types of a struct definition's fields, in declaration order, or
    // nullptr if this Checker never saw that definition.
    //
    // Cached for the same reason as funcSig(): a generic struct's field can be
    // annotated with the enclosing type parameter (`struct Box[A] { first: A; }`),
    // and resolveType() can only answer that while the parameter is in scope.
    // registerTopLevel does not otherwise resolve field types at all, so without
    // this a later reader gets "unknown type 'A'".
    const std::vector<TyP>* fieldTypes(const ast::Stmt& def) const {
        auto it = structFields_.find(&def);
        return it == structFields_.end() ? nullptr : &it->second;
    }

    const FuncSig* funcSig(const ast::Stmt& def) const {
        auto it = funcSigs_.find(&def);
        return it == funcSigs_.end() ? nullptr : &it->second;
    }

private:
    // ---- infrastructure ----
    void error(uint32_t line, uint32_t col, const std::string& msg);
    void error(const ast::Span& s, const std::string& msg);
    void warning(SpanRange span, const std::string& code,
                 const std::string& msg);
    // W-narrow emission that respects `--strict` (error instead of warning).
    void narrowDiag(uint32_t line, uint32_t col, const std::string& name);
    void reportUnused(Scope& s, bool topLevel);
    Scope& push();
    void pop();

    // ---- declaration collection ----
    void predeclareBuiltins();
    SymP makeBuiltinFunc(std::string name, std::vector<TyP> params,
                         std::vector<std::string> paramNames, TyP ret,
                         bool variadic, size_t required);
    SymP declareConst(std::string name, TyP t);
    void registerNominals(const std::vector<ast::StmtP>& prog);
    void fillNominals(const std::vector<ast::StmtP>& prog);
    void registerTopLevel(const std::vector<ast::StmtP>& prog);
    void fillStructBody(SymP st, const ast::Stmt& s);

    TyP lookupNominal(TyK k, const std::string& name) const;
    bool assignable(const TyP& from, const TyP& to) const;
    TyP unify(const TyP& a, const TyP& b, uint32_t line, uint32_t col,
              const char* ctx);
    TyP unwrapOpt(const TyP& t) const;          // Opt[T] -> T ; passthrough else
    TyP unwrapResult(const TyP& t) const;       // result[T,E] -> T

    // ---- statements ----
    void checkStmt(const ast::Stmt& s);
    void checkBlock(const std::vector<ast::StmtP>& body);
    void checkFuncLike(const ast::Stmt& s, TyP selfTy, SymP funcSym);
    void checkAssignTargets(const ast::Stmt& s);
    void checkPatternBindings(const ast::Pat& p, const TyP& subject);
    void requireBool(const TyP& t, uint32_t line, uint32_t col, const char* what);
    void requireIntish(const TyP& t, const char* what, uint32_t line,
                       uint32_t col);
    void compClause(const ast::CompClause& cl, uint32_t line);
    void checkReassignable(const ast::Expr& tgt);
    void assignTarget(const ast::Expr& tgt, const TyP& vt, bool multi = false);
    // Walrus `(name := value)`. Binds/assigns the target and marks it used
    // (the value it yields is its own use), so it never trips W0102.
    void bindWalrus(const ast::Expr& tgt, const TyP& vt);

    // ---- expressions ----
    // checkExpr is the public caching wrapper; the real walk lives in
    // checkExprImpl. Every expression node visited gets its resolved TyP
    // recorded in typeCache_ so codegen can query it later.
    TyP checkExpr(const ast::Expr& e);
    TyP checkExprImpl(const ast::Expr& e);
    TyP checkCall(const ast::Expr& e);
    TyP checkMemberCall(const TyP& recv, const std::string& name,
                        const std::vector<ast::CallArg>& args, uint32_t line,
                        uint32_t col);
    // Resolves a generic function call's return type: constraints gathered from
    // actual arguments win over declared defaults; unconstrained TypeVars
    // without a default stay dynamic (inference-only generics).
    TyP genericApply(const Symbol* sym,
                     const std::vector<ast::CallArg>& args, uint32_t line,
                     uint32_t col);
    bool matchArgs(const FuncSig& sig, const std::vector<ast::CallArg>& args,
                   uint32_t line, uint32_t col, const char* what);
    TyP checkEnumCtorCall(const Symbol* vs,
                          const std::vector<ast::CallArg>& args);
    // Exhaustiveness (PLAN 5.4): enum subjects must cover every variant; a
    // wildcard is the only way to cover an integer/char subject today.
    void checkExhaustiveness(const TyP& subj,
                             const std::vector<ast::MatchArm>& arms,
                             uint32_t line, uint32_t col);
    TyP memberAccess(const TyP& obj, const std::string& name, uint32_t line,
                     uint32_t col, bool nilSafe);

    // ---- M3: flow-sensitive narrowing ----
    // Applies the facts a condition `c` establishes (when `positive` is true
    // for the then-branch, false for the else-branch) into `out`, keyed by
    // symbol. Only stable (non-`var`) bindings are narrowed; a `var` subject
    // yields W-narrow instead.
    void narrowApply(const ast::Expr* c, bool positive,
                     std::map<const Symbol*, TyP>& out);
    // M3: the type fact a pattern establishes for its subject. `positive`
    // selects the matching-branch fact (subject is the pattern's type) vs the
    // fall-through fact (subject failed the pattern, e.g. an optional that was
    // not `none` is unwrapped). Returns null when the pattern implies no change.
    TyP patternFact(const ast::Pat& p, const TyP& subject, bool positive);
    // Applies the facts for one `case` arm to `narrowed_` inside the arm's
    // scope: the running type `cur` (what failing earlier arms guarantees) plus
    // the arm pattern's own positive refinement. Warns (once) for a mutable
    // subject, matching narrowApply's W-narrow contract.
    void narrowSubject(Symbol* sym, const TyP& cur, const ast::Pat* pat,
                       uint32_t line, uint32_t col);
    // True when a block definitely transfers control (return/raise/break/
    // continue) on every path that reaches its end.
    bool alwaysExits(const std::vector<ast::StmtP>& body) const;
    // Opt[T] -> T, collapsing a nested optional; passthrough otherwise.
    TyP collapseOpt(const TyP& t) const;
    // M3: element type contributed by a `*expr` unpack in a list/set
    // display or comprehension; reports when the operand is not iterable.
    TyP unpackElem(const ast::Expr& spread, const char* where);

    // ---- helpers ----
    SymP declareLocal(SymK kind, const std::string& name, TyP t, bool mut,
                      uint32_t line, uint32_t col);
    TyP iterableElem(const TyP& t) const;       // nullopt-equivalent: unkTy()
    std::optional<FuncSig> methodLookup(const TyP& recv, const std::string& name) const;
    // True when a struct/trait-object receiver has `name` as an inherent method
    // or as a method supplied by any `impl Trait for T` block. Used by the
    // `with`/`using` protocol checks, which must see impl-provided methods.
    bool typeHasMethod(const TyP& recv, const std::string& name) const;
    bool isSendable(const TyP& t, std::string* why);
    bool isSendableVisit(const TyP& t, std::string* why,
                         std::set<std::string>& seeing);

    DiagEngine& diags_;
    Scope* scope_ = nullptr;
    std::vector<std::string> builtinNames_;
    std::unordered_map<const ast::Stmt*, FuncSig> funcSigs_;
    std::unordered_map<const ast::Stmt*, std::vector<TyP>> structFields_;
    int rootScopeId_ = 0;             // module top-level scope id (for local/global/temp)
    std::map<std::string, SymP> structs_;
    std::map<std::string, const ast::Stmt*> structDecls_;  // name -> StructDef node
    std::map<std::string, SymP> enums_;
    std::map<std::string, SymP> traits_;
    std::set<std::pair<std::string, std::string>> impls_;   // trait -> struct
    std::map<std::string, std::set<std::string>> implMethods_;  // struct -> impl-provided method names
    std::map<std::string, SymP> funcs_;                     // top-level funcs
    std::map<std::string, TyP> typeVars_;                   // active generics
    std::set<std::string> importRoots_;
    std::set<std::string> builtins_;                        // print/len/sqrt
    TyP selfTy_;
    TyP currentRet_;
    int loopDepth_ = 0;
    bool inGen_ = false;       // checking a generator function body
    TyP genElem_;              // unified element type of the yields
    int gatherDepth_ = 0;      // >0 while checking inside a gather { } body
    // Per-block `label name :` maps (label -> statement index), innermost
    // first. `goto` resolves against the top map only (block-local targets).
    std::vector<std::map<std::string, std::size_t>> labelScopes_;
    int quiet_ = 0;   // >0: error() is suppressed (speculative re-walks)
    std::set<std::string> lintAllow_;
    std::set<std::string> lintDeny_;
    // expr node -> resolved TyP, populated by the checkExpr wrapper. Keyed by
    // node address; safe because AST nodes are stable unique_ptrs.
    std::unordered_map<const ast::Expr*, TyP> typeCache_;
    // M1 pipe topic: type of `%` inside each enclosing `|>` body, innermost
    // last. Empty outside a pipe (a `%` there is E-PIPE-TOPIC).
    std::vector<TyP> topicStack_;
    // M3 flow-sensitive narrowing facts, keyed by Symbol*. Scoped with
    // push()/pop() (narrowedStack_) so facts about a scope's bindings die with
    // it -- otherwise a destroyed Symbol's address could be reused by a later
    // declaration and inherit a stale narrowed type.
    std::map<const Symbol*, TyP> narrowed_;
    std::vector<std::map<const Symbol*, TyP>> narrowedStack_;
    // Symbols already reported by W-narrow for the condition/pattern currently
    // being checked. Cleared per `if`/`case` so one unstable binding yields one
    // diagnostic, not one per branch.
    std::set<const Symbol*> narrowWarned_;
    // Advisory diagnostics become errors when set (see setStrict).
    bool strict_ = false;
};

} // namespace sema
} // namespace kirn
