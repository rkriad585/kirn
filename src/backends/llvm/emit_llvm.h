#pragma once
// Phase 4 LLVM backend — BUILDER-PLAN §4.7, opt-in via KIRN_WITH_LLVM.
//
// Lower a verified MIR Program to an LLVM module for the scalar subset: i64 /
// double / bool values (the `isScalarTy` set the C++ backend uses,
// src/backend/native.cpp:17), while/if/match/ternary control flow, user
// functions, and the `print` builtin. Everything outside the subset is rejected
// with a diagnostic rather than silently miscompiled. MIR is alloca/load/store
// shaped by design (mir.h §Values and memory), so the module runs mem2reg to
// produce SSA — the front end never builds SSA by hand.
//
// The emitted module is assembly text ready for `opt -passes=verify` and `lli`,
// plus optionally a .bc file.

#include <cstdint>
#include <string>
#include <vector>

namespace kirn {
namespace mir {
struct Program;
}  // namespace mir

namespace backends {
namespace llvm {

struct EmitOptions {
    bool promoteAllocas = true;   // run mem2reg (PromotePass) over the module
    bool emitBitcode = false;     // also serialize the module as bitcode
};

// One reason an emit was rejected. line/col are the source position of the
// offending instruction (MIR instruction spans carry them).
struct EmitDiag {
    uint32_t line = 0;
    uint32_t col = 0;
    std::string message;
};

// Lower `prog` (assumed to have passed mir::verify) to an LLVM module.
//
// Returns false and fills `diags` when the program uses something outside the
// scalar subset. On success the module is verified and serialized: `outLl`
// receives the assembly text, and `outBc` (when opts.emitBitcode) the bitcode.
bool emitModule(const mir::Program& prog, const EmitOptions& opts,
                std::vector<EmitDiag>& diags, std::string& outLl,
                std::string* outBc = nullptr);

}  // namespace llvm
}  // namespace backends
}  // namespace kirn