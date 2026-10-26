#pragma once

#include "mir/mir.h"

namespace kirn::mir {

// Strict MIR borrow checking.  This deliberately runs after HIR and MIR
// verification, so it checks the lowered ownership boundary rather than
// duplicating the parser/checker's AST escape heuristics.
std::vector<Violation> borrowCheck(const Program& p);

}  // namespace kirn::mir
