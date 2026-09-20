#pragma once
// Conservative escape analysis for local borrows (PLAN 5.5).
// Flags `&local` expressions that are returned, stored, or passed to a
// function call — early warning for borrows that will escape the owner's
// scope once NLL borrow semantics land.
//
// This is a free-standing pass over the AST (no type info needed beyond the
// local-declaration name set).  Called by Checker::checkModule after the
// type-checking pass so the corpus remains green.

#include "ast/ast.h"
#include "support/diag.h"
#include <set>
#include <string>

namespace kirn {
namespace sema {

class BorrowChecker {
public:
    explicit BorrowChecker(DiagEngine& diags) : diags_(diags) {}
    void checkModule(const std::vector<ast::StmtP>& prog);

private:
    DiagEngine& diags_;
};

} // namespace sema
} // namespace kirn
