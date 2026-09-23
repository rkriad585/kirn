#pragma once
// Abstract Syntax Tree for Kirn (grammar/coco.ebnf §3).
// Ownership: unique_ptr everywhere (plan §11.2); arenas arrive with MIR.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kirn {
namespace ast {

struct Expr;
struct Stmt;
struct Pat;
struct Type;
using ExprP  = std::unique_ptr<Expr>;
using StmtP  = std::unique_ptr<Stmt>;
using PatP   = std::unique_ptr<Pat>;
using TypeP  = std::unique_ptr<Type>;

struct Span {
    uint32_t line = 0, col = 0;
    uint32_t endLine = 0, endCol = 0;   // inclusive end; 0 => single-point
};

// ---- Types ----------------------------------------------------------------

enum class TyKind { Name, Pointer, Ref, Optional, Fn, Tuple };

struct Type {
    TyKind kind{};
    Span span;

    std::string name;                 // Name (+ generic args)
    std::vector<TypeP> generics;
    TypeP inner;                      // Pointer/Ref/Optional
    bool refMut = false;              // Ref: &mut
    std::vector<TypeP> params;        // Fn params / Tuple elems
    TypeP ret;                        // Fn return (may be null)

    static TypeP makeName(std::string n, Span s, std::vector<TypeP> gens = {});
    static TypeP makePointer(TypeP t, Span s);
    static TypeP makeRef(TypeP t, bool mut, Span s);
    static TypeP makeOptional(TypeP t, Span s);
    static TypeP makeFn(std::vector<TypeP> ps, TypeP r, Span s);
    static TypeP makeTuple(std::vector<TypeP> ts, Span s);
};

// ---- Patterns ---------------------------------------------------------------

enum class PatKind {
    Wild, Literal, Range, Tuple, Ctor, Bind, BindAlias, Or, Slice, Rest,
    Ref,      // `&pat` : match by dereference (transparent in the v1 by-value model)
};

struct PatField {                     // Ctor field: named `x: pat` or positional
    std::string name;                 // empty => positional
    PatP pat;
};

struct Pat {
    PatKind kind{};
    Span span;

    ExprP literal;                    // Literal
    PatP lo, hi;                      // Range (hi inclusive per '..=')
    bool inclusive = false;
    std::vector<PatP> elems;          // Tuple
    std::string ctorName;             // Ctor
    std::vector<PatField> fields;     // Ctor
    std::string bindName;             // Bind
    TypeP bindType;                   // Bind: `x is T`
    PatP aliasSub;                    // BindAlias: `pat @ name` (Rust `x @ pat`)
    std::vector<PatP> alts;           // Or: `p1 | p2 | ...`
    PatP inner;                   // Ref: `&inner-pattern`
    std::string restName;             // Slice/Tuple: `..rest` tail capture (""=none)
    bool hasRest = false;             // Slice/Tuple: a `..` appears (even unnamed)
};

// ---- Expressions -------------------------------------------------------------

enum class ExKind {
    Int, Float, CharLit, Str, FString,
    Ident, Unary, Binary, Call, Index, Slice, Member, Try,
    Lambda, Cond, ListComp, Generator,
    List, Dict, Set, Tuple, New, Cast, Match,
};

enum class StrFlavor { Normal, Raw, Byte, C };

struct CallArg {
    std::string name;                 // empty for positional
    ExprP value;
};

struct CompClause {                   // `for pat in iter` | `if cond`
    bool isFor = false;
    PatP pat;                         // for-clause
    ExprP iter;                       // for-clause
    ExprP cond;                       // if-clause
};

struct FStrPart {
    bool isExpr = false;
    std::string text;                 // text part / format spec carrier
    ExprP expr;
    std::string spec;                 // format spec after ':' inside {}
};

// One `case pat [if guard] { body }` arm, shared by the `match` statement and
// the match-*expression* (s = match v { … }). The arm's value (for expression
// form) is the trailing expression statement of `body`; otherwise `none`.
struct MatchArm {
    PatP pat;
    ExprP guard;                      // optional `if` guard; null if none
    std::vector<StmtP> body;
};

// Function/closure parameter. Declared before `Expr` so `Expr` can hold a
// typed closure parameter list (`fn (a: int, b: int) { … }`).
struct Param {
    std::string name;
    bool mutable_ = false;
    bool variadic = false;
    bool selfParam = false;
    TypeP type;                       // may be null (inferred)
    ExprP defaultValue;               // may be null
    Span span;
};

struct Expr {
    ExKind kind{};
    Span span;

    std::string text;                 // literals / ident name / member name
    StrFlavor flavor = StrFlavor::Normal;

    std::vector<FStrPart> parts;      // FString

    ExprP lhs, rhs;                   // Unary(rhs only) / Binary / Member(obj) /
                                      // Call(callee=lhs) / Index / Try / Cond
    std::string op;                   // Unary/Binary op spelling (".." "..=" too)
    bool nilSafe = false;             // Member via .?.

