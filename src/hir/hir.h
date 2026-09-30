#pragma once
// HIR — a lowered, sugar-free IR. Phase 3 of BUILDER-PLAN.md section 4.4.
//
// ## Why this exists
//
// `ast::Expr` is one fat struct: a `kind` tag plus 20 member declarations, of
// which any given node uses a handful. An `Ident` node still carries 5 owning
// pointers and 9 vectors of dead payload. `ast::Stmt` is worse (43 members for
// 37 kinds). That shape is why the VM compiler needs a `default:` that sets
// `interpreted = true` in two places, and why a desugaring has nowhere to put a
// node that is "usually" one thing and occasionally another.
//
// The fix is a per-kind payload union, which is what every serious compiler
// surveyed does (Nim's variant record, Swift/Kotlin's class per node kind,
// LLVM's Instruction subclasses, MLIR's Operation per op). A `std::variant`
// makes that unrepresentable-by-accident in a way a hand-rolled union does not:
// only the active alternative is well-formed, and a `std::visit` over the
// payload fails to COMPILE when a kind is added, so the exhaustive switch the
// lowering relies on cannot silently go stale. See `Lowering::expr`.
//
// ## Ownership
//
// `unique_ptr`, not an arena. The plan proposes "for HIR keep unique_ptr
// ownership to match house style, revisit at MIR", but that split has no
// precedent: rustc arena-allocates HIR too, while Swift, MLIR and Kotlin use
// pointer-stable trees for every stage. The transferable lesson from the
// research is the opposite of the plan's — do not mix ownership strategies
// across stages. So this stage is pointer-owned, and the MIR decision has to be
// made explicitly rather than by defaulting. See BUILDER-PLAN section 4.5.
//
// ## Scopes are explicit nodes
//
// `Block` is a node with a `ScopeId`, not an implicit region of the tree. This
// is not cosmetic. Two interpreter bugs found in phase 3 — a `var` that
// shadowed an outer binding escaping its block, and an `else` branch giving
// each statement its own frame — were both "which frame does this binding go
// in" questions that the tree could not answer. Here the frame is named.
//
// ## What is deliberately NOT in HIR
//
// Item bodies. A `struct`/`enum`/`trait`/`impl` is a declaration with a symbol;
// it is not a control-flow body, so it does not get lowered into statements.
// rustc makes the same cut (THIR "has no representation for items like
// structs or traits"). Declarations live in the Context's `decls` table and the
// module-level `items` list; only function bodies are lowered.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ast/ast.h"
#include "sema/checker.h"
#include "sema/symbols.h"
#include "sema/type.h"
#include "support/diag.h"

namespace kirn::hir {

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

// A stable integer handle into `Context::decls`. The minimum needed for HIR to
// refer to a resolved declaration: an index plus a sentinel. Name interning is
// deliberately NOT part of this — it is a separate concern (diagnostics, dumps)
// and rustc and Nim both keep it separate from resolution for good reason.
using DeclId = uint32_t;
inline constexpr DeclId NO_DECL = 0xFFFFFFFFu;

struct Decl {
    enum class Kind : uint8_t {
        Func, Param, Var, Const, GlobalVar, Type, Struct, Enum, Trait, Impl, Import, Builtin, Field, Variant
    };

    Kind kind = Kind::Var;
    std::string name;
    sema::TyP type;              // resolved type, when one is available
    std::string declaredType;    // the type AS WRITTEN, for declarations.
                                 // sema::Checker keeps resolveType() private,
                                 // so a declaration's type cannot be resolved
                                 // from outside it yet; expressions are fully
                                 // typed via the public typeOf(). Widening the
                                 // Checker's API is the follow-on, and doing it
                                 // here would mean re-resolving types and
                                 // risking a second opinion disagreeing with the
                                 // one that already checked the program.
    sema::FuncSig sig;          // only meaningful for Kind::Func
    ast::Span span{};
    DeclId parent = NO_DECL;     // enclosing function, for locals and params
    uint32_t slot = 0;           // dense frame-slot index, for locals and params
    bool isLocal = false;        // declared inside a function body
    bool mut = false;
    bool pub = false;
};

// A lexical scope. Scopes are referred to by id so that HIR can state which
// frame a binding belongs to rather than implying it by tree position.
using ScopeId = uint32_t;
inline constexpr ScopeId NO_SCOPE = 0xFFFFFFFFu;

struct Scope {
    ScopeId parent = NO_SCOPE;
    std::vector<DeclId> bindings;   // in declaration order
    bool isFunctionTop = false;
};

// The per-compilation context. Owned outside the IR and handed to the lowering,
// mirroring Swift's ASTContext and rustc's interner: the table of resolved
// declarations does not live in the IR, so a pass can rebuild the IR without
// re-resolving names, and the IR stays copyable and comparable.
struct Context {
    std::vector<Decl> decls;
    std::vector<Scope> scopes;
    std::unordered_map<std::string, std::vector<DeclId>> byName;

