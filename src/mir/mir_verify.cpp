// MIR structural verification and dumping.
//
// The checks are LLVM's, in LLVM's order, and the order is load-bearing rather
// than cosmetic. LLVM's Verifier::verify does this first:
//
//   // First check that every basic block has a terminator, otherwise we can't
//   // even inspect the CFG.
//   for (const BasicBlock &BB : F) { ... "Basic Block ... does not terminate!" }
//   DT.recalculate(F);
//
// Dominator construction is a PRECONDITION of every later check. Computed lazily
// instead, a malformed block gives undefined behaviour or a hang rather than a
// diagnosis. So "every block ends in a terminator" is phase A here, and only
// then does phase B inspect edges.
//
// What is deliberately NOT checked, from the survey:
//
//   - "Every instruction has at least one use." Not an LLVM invariant -- the
//     verifier has no such check, and dead instructions are legal IR. DCE is an
//     optimisation pass, not a well-formedness rule. Checking it here would be
//     inventing a rule, and it would also be wrong for stores and void calls,
//     which are never used by construction.
//   - Dominance inside unreachable blocks. LLVM relaxes self-reference checks in
//     unreachable code; skipping dominance there entirely is the same choice.
//
// Phi handling is where hand-rolled verifiers go wrong, so it follows LangRef
// exactly: "for the purposes of the SSA form, the use of each incoming value is
// deemed to occur on the edge from the corresponding predecessor block". A phi's
// incoming list is therefore compared against the predecessor SET for EQUALITY:
// a missing entry, an extra entry, and two entries for one predecessor are three
// different miscompiles, and only set equality catches all three.

#include "mir/mir.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace kirn::mir {
namespace {

const char* opName(Op o) {
    switch (o) {
        case Op::Ret: return "ret";
        case Op::Br: return "br";
        case Op::CondBr: return "condbr";
        case Op::Unreachable: return "unreachable";
        case Op::Alloca: return "alloca";
        case Op::Load: return "load";
        case Op::Store: return "store";
        case Op::Const: return "const";
        case Op::Unary: return "unary";
        case Op::Binary: return "binary";
        case Op::Cast: return "cast";
        case Op::Index: return "index";
        case Op::Field: return "field";
        case Op::MakeList: return "makelist";
        case Op::MakeSet: return "makeset";
        case Op::MakeTuple: return "maketuple";
        case Op::MakeDict: return "makedict";
        case Op::MakeStruct: return "makestruct";
        case Op::MakeEnum: return "makeenum";
        case Op::Call: return "call";
        case Op::CallIndirect: return "call_indirect";
        case Op::Phi: return "phi";
        case Op::EnumTag: return "enumtag";
        case Op::Aggregate: return "aggregate";
        case Op::Discriminant: return "discriminant";
    }
    return "?";
}

bool isTerm(Op o) {
    return o == Op::Ret || o == Op::Br || o == Op::CondBr || o == Op::Unreachable;
}

std::string tyName(const Func& f, Type t) {
    if (t == NO_VALUE || t >= f.types.size()) return "?";
    const TypeEntry& e = f.types[t];
    std::string base = e.name;
    switch (e.kind) {
        case Ty::Void: base = "void"; break;
        case Ty::Bool: base = "bool"; break;
        case Ty::Int: if (base.empty()) base = "int"; break;
        case Ty::Float: if (base.empty()) base = "float"; break;
        case Ty::Char: if (base.empty()) base = "char"; break;
        case Ty::Str: base = "str"; break;
        case Ty::Bytes: base = "bytes"; break;
        case Ty::CStr: base = "cstr"; break;
        case Ty::List: base = "list"; break;
        case Ty::Dict: base = "dict"; break;
        case Ty::Set: base = "set"; break;
        case Ty::Tuple: base = "tuple"; break;
        case Ty::Chan: base = "chan"; break;
        case Ty::Range: base = "range"; break;
        case Ty::Gen: base = "gen"; break;
        case Ty::Opt: base = "opt"; break;
        case Ty::Ptr: base = "ptr"; break;
        case Ty::Ref: base = "ref"; break;
        case Ty::Fn: base = "fn"; break;
        case Ty::Any: base = "any"; break;
        case Ty::Poison: base = "poison"; break;
        default: break;
    }
    if (!e.args.empty()) {
        base += "<";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i) base += ", ";
            base += tyName(f, e.args[i]);
        }
        base += ">";
    }
    if (e.inner != NO_VALUE) base += "(" + tyName(f, e.inner) + ")";
    return base;
}

struct Verifier {
    const Program& p;
    std::vector<Violation> out;
    explicit Verifier(const Program& prog) : p(prog) {}

    void bad(const ast::Span& s, std::string m) { out.push_back(Violation{s, std::move(m)}); }

    void run() {
        for (const auto& f : p.funcs) runFunc(f);
    }

