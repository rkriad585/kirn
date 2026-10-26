#include "mir/borrowck.h"

#include <charconv>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace kirn::mir {
namespace {

bool localIndex(const Inst& in, uint32_t& out) {
    if (in.opName.empty()) return false;
    const char* first = in.opName.data();
    const char* last = first + in.opName.size();
    auto r = std::from_chars(first, last, out);
    return r.ec == std::errc{} && r.ptr == last;
}

struct FunctionBorrowState {
    struct Borrow {
        bool active = false;
        uint32_t sourceLocal = UINT32_MAX;
    };

    std::vector<Borrow> valueBorrows;
    std::vector<Borrow> localBorrows;
    std::vector<bool> localMoved;

    explicit FunctionBorrowState(const Func& f)
        : valueBorrows(f.valueTypes.size()),
          localBorrows(f.localTypes.size()),
          localMoved(f.localTypes.size(), false) {}

    Borrow borrow(Value v) const {
        return v != NO_VALUE && v < valueBorrows.size() ? valueBorrows[v] : Borrow{};
    }
};

struct Checker {
    const Program& program;
    std::vector<Violation> violations;
    std::unordered_set<std::string> reported;

    void bad(const Inst& in, std::string message) {
        // CFG joins can revisit an instruction while the dataflow fixpoint is
        // converging. Keep diagnostics stable and report each source failure
        // once, like rustc's primary-span diagnostics.
        std::string key = std::to_string(in.span.line) + ":" +
                          std::to_string(in.span.col) + ":" + message;
        if (reported.insert(key).second)
            violations.push_back(Violation{in.span, std::move(message)});
    }

    void run() {
        for (const Func& f : program.funcs) runFunction(f);
    }

    void runFunction(const Func& f) {
        using State = FunctionBorrowState;
        const size_t n = f.blocks.size();
        std::vector<std::optional<State>> entry;
        entry.reserve(n);
        for (size_t i = 0; i < n; ++i) entry.emplace_back();
        std::deque<BlockId> work;
        entry[0].emplace(f);
        work.push_back(0);

        auto merge = [](State& into, const State& from) {
            bool changed = false;
            for (size_t i = 0; i < into.localMoved.size(); ++i) {
                const bool moved = into.localMoved[i] || from.localMoved[i];
                changed = changed || moved != into.localMoved[i];
                into.localMoved[i] = moved;
            }
            for (size_t i = 0; i < into.localBorrows.size(); ++i) {
                const auto old = into.localBorrows[i];
                auto joined = old;
                if (from.localBorrows[i].active) {
                    joined.active = true;
                    if (!old.active || old.sourceLocal != from.localBorrows[i].sourceLocal)
                        joined.sourceLocal = UINT32_MAX;
                }
                changed = changed || joined.active != old.active ||
                          joined.sourceLocal != old.sourceLocal;
                into.localBorrows[i] = joined;
            }
            for (size_t i = 0; i < into.valueBorrows.size(); ++i) {
                const auto old = into.valueBorrows[i];
                auto joined = old;
                if (from.valueBorrows[i].active) {
                    joined.active = true;
                    if (!old.active || old.sourceLocal != from.valueBorrows[i].sourceLocal)
                        joined.sourceLocal = UINT32_MAX;
                }
                changed = changed || joined.active != old.active ||
                          joined.sourceLocal != old.sourceLocal;
                into.valueBorrows[i] = joined;
            }
            return changed;
        };

        while (!work.empty()) {
            const BlockId id = work.front();
            work.pop_front();
            if (id >= f.blocks.size() || !entry[id]) continue;
            State state = *entry[id];
            const Block& block = f.blocks[id];
            for (const Inst& in : block.insts) {
                const State::Borrow resultBorrow = [&] {
                    if (in.op == Op::Unary && in.opName == "&") {
                        uint32_t source = UINT32_MAX;
                        if (!in.args.empty()) {
                            uint32_t local = 0;
                            // The operand of `&x` is a Load carrying the local
                            // identity. The source is recovered from that load,
                            // which keeps regions attached to places, not names.
                            for (const Block& scan : f.blocks)
                                for (const Inst& candidate : scan.insts)
                                    if (candidate.result == in.args[0] &&
                                        candidate.op == Op::Load &&
                                        localIndex(candidate, local))
                                        source = local;
                        }
                        return State::Borrow{true, source};
                    }
                    if (in.op == Op::Load) {
                        uint32_t local = 0;
                        return localIndex(in, local) &&
                               local < state.localBorrows.size()
                                   ? state.localBorrows[local]
                                   : State::Borrow{};
                    }
                    return State::Borrow{};
                }();
                if (in.op == Op::Move) {
                    uint32_t local = 0;
                    if (localIndex(in, local) && local < state.localMoved.size()) {
                        state.localMoved[local] = true;
                        state.localBorrows[local] = {};
                    }
                }
                if (in.op == Op::Load) {
                    uint32_t local = 0;
                    if (localIndex(in, local) && local < state.localMoved.size() &&
                        state.localMoved[local])
                        bad(in, "use of moved local after del");
                }
                if (in.result != NO_VALUE && in.result < state.valueBorrows.size())
                    state.valueBorrows[in.result] = resultBorrow;

                if (in.op == Op::Store && !in.args.empty()) {
                    uint32_t local = 0;
                    const bool isLocalStore = localIndex(in, local);
                    if (isLocalStore && local < state.localBorrows.size())
                        state.localBorrows[local] = state.borrow(in.args[0]);
                    if (isLocalStore && local < state.localMoved.size())
                        state.localMoved[local] = false;
                    if (!isLocalStore && state.borrow(in.args[0]).active)
                        bad(in, "borrow stored in a local; the referent may outlive its scope");
                }

                if (in.op == Op::Ret) {
                    for (Value v : in.args) {
                        if (state.borrow(v).active) {
                            bad(in, "borrow escapes through return");
                            break;
                        }
                    }
                }

                if (in.op == Op::Call || in.op == Op::CallIndirect) {
                    for (Value v : in.args) {
                        const bool isPointer =
                            v < f.valueTypes.size() &&
                            f.valueTypes[v] < f.types.size() &&
                            (f.types[f.valueTypes[v]].kind == Ty::Ptr ||
                             f.types[f.valueTypes[v]].kind == Ty::Ref);
                        if (state.borrow(v).active || isPointer) {
                            bad(in, "borrow passed to a call; strict mode requires an owned value");
                            break;
                        }
                    }
                }
            }

            // Propagate the inferred region state across the CFG. A join is a
            // may-analysis: if a borrow or move exists on any incoming path it
            // remains live, preventing an unsound path-sensitive acceptance.
            for (BlockId succ : block.succs) {
                if (succ >= n) continue;
                if (!entry[succ]) {
                    entry[succ] = state;
                    work.push_back(succ);
                } else if (merge(*entry[succ], state)) {
                    work.push_back(succ);
                }
            }
        }
    }
};

}  // namespace

std::vector<Violation> borrowCheck(const Program& p) {
    Checker checker{p, {}, {}};
    checker.run();
    return checker.violations;
}

}  // namespace kirn::mir