    DeclId addDecl(Decl d) {
        // Dense ids: the verifier asserts decls are 0..decls.size()-1 with no
        // holes, which is only meaningful if addDecl never skips a slot. This is
        // rustc's HIR density invariant, applied to the declaration table.
        DeclId id = static_cast<DeclId>(decls.size());
        byName[d.name].push_back(id);
        decls.push_back(std::move(d));
        return id;
    }
    const Decl* get(DeclId id) const {
        return id < decls.size() ? &decls[id] : nullptr;
    }
    ScopeId addScope(ScopeId parent, bool isFnTop) {
        ScopeId id = static_cast<ScopeId>(scopes.size());
        scopes.push_back(Scope{parent, {}, isFnTop});
        return id;
    }
};

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

struct Pattern;
using PatP = std::unique_ptr<Pattern>;

// One node per shape, so a Wild carries nothing and a Ctor carries only what a
// Ctor needs. `ast::Pat` is a single struct with all 12 PatKind payloads.
struct Pattern {
    enum class Kind : uint8_t { Wild, Literal, Range, Tuple, Ctor, Bind, BindAlias, Or, Slice, Rest, Ref };

    Kind kind = Kind::Wild;
    ast::Span span{};
    sema::TyP type;

    // Bind / BindAlias
    DeclId binding = NO_DECL;
    std::string name;
    PatP sub;                       // BindAlias `p as n` / Ref `&p` / Rest `..`

    // Literal / Range
    struct Lit { int64_t i = 0; double f = 0; std::string s; bool isFloat = false; bool isStr = false; };
    Lit lit;
    PatP lo, hi;                    // Range

