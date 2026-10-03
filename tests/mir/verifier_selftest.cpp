// A direct test of kirn::mir::verify, compiled and run by gates_linux.sh.
//
// Same reasoning as tests/hir/verifier_selftest.cpp, and the same conclusion: no
// valid source can produce a MIR violation. sema::Checker rejects every
// source-level defect, HIR verifies, and the builder either produces a
// well-formed CFG or crashes. So the malformed CFG is built here, in memory.
//
// The CFG defects below are the ones a text dump cannot show you and that the
// corpus cannot contain, because the builder does not produce them. Each one is a
// real miscompile class from LLVM's list.
//
// This found a genuine hole the first time it ran, in the same spirit as the HIR
// self-test: a value allocated by the builder but never claimed by an
// instruction was invisible to every other check, because the operand index was
// still in range. Hence the "value is never defined" case below.

#include <cstdio>
#include <string>
#include <vector>

#include "mir/mir.h"

using namespace kirn;
using namespace kirn::mir;

namespace {

int failures = 0;

void expectRejected(const char* what, Program p) {
    auto v = verify(p);
    if (v.empty()) {
        std::printf("  FAIL  %s: verifier ACCEPTED a broken CFG\n", what);
        ++failures;
    } else {
        std::printf("  ok    %s: %s\n", what, v.front().message.c_str());
    }
}

void expectAccepted(const char* what, Program p) {
    auto v = verify(p);
    if (!v.empty()) {
        std::printf("  FAIL  %s: verifier rejected a well-formed CFG (%s)\n",
                    what, v.front().message.c_str());
        ++failures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

Inst term(Op op, BlockId succ = NO_BLOCK, BlockId els = NO_BLOCK) {
    Inst i;
    i.op = op;
    i.succ = succ;
    i.elseSucc = els;
    return i;
}

// A minimal well-formed function: entry -> condbr -> (a, b) -> join -> ret.
Func baseFunc() {
    Func f;
    f.name = "f";
    f.newBlock();  // 0 entry
    f.newBlock();  // 1 then
    f.newBlock();  // 2 else
    f.newBlock();  // 3 join
    f.blocks[0].insts.push_back(term(Op::CondBr, 1, 2));
    f.blocks[1].insts.push_back(term(Op::Br, 3));
    f.blocks[2].insts.push_back(term(Op::Br, 3));
    f.blocks[3].insts.push_back(term(Op::Ret));
    f.rebuildSuccs();
    return f;
}

Program oneFunc(Func f) {
    Program p;
    p.funcs.push_back(std::move(f));
    return p;
}

}  // namespace

int main() {
    // 1. Well-formed must be ACCEPTED, or a reject-everything verifier passes the
    //    rest of this file.
    expectAccepted("well-formed CFG is accepted", oneFunc(baseFunc()));

    // 2. A block with no terminator. This is PHASE A in the verifier for a
    //    reason: dominator construction is a precondition of the later checks, so
    //    without it a malformed block gives undefined behaviour rather than a
    //    diagnosis. LLVM checks it first for the same reason.
    {
        auto f = baseFunc();
        f.blocks[2].insts.clear();
        f.blocks[2].insts.push_back(term(Op::Store));
        expectRejected("block without a terminator", oneFunc(std::move(f)));
    }

    // 3. An EMPTY block. Malformed, not vacuously fine: LLVM's hasTerminator()
    //    is `!InstList.empty() && back().isTerminator()`.
    {
        auto f = baseFunc();
        f.blocks[2].insts.clear();
        expectRejected("empty block", oneFunc(std::move(f)));
    }

    // 4. A terminator in the MIDDLE of a block.
    {
        auto f = baseFunc();
        f.blocks[0].insts.push_back(term(Op::Ret));
        f.blocks[0].insts.push_back(term(Op::CondBr, 1, 2));
        expectRejected("terminator in the middle of a block", oneFunc(std::move(f)));
    }

    // 5. A branch to a block that does not exist. The classic off-by-one in a
    //    jump target: a text dump shows a number, not a violation.
    {
        auto f = baseFunc();
        f.blocks[1].insts[0].succ = 99;
        f.rebuildSuccs();
        expectRejected("branch to a nonexistent block", oneFunc(std::move(f)));
    }

    // 6. The entry block must not have a predecessor. A do-while that reused the
    //    current block as its loop head produced exactly this.
    {
        auto f = baseFunc();
        f.blocks[1].insts[0].succ = 0;
        f.rebuildSuccs();
        expectRejected("entry block has a predecessor", oneFunc(std::move(f)));
    }

    // 7. A phi whose incoming set does not match the predecessor set. This is the
    //    check that must be EQUALITY rather than containment: a missing entry, an
    //    extra entry, and a duplicated entry are three different miscompiles.
    {
        auto f = baseFunc();
        Value v = f.newValue(NO_VALUE);
        Inst phi;
        phi.op = Op::Phi;
        phi.result = v;
        phi.incoming = {{1, v}};  // only one of the two predecessors
        f.blocks[3].insts.insert(f.blocks[3].insts.begin(), phi);
        f.rebuildSuccs();
        expectRejected("phi missing a predecessor", oneFunc(std::move(f)));
    }

    // 8. A phi with an entry for a block that does not branch here.
    {
        auto f = baseFunc();
        Value v = f.newValue(NO_VALUE);
        Inst phi;
        phi.op = Op::Phi;
        phi.result = v;
        phi.incoming = {{1, v}, {2, v}, {0, v}};
        f.blocks[3].insts.insert(f.blocks[3].insts.begin(), phi);
        f.rebuildSuccs();
        expectRejected("phi with an extra incoming block", oneFunc(std::move(f)));
    }

    // 9. A phi NOT grouped at the top of its block. A phi after a side-effecting
    //    instruction reads an uninitialised value on the first entry.
    {
        auto f = baseFunc();
        Value v = f.newValue(NO_VALUE);
        Inst store;
        store.op = Op::Store;
        f.blocks[3].insts.push_back(store);
        Inst phi;
        phi.op = Op::Phi;
        phi.result = v;
        phi.incoming = {{1, v}, {2, v}};
        f.blocks[3].insts.push_back(phi);
        f.rebuildSuccs();
        expectRejected("phi not grouped at the top of the block", oneFunc(std::move(f)));
    }

    // 10. A value that is never defined. The builder allocated it, an operand
    //     referenced it, and no instruction ever claimed it -- so the operand
    //     index was in range and every other check passed. The density invariant
    //     is the only thing that sees this class.
    {
        auto f = baseFunc();
        f.newValue(NO_VALUE);   // allocated, never defined
        expectRejected("value allocated but never defined", oneFunc(std::move(f)));
    }

    // 11. A value defined twice. Values are single-assignment.
    {
        auto f = baseFunc();
        Value v = f.newValue(NO_VALUE);
        Inst a;
        a.op = Op::Const;
        a.result = v;
        Inst b;
        b.op = Op::Const;
        b.result = v;
        f.blocks[1].insts.insert(f.blocks[1].insts.begin(), a);
        f.blocks[2].insts.insert(f.blocks[2].insts.begin(), b);
        f.rebuildSuccs();
        expectRejected("value defined twice", oneFunc(std::move(f)));
    }

    // 12. An operand referring to a value outside the table.
    {
        auto f = baseFunc();
        Inst s;
        s.op = Op::Store;
        s.args = {4242};
        f.blocks[1].insts.insert(f.blocks[1].insts.begin(), s);
        f.rebuildSuccs();
        expectRejected("operand out of range", oneFunc(std::move(f)));
    }

    // 13. A local index that does not exist. Alloca/Load/Store/Phi carry it as an
    //     immediate, so nothing else would notice a bad one.
    {
        auto f = baseFunc();
        Inst s;
        s.op = Op::Store;
        s.opName = "7";   // the function has no locals
        f.blocks[1].insts.insert(f.blocks[1].insts.begin(), s);
        f.rebuildSuccs();
        expectRejected("local index out of range", oneFunc(std::move(f)));
    }

    // 14. A phi in a block with no predecessors, which is why LangRef says the
    //     entry block "cannot have any PHI nodes".
    {
        auto f = baseFunc();
        Value v = f.newValue(NO_VALUE);
        Inst phi;
        phi.op = Op::Phi;
        phi.result = v;
        f.blocks[2].insts.insert(f.blocks[2].insts.begin(), phi);
        f.blocks[2].insts[1] = term(Op::Br, 3);
        f.rebuildSuccs();
        // block 2 still has predecessor 0, so retarget it away first.
        f.blocks[0].insts[0].elseSucc = 3;
        f.blocks[1].insts[0].succ = 3;
        f.rebuildSuccs();
        expectRejected("phi in a block with no predecessors", oneFunc(std::move(f)));
    }

    if (failures) {
        std::printf("%d MIR verifier self-test case(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all MIR verifier self-test cases passed\n");
    return 0;
}