    void runFunc(const Func& f) {
        const std::string where = "function '" + f.name + "'";
        if (f.blocks.empty()) {
            bad(f.span, where + " has no blocks");
            return;
        }
        // Block ids are the address space for every edge in this IR, so a hole
        // makes every reference ambiguous. Checked before anything else uses id().
        for (size_t i = 0; i < f.blocks.size(); ++i) {
            if (f.blocks[i].id != i) {
                bad(f.span, where + ": block " + std::to_string(i) + " has id " +
                                std::to_string(f.blocks[i].id) + "; ids must be dense");
                return;
            }
        }

        // ---- PHASE A: terminators, before anything inspects the CFG -------
        for (const auto& b : f.blocks) {
            if (b.insts.empty()) {
                bad(f.span, where + ": block " + std::to_string(b.id) +
                                " is EMPTY. An empty block is malformed, not vacuously "
                                "well formed -- LLVM's hasTerminator() requires !empty().");
                continue;
            }
            for (size_t i = 0; i + 1 < b.insts.size(); ++i) {
                if (isTerm(b.insts[i].op)) {
                    bad(b.insts[i].span,
                        where + ": block " + std::to_string(b.id) + ": terminator '" +
                            opName(b.insts[i].op) + "' found in the middle of a block");
                }
            }
            if (!isTerm(b.insts.back().op)) {
                bad(f.span, where + ": block " + std::to_string(b.id) +
                                " does not end with a terminator (last is '" +
                                opName(b.insts.back().op) + "')");
            }
        }
        if (!out.empty()) return;   // phase B needs a well-formed CFG

        // ---- PHASE B: operands, edges, phis ------------------------------
        // "The entry block to a function must not have predecessors!"
        for (BlockId pred : f.predsOf(0))
            bad(f.span, where + ": entry block has a predecessor (block " +
                            std::to_string(pred) + ")");

        // Every value must be defined EXACTLY ONCE. This is rustc's density
        // invariant applied to the value table, and it is the check that catches a
        // builder which allocates a value and forgets to claim it: the operand is
        // then in range, so every other check passes, and the definition simply
        // does not exist. That happened -- `read()` created a Value for its Load
        // and never set Inst::result -- and the CFG looked entirely plausible.
        std::vector<int> defined(f.valueTypes.size(), 0);
        // Parameter values are defined by the function's signature, not by an
        // instruction, so they are pre-counted as defined.
        for (Value pv : f.paramValues)
            if (pv < defined.size()) defined[pv]++;

        for (const auto& b : f.blocks) {
            for (size_t i = 0; i < b.insts.size(); ++i) {
                const Inst& in = b.insts[i];

                for (size_t k = 0; k < in.args.size(); ++k) {
                    Value v = in.args[k];
                    if (v == NO_VALUE || v >= f.valueTypes.size()) {
                        bad(in.span, where + ": block " + std::to_string(b.id) + " '" +
                                         opName(in.op) + "' operand " + std::to_string(k) +
                                         " is value " + std::to_string(v) +
                                         ", out of range (have " +
                                         std::to_string(f.valueTypes.size()) + ")");
                        continue;
                    }
                    Type want = k < in.argTypes.size() ? in.argTypes[k] : NO_VALUE;
                    if (want != NO_VALUE && want < f.types.size() &&
                        want != f.valueTypes[v]) {
                        bad(in.span, where + ": block " + std::to_string(b.id) + " '" +
                                         opName(in.op) + "' operand " + std::to_string(k) +
                                         " is used as " + tyName(f, want) +
                                         " but was defined as " +
                                         tyName(f, f.valueTypes[v]));
                    }
                }
                if (in.result != NO_VALUE) {
                    if (in.result >= f.valueTypes.size()) {
                        bad(in.span, where + ": block " + std::to_string(b.id) + " '" +
                                         opName(in.op) + "' defines value " +
                                         std::to_string(in.result) + ", out of range");
                    } else {
                        defined[in.result]++;
                    }
                }

                // Alloca/Load/Store/Phi carry a local index as an immediate.
                if ((in.op == Op::Alloca || in.op == Op::Load || in.op == Op::Store ||
                     in.op == Op::Phi) &&
                    !in.opName.empty()) {
                    long idx = std::strtol(in.opName.c_str(), nullptr, 10);
                    if (idx < 0 || (size_t)idx >= f.localTypes.size()) {
                        bad(in.span, where + ": '" + opName(in.op) + "' names local " +
                                         in.opName + " but the function has " +
                                         std::to_string(f.localTypes.size()));
                    }
                }

                if (in.op == Op::Phi) {
                    // "PHI nodes must be the first thing in a basic block, all
                    // grouped together."
                    for (size_t j = 0; j < i; ++j) {
                        if (b.insts[j].op != Op::Phi) {
                            bad(in.span, where + ": block " + std::to_string(b.id) +
                                             ": phi is not grouped at the top of the block");
                            break;
                        }
                    }
                    std::vector<BlockId> preds = f.predsOf(b.id);
                    std::vector<BlockId> got;
                    got.reserve(in.incoming.size());
                    for (const auto& inc : in.incoming) got.push_back(inc.pred);

                    std::set<BlockId> want(preds.begin(), preds.end());
                    std::set<BlockId> have(got.begin(), got.end());
                    if (want != have) {
                        std::ostringstream m;
                        m << where << ": block " << b.id << ": phi incoming {";
                        for (size_t k = 0; k < got.size(); ++k) m << (k ? ", " : "") << got[k];
                        m << "} does not match predecessors {";
                        for (size_t k = 0; k < preds.size(); ++k) m << (k ? ", " : "") << preds[k];
                        m << "}";
                        bad(in.span, m.str());
                    }
                    if (got.size() != preds.size()) {
                        bad(in.span, where + ": block " + std::to_string(b.id) +
                                         ": phi has " + std::to_string(got.size()) +
                                         " entries for " + std::to_string(preds.size()) +
                                         " predecessors");
                    }
                    // Duplicates are legal only when the values agree.
                    for (size_t k = 0; k + 1 < in.incoming.size(); ++k) {
                        if (in.incoming[k].pred == in.incoming[k + 1].pred &&
                            in.incoming[k].val != in.incoming[k + 1].val) {
                            bad(in.span, where + ": block " + std::to_string(b.id) +
                                             ": phi has two entries for block " +
                                             std::to_string(in.incoming[k].pred) +
                                             " with different values");
                        }
                    }
                    // LangRef: because the entry block can have no predecessors,
                    // "it also cannot have any PHI nodes".
                    if (preds.empty())
                        bad(in.span, where + ": block " + std::to_string(b.id) +
                                         " has a phi but no predecessors");
                }

                if (in.op == Op::Br || in.op == Op::CondBr) {
                    auto check = [&](BlockId t, const char* which) {
                        if (t == NO_BLOCK || t >= f.blocks.size())
                            bad(in.span, where + ": block " + std::to_string(b.id) +
                                             ": " + which + " target " + std::to_string(t) +
                                             " is not a block");
                    };
                    check(in.succ, "successor");
                    if (in.op == Op::CondBr) check(in.elseSucc, "else");
                }
            }
        }

        for (size_t v = 0; v < defined.size(); ++v) {
            if (defined[v] == 0)
                bad(f.span, where + ": value " + std::to_string(v) +
                                " is never defined by any instruction (and is not a "
                                "parameter). Values must be densely defined.");
            else if (defined[v] > 1)
                bad(f.span, where + ": value " + std::to_string(v) + " is defined " +
                                std::to_string(defined[v]) +
                                " times; a value is single-assignment");
        }
    }
};

}  // namespace