    // Tuple / Slice / Ctor / Or
    std::vector<PatP> elems;        // Tuple, Slice, Or
    // `..name` in a tuple/slice pattern: a real binding, and it lives on the
    // pattern rather than in `elems`, which is why it is easy to miss.
    std::string restName;
    DeclId restBinding = NO_DECL;
    bool hasRest = false;
    std::string ctor;               // Ctor
    std::vector<struct PatField> fields;
};

struct PatField {
    std::string name;
    PatP pat;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;

// Per-kind payloads. Grouped by shared shape, never by "what the AST happened
// to put in one struct" — the grouping here is chosen so that no payload has a
// field that is invalid for its own kind.
namespace payload {

struct Lit {
    int64_t i = 0;
    double f = 0;
    std::string s;                 // also holds char literals
    ast::StrFlavor flavor = ast::StrFlavor::Normal;
    bool isFloat = false, isStr = false, isChar = false;
};
struct Name {
    DeclId decl = NO_DECL;         // resolved. NO_DECL means unresolved, which
                                   // the verifier rejects outside Error nodes.
    std::string spelling;          // kept for diagnostics and dumps
};
struct Unary {
    std::string op;                // "-", "!", "~", "spawn", "try"
    ExprP operand;
};
struct Binary {
    std::string op;                // includes "and"/"or"/"is"/"in"
    ExprP lhs, rhs;
};
struct Call {
    ExprP callee;
    std::vector<ExprP> args;
    bool nilSafe = false;
};
struct Index { ExprP obj, idx; };
struct Slice { ExprP obj, lo, hi, step; };
struct Member {
    ExprP obj;
    std::string name;
    DeclId field = NO_DECL;
    bool nilSafe = false;
};
struct Composite { std::vector<ExprP> elems; };   // List / Set / Tuple
struct DictLit {
    std::vector<ExprP> keys, values;
};
struct FString {
    // A desugaring, not sugar: parts become Call(Build, [text, expr, ...]).
    struct Part {
        bool isExpr = false;
        std::string text;
        ExprP expr;
        std::string spec;
    };
    std::vector<Part> parts;
};
struct Lambda {
    std::vector<DeclId> params;
    std::vector<StmtP> body;      // empty for an arrow lambda, which is a Call
    ExprP arrow;                   // `=> expr` form
    bool isBlock = false;
};
struct Cond { ExprP cond, then, otherwise; };
struct Comp {
    // Comprehension clauses, normalised so the order is explicit.
    struct Clause { std::string kind; Pattern pat; ExprP iterable, cond; };
    std::vector<Clause> clauses;
    ExprP element;                 // null for a Generator
    bool isGenerator = false;
};
struct MatchExpr {
    struct Arm { Pattern pat; std::vector<StmtP> body; ExprP value; bool hasValue = false; };
    ExprP subject;
    std::vector<Arm> arms;
};
struct NewExpr { std::string typeName; sema::TyP type; std::vector<ExprP> args; };
struct CastExpr { ExprP value; std::string typeName; sema::TyP type; };
struct TryExpr { ExprP operand; };              // postfix `?`
struct Error { std::string message; };          // a lowering that could not proceed

}  // namespace payload

// The per-kind union. `std::monostate` is the invalid/error state and is
// rejected by the verifier unless the node is an error node.
using ExprPayload = std::variant<
    std::monostate,
    payload::Lit, payload::Name, payload::Unary, payload::Binary, payload::Call,
    payload::Index, payload::Slice, payload::Member, payload::Composite,
    payload::DictLit, payload::FString, payload::Lambda, payload::Cond,
    payload::Comp, payload::MatchExpr, payload::NewExpr, payload::CastExpr,
    payload::TryExpr, payload::Error>;

enum class ExprKind : uint8_t {
    Int, Float, CharLit, Str, FString, Name, Unary, Binary, Call, Index, Slice,
    Member, Try, Lambda, Cond, ListComp, Generator, List, Dict, Set, Tuple, New,
    Cast, Match, Error
};

struct Expr {
    ExprKind kind = ExprKind::Error;
    ast::Span span{};
    sema::TyP type;               // from sema::Checker::typeOf
    ExprPayload p;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

namespace payload {

struct Block {
    std::vector<StmtP> stmts;
    ScopeId scope = NO_SCOPE;     // explicit: this is the frame these bind in
};
struct VarDecl { DeclId decl = NO_DECL; ExprP init; bool isConst = false; bool isLocal = false; };
struct Assign { ExprP target; ExprP value; };
struct AugAssign { ExprP target; std::string op; ExprP value; };
struct ExprStmt { ExprP value; };
struct If { ExprP cond; Block then_; std::vector<Block> elifs; std::vector<ExprP> elifConds; Block otherwise; bool hasElse = false; };
struct While { ExprP cond; Block body; std::string label; };
struct DoWhile { Block body; ExprP cond; std::string label; };
struct For {
    Pattern pat;
    ExprP iterable;
    Block body;
    std::string label;
    DeclId binding = NO_DECL;     // the simple-binding fast case
};
struct Return { std::vector<ExprP> values; };
struct Break { std::string label; };
struct Continue { std::string label; };
struct Raise { ExprP value; };
struct Defer { std::vector<StmtP> body; };
struct MatchStmt {
    ExprP subject;
    struct Arm { Pattern pat; std::vector<StmtP> body; };
    std::vector<Arm> arms;
};
struct Select {
    // A select arm binds a receive name and an optional channel op; unlike a
    // match arm it has no pattern (ast::Stmt::SelArm).
    struct Arm { std::string bind; DeclId decl = NO_DECL; ExprP chanOp; std::vector<StmtP> body; };
    ExprP subject;
    std::vector<Arm> arms;
};
struct TryStmt {
    std::vector<StmtP> body;
    std::string var;              // catch binding, empty for `catch { }`
    DeclId catchDecl = NO_DECL;
    std::vector<StmtP> handler;
};
struct UnsafeBlock { Block body; };
struct Spawn { ExprP expr; };
struct Goto { std::string label; };
struct Label { std::string name; Block body; };
struct Yield { ExprP value; bool hasValue = false; };
struct Del { std::vector<ExprP> targets; };
// `gather <expr>,` -- enqueue one more work item. The worklist loop itself is
// the enclosing `gather` block; this is how a body iteration re-queues.
struct GatherItem { ExprP item; };
// `gather { seeds } { body }` -- a worklist loop. Seeds are the initial items.
struct GatherLoop {
    std::vector<ExprP> seeds;
    Block body;
    // `item` is the implicit work-item binding the gather body sees.
    DeclId itemBinding = NO_DECL;
};

// A statement with no payload. `dropped` distinguishes a statement that really
// is nothing (`pass`) from one the LOWERING could not represent.
//
// This distinction is the whole point: a dropped statement used to be
// indistinguishable from a no-op, so HIR silently discarded code -- exactly the
// failure the design claims to avoid. The verifier rejects a dropped Nop, which
// makes "this construct has no mapping" a hard error at the point of lowering
// rather than a warning nobody reads. It immediately found three more drops
// (gather, bucket release) that a coverage count alone had reported as passing.
struct Nop {
    bool dropped = false;
    std::string reason;
};

}  // namespace payload

using StmtPayload = std::variant<
    std::monostate, payload::Nop, payload::Block, payload::VarDecl,
    payload::Assign, payload::AugAssign, payload::ExprStmt, payload::If,
    payload::While, payload::DoWhile, payload::For, payload::Return,
    payload::Break, payload::Continue, payload::Raise, payload::Defer,
    payload::MatchStmt, payload::Select, payload::TryStmt,
    payload::UnsafeBlock, payload::Spawn, payload::Goto, payload::Label,
    payload::Yield, payload::Del, payload::GatherItem, payload::GatherLoop>;

enum class StmtKind : uint8_t {
    Nop, Block, VarDecl, ExprStmt, Assign, AugAssign, Return, Raise, Break,
    Continue, Defer, If, While, For, Match, Select, Try, Unsafe, Spawn,
    DoWhile, Goto, Label, Yield, Del, GatherItem, GatherLoop
};

struct Stmt {
    StmtKind kind = StmtKind::Nop;
    ast::Span span{};
    StmtPayload p;
};

// ---------------------------------------------------------------------------
// Items and the module
// ---------------------------------------------------------------------------

// A lowered function: a signature plus a body. This is the unit MIR is built
// from, so it is the unit the HIR verifier iterates.
struct Func {
    DeclId decl = NO_DECL;
    std::string name;
    sema::FuncSig sig;
    std::vector<DeclId> params;
    ast::Span span{};
    ScopeId bodyScope = NO_SCOPE;
    std::vector<StmtP> body;
    std::vector<std::string> paramNames;
};

// Module-level items. Declarations are NOT lowered to statements; only their
// bodies are, and only functions have bodies MIR cares about.
struct StructDef {
    std::string name;
    std::vector<DeclId> fields;
    ast::Span span{};
};
struct EnumDef {
    std::string name;
    std::vector<DeclId> variants;
    ast::Span span{};
};
struct TraitDef {
    std::string name;
    std::vector<DeclId> methods;
    ast::Span span{};
};
struct ImplDef {
    std::string typeName, traitName;
    std::vector<DeclId> methods;
    ast::Span span{};
};
struct ImportDef {
    std::string moduleName, alias;
    // `from mod import a as b`: one DeclId per imported item, under the alias
    // when there is one. `alias` covers only `import mod as m`.
    std::vector<DeclId> items;
    ast::Span span{};
};
struct GlobalVar {
    DeclId decl = NO_DECL;
    ExprP init;
    ast::Span span{};
};

struct Item {
    enum class Kind : uint8_t { Func, Struct, Enum, Trait, Impl, Import, GlobalVar, Export, Unhandled };
    Kind kind = Kind::Unhandled;
    DeclId decl = NO_DECL;
    ast::Span span{};
    std::string name;             // for diagnostics; the real identity is decl

