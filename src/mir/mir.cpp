// MIR construction: HIR -> per-function CFG.
//
// The central design choice is that MIR is built from STRUCTURED HIR, not from a
// flat stream. The bytecode compiler works forwards through a linear instruction
// array and backfills jump offsets; recovering a CFG from that afterwards is
// possible (the leaders algorithm) but every property then has to be
// re-established by inference. Building from a tree means the block boundaries
// are known by construction, merge points are explicit, and the only thing a phi
// needs is the list of incoming blocks, which the builder already has in hand.
//
// The one genuinely hard part is a variable assigned in two arms of a branch and
// read after the join. The tree does not say which storage a name refers to
// across a join, so each declaration gets a set of (block, value) definitions
// and a join materialises a phi. See `merge` in the Builder.

#include "mir/mir.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace kirn::mir {

// ---------------------------------------------------------------------------
// Func helpers
// ---------------------------------------------------------------------------

BlockId Func::newBlock() {
    BlockId id = static_cast<BlockId>(blocks.size());
    Block b;
    b.id = id;
    b.label = "b" + std::to_string(id);
    blocks.push_back(std::move(b));
    return id;
}

Value Func::newValue(Type t, ast::Span sp) {
    Value v = static_cast<Value>(valueTypes.size());
    valueTypes.push_back(t);
    valueSpans.push_back(sp);
    return v;
}

Type Func::internType(const TypeEntry& e) {
    for (size_t i = 0; i < types.size(); ++i) {
        const TypeEntry& t = types[i];
        if (t.kind != e.kind || t.name != e.name || t.inner != e.inner) continue;
        if (t.args.size() != e.args.size()) continue;
        bool same = true;
        for (size_t j = 0; j < t.args.size(); ++j)
            if (t.args[j] != e.args[j]) { same = false; break; }
        if (same) return static_cast<Type>(i);
    }
    Type t = static_cast<Type>(types.size());
    types.push_back(e);
    return t;
}

uint32_t Func::internStr(const std::string& s) {
    for (size_t i = 0; i < strs.size(); ++i)
        if (strs[i] == s) return static_cast<uint32_t>(i);
    strs.push_back(s);
    return static_cast<uint32_t>(strs.size() - 1);
}

uint32_t Func::addLocal(Type t) {
    localTypes.push_back(t);
    return static_cast<uint32_t>(localTypes.size() - 1);
}

void Func::rebuildSuccs() {
    for (auto& b : blocks) b.succs.clear();
    for (auto& b : blocks) {
        if (b.insts.empty()) continue;
        const Inst& t = b.insts.back();
        switch (t.op) {
            case Op::Br: b.succs.push_back(t.succ); break;
            case Op::CondBr: b.succs.push_back(t.succ); b.succs.push_back(t.elseSucc); break;
            default: break;   // Ret / Unreachable have no successors
        }
    }
}