    std::vector<CallArg> args;        // Call / New
    std::vector<std::string> lambdaParams;
    // Block-bodied closure (`fn (x) { stmts }`): `closureParams` carries the
    // parameters (each `type` may be null = inferred), `lambdaBody` the
    // statement block, `retType` the optional declared `->` return type.
    // Arrow lambdas (`(x) => e`) keep `lambdaParams` + `rhs` and leave
    // closureParams/lambdaBody empty and retType null.
    std::vector<Param> closureParams;
    std::vector<StmtP> lambdaBody;
    TypeP retType;                    // declared closure return type (may be null)
    ExprP cond;                       // Cond / ListComp element guard base
    std::vector<ExprP> elems;         // List/Set/Tuple
    std::vector<std::pair<ExprP, ExprP>> pairs;   // Dict
    PatP pat;                         // Comprehension clause binding / Match subject
    std::vector<CompClause> clauses;  // ListComp/Generator
    TypeP newType;                    // New
    std::vector<MatchArm> matchArms;  // Match expression arms
};

// ---- Statements & declarations -------------------------------------------------

enum class StKind {
    FuncDef, StructDef, EnumDef, TraitDef, ImplDef,
    ConstDecl, VarDecl, Pass,
    ExprStmt, Assign, AugAssign,
    Return, Raise, Break, Continue, Defer,
    If, While, For, Match, Select, Try, Unsafe, Spawn,
    DoWhile, Goto, Label, Gather, GatherStmt,
    Import, Export,
    Del, LocalDecl, GlobalDecl, TempDecl, BucketDecl, Yield,
};

struct FieldDecl {
    bool weak = false;
    bool mutable_ = false;
    bool pub = false;
    std::string name;
    TypeP type;
    ExprP defaultValue;
    Span span;
};

struct VariantPayload { std::string name; TypeP type; };

struct Variant {
    std::string name;
    std::vector<VariantPayload> payload;   // unit variants: empty
    Span span;
};

struct TraitMethodSig {               // signature-only trait method
    std::string name;
    std::vector<Param> params;
    TypeP ret;
    Span span;
};

// A generic type parameter: `T`, `T is Bound`, or `T [is Bound] = Default`.
struct TypeParam {
    std::string name;
    TypeP bound;                        // `T is Bound`; may be null
    TypeP def;                          // `T = Default`; may be null
};

struct Stmt {
    StKind kind{};
    Span span;

    // FuncDef / TraitDef
    std::string name;
    std::vector<TypeParam> typeParams;  // [T [is Bound] [= Default]]
    std::vector<Param> params;
    TypeP ret;
    std::vector<StmtP> body;
    bool pub = false;
    bool privateDecl = false;         // `pr` modifier: explicit module-private
    bool externDef = false;           // `extern def f(...) -> T` (no body)
    bool isGenerator = false;         // body contains a `yield` statement

    // Import
    std::string moduleName;           // dotted as written
    struct ImportItem { std::string name, alias; };
    std::vector<ImportItem> importItems;
    bool starImport = false;
    std::string importAlias;          // `import a.b as c`
    bool fromImport = false;

    // StructDef
    std::vector<FieldDecl> fields;

    // EnumDef
    std::vector<Variant> variants;

    // TraitDef
    std::vector<TraitMethodSig> sigs;

    // ImplDef
    TypeP implTrait, implType;

    // ConstDecl / VarDecl
    ExprP target;                     // var target / const name-as-Ident
    TypeP declType;
    ExprP value;
    bool varKw = false;               // VarDecl written with `var` keyword (mutable)

    // TempDecl: `temp <name> <N>[: <Type>] = <value>` — a compile-time usage
    // budget. The binding is usable at most N (1..10) times; use #N+1 is a
    // compile error. `tempVarName`/`tempBudget` carry the parsed values.
    std::string tempVarName;
    int tempBudget = 0;

    // BucketDecl: `bucket <name> = <value>` parks `name` (value stored in the
    // separate bucket store, invisible to normal lookup); `bucket release <name>`
    // moves it back to a normal variable. `bucketHybrid`=false for park/release.
    bool bucketRelease = false;

    // Yield (generator stub; real generator frames land in the yield milestone)
    ExprP yieldExpr;

    // ExprStmt / Return/Raise/Defer/Spawn payloads
    std::vector<ExprP> exprs;         // Assign targets+values flattened:
                                      // [t1..tn, v1..vm]
    std::string augOp;

    // Labeled control flow
    std::string label;                // loop labels + break/continue targets

    // Try: catch binding name ("" = discard the raised value)
    std::string catchParam;

    // Struct heritage (single inheritance, from `class X extends Base`):
    // "" = none. Base fields are inherited (own fields override same-named base
    // fields) and base methods are inherited unless the derived class redefines
    // the same name (virtual dispatch).
    std::string baseName;

    // If / While / DoWhile / For / Match / Select
    std::vector<StmtP> elseBody;      // If else-chain tail / Try catch-block
    std::vector<StmtP> elifBodies;    // parallel to elifConds
    std::vector<ExprP> elifConds;
    PatP pat;                         // For binding pattern
    std::vector<MatchArm> arms;       // Match (statement); MatchArm reused
    struct SelArm { std::string bind; ExprP chanOp; std::vector<StmtP> body; };
    std::vector<SelArm> selArms;      // Select

    // DoWhile — `do { body } while (cond) ;` (also used as a search loop when
    // `pat` is present: `do { ... } while (pat = it.next()) { body }`).
    ExprP cond;                       // `while (cond)` expression, evaluated
                                     // at the end of each iteration
    // Goto / Gather. `goto lbl;` jumps to the `lbl:` label declared in the
    // same function (same or enclosing block). Labels never cross a function
    // boundary and the checker forbids forward jumps that skip a declaration.
    // A Label statement (`lbl:` followed by any statement) is itself a no-op.
    // Gather — `gather { <expr> { , <expr> }* }` seeds a worklist; each
    // `gather <expr>,` (GatherStmt) in the body appends an item. The loop
    // runs until the worklist empties.
    std::vector<ExprP> seedExprs;     // Gather: seed items evaluated first
};

// pretty-printer (used by cocoparse --ast)
void dump(const Stmt& s, int indent = 0);

} // namespace ast
} // namespace kirn