    std::unique_ptr<Func> func;
    std::unique_ptr<StructDef> strct;
    std::unique_ptr<EnumDef> enm;
    std::unique_ptr<TraitDef> trait;
    std::unique_ptr<ImplDef> impl;
    std::unique_ptr<ImportDef> imp;
    std::unique_ptr<GlobalVar> global;
    // `Unhandled` carries a note about which ast::StKind had no HIR mapping, so
    // the dump and the verifier can surface it instead of dropping it silently.
    std::string unhandledNote;
};

struct Module {
    std::string name;
    std::vector<Item> items;
    std::vector<Func*> funcs;     // in source order; MIR is built per function
};

// ---------------------------------------------------------------------------
// Lowering
// ---------------------------------------------------------------------------

struct LowerOptions {
    // Keep the `elif` chain flat. Off means the AST's nested-If-in-elseBody form
    // is preserved, because `ast::Stmt::elifConds` is never assigned by the
    // parser (verified: the parser nests `elif` as a synthetic If in
    // elseBody[0], and six separate readers of the dead fields exist).
    bool flattenElif = true;
    bool keepSpans = true;
};

// Lower a checked module. `chk` must be the Checker that produced the types on
// this AST; HIR does not re-run type checking, it only reads `typeOf` and
// records resolved DeclIds in `ctx`.
Module lower(const std::vector<ast::StmtP>& prog, sema::Checker& chk, Context& ctx,
             DiagEngine& diags, const LowerOptions& opts = {});

// ---------------------------------------------------------------------------
// Structural verification
// ---------------------------------------------------------------------------

struct Violation {
    ast::Span span{};
    std::string message;
};

// Verifies structural well-formedness WITHOUT executing the IR. This is the
// phase's correctness criterion: MIR is not executable yet (BUILDER-PLAN 4.5
// keeps it a separate AOT feeder), so a verifier is the only signal available.
//
// Scope is deliberately the same as LLVM's: LangRef says the verifier's
// violations "indicate bugs in transformation passes or input to the parser" —
// i.e. this finds compiler bugs, it does not validate user programs. Every
// check below is mechanical and names the node that broke it.
std::vector<Violation> verify(const Module& m, const Context& ctx);

// Dump. Deterministic by construction: declaration order, not hash order, and
// ids printed as dense indices, so a golden comparison is meaningful.
void dump(const Module& m, const Context& ctx, std::ostream& out);

}  // namespace kirn::hir
