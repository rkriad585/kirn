#pragma once
// Bytecode VM for Kirn (PLAN Phase 4) — core vertical slice.
//
// Design (per PLAN.md §4.3): the tree-walking interpreter stays the
// authoritative correctness model. This compiler lowers the AST to a linear
// instruction stream whose operands are the SAME `Value`/`Env` types the
// interpreter uses, and delegates complex/stateful operations (binops, method
// invocation, pattern matching, struct/enum construction, iteration, f-strings)
// to the existing Interpreter helpers — so semantics are identical by
// construction. The speed win comes from compact flat dispatch instead of
// recursive AST walks.
//
// A function is compiled as one VmFunction. The compiler marks a function as
// "interpreted" (fall back to the tree-walker's runFunc) whenever it meets a
// construct outside the core slice, guaranteeing the VM is always a correct
// superset path.
#include <cstdint>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "interp/value.h"

namespace kirn {
namespace vm {

using interp::Value;

enum Op : uint16_t {
    OP_NONE = 0,

    // constants / literals
    OP_INT,        // a = const index (int64)
    OP_FLOAT,      // a = const index (double)
    OP_CHAR,       // a = const index (char32_t)
    OP_STR,        // a = const index (normal decoded string)
    OP_STR_RAW,    // a = const index (raw string)
    OP_STR_BYTES,  // a = const index -> Value k=Bytes
    OP_STR_C,      // a = const index -> Value k=Ptr
    OP_TRUE,
    OP_FALSE,
    OP_NONEZ,
    OP_BOOL,       // pop v; push Value::boolean(truthy(v))
    OP_PUSH_CONST_STR,  // a = str-const index; push Value::str(strConsts[a])

    // iteration (For loops)
    OP_ITER_BEGIN,      // pop iterable; push iterator handle onto VM iter stack
    OP_ITER_NEXT,       // advance top iterator; push bool(hasNext); if hasNext push value
    OP_ITER_VALUE_VAR,  // a = name idx; pop value; bind into current env
    OP_ITER_VALUE_LOCAL, // a = local slot idx; pop value; write frame locals[slot]
    OP_ITER_END,        // pop iterator handle

    // variables (Env-based lexical scoping)
    OP_LOAD,       // a = name const index; push env->find(name)
    OP_LOAD_LOCAL, // a = local slot index; push frame locals[slot]
    OP_LOAD_FN,    // a = name const index; push VK::Fn for a named/user function
    OP_LOAD_MOD,   // a = name const index; push struct/enum marker module
    OP_STORE,      // a = name const index; assign top into env frame (bind local)
    OP_STORE_LOCAL,// a = local slot index; assign top into frame locals[slot]
    OP_ASSIGNTO,   // pop value; a = target expr kind (handled by assignTo)

    // containers
    OP_MAKE_LIST,  // pop b values -> list
    OP_MAKE_SET,   // pop b values -> set
    OP_MAKE_TUPLE, // pop b values -> tuple
    OP_MAKE_DICT,  // pop 2*b (k,v) -> dict
    OP_INDEX,      // pop obj,idx -> obj[idx]
    OP_SLICE,      // pop obj,lo,hi,step -> slice
    OP_SLICE3,     // pop obj,lo,hi,step (three optional) -> slice
    OP_MEMBER,     // pop obj; a=name idx; b=nilSafe -> member/field value
    OP_MEMBER_METHOD, // pop recv + b args; a=name idx; c=nilSafe -> method result

    // ops
    OP_UNARY,      // pop v; a = op-string idx (generic fallback)
    OP_UNARANGE,   // pop lo (int); push open-ended range lo..(hi=-1)
    OP_BINARY,     // pop r,l; a = op idx (generic fallback)

    // Specialized binary opcodes (numeric immediates, no string compare).
    // a = unused. Each does an inline int/float fast path, falling back to the
    // same tree-walker helper (binop/compareOne) used by the generic OP_BINARY.
    OP_BINARY_ADD, OP_BINARY_SUB, OP_BINARY_MUL, OP_BINARY_DIV, OP_BINARY_MOD,
    OP_BINARY_POW,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_RANGE,       // pop lo,hi -> range (a=1 for ..= inclusive)
    OP_NEG,         // pop v -> -v (int/float fast path)
    OP_NOT,         // pop v -> boolean(!truthy(v))

    OP_AND,        // pop l; if truthy(pop r) ... -> boolean truthy(and)
    OP_OR,
    OP_IS,         // pop r,l; a = type-name idx or -1(none)
    OP_IN,         // pop r,l
    OP_CMP,        // pop r,l; a = op idx
    OP_CMPCHAIN,   // pop r,l; a = op idx (chained comparison)

    // calls / construction
    OP_CALL,       // pop callee + a args -> result
    OP_CALL_NAME,  // a = name idx (builtin/named); pops a args -> result
    OP_MAKE_STRUCT,// pop a named-field pairs -> struct value
    OP_MAKE_HEAP,  // pop a named-field pairs -> new heap
    OP_NEW,        // pop a args; a2 = type-name idx -> chan or heap new
    OP_CAST,       // pop v; a = type idx
    OP_TRY,        // pop v; propagate result err / push payload
    OP_FUNC,       // a = function const idx -> push VK::Fn (by Stmt ptr)

    // iterable iteration handle life-cycle

    // flow / scope
    OP_JUMP,           // a = signed absolute target
    OP_JUMP_IF_FALSE,  // pop cond
    OP_JUMP_IF_TRUE,   // pop cond
    OP_POP,
    OP_SCOPE_ENTER,    // push child Env
    OP_SCOPE_LEAVE,    // pop frame (trivial in Env model: no-op, kept for parity)
    OP_RETURN,         // pop one and return
    OP_RETURN_NONE,
    OP_EXPR,           // evaluate and discard top
    OP_IF,             // placeholder marker (compiler emits JUMP-based code)
    OP_BREAK,          // a = label idx or -1
    OP_CONTINUE,       // a = label idx or -1

    OP_HALT,
};

struct Ins {
    uint16_t op = OP_HALT;
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
};

struct VmFunction {
    std::string name;
    std::vector<Ins> code;
    std::vector<Value> constants;
    std::vector<std::string> strConsts;   // string pool (names/ops/types)
    bool interpreted = false;             // fall back to tree-walker runFunc
    int32_t nParams = 0;
    int32_t varIdx = -1;                  // variadic param index or -1
    std::vector<std::string> paramNames;
    bool isResult = false;                // def returns result[T,E]
    // Slot-based locals (frame storage). Empty => use env lookup path.
    // slotNames[i] is the local bound to frame slot i; useSlots true means the
    // body was compiled with OP_LOAD_LOCAL/OP_STORE_LOCAL for these names.
    std::vector<std::string> slotNames;
    bool useSlots = false;
};

struct VmProgram {
    std::vector<VmFunction> funcs;
    const ast::Stmt* mainFn = nullptr;
};

} // namespace vm
} // namespace kirn
