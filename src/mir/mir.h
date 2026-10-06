#pragma once
// MIR — a CFG-based, value-oriented IR. Phase 3 of BUILDER-PLAN section 4.5.
//
// ## Why a CFG at all
//
// Nothing in this codebase has one. The bytecode is a flat linear stream whose
// only control-flow structure is a relative offset in an operand -- a "CFG" that
// exists purely as a property of arithmetic on `a`. `SasmEmitter` emits symbolic
// labels but is a text listing with no machine representation. So the blocks
// here are genuinely new, and the verifier is the only way to know they are
// right, since nothing executes MIR yet.
//
// ## The rules this follows, and where they come from
//
// Block structure is LLVM's, and LLVM's is the one every CFG-based IR shares:
// LangRef says a function "contains a list of basic blocks, forming the CFG ...
// each basic block ... contains a list of instructions and ends with a
// terminator instruction". `BasicBlock.h` adds the part that actually matters
// for construction: "The BasicBlock class allows malformed basic blocks to occur
// because it may be useful in the intermediate stage of constructing or
// modifying a program. However, the verifier will ensure that basic blocks are
// 'well formed'." This IR permits the same during construction and leans on
// `verify()` to catch it.
//
// Successor edges are NOT stored. LLVM has no `succs` vector -- edges are derived
// from the terminator -- and the reason is worth copying rather than
// rediscovering: it makes "the terminator says one thing and the edge list says
// another" unrepresentable, because there is no second source of truth. So
// `successors()` is computed, and the verifier checks that every successor is in
// range rather than checking an edge list against a terminator.
//
// Entry is a derived property, not a flag: `id() == 0`. LangRef: the entry block
// "is not allowed to have predecessor basic blocks ... Because the block can
// have no predecessors, it also cannot have any PHI nodes." LLVM models
// `landingpad` the same way -- as a predicate over the *incoming edge*, not a
// block tag -- and this IR does not attempt to model EH at all.
//
// Two rules from the plan that the survey did NOT support are not implemented,
// deliberately:
//
//   - "Destructor-free" (§4.5) is not a documented IR term from any major
//     compiler, and LLVM is a counter-example: `llvm::Value` is non-copyable by
//     design and keeps its name in a side table. The spirit of the requirement --
//     IR values cheap to copy and safe to arena-allocate -- is met by holding
//     operands as 32-bit indices rather than by a naming trick.
//   - "Every instruction has at least one use" is not an LLVM invariant. The LLVM
//     verifier has no such check and dead instructions are legal; DCE is an
//     optimization pass, not a well-formedness rule. Emitting unused values here
//     is legal, and `verify()` does not complain.
//
// ## Values and memory
//
// No SSA construction. The plan's own §4.6 says the right thing: "the front end
// never builds SSA by hand; it emits allocas + stores, and the `mem2reg` pass
// produces SSA". So MIR is alloca/load/store shaped -- memory is explicit,
// values are single-assignment -- and Phase 4's LLVM backend gets SSA for free
// from mem2reg. Hand-rolled dominance analysis is therefore not needed here
// either, which is a large amount of code avoided rather than a feature
// deferred.
//
// Dominance IS still checked, and cheaply: the research is clear that dominance is
// a real correctness property (LLVM: "Instruction does not dominate all uses!")
// and that its absence is a genuine miscompile source. Because this IR is built
// straight from structured HIR rather than from a flat stream, every use is
// dominated by construction -- so the check is a real invariant that should never
// fire, and a `verify()` that reports it is telling you the lowering is wrong.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "hir/hir.h"

namespace kirn::mir {

// ---------------------------------------------------------------------------
// Values and types
// ---------------------------------------------------------------------------

// A dense index into a function's value table. Values are never referenced by
// pointer, which is the point: an index is trivially copyable, compares by
// integer, and survives serialization. It is the same reason rustc uses
// `LocalId`/`BasicBlockId` and Go uses `type Register int32`.
using Value = uint32_t;
inline constexpr Value NO_VALUE = 0xFFFFFFFFu;

using BlockId = uint32_t;
inline constexpr BlockId NO_BLOCK = 0xFFFFFFFFu;

// Mirrors sema::TyK rather than inventing a parallel hierarchy, so a MIR type
// and the Checker's type for the same expression are recognisably the same thing.
enum class Ty : uint8_t {
    Void, Bool, Int, Float, Char, Str, Bytes, CStr,
    List, Dict, Set, Tuple, Chan, Range, Gen, Opt, Ptr, Ref,
    Struct, Enum, TraitObj, Fn, TypeVar, Any, Poison,
};

using Type = uint32_t;   // index into Func::types / Program::types

struct TypeEntry {
    Ty kind = Ty::Void;
    std::string name;          // nominal name, or the printed primitive
    std::vector<Type> args;    // element types / fn params
    Type inner = NO_VALUE;     // Opt/Ptr/Ref pointee, Fn return
};

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

enum class Op : uint8_t {
    // Terminators. Exactly one per block, always last.
    Ret,        // return operands.back() (empty => void)
    Br,         // unconditional to succ
    CondBr,     // cond ? then : else
    Unreachable,// e.g. the exit of an infinite loop with no break