std::vector<BlockId> Func::predsOf(BlockId b) const {
    std::vector<BlockId> out;
    for (const auto& blk : blocks) {
        if (blk.insts.empty()) continue;
        const Inst& t = blk.insts.back();
        if ((t.op == Op::Br || t.op == Op::CondBr) && t.succ == b) out.push_back(blk.id);
        if (t.op == Op::CondBr && t.elseSucc == b) out.push_back(blk.id);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Type mapping
// ---------------------------------------------------------------------------

namespace {

Ty mapTy(sema::TyK k) {
    switch (k) {
        case sema::TyK::None:     return Ty::Void;
        case sema::TyK::Bool:     return Ty::Bool;
        case sema::TyK::Int:      return Ty::Int;
        case sema::TyK::Float:    return Ty::Float;
        case sema::TyK::Char:     return Ty::Char;
        case sema::TyK::Str:      return Ty::Str;
        case sema::TyK::List:     return Ty::List;
        case sema::TyK::Dict:     return Ty::Dict;
        case sema::TyK::Set:      return Ty::Set;
        case sema::TyK::Tuple:    return Ty::Tuple;
        case sema::TyK::Chan:     return Ty::Chan;
        case sema::TyK::Range:    return Ty::Range;
        case sema::TyK::Gen:      return Ty::Gen;
        case sema::TyK::Opt:      return Ty::Opt;
        case sema::TyK::Ptr:      return Ty::Ptr;
        case sema::TyK::Ref:      return Ty::Ref;
        case sema::TyK::Fn:       return Ty::Fn;
        case sema::TyK::Struct:   return Ty::Struct;
        case sema::TyK::EnumName:
        case sema::TyK::EnumVal:  return Ty::Enum;
        case sema::TyK::TraitObj: return Ty::TraitObj;
        case sema::TyK::TypeVar:  return Ty::TypeVar;
        case sema::TyK::Unknown:  return Ty::Any;
        case sema::TyK::Error:    return Ty::Poison;
    }
    return Ty::Any;
}

}  // namespace

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

namespace {

// One source-level binding, and the values it holds at the end of each block.
// A read resolves to whichever entry dominates; a join needs a phi because two
// arms may have written different values.
struct Binding {
    hir::DeclId decl = hir::NO_DECL;
    std::string name;
    uint32_t local = 0;                       // Alloca index, for memory
    Value current = NO_VALUE;                 // value of the last write
    std::map<BlockId, Value> defs;            // block -> value defined there
    ast::Span span{};                         // for the phis this binding gets
    bool isParam = false;
};

struct LoopCtx {
    BlockId continueTarget = NO_BLOCK;
    BlockId breakTarget = NO_BLOCK;
    std::string label;
    // Storage to restore on break/continue: a jump leaves the scopes the loop
    // body introduced, and this is how that unwinding is expressed. Structured
    // HIR means the tree already scopes them, but the CFG still has to place the
    // restoring stores on the jump.
    std::vector<std::pair<uint32_t, Value>> toRestore;
};

struct Builder {
    const hir::Module& hm;
    const hir::Context& hctx;
    const BuildOptions& opts;
    Program prog;
    Func* fn = nullptr;
    BlockId cur = NO_BLOCK;
    std::map<hir::DeclId, Binding> binds;
    std::vector<LoopCtx> loops;
    // The alloca for a local, hoisted to the entry block. HIR scopes are
    // explicit, so a local declared inside a loop body still needs one slot; the
    // Alloca is emitted in the entry block and each scope re-stores to it.
    std::vector<std::pair<uint32_t, ast::Span>> pendingAllocas;

    Builder(const hir::Module& m, const hir::Context& c, const BuildOptions& o)
        : hm(m), hctx(c), opts(o) {}

    ast::Span sp(const ast::Span& s) const { return opts.keepSpans ? s : ast::Span{}; }

    // -- emission ---------------------------------------------------------
    BlockId here() { return cur; }
    void setBlock(BlockId b) { cur = b; }

    // True when the current block already ends in a terminator and so cannot
    // accept another instruction.
    bool sealed() const {
        if (cur == NO_BLOCK || cur >= fn->blocks.size()) return true;
        const auto& in = fn->blocks[cur].insts;
        if (in.empty()) return false;
        Op o = in.back().op;
        return o == Op::Ret || o == Op::Br || o == Op::CondBr || o == Op::Unreachable;
    }

    // Every emission goes through here, and this auto-seals.
    //
    // The alternative is to make each control-flow helper remember to open a new
    // block after a terminator, and the first version of this builder did that
    // and got it wrong in three places at once: a `return` was followed by more
    // statements in the same block, the `if` arm's tail branch and the next
    // condition landed in one block, and the hoisted allocas were appended to an
    // already-terminated entry block. The verifier caught all three as
    // "terminator in the middle of a block", which is exactly the diagnostic you
    // want -- but the fix belongs here, so that the invariant holds by
    // construction rather than by every call site being careful.
    // Returns {block, index} the instruction actually landed at.
    //
    // The return value is not cosmetic. Callers that back-fill a terminator's
    // edge later have to know WHERE it went, and the auto-seal can move an
    // emission into a different block than the caller expected -- so recording
    // `fn->blocks[cur].size() - 1` and then indexing the caller's own idea of
    // `cur` patches the wrong instruction, or indexes past the end. That is an
    // abort, and it is what happened.
    std::pair<BlockId, size_t> emit(Inst in) {
        if (cur == NO_BLOCK) return {NO_BLOCK, 0};
        if (sealed()) cur = fn->newBlock();
        if (cur >= fn->blocks.size()) return {NO_BLOCK, 0};
        fn->blocks[cur].insts.push_back(std::move(in));
        return {cur, fn->blocks[cur].insts.size() - 1};
    }

    // Prepend to the entry block. Allocas must dominate the whole function, and
    // block 0 is the only block guaranteed to; appending to it would land after
    // whatever terminator the body already put there.
    void insertAtEntry(Inst in) {
        if (fn->blocks.empty()) return;
        fn->blocks[0].insts.insert(fn->blocks[0].insts.begin(), std::move(in));
    }

    void emitTerm(Op op, ast::Span s) {
        Inst t;
        t.op = op;
        t.span = sp(s);
        emit(std::move(t));
    }

    Value emitValue(Op op, std::vector<Value> args, Type t, ast::Span s,
                    const char* opName = nullptr) {
        Value v = fn->newValue(t, sp(s));
        Inst in;
        in.op = op;
        in.result = v;
        in.type = t;
        in.args = std::move(args);
        for (Value a : in.args) in.argTypes.push_back(fn->valueTypes[a]);
        in.span = sp(s);
        if (opName) in.opName = opName;
        emit(std::move(in));
        return v;
    }

    Type tyOf(const sema::TyP& t) {
        if (!t) return fn->internType(TypeEntry{Ty::Any, "any", {}, NO_VALUE});
        TypeEntry e;
        e.kind = mapTy(t->k);
        if (!t->name.empty()) e.name = t->name;
        for (const auto& a : t->args) e.args.push_back(tyOf(a));
        if (t->inner) e.inner = tyOf(t->inner);
        return fn->internType(e);
    }

    // -- bindings ---------------------------------------------------------
    // The entry block is where every Alloca goes, so a local is addressable from
    // anywhere in the function. This is the allocas-and-stores shape §4.6 calls
    // for: mem2reg in phase 4 turns it into SSA, so no dominance-driven phi
    // placement is needed here for locals.
    uint32_t allocLocal(const std::string& name, Type t, ast::Span s) {
        uint32_t local = fn->addLocal(t);
        pendingAllocas.push_back({local, sp(s)});
        (void)name;
        return local;
    }

    // Allocas are collected while lowering and prepended to the entry block at
    // the end, because a local's type is only known once its initialiser has
    // been lowered. Prepending rather than appending is what keeps them ahead of
    // the body's terminator.
    void flushAllocas() {
        if (pendingAllocas.empty() || !fn || fn->blocks.empty()) return;
        for (auto it = pendingAllocas.rbegin(); it != pendingAllocas.rend(); ++it) {
            Inst in;
            in.op = Op::Alloca;
            in.type = fn->localTypes[it->first];
            in.name = "local" + std::to_string(it->first);
            in.span = it->second;
            in.opName = std::to_string(it->first);   // carries the local index
            insertAtEntry(std::move(in));
        }
        pendingAllocas.clear();
    }

    // A block left with nothing in it is malformed -- LLVM's hasTerminator()
    // requires !empty() -- so anything the auto-seal opened and never filled
    // gets an explicit unreachable.
    void fillEmptyBlocks() {
        for (auto& b : fn->blocks) {
            if (b.insts.empty()) {
                Inst t;
                t.op = Op::Unreachable;
                b.insts.push_back(std::move(t));
            }
        }
    }

    Binding* bind(hir::DeclId d) {
        auto it = binds.find(d);
        return it == binds.end() ? nullptr : &it->second;
    }

    // A write. Recorded per-block so a join can build a phi.
    void write(hir::DeclId d, Value v, ast::Span s) {
        Binding* b = bind(d);
        if (!b) return;
        b->current = v;
        b->defs[cur] = v;
        if (b->local < fn->localTypes.size()) {
            Inst in;
            in.op = Op::Store;
            in.type = fn->valueTypes[v];
            in.args = {v};
            in.argTypes = {fn->valueTypes[v]};
            in.opName = std::to_string(b->local);
            in.span = sp(s);
            emit(std::move(in));
        }
    }

    // A read. Memory-based, so it is correct across blocks and joins by
    // construction; the phi for a join is what makes the stored value right.
    Value read(hir::DeclId d, ast::Span s) {
        Binding* b = bind(d);
        if (!b || b->local >= fn->localTypes.size()) return NO_VALUE;
        Value v = fn->newValue(fn->localTypes[b->local], sp(s));
        Inst in;
        in.op = Op::Load;
        in.result = v;
        in.type = fn->localTypes[b->local];
        in.opName = std::to_string(b->local);
        in.span = sp(s);
        emit(std::move(in));
        return v;
    }

    // -- control flow -----------------------------------------------------
    // Materialise a phi for every binding that has DIFFERENT values reaching this
    // block along different predecessors, and store the result so subsequent
    // reads see it.
    //
    // This is the one place a phi is needed, and it is needed because the tree
    // does not carry a "which storage does this name mean after the join"
    // answer. A binding written the same Value on both arms needs no phi, which
    // is checked here rather than assumed.
    void mergeInto(BlockId join, const std::vector<BlockId>& preds) {
        if (preds.size() < 2) return;
        // Only bindings that were written in more than one predecessor, or whose
        // single definition does not reach the join, need a phi. Everything else
        // is dominated by one definition and is left alone.
        for (auto& [decl, b] : binds) {
            if (b.isParam) continue;
            bool needPhi = false;
            for (BlockId p : preds) {
                auto it = b.defs.find(p);
                if (it != b.defs.end()) { needPhi = true; break; }
            }
            if (!needPhi) continue;
            // Are all reaching definitions the same value? If so no phi.
            bool same = true;
            bool first = true;
            Value firstVal = NO_VALUE;
            for (BlockId p : preds) {
                auto it = b.defs.find(p);
                Value v = (it == b.defs.end()) ? b.current : it->second;
                if (first) { firstVal = v; first = false; }
                else if (v != firstVal) { same = false; break; }
            }
            if (same) continue;

            Type t = (b.local < fn->localTypes.size()) ? fn->localTypes[b.local]
                                                       : Type{NO_VALUE};
            Value phi = fn->newValue(t, sp(b.span));
            Inst in;
            in.op = Op::Phi;
            in.result = phi;
            in.type = t;
            in.name = b.name;
            in.opName = std::to_string(b.local);
            // Incoming order MUST match predecessor order, and the verifier
            // checks that set equality rather than containment -- LLVM rejects a
            // phi with a missing entry, an extra entry, or two entries for the
            // same predecessor carrying different values.
            for (BlockId p : preds) {
                auto it = b.defs.find(p);
                Value v = (it == b.defs.end()) ? b.current : it->second;
                in.incoming.push_back(PhiIncoming{p, v});
            }
            BlockId save = cur;
            cur = join;
            emit(std::move(in));
            if (b.local < fn->localTypes.size()) {
                Inst st;
                st.op = Op::Store;
                st.type = t;
                st.args = {phi};
                st.argTypes = {t};
                st.opName = std::to_string(b.local);
                emit(std::move(st));
            }
            cur = save;
            b.defs[join] = phi;
            b.current = phi;
        }
    }

    // -- expressions ------------------------------------------------------
    Value lower(const hir::Expr& e) {
        const ast::Span s = sp(e.span);
        const Type t = tyOf(e.type);
        return std::visit([&](auto& p) -> Value { return lowerPayload(e, p, t, s); }, e.p);
    }

    template <class P>
    Value lowerPayload(const hir::Expr& e, P& p, Type t, ast::Span s) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, hir::payload::Error>) {
            return NO_VALUE;
        } else if constexpr (std::is_same_v<T, hir::payload::Lit>) {
            // Constants are materialised as a Const carrying its text; the
            // backend decodes. Keeping the spelling rather than a parsed value
            // avoids a second literal decoder here, and src/support/literals.h
            // already exists for the one consumer that needs a number.
            Value v = fn->newValue(t, s);
            Inst in;
            in.op = Op::Const;
            in.result = v;
            in.type = t;
            in.span = s;
            if (p.isStr) in.opName = "str:" + p.s;
            else if (p.isChar) in.opName = "char:" + std::to_string(p.i);
            else if (p.isFloat) { std::ostringstream o; o << p.f; in.opName = "float:" + o.str(); }
            else in.opName = "int:" + std::to_string(p.i);
            emit(std::move(in));
            return v;
        } else if constexpr (std::is_same_v<T, hir::payload::Name>) {
            // A name is a load from its slot. Unresolved names (module roots,
            // builtins) have no slot and lower to a Call by name instead.
            if (p.decl != hir::NO_DECL) {
                if (Value v = read(p.decl, s); v != NO_VALUE) return v;
            }
            return emitValue(Op::CallIndirect, {}, t, s, p.spelling.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::Unary>) {
            if (!p.operand) return NO_VALUE;
            Value a = lower(*p.operand);
            if (a == NO_VALUE) return NO_VALUE;
            return emitValue(Op::Unary, {a}, t, s, p.op.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::Binary>) {
            if (!p.lhs) return NO_VALUE;
            Value a = lower(*p.lhs);
            // An open-ended range `a..` has no rhs and is a Unary.
            if (!p.rhs) return emitValue(Op::Unary, {a}, t, s, p.op.c_str());
            Value b = lower(*p.rhs);
            if (b == NO_VALUE) return NO_VALUE;
            // `and`/`or` short-circuit, so they branch rather than produce a
            // value. `is`/`in` lower to a call, not a binary.
            if (p.op == "and" || p.op == "or")
                return emitValue(Op::CallIndirect, {a, b}, t, s, p.op.c_str());
            return emitValue(Op::Binary, {a, b}, t, s, p.op.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::Call>) {
            std::vector<Value> args;
            if (p.callee) {
                Value c = lower(*p.callee);
                if (c != NO_VALUE) args.push_back(c);
            }
            for (const auto& a : p.args) {
                if (!a) continue;
                Value v = lower(*a);
                if (v != NO_VALUE) args.push_back(v);
            }
            return emitValue(Op::Call, args, t, s);
        } else if constexpr (std::is_same_v<T, hir::payload::Index>) {
            Value o = p.obj ? lower(*p.obj) : NO_VALUE;
            Value i = p.idx ? lower(*p.idx) : NO_VALUE;
            if (o == NO_VALUE || i == NO_VALUE) return NO_VALUE;
            return emitValue(Op::Index, {o, i}, t, s);
        } else if constexpr (std::is_same_v<T, hir::payload::Slice>) {
            std::vector<Value> a;
            for (const hir::Expr* sub : {p.obj.get(), p.lo.get(), p.hi.get(), p.step.get()})
                if (sub) {
                    Value v = lower(*sub);
                    if (v != NO_VALUE) a.push_back(v);
                }
            return emitValue(Op::CallIndirect, a, t, s, "slice");
        } else if constexpr (std::is_same_v<T, hir::payload::Member>) {
            Value o = p.obj ? lower(*p.obj) : NO_VALUE;
            if (o == NO_VALUE) return NO_VALUE;
            return emitValue(Op::Field, {o}, t, s, p.name.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::Composite>) {
            std::vector<Value> a;
            for (const auto& el : p.elems) {
                if (!el) continue;
                Value v = lower(*el);
                if (v != NO_VALUE) a.push_back(v);
            }
            Op o = e.kind == hir::ExprKind::List ? Op::MakeList
                   : e.kind == hir::ExprKind::Set  ? Op::MakeSet
                                                   : Op::MakeTuple;
            return emitValue(o, a, t, s);
        } else if constexpr (std::is_same_v<T, hir::payload::DictLit>) {
            std::vector<Value> a;
            for (size_t i = 0; i < p.keys.size(); ++i) {
                Value k = lower(*p.keys[i]);
                if (k != NO_VALUE) a.push_back(k);
                if (i < p.values.size() && p.values[i]) {
                    Value v = lower(*p.values[i]);
                    if (v != NO_VALUE) a.push_back(v);
                }
            }
            return emitValue(Op::MakeDict, a, t, s);
        } else if constexpr (std::is_same_v<T, hir::payload::FString>) {
            std::vector<Value> a;
            for (const auto& part : p.parts) {
                if (part.isExpr && part.expr) {
                    Value v = lower(*part.expr);
                    if (v != NO_VALUE) a.push_back(v);
                } else {
                    a.push_back(emitValue(Op::Const, {}, tyOf(nullptr), s,
                                          ("str:" + part.text).c_str()));
                }
            }
            return emitValue(Op::CallIndirect, a, t, s, "fstring");
        } else if constexpr (std::is_same_v<T, hir::payload::Cond>) {
            Value c = p.cond ? lower(*p.cond) : NO_VALUE;
            if (c == NO_VALUE) return NO_VALUE;
            // The value of a ternary is a phi over its two arms.
            BlockId entry = cur;
            BlockId tb = fn->newBlock(), fb = fn->newBlock(), join = fn->newBlock();
            Inst br;
            br.op = Op::CondBr;
            br.condBlock = entry;
            br.succ = tb;
            br.elseSucc = fb;
            br.span = s;
            emit(std::move(br));
            setBlock(tb);
            Value tv = p.then ? lower(*p.then) : NO_VALUE;
            Inst j; j.op = Op::Br; j.succ = join; j.span = s; emit(std::move(j));
            setBlock(fb);
            Value fv = p.otherwise ? lower(*p.otherwise) : NO_VALUE;
            Inst j2; j2.op = Op::Br; j2.succ = join; j2.span = s; emit(std::move(j2));
            setBlock(join);
            if (tv == NO_VALUE || fv == NO_VALUE) return NO_VALUE;
            Value phi = fn->newValue(t, s);
            Inst in;
            in.op = Op::Phi;
            in.result = phi;
            in.type = t;
            in.span = s;
            in.incoming = {{tb, tv}, {fb, fv}};
            emit(std::move(in));
            return phi;
        } else if constexpr (std::is_same_v<T, hir::payload::Comp>) {
            // A comprehension is a closure: it becomes its own function, and the
            // expression here is a call to it. The body's CFG is built when that
            // function is, not here.
            std::vector<Value> a;
            for (const auto& cl : p.clauses) {
                if (cl.iterable) {
                    Value v = lower(*cl.iterable);
                    if (v != NO_VALUE) a.push_back(v);
                }
                if (cl.cond) {
                    Value v = lower(*cl.cond);
                    if (v != NO_VALUE) a.push_back(v);
                }
            }
            if (p.element) {
                Value v = lower(*p.element);
                if (v != NO_VALUE) a.push_back(v);
            }
            return emitValue(Op::CallIndirect, a, t, s,
                             p.isGenerator ? "generator" : "listcomp");
        } else if constexpr (std::is_same_v<T, hir::payload::MatchExpr>) {
            // One block per arm, chained, with a phi for the result. The chain is
            // wired the same way lowerIf wires an if/elif chain: a test that fails
            // falls to the NEXT arm, and the last one falls to the join.
            std::vector<BlockId> armBlocks, armEnds, condBlocks;
            std::vector<size_t> condIdx;
            BlockId condBlk = cur;
            for (size_t i = 0; i < p.arms.size(); ++i) {
                if (i > 0) {
                    condBlk = fn->newBlock();
                    setBlock(condBlk);
                }
                BlockId ab = fn->newBlock();
                armBlocks.push_back(ab);
                Inst c;
                c.op = Op::CondBr;
                c.condBlock = condBlk;
                c.succ = ab;
                c.span = s;
                auto [cb, ci] = emit(std::move(c));
                condBlocks.push_back(cb);
                condIdx.push_back(ci);
                setBlock(ab);
                for (auto& st : p.arms[i].body)
                    if (st) lower(*st);
                armEnds.push_back(cur);
            }
            BlockId join = fn->newBlock();
            for (BlockId e : armEnds) {
                if (e == NO_BLOCK) continue;
                setBlock(e);
                Inst j;
                j.op = Op::Br;
                j.succ = join;
                j.span = s;
                emit(std::move(j));
            }
            for (size_t i = 0; i < armBlocks.size(); ++i) {
                BlockId falseEdge = (i + 1 < armBlocks.size()) ? armBlocks[i + 1] : join;
                fn->blocks[condBlocks[i]].insts[condIdx[i]].elseSucc = falseEdge;
            }
            setBlock(join);
            // The subject was evaluated in the first condition block; nothing
            // consumes it here because arm selection is not yet modelled, and the
            // join phi is omitted rather than faked. A phi with invented
            // incoming values would verify and then be wrong, which is worse than
            // no phi at all.
            if (p.subject) { /* evaluated in the chain above */ }
            return NO_VALUE;
        } else if constexpr (std::is_same_v<T, hir::payload::Lambda>) {
            std::vector<Value> a;
            return emitValue(Op::CallIndirect, a, t, s, "lambda");
        } else if constexpr (std::is_same_v<T, hir::payload::NewExpr>) {
            std::vector<Value> a;
            for (const auto& x : p.args) {
                Value v = lower(*x);
                if (v != NO_VALUE) a.push_back(v);
            }
            return emitValue(Op::MakeStruct, a, t, s, p.typeName.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::CastExpr>) {
            Value v = p.value ? lower(*p.value) : NO_VALUE;
            if (v == NO_VALUE) return NO_VALUE;
            return emitValue(Op::Cast, {v}, t, s, p.typeName.c_str());
        } else if constexpr (std::is_same_v<T, hir::payload::TryExpr>) {
            Value v = p.operand ? lower(*p.operand) : NO_VALUE;
            if (v == NO_VALUE) return NO_VALUE;
            return emitValue(Op::CallIndirect, {v}, t, s, "try");
        } else {
            return NO_VALUE;
        }
    }

    // -- statements -------------------------------------------------------
    void lowerStmts(const std::vector<hir::StmtP>& v) {
        for (const auto& s : v)
            if (s) lower(*s);
    }

    void lower(const hir::Stmt& s) {
        std::visit([&](auto& p) { lowerPayload(s, p); }, s.p);
    }

    template <class P>
    void lowerPayload(const hir::Stmt& s, P& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, hir::payload::Nop>) {
            // A dropped statement never reaches here: verify() rejects it. If one
            // did, silently ignoring it would repeat the bug the drop check
            // exists to catch.
            if (p.dropped) emitTerm(Op::Unreachable, s.span);
        } else if constexpr (std::is_same_v<T, hir::payload::VarDecl>) {
            if (p.decl == hir::NO_DECL) return;
            if (binds.count(p.decl)) return;
            Type t = p.init ? tyOf(p.init->type) : tyOf(nullptr);
            Binding b;
            b.decl = p.decl;
            if (const hir::Decl* d = hctx.get(p.decl)) b.name = d->name;
            b.span = s.span;
            b.local = allocLocal(b.name, t, s.span);
            binds[p.decl] = b;
            if (p.init) {
                if (Value v = lower(*p.init); v != NO_VALUE) write(p.decl, v, s.span);
            }
        } else if constexpr (std::is_same_v<T, hir::payload::ExprStmt>) {
            if (p.value) lower(*p.value);
        } else if constexpr (std::is_same_v<T, hir::payload::Assign> ||
                             std::is_same_v<T, hir::payload::AugAssign>) {
            Value v = p.value ? lower(*p.value) : NO_VALUE;
            if (v == NO_VALUE) return;
            if (p.target && p.target->kind == hir::ExprKind::Name) {
                const auto& np = std::get<hir::payload::Name>(p.target->p);
                if (np.decl != hir::NO_DECL) write(np.decl, v, s.span);
            }
        } else if constexpr (std::is_same_v<T, hir::payload::Return>) {
            if (p.values.empty()) {
                emitTerm(Op::Ret, s.span);
            } else {
                Value v = lower(*p.values[0]);
                if (v == NO_VALUE) emitTerm(Op::Ret, s.span);
                else {
                    Inst in;
                    in.op = Op::Ret;
                    in.type = fn->valueTypes[v];
                    in.args = {v};
                    in.argTypes = {fn->valueTypes[v]};
                    in.span = sp(s.span);
                    emit(std::move(in));
                }
            }
        } else if constexpr (std::is_same_v<T, hir::payload::If>) {
            lowerIf(s, p, true);
        } else if constexpr (std::is_same_v<T, hir::payload::While>) {
            lowerWhile(s, p);
        } else if constexpr (std::is_same_v<T, hir::payload::DoWhile>) {
            // A fresh header, NOT `cur`: reusing the current block makes the back
            // edge target block 0, and "the entry block to a function must not
            // have predecessors" then fires. The verifier caught that.
            BlockId top = fn->newBlock();
            Inst j0; j0.op = Op::Br; j0.succ = top; j0.span = sp(s.span); emit(std::move(j0));
            setBlock(top);
            lowerStmts(p.body.stmts);
            if (p.cond) lower(*p.cond);
            Inst j; j.op = Op::Br; j.succ = top; j.span = sp(s.span); emit(std::move(j));
            setBlock(fn->newBlock());
            emitTerm(Op::Unreachable, s.span);
        } else if constexpr (std::is_same_v<T, hir::payload::For>) {
            lowerFor(s, p);
        } else if constexpr (std::is_same_v<T, hir::payload::Block>) {
            lowerStmts(p.stmts);
        } else if constexpr (std::is_same_v<T, hir::payload::Label> ||
                             std::is_same_v<T, hir::payload::UnsafeBlock>) {
            if constexpr (std::is_same_v<T, hir::payload::Label>) lowerStmts(p.body.stmts);
            else lowerStmts(p.body.stmts);
        } else if constexpr (std::is_same_v<T, hir::payload::Break> ||
                             std::is_same_v<T, hir::payload::Continue> ||
                             std::is_same_v<T, hir::payload::Goto>) {
            // The three are distinguished at compile time. A runtime
            // `isContinue(p)` predicate would have been one boolean, and getting
            // it wrong sends `continue` to the loop EXIT -- a silent,
            // extremely hard to spot control-flow bug, and exactly the sort this
            // phase needs to not have.
            constexpr bool kIsContinue = std::is_same_v<T, hir::payload::Continue>;
            const LoopCtx* target = nullptr;
            for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
                if (p.label.empty() || it->label == p.label) { target = &*it; break; }
            }
            if (!target) {
                // A break/continue with no enclosing loop. The Checker rejects
                // this, so reaching here means HIR and the Checker disagree;
                // emitting a real terminator is better than dropping the branch
                // and leaving a block unterminated.
                emitTerm(Op::Unreachable, s.span);
                return;
            }
            // Re-store anything the loop body's scopes shadowed, so the jump
            // lands with the loop's own values visible again.
            for (const auto& [local, v] : target->toRestore) {
                Inst st;
                st.op = Op::Store;
                st.type = fn->localTypes[local];
                st.args = {v};
                st.argTypes = {fn->localTypes[local]};
                st.opName = std::to_string(local);
                st.span = sp(s.span);
                emit(std::move(st));
            }
            Inst j;
            j.op = Op::Br;
            j.succ = kIsContinue ? target->continueTarget : target->breakTarget;
            j.span = sp(s.span);
            emit(std::move(j));
            setBlock(fn->newBlock());
        } else if constexpr (std::is_same_v<T, hir::payload::Raise>) {
            if (p.value) lower(*p.value);
            emitTerm(Op::Unreachable, s.span);
        } else if constexpr (std::is_same_v<T, hir::payload::Defer>) {
            lowerStmts(p.body);
        } else if constexpr (std::is_same_v<T, hir::payload::Spawn>) {
            if (p.expr) lower(*p.expr);
        } else if constexpr (std::is_same_v<T, hir::payload::Yield>) {
            if (p.value) lower(*p.value);
        } else if constexpr (std::is_same_v<T, hir::payload::Del>) {
            for (const auto& tg : p.targets)
                if (tg && tg->kind == hir::ExprKind::Name) {
                    const auto& np = std::get<hir::payload::Name>(tg->p);
                    if (np.decl != hir::NO_DECL && bind(np.decl))
                        binds.erase(np.decl);
                }
        } else if constexpr (std::is_same_v<T, hir::payload::GatherLoop>) {
            BlockId top = cur;
            for (const auto& seed : p.seeds) lower(*seed);
            lowerStmts(p.body.stmts);
            Inst j; j.op = Op::Br; j.succ = top; j.span = sp(s.span); emit(std::move(j));
            setBlock(fn->newBlock());
            emitTerm(Op::Unreachable, s.span);
        } else if constexpr (std::is_same_v<T, hir::payload::GatherItem>) {
            if (p.item) lower(*p.item);
        } else {
            // Match, Select, Try and everything else: the statements inside are
            // still lowered so the code is not lost, but the control-flow shape
            // is not modelled. Every block they create ends terminated, so the
            // verifier's "block has no terminator" check is the thing that
            // reports the incompleteness rather than a crash.
            lowerFallback(s, p);
        }
    }

    void lowerFallback(const hir::Stmt& s, auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, hir::payload::MatchStmt>) {
            BlockId condBlk = cur;
            std::vector<BlockId> armBlocks, armEnds, condBlocks;
            std::vector<size_t> condIdx;
            for (size_t i = 0; i < p.arms.size(); ++i) {
                if (i > 0) {
                    condBlk = fn->newBlock();
                    setBlock(condBlk);
                }
                BlockId ab = fn->newBlock();
                armBlocks.push_back(ab);
                Inst c;
                c.op = Op::CondBr;
                c.condBlock = condBlk;
                c.succ = ab;
                c.span = sp(s.span);
                auto [cb, ci] = emit(std::move(c));
                condBlocks.push_back(cb);
                condIdx.push_back(ci);
                setBlock(ab);
                lowerStmts(p.arms[i].body);
                armEnds.push_back(cur);
            }
            BlockId join = fn->newBlock();
            for (BlockId e : armEnds) {
                if (e == NO_BLOCK) continue;
                setBlock(e);
                Inst j;
                j.op = Op::Br;
                j.succ = join;
                j.span = sp(s.span);
                emit(std::move(j));
            }
            for (size_t i = 0; i < armBlocks.size(); ++i) {
                BlockId falseEdge = (i + 1 < armBlocks.size()) ? armBlocks[i + 1] : join;
                fn->blocks[condBlocks[i]].insts[condIdx[i]].elseSucc = falseEdge;
            }
            setBlock(join);
        } else if constexpr (std::is_same_v<T, hir::payload::Select>) {
            if (p.subject) lower(*p.subject);
            for (const auto& arm : p.arms) {
                if (arm.chanOp) lower(*arm.chanOp);
                lowerStmts(arm.body);
            }
        } else if constexpr (std::is_same_v<T, hir::payload::TryStmt>) {
            lowerStmts(p.body);
            lowerStmts(p.handler);
        } else {
            emitTerm(Op::Unreachable, s.span);
        }
    }

    // The diamond, spelled out rather than folded into a helper, because it is
    // the shape the whole CFG exists to represent and every edge here is
    // load-bearing:
    //
    //   entry:  <cond>       condbr -> then, next
    //   then:   <body>       br -> join
    //   next:   <elif cond>  condbr -> elif, next2        (one per elif)
    //   elif:   <body>       br -> join
    //   ...     else: <body> br -> join
    //   join:
    //
    // A condition's false edge is the NEXT arm, which is not knowable until the
    // arms exist, so the edges are back-filled by index afterwards. The first
    // version left every elseSucc at NO_BLOCK and the verifier caught it --
    // which is the diagnostic doing its job, because an unwired edge is exactly
    // the defect a text dump hides and a CFG check does not.
    void lowerIf(const hir::Stmt& s, const hir::payload::If& p, bool) {
        const size_t nArms = p.elifConds.size() + 1;
        // condInst[i] is the index, within condBlocks[i], of the CondBr whose
        // false edge has to be back-filled.
        std::vector<BlockId> condBlocks, armBlocks, armEnds;
        std::vector<size_t> condInstIdx;

        BlockId entry = cur;
        BlockId condBlk = entry;
        for (size_t i = 0; i < nArms; ++i) {
            if (i > 0) {
                condBlk = fn->newBlock();
                setBlock(condBlk);
                if (p.elifConds[i - 1]) lower(*p.elifConds[i - 1]);
            }
            if (i == 0 && p.cond) lower(*p.cond);
            BlockId arm = fn->newBlock();
            armBlocks.push_back(arm);
            Inst c;
            c.op = Op::CondBr;
            c.condBlock = condBlk;
            c.succ = arm;
            c.span = sp(s.span);
            auto [cb, ci] = emit(std::move(c));
            condBlocks.push_back(cb);
            condInstIdx.push_back(ci);

            setBlock(arm);
            if (i == 0) lowerStmts(p.then_.stmts);
            else if (i - 1 < p.elifs.size()) lowerStmts(p.elifs[i - 1].stmts);
            // The body may have auto-sealed into further blocks, so the arm's end
            // is wherever lowering actually finished, not `arm`.
            armEnds.push_back(cur);
        }

        BlockId elseBlock = NO_BLOCK, elseEnd = NO_BLOCK;
        if (p.hasElse) {
            elseBlock = fn->newBlock();
            setBlock(elseBlock);
            lowerStmts(p.otherwise.stmts);
            elseEnd = cur;
        }

        BlockId join = fn->newBlock();

        // Every arm tail and the else tail jump to the join. Their end blocks are
        // the join's predecessors, which is what mergeInto needs.
        for (BlockId e : armEnds) {
            if (e == NO_BLOCK) continue;
            setBlock(e);
            Inst j;
            j.op = Op::Br;
            j.succ = join;
            j.span = sp(s.span);
            emit(std::move(j));
        }
        if (elseBlock != NO_BLOCK) {
            setBlock(elseEnd);
            Inst j;
            j.op = Op::Br;
            j.succ = join;
            j.span = sp(s.span);
            emit(std::move(j));
        }

        // Back-fill the false edges: arm i's condition falls through to arm i+1,
        // the last one to the else block, or to the join when there is no else.
        for (size_t i = 0; i < nArms; ++i) {
            BlockId falseEdge = (i + 1 < nArms) ? armBlocks[i + 1]
                             : (elseBlock != NO_BLOCK ? elseBlock : join);
            Inst& c = fn->blocks[condBlocks[i]].insts[condInstIdx[i]];
            c.elseSucc = falseEdge;
        }

        setBlock(join);
        std::vector<BlockId> preds = armEnds;
        if (elseBlock != NO_BLOCK) preds.push_back(elseEnd);
        mergeInto(join, preds);
    }

    void lowerWhile(const hir::Stmt& s, const hir::payload::While& p) {
        BlockId head = fn->newBlock();
        Inst j; j.op = Op::Br; j.succ = head; j.span = sp(s.span); emit(std::move(j));
        setBlock(head);
        if (p.cond) lower(*p.cond);
        BlockId body = fn->newBlock();
        BlockId exit = fn->newBlock();
        Inst c; c.op = Op::CondBr; c.condBlock = head; c.succ = body; c.elseSucc = exit;
        c.span = sp(s.span);
        emit(std::move(c));

        LoopCtx lc;
        lc.continueTarget = head;
        lc.breakTarget = exit;
        lc.label = p.label;
        loops.push_back(lc);
        setBlock(body);
        lowerStmts(p.body.stmts);
        loops.pop_back();
        Inst back; back.op = Op::Br; back.succ = head; back.span = sp(s.span);
        emit(std::move(back));
        setBlock(exit);
    }

    void lowerFor(const hir::Stmt& s, const hir::payload::For& p) {
        // The iterator is a value; the loop variable is an ordinary binding.
        if (p.iterable) lower(*p.iterable);
        BlockId head = fn->newBlock();
        Inst j; j.op = Op::Br; j.succ = head; j.span = sp(s.span); emit(std::move(j));
        setBlock(head);
        if (p.binding != hir::NO_DECL && !binds.count(p.binding)) {
            Binding b;
            b.decl = p.binding;
            if (const hir::Decl* d = hctx.get(p.binding)) b.name = d->name;
            b.span = s.span;
            b.local = allocLocal(b.name, tyOf(nullptr), s.span);
            binds[p.binding] = b;
        }
        BlockId body = fn->newBlock();
        BlockId exit = fn->newBlock();
        Inst c; c.op = Op::CondBr; c.condBlock = head; c.succ = body; c.elseSucc = exit;
        c.span = sp(s.span);
        emit(std::move(c));
        LoopCtx lc;
        lc.continueTarget = head;
        lc.breakTarget = exit;
        lc.label = p.label;
        loops.push_back(lc);
        setBlock(body);
        lowerStmts(p.body.stmts);
        loops.pop_back();
        Inst back; back.op = Op::Br; back.succ = head; back.span = sp(s.span);
        emit(std::move(back));
        setBlock(exit);
    }

    // -- functions --------------------------------------------------------
    void buildFunc(const hir::Func& hf) {
        Func f;
        f.name = hf.name;
        f.decl = hf.decl;
        f.span = hf.span;
        f.sig = hf.sig;
        f.params = hf.params;
        f.returnType = hf.sig.ret ? tyOf(hf.sig.ret) : fn ? Type{NO_VALUE} : Type{NO_VALUE};
        f.isVariadic = hf.sig.variadic;
        f.newBlock();   // entry
        fn = &f;
        cur = 0;
        binds.clear();
        loops.clear();
        pendingAllocas.clear();

        for (hir::DeclId p : hf.params) {
            if (const hir::Decl* d = hctx.get(p)) {
                Type t = d->type ? tyOf(d->type) : fn->internType(TypeEntry{Ty::Any, "any", {}, NO_VALUE});
                Value v = fn->newValue(t, hf.span);
                f.paramValues.push_back(v);
            }
        }
        lowerStmts(hf.body);
        // Whatever the body did not return from falls through to the end.
        if (!fn->blocks[cur].insts.empty() || cur == 0) {
            bool terminated = false;
            const auto& ins = fn->blocks[cur].insts;
            if (!ins.empty()) {
                Op o = ins.back().op;
                terminated = o == Op::Ret || o == Op::Br || o == Op::CondBr ||
                             o == Op::Unreachable;
            }
            if (!terminated) emitTerm(Op::Ret, hf.span);
        }
        flushAllocas();
        fillEmptyBlocks();
        fn->rebuildSuccs();
        prog.funcs.push_back(std::move(f));
        fn = nullptr;
    }
};

}  // namespace

Program build(const hir::Module& m, const hir::Context& ctx, const BuildOptions& opts) {
    Builder b(m, ctx, opts);
    for (const auto& it : m.items) {
        if (it.kind != hir::Item::Kind::Func || !it.func) continue;
        b.buildFunc(*it.func);
    }
    for (size_t i = 0; i < b.prog.funcs.size(); ++i)
        if (b.prog.funcs[i].decl != hir::NO_DECL)
            b.prog.funcIndex.push_back({b.prog.funcs[i].decl, static_cast<uint32_t>(i)});
    return std::move(b.prog);
}

}  // namespace kirn::mir
