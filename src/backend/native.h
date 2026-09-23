#pragma once
// Native code generation for Kirn (PLAN Phase 8.2).
//
// The bytecode VM (Phase 4) and the tree-walking interpreter both evaluate a
// dynamically-boxed `Value` at runtime. A real "faster like C/C++" path lowers
// statically-typed *scalar* user functions to plain C++ that the host
// toolchain compiles into the produced binary (no interpreter involvement for
// those functions).
//
// This module analyzes a checked program and emits C++ source for every user
// function it can prove lowerable. A function is lowerable iff it is a
// top-level, non-generic, non-result `FuncDef` whose parameters and return type
// are scalar (int/float/bool/char/none) and whose body stays within the scalar
// subset (scalar locals, arithmetic/comparison/logical ops, if/elif/else,
// while, int-range for, return, calls to other lowerable functions). Everything
// else is left to the interpreter/VM — the runtime preference is native -> VM ->
// tree-walker, so non-lowerable code keeps exact existing behaviour.
//
// The emitted C++ is appended to the standalone launcher in `kirn build
// --native` and compiled by the resolved toolchain. Functions read their scalar
// parameters from the received `interp::Env` and return a scalar `interp::Value`.
#include "ast/ast.h"
#include "sema/checker.h"
#include "sema/type.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace kirn {
namespace backend {

struct NativeFunc {
    const ast::Stmt* fn = nullptr;   // the lowered FuncDef
    std::string name;                // Kirn function name
    std::string cName;               // emitted C++ function name
    std::vector<std::string> params; // scalar parameter names, in order
    std::vector<std::string> pTypes; // cpp types of params ("int64_t"/"double"/...)
    std::string retCpp;              // "int64_t"/"double"/"bool"/"char32_t"/"" (none)
};

struct NativeProgram {
    std::vector<NativeFunc> funcs;   // functions lowered to native C++
    bool any = false;                // at least one function lowered
};

// Analyze `prog` (already type-checked via `chk`) and emit the C++ bodies for
// all lowerable scalar functions into `out`, plus a `kirn_native::registerAll`
// helper that wires them into an Interpreter. Returns which functions were
// lowered. Emits nothing (and returns .any=false) when nothing is lowerable.
NativeProgram emitNative(std::ostream& out,
                         const std::vector<ast::StmtP>& prog,
                         const sema::Checker& chk);

// True if `t` is a statically-scalar type we can lower (int/float/bool/char).
bool isScalarTy(const sema::TyP& t);

} // namespace backend
} // namespace kirn