    // Memory. Not SSA: these are the allocas mem2reg will clean up.
    Alloca,
    Load,
    Store,

    // Values.
    Const,      // an integer/float/char/str constant
    Unary,      // op index into Func::strs
    Binary,     // op index into Func::strs
    Cast,
    Index,      // list/tuple/dict/string subscript
    Field,      // struct field
    MakeList, MakeSet, MakeTuple, MakeDict, MakeStruct, MakeEnum,
    Call,       // callee is a Value, args are Values
    CallIndirect,
    Phi,        // incoming, one per predecessor, in PREDECESSOR ORDER
    EnumTag,    // discriminant of an enum value
    Aggregate,  // pack args into a tuple/struct
    Discriminant,
};

struct PhiIncoming {
    BlockId pred = NO_BLOCK;
    Value val = NO_VALUE;
};

struct Inst {
    Op op = Op::Unreachable;
    // The Value this instruction defines, or NO_VALUE for a terminator and for
    // void results. Stored explicitly rather than recovered from position,
    // because the dump has to name it and a positional index would be wrong the
    // moment an instruction is added or removed.
    Value result = NO_VALUE;
    Type type = NO_VALUE;      // result type; meaningless for terminators
    std::vector<Value> args;   // operands, index-addressed
    std::vector<Type> argTypes;
    std::string name;          // optional, for the dump
    ast::Span span{};

    // Phi only. Kept inline rather than in a side table so the invariant "a Phi
    // has one incoming per predecessor" is checkable without indirection.
    std::vector<PhiIncoming> incoming;

    // Op-specific immediates.
    std::string opName;        // Unary/Binary/Call name
    BlockId succ = NO_BLOCK;   // Br / Ret-ish successors
    BlockId elseSucc = NO_BLOCK;
    BlockId condBlock = NO_BLOCK;  // CondBr: the block holding the condition
    std::vector<BlockId> preds;     // Phi incoming blocks, parallel to `incoming`
};

// ---------------------------------------------------------------------------
// Blocks and functions
// ---------------------------------------------------------------------------

struct Block {
    BlockId id = NO_BLOCK;
    std::string label;         // for the dump; derived, never load-bearing
    std::vector<Inst> insts;
    // Cache of the terminator's successors, in successor order. Derived, and
    // rebuilt by rebuildSuccs(); never an independent source of truth.
    std::vector<BlockId> succs;
};

struct Func {
    std::string name;
    hir::DeclId decl = hir::NO_DECL;
    ast::Span span{};

    std::vector<Block> blocks;        // blocks[0] is the entry
    std::vector<Value> valueTypes;    // valueTypes[v] is v's type
    std::vector<ast::Span> valueSpans;
    std::vector<Type> localTypes;     // per Alloca
    std::vector<TypeEntry> types;     // the function's type table
    std::vector<std::string> strs;    // interned operator/member/field names
    std::vector<std::string> blockLabels;

    sema::FuncSig sig;
    std::vector<hir::DeclId> params;
    // Parameter Values, in order. A parameter is at once a Value (its entry
    // value, defined by the signature rather than by an instruction) and a
    // Binding: buildFunc gives every param an alloca and stores its entry
    // value once at the top of the entry block, the same shape a local VarDecl
    // takes, so `read` of a param produces a Load and mem2reg in phase 4
    // promotes it. Keep both views -- a direct AOT backend can use paramValues
    // without a memory round-trip, while the CFG stays alloca/load/store.
    std::vector<Value> paramValues;

    Type returnType = NO_VALUE;
    bool isVariadic = false;

    // -- construction helpers, used by the builder --
    BlockId newBlock();
    Block& block(BlockId b) { return blocks[b]; }
    Value newValue(Type t, ast::Span sp = {});
    Type internType(const TypeEntry& e);
    uint32_t internStr(const std::string& s);
    uint32_t addLocal(Type t);
    void rebuildSuccs();
    std::vector<BlockId> predsOf(BlockId b) const;
};

struct Program {
    std::vector<Func> funcs;
    std::vector<TypeEntry> types;     // global type table
    // DefId -> func index, for resolving a call to a declaration.
    std::vector<std::pair<hir::DeclId, uint32_t>> funcIndex;
};

// ---------------------------------------------------------------------------
// Building from HIR
// ---------------------------------------------------------------------------

struct BuildOptions {
    bool keepSpans = true;
};

Program build(const hir::Module& m, const hir::Context& ctx, const BuildOptions& opts = {});

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

struct Violation {
    ast::Span span{};
    std::string message;
};

// Structural verification. LLVM's scope, per LangRef: the violations "indicate
// bugs in transformation passes or input to the parser" -- this finds compiler
// bugs, it does not validate user programs.
//
// The checks, in the order LLVM's verifier runs them, because the order is load
// bearing: dominator computation is a PRECONDITION of the later checks, so
// "every block ends in a terminator" must come first or a malformed block makes
// the rest undefined rather than diagnosable.
std::vector<Violation> verify(const Program& p);

// Dump. Deterministic: no pointers, no hash iteration, ids as dense indices.
void dump(const Program& p, std::ostream& out);

}  // namespace kirn::mir
