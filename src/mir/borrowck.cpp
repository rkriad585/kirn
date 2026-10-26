#include "mir/borrowck.h"

#include <charconv>
#include <cstdint>
#include <string>
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
    std::vector<bool> valueIsBorrow;
    std::vector<bool> localIsBorrow;

    explicit FunctionBorrowState(const Func& f)
        : valueIsBorrow(f.valueTypes.size(), false),
          localIsBorrow(f.localTypes.size(), false) {}

    bool isBorrow(Value v) const {
        return v != NO_VALUE && v < valueIsBorrow.size() && valueIsBorrow[v];
    }
};

struct Checker {
    const Program& program;
    std::vector<Violation> violations;

    void bad(const Inst& in, std::string message) {
        violations.push_back(Violation{in.span, std::move(message)});
    }

    void run() {
        for (const Func& f : program.funcs) runFunction(f);
    }

    void runFunction(const Func& f) {
        FunctionBorrowState state(f);
        bool functionTakesAddress = false;
        for (const Block& block : f.blocks) {
            for (const Inst& in : block.insts) {
                const bool resultBorrow = [&] {
                    if (in.op == Op::Unary && in.opName == "&") return true;
                    if (in.op == Op::Load) {
                        uint32_t local = 0;
                        return localIndex(in, local) &&
                               local < state.localIsBorrow.size() &&
                               state.localIsBorrow[local];
                    }
                    return false;
                }();
                if (in.result != NO_VALUE && in.result < state.valueIsBorrow.size())
                    state.valueIsBorrow[in.result] = resultBorrow;
                functionTakesAddress = functionTakesAddress || resultBorrow;

                if (in.op == Op::Store && !in.args.empty()) {
                    uint32_t local = 0;
                    const bool isLocalStore = localIndex(in, local);
                    if (isLocalStore && local < state.localIsBorrow.size())
                        state.localIsBorrow[local] = state.isBorrow(in.args[0]);
                    if (!isLocalStore && state.isBorrow(in.args[0]))
                        bad(in, "borrow stored in a local; the referent may outlive its scope");
                }

                if (in.op == Op::Ret) {
                    for (Value v : in.args) {
                        if (state.isBorrow(v)) {
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
                        if (state.isBorrow(v) || (functionTakesAddress && isPointer)) {
                            bad(in, "borrow passed to a call; strict mode requires an owned value");
                            break;
                        }
                    }
                }
            }
        }
    }
};

}  // namespace

std::vector<Violation> borrowCheck(const Program& p) {
    Checker checker{p, {}};
    checker.run();
    return checker.violations;
}

}  // namespace kirn::mir
