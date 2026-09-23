#pragma once
// AST -> bytecode compiler (core slice). See bytecode.h for the design.
#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "vm/bytecode.h"

namespace kirn {
namespace vm {

// Compile result: the program's functions plus, for each compiled `def`
// (by Stmt*), its function index, and for each compiled lambda (by Expr*)
// its function index. These let the VM map a VK::Fn value back to bytecode.
struct CompileResult {
    VmProgram prog;
    std::map<const ast::Stmt*, int32_t> defIdx;
    std::map<const ast::Expr*, int32_t> lamIdx;
};

// Compile the whole program (top-level statements + all reachable defs).
// `entry` is the main FuncDef Stmt. Every compiled def/lambda is registered in
// defIdx/lamIdx. Constructs outside the core slice mark the enclosing function
// `interpreted` (caller falls back to the tree-walker).
//
// `userFuncs`: names of every user-defined `def` (used to recognise safe calls;
//               a call to any other name is treated as struct/enum construction
//               or a module/unknown and marks the function interpreted).
// `builtins`:  names prebound by the interpreter (safe direct calls).
// `modules`:   bundled top-level module names (math, time, ...) treated as
//               non-core receivers.
CompileResult compileProgram(const std::vector<ast::StmtP>& body,
                             const ast::Stmt* entry,
                             const std::unordered_set<std::string>& userFuncs,
                             const std::unordered_set<std::string>& builtins,
                             const std::unordered_set<std::string>& modules);

} // namespace vm
} // namespace kirn
