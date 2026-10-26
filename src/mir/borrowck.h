#pragma once

#include "mir/mir.h"

namespace kirn::mir {

// Strict MIR borrow checking. This deliberately runs after HIR and MIR
// verification, so it checks the lowered ownership boundary rather than
// duplicating the parser/checker's AST escape heuristics. The implementation
// is a CFG dataflow pass: local moves and borrow regions are joined at every
// predecessor, then checked at uses, calls, stores, and returns.
std::vector<Violation> borrowCheck(const Program& p);

}  // namespace kirn::mir
