#pragma once
// Semantic analysis for Kirn v1: symbol collection, name resolution,
// type inference/checking over the parsed AST. Pragmatic ruleset aligned
// with docs/COCO_PLAN.md §5 and the frozen example corpus.
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

    // Resolved type of an expression node, as recorded when that node was
    // type-checked by checkExpr (see typeCache_). Returns unkTy() if the node
    // was never reached / cached. This is the read-only query a code generation
    // backend uses to lower statically-typed expressions (PLAN Phase 8.2).
    TyP typeOf(const ast::Expr& e) const;

private:
    // ---- infrastructure ----
    void error(uint32_t line, uint32_t col, const std::string& msg);
    void error(const ast::Span& s, const std::string& msg);
    void warning(SpanRange span, const std::string& code,
                 const std::string& msg);
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

    // ---- types ----
    TyP resolveType(const ast::TypeP& t);
    FuncSig resolveSig(const std::vector<ast::Param>& params, const ast::TypeP& ret,
                       bool stripSelf);
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

    // ---- helpers ----
    SymP declareLocal(SymK kind, const std::string& name, TyP t, bool mut,
                      uint32_t line, uint32_t col);
    TyP iterableElem(const TyP& t) const;       // nullopt-equivalent: unkTy()
    std::optional<FuncSig> methodLookup(const TyP& recv, const std::string& name) const;
    bool isSendable(const TyP& t, std::string* why);
    bool isSendableVisit(const TyP& t, std::string* why,
                         std::set<std::string>& seeing);

    DiagEngine& diags_;
    Scope* scope_ = nullptr;
    int rootScopeId_ = 0;             // module top-level scope id (for local/global/temp)
    std::map<std::string, SymP> structs_;
    std::map<std::string, const ast::Stmt*> structDecls_;  // name -> StructDef node
    std::map<std::string, SymP> enums_;
    std::map<std::string, SymP> traits_;
    std::set<std::pair<std::string, std::string>> impls_;   // trait -> struct
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
};

} // namespace sema
} // namespace kirn