std::vector<Violation> verify(const Program& p) {
    Verifier v(p);
    v.run();
    return std::move(v.out);
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

void dump(const Program& p, std::ostream& out) {
    out << "mir 1\n";
    out << "functions " << p.funcs.size() << "\n";
    for (const auto& f : p.funcs) {
        out << "func " << f.name << " #" << f.decl
            << " blocks=" << f.blocks.size()
            << " values=" << f.valueTypes.size()
            << " locals=" << f.localTypes.size();
        if (!f.params.empty()) {
            out << " params=";
            for (size_t i = 0; i < f.params.size(); ++i) {
                if (i) out << ' ';
                out << '#' << f.params[i];
                if (i < f.paramValues.size()) out << "=%" << f.paramValues[i];
            }
        }
        out << " ret=" << tyName(f, f.returnType) << "\n";
        for (size_t i = 0; i < f.localTypes.size(); ++i)
            out << "  local " << i << " : " << tyName(f, f.localTypes[i]) << "\n";
        for (const auto& b : f.blocks) {
            // Predecessor count is printed because "block 4 has 2 preds" is the
            // first thing you want when a phi looks wrong.
            out << "  " << b.label << ":" << b.id
                << "  ; preds=" << f.predsOf(b.id).size()
                << " succs=" << b.succs.size() << "\n";
            for (const auto& in : b.insts) {
                out << "    ";
                if (in.result != NO_VALUE) out << "%" << in.result << " = ";
                out << opName(in.op);
                if (in.type != NO_VALUE) out << " : " << tyName(f, in.type);
                if (in.op == Op::Phi) {
                    out << " [";
                    for (size_t k = 0; k < in.incoming.size(); ++k) {
                        if (k) out << ", ";
                        out << "%" << in.incoming[k].val
                            << " from b" << in.incoming[k].pred;
                    }
                    out << "]";
                } else {
                    for (Value a : in.args) out << " %" << a;
                }
                if (!in.opName.empty() && in.op != Op::Phi) out << "   ; " << in.opName;
                if (in.op == Op::Br) out << "  ; -> b" << in.succ;
                if (in.op == Op::CondBr) out << "  ; -> b" << in.succ
                                             << " / b" << in.elseSucc;
                out << "\n";
            }
        }
    }
    out << "end\n";
}

}  // namespace kirn::mir
