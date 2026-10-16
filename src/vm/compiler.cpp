// AST -> bytecode compiler (core slice). See compiler.h / bytecode.h.
//
// Core slice: literals, identifiers, unary/binary numeric ops, and/or/is/in,
// comparison, calls to user functions & builtins, list/dict/set/tuple, index/
// slice, member/method on value receivers, ternary, var/const/assign/augassign,
// if/while/simple-bind for loops, return, break/continue.
//
// A function is only VM-compiled when its whole body stays in the core slice;
// any construct that needs the full tree-walker (struct/enum/chan construction,
// module members, match/select/spawn/defer/raise, lambdas, f-strings, patterns,
// new/cast) marks the function `interpreted` so the VM falls back to runFunc.
#include <cctype>
#include <cstdlib>
#include <vector>

#include "vm/compiler.h"
#include "vm/bytecode.h"
#include "interp/escapes.h"
#include "support/literals.h"

#include <unordered_map>
#include <unordered_set>

namespace kirn {
namespace vm {

using ast::Stmt;
using ast::StmtP;
using ast::Expr;
using ast::ExprP;
using ast::ExKind;
using ast::StKind;

namespace {

const std::unordered_set<std::string>& kBuiltins() {
    static const std::unordered_set<std::string> s = {
        "print","len","range","panic","catch_panic","printf","strlen",
        "assert","assert_eq","type","typeof","sum",
        "any","all","sorted","reversed","enumerate","map","filter","reduce",
        "upper","lower","trim","contains","starts_with","ends_with","replace",
        "split","join",
        // Kirn-style CapCase free builtins (CapCase + __CapCase forms)
        "__Sqrt","Ord","__Ord","Chr","__Chr",
        "BitLength","__BitLength","BitCount","__BitCount",
        "Rational","__Rational","Numerator","__Numerator",
        "Denominator","__Denominator",
        "ToFloat","__ToFloat","ToInt","__ToInt","ToRational","__ToRational",
        "FAbs","__FAbs","__Abs",
        "Exp","__Exp","Log","__Log","Log2","__Log2","Log10","__Log10",
        "Sin","__Sin","Cos","__Cos","Tan","__Tan",
        "Sinh","__Sinh","Cosh","__Cosh","Tanh","__Tanh",
        "Floor","__Floor","Ceil","__Ceil","Round","__Round",
        "Trunc","__Trunc","Fract","__Fract",
        "IsNan","__IsNan","IsInfinite","__IsInfinite","IsFinite","__IsFinite",
        "BigInt","__BigInt","IsPrime","__IsPrime",
        "PowMod","__PowMod","ToBytes","__ToBytes","FromBytes","__FromBytes",
        "HexDigits","__HexDigits","ToString","__ToString",
        "RationalAdd","__RationalAdd","RationalSub","__RationalSub",
        "RationalMul","__RationalMul","RationalDiv","__RationalDiv",
        "RationalPow","__RationalPow","RationalEq","__RationalEq",
        "RationalNe","__RationalNe","RationalLt","__RationalLt",
        "RationalLe","__RationalLe","RationalGt","__RationalGt",
        "RationalGe","__RationalGe",
        "CopySign","__CopySign","Pow","__Pow",
        "Str","Int","Float","Bool","Min","Max",
    };
    return s;
}
const std::unordered_set<std::string>& kModules() {
    static const std::unordered_set<std::string> s = {
        "math","time","io","mem","json","text","os",
    };
    return s;
}

struct Emitter {
    VmFunction f;
    std::unordered_map<std::string, int32_t> strPool;
    int32_t scopeDepth = 0;

    int32_t strId(const std::string& s) {
        auto it = strPool.find(s);
        if (it != strPool.end()) return it->second;
        int32_t id = (int32_t)f.strConsts.size();
        f.strConsts.push_back(s);
        strPool.emplace(s, id);
        return id;
    }
    int32_t constId(Value v) {
        int32_t id = (int32_t)f.constants.size();
        f.constants.push_back(std::move(v));
        return id;
    }
    int32_t here() const { return (int32_t)f.code.size(); }
    Ins& at(int32_t i) { return f.code[(size_t)i]; }
    void emit(uint16_t op, int32_t a = 0, int32_t b = 0, int32_t c = 0) {
        f.code.push_back({op, a, b, c});
    }
};

struct LoopCtx {
    int32_t scopeDepthAtLoop;
    int32_t top;
    std::vector<int32_t> contJumps;
    std::vector<int32_t> breakJumps;
    std::string label;
};

bool isCmpOp(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

// Map an arithmetic/comparison operator token to a specialized opcode, or 0 if
// no specialized opcode exists (caller then falls back to generic OP_BINARY).
uint16_t specializedBinaryOp(const std::string& op) {
    if (op == "+")  return OP_BINARY_ADD;
    if (op == "-")  return OP_BINARY_SUB;
    if (op == "*")  return OP_BINARY_MUL;
    if (op == "/")  return OP_BINARY_DIV;
    if (op == "%" || op == "mod") return OP_BINARY_MOD;
    if (op == "**" || op == "pow") return OP_BINARY_POW;
    if (op == "<")  return OP_LT;
    if (op == "<=") return OP_LE;
    if (op == ">")  return OP_GT;
    if (op == ">=") return OP_GE;
    if (op == "==") return OP_EQ;
    if (op == "!=") return OP_NE;
    if (op == "..") return OP_RANGE;
    if (op == "..=") return OP_RANGE;   // handled via OP_RANGE; inclusive flag set below
    return 0;
}

// Collect the names that are definitively local to a function body (params plus
// any name used as a declaration/assignment/for-bind target). Used to decide at
// compile time whether an Ident receiver of `.member`/`.method` is a value
// (local) rather than a module/global struct the VM cannot take apart.
class LocalScanner {
public:
    void scanStmt(const Stmt& s) {
        switch (s.kind) {
            case StKind::VarDecl: case StKind::ConstDecl:
                scanTarget(*s.target);
                break;
            case StKind::Assign: case StKind::AugAssign:
                if (!s.exprs.empty()) scanTarget(*s.exprs[0]);
                break;
            case StKind::If:
                for ([[maybe_unused]] const auto& e : s.elifConds) { /* conds don't bind */ }
                for (const auto& st : s.body) scanStmt(*st);
                for (const auto& st : s.elifBodies) scanStmt(*st);
                for (const auto& st : s.elseBody) scanStmt(*st);
                break;
            case StKind::While:
            case StKind::Unsafe:
                for (const auto& st : s.body) scanStmt(*st);
                break;
            case StKind::Try:
                for (const auto& st : s.body) scanStmt(*st);
                for (const auto& st : s.elseBody) scanStmt(*st);
                break;
            case StKind::For:
                if (s.pat && s.pat->kind == ast::PatKind::Bind && !s.pat->bindName.empty())
                    add(s.pat->bindName);
                for (const auto& st : s.body) scanStmt(*st);
                break;
            default:
                break;
        }
    }
    void add(const std::string& n) { locals.insert(n); }
    bool isLocal(const std::string& n) const { return locals.count(n) > 0; }
    std::unordered_set<std::string> locals;
private:
    void scanTarget(const Expr& e) {
        if (e.kind == ExKind::Ident) add(e.text);
        else if (e.kind == ExKind::Tuple || e.kind == ExKind::List)
            for (const auto& el : e.elems) scanTarget(*el);
    }
};

class Compiler {
public:
    CompileResult& out;
    const std::unordered_set<std::string>& userFuncs;
    std::vector<LoopCtx> loops;
    LocalScanner ls;

    Compiler(CompileResult& o, const std::unordered_set<std::string>& uf)
        : out(o), userFuncs(uf) {
        out.prog.funcs.emplace_back();
    }

    // --- Slot-based locals -------------------------------------------------
    // Frame-level locals (params + names first bound at scope depth 0) are
    // stored in a flat Value[] per call and accessed by integer slot index
    // (OP_LOAD_LOCAL/OP_STORE_LOCAL) instead of env->find string hashing.
    // Slot mode is disabled ('useSlots' = false) if any such frame name is
    // shadowed by a nested declaration, in which case the exact env path
    // (verified against the tree-walker) is used for the whole body.
    std::unordered_map<std::string, int32_t> slotPlan_;
    bool slotUse_ = false;

    // M1/M3 sugar desugaring: each `|>` / `??` / `.?[` / `.?(` node needs one
    // hidden frame local to bind its input (so eval-once holds). The names use
    // a leading control byte so they can never collide with a user identifier;
    // they are allocated during planSlots and looked up by node address.
    std::unordered_map<const Expr*, std::string> tmpOf_;
    int32_t tmpSeq_ = 0;
    std::string curTopic_;   // hidden name bound to the enclosing pipe's `%`

    void planSlots(const std::vector<ast::Param>& params,
                   const std::vector<StmtP>& body, Emitter& em) {
        std::unordered_map<std::string, int32_t> slotOf;
        std::unordered_map<std::string, int> slotDepth_;
        std::vector<std::string> order;
        bool unsafe = false;
        // A name owns ONE slot shared across the whole function. Sharing a slot
        // is only correct when the name's bindings never nest (siblings reuse the
        // same slot; a deeper redeclaration would be a live shadow -> unsafe).
        auto bindDecl = [&](const std::string& n, int depth) {
            auto it = slotDepth_.find(n);
            if (it == slotDepth_.end()) {
                slotDepth_[n] = depth;
                slotOf[n] = (int32_t)order.size();
                order.push_back(n);
            } else if (depth > it->second && it->second >= 0) {
                unsafe = true;   // nested declaration shadows an outer slot
            }
        };
        // Assigns never shadow: an assign to an existing binding updates it in
        // place; a first-use assign at depth 0 creates the frame binding.
        auto bindAssign = [&](const std::string& n, int depth) {
            if (!slotDepth_.count(n) && depth == 0) {
                slotDepth_[n] = 0;
                slotOf[n] = (int32_t)order.size();
                order.push_back(n);
            }
        };
        std::function<void(const Stmt&, int)> scan =
            [&](const Stmt& s, int depth) {
                switch (s.kind) {
                    case StKind::VarDecl: case StKind::ConstDecl:
                        if (s.target && s.target->kind == ExKind::Ident)
                            bindDecl(s.target->text, depth);
                        break;
                    case StKind::Assign: case StKind::AugAssign:
                        if (!s.exprs.empty() && s.exprs[0]->kind == ExKind::Ident)
                            bindAssign(s.exprs[0]->text, depth);
                        break;
                    case StKind::For:
                        if (s.pat && s.pat->kind == ast::PatKind::Bind &&
                            !s.pat->bindName.empty())
                            bindDecl(s.pat->bindName, depth + 1);
                        for (const auto& st : s.body) scan(*st, depth + 1);
                        break;
                    case StKind::If:
                        for (const auto& st : s.body) scan(*st, depth + 1);
                        for (const auto& st : s.elifBodies) scan(*st, depth + 1);
                        for (const auto& st : s.elseBody) scan(*st, depth + 1);
                        break;
                    case StKind::While: case StKind::Unsafe:
                        for (const auto& st : s.body) scan(*st, depth + 1);
                        break;
                    case StKind::Try:
                        for (const auto& st : s.body) scan(*st, depth + 1);
                        for (const auto& st : s.elseBody) scan(*st, depth + 1);
                        break;
                    default: break;
                }
            };
        for (const auto& p : params) bindDecl(p.name, 0);
        for (const auto& st : body) scan(*st, 0);

        // Allocate one hidden frame local per pipe/coalesce/safe-access node so
        // the bytecode can evaluate the receiver exactly once. These names are
        // never referenced by user code, so they are always frame-level.
        tmpOf_.clear();
        tmpSeq_ = 0;
        std::function<void(const Expr&)> escan = [&](const Expr& e) {
            switch (e.kind) {
                case ExKind::Pipe:
                case ExKind::Coalesce:
                case ExKind::SafeIndex:
                case ExKind::SafeCall:
                    if (!tmpOf_.count(&e))
                        tmpOf_[&e] = std::string("\x01t") + std::to_string(tmpSeq_++);
                    break;                // Comprehension clause bindings are scoped to the comprehension
                // (a child Env in the tree-walker) and are not part of the
                // function's slot plan. A clause name could shadow an outer
                // slot, so force the verified env path for the whole function.
                case ExKind::ListComp:
                case ExKind::Generator:
                case ExKind::SetComp:
                case ExKind::DictComp:
                    unsafe = true;
                    break;
                default: break;
            }
            if (e.lhs) escan(*e.lhs);
            if (e.rhs) escan(*e.rhs);
            if (e.value) escan(*e.value);
            if (e.cond) escan(*e.cond);
            for (const auto& x : e.elems) if (x) escan(*x);
            for (const auto& pr : e.pairs) {
                if (pr.first) escan(*pr.first);
                if (pr.second) escan(*pr.second);
            }
            for (const auto& a : e.args) if (a.value) escan(*a.value);
            for (const auto& cl : e.clauses) {
                if (cl.iter) escan(*cl.iter);
                if (cl.cond) escan(*cl.cond);
            }
        };
        std::function<void(const Stmt&)> es = [&](const Stmt& s) {
            for (const auto& x : s.exprs) if (x) escan(*x);
            if (s.value) escan(*s.value);
            if (s.cond) escan(*s.cond);
            for (const auto& st : s.body) es(*st);
            for (const auto& st : s.elseBody) es(*st);
            for (const auto& e : s.elifConds) if (e) escan(*e);
            for (const auto& st : s.elifBodies) es(*st);
        };
        for (const auto& st : body) es(*st);

        em.f.useSlots = !unsafe && !order.empty();
        em.f.slotNames = std::move(order);
        slotUse_ = em.f.useSlots;
        slotPlan_ = std::move(slotOf);
    }

    int32_t slotOf(const std::string& n) const {
        auto it = slotPlan_.find(n);
        return it == slotPlan_.end() ? -1 : it->second;
    }

    int32_t compileFunction(const std::string& name,
                            const std::vector<ast::Param>& params,
                            const std::vector<StmtP>& body, bool isResult) {
        int32_t idx = (int32_t)out.prog.funcs.size();
        out.prog.funcs.emplace_back();
        Emitter em;
        em.f.name = name;
        int32_t varIdx = -1;
        ls.locals.clear();
        for (size_t i = 0; i < params.size(); ++i) {
            em.f.paramNames.push_back(params[i].name);
            ls.add(params[i].name);
            if (params[i].variadic) varIdx = (int32_t)i;
        }
        em.f.nParams = (int32_t)params.size();
        em.f.varIdx = varIdx;
        em.f.isResult = isResult;
        loops.clear();
        curTopic_.clear();
        for (const auto& sp : body) ls.scanStmt(*sp);
        planSlots(params, body, em);
        for (const auto& sp : body) {
            if (em.f.interpreted) break;
            compileStmt(em, *sp);
        }
        em.emit(OP_RETURN_NONE);
        out.prog.funcs[idx] = std::move(em.f);
        return idx;
    }

      void compileBlock(Emitter& em, const std::vector<StmtP>& body) {
          for (const auto& sp : body) {
              if (em.f.interpreted) return;
              compileStmt(em, *sp);
          }
      }

      // Compile `body` inside a fresh child Env, matching the tree-walker's
      // makeChild(env) for `if` branches and `while` bodies.
      //
      // This was missing, and the consequence was a miscompile rather than a
      // missed optimisation. The tree-walker gives every block a child frame so
      // a block-local `var` that shadows an outer binding dies with the block;
      // the fast path stored it into the shared frame instead, so
      //
      //     var x = 1; if true { var x = 2; print(x) } print(x)
      //
      // printed `2` then `2` here, where the tree-walker prints `2` then `1`.
      // That contradicts bytecode.h's own claim that the VM is "always a
      // correct superset path", so it violated the file's stated invariant and
      // not merely a test. 164 gates and 215 goldens could not see it: shadowing
      // is only a W0105 warning, and no corpus file shadows a variable while
      // staying on the fast path.
      //
      // Raising em.scopeDepth is what makes the existing unwind machinery work:
      // a `break`/`continue` inside this block computes
      // `scopeDepth - scopeDepthAtLoop` and emits that many OP_SCOPE_LEAVEs.
      void compileScopedBlock(Emitter& em, const std::vector<StmtP>& body) {
          em.emit(OP_SCOPE_ENTER);
          em.scopeDepth++;
          compileBlock(em, body);
          em.emit(OP_SCOPE_LEAVE);
          em.scopeDepth--;
      }

    void compileStmt(Emitter& em, const Stmt& s) {
        switch (s.kind) {
            case StKind::Pass: case StKind::Export: break;

            case StKind::ConstDecl:
            case StKind::VarDecl:
                compileExpr(em, *s.value);
                emitDefine(em, *s.target);
                break;

            case StKind::ExprStmt:
                for (const auto& e : s.exprs) { compileExpr(em, *e); em.emit(OP_EXPR); }
                break;

            case StKind::Assign: {
                if (s.exprs.size() != 2) { em.f.interpreted = true; break; }
                compileExpr(em, *s.exprs[1]);
                emitStore(em, *s.exprs[0]);
                break;
            }

            case StKind::AugAssign: {
                // `??=` is lazy: assign only when the target is none. Desugar to
                // `if a is none { a = b }` with the existing IS/JUMP ops.
                if (s.augOp == "??=") {
                    if (s.exprs.size() != 2 || s.exprs[0]->kind != ExKind::Ident) {
                        em.f.interpreted = true;
                        break;
                    }
                    loadName(em, s.exprs[0]->text);
                    em.emit(OP_IS, -1);
                    int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                    compileExpr(em, *s.exprs[1]);
                    storeName(em, s.exprs[0]->text);
                    em.at(jF).a = em.here() - jF;
                    break;
                }
                // `@=` may dispatch to `__imatmul__`, which OP_BINARY does not
                // model; run the function in the interpreter so tree and VM
                // agree.
                if (s.augOp == "@=") { em.f.interpreted = true; break; }
                compileExpr(em, *s.exprs[0]);
                compileExpr(em, *s.exprs[1]);
                std::string op = s.augOp.empty() ? "+" : s.augOp.substr(0, s.augOp.size() - 1);
                uint16_t spOp = specializedBinaryOp(op);
                if (spOp && spOp != OP_RANGE) em.emit(spOp);
                else em.emit(OP_BINARY, em.strId(op));
                emitStore(em, *s.exprs[0]);
                break;
            }

            case StKind::Return:
                if (s.exprs.empty()) em.emit(OP_RETURN_NONE);
                else { compileExpr(em, *s.exprs[0]); em.emit(OP_RETURN); }
                break;

            case StKind::Break: case StKind::Continue: {
                const LoopCtx* lp = nullptr;
                for (auto it = loops.rbegin(); it != loops.rend(); ++it)
                    if (s.label.empty() || it->label == s.label) { lp = &*it; break; }
                if (!lp) { em.f.interpreted = true; break; }
                const LoopCtx& L = *lp;
                int32_t unwind = em.scopeDepth - L.scopeDepthAtLoop;
                for (int32_t i = 0; i < unwind; ++i) em.emit(OP_SCOPE_LEAVE);
                em.scopeDepth = L.scopeDepthAtLoop;
                int32_t j = em.here();
                em.emit(OP_JUMP, 0);
                if (s.kind == StKind::Break)
                    const_cast<LoopCtx&>(L).breakJumps.push_back(j);
                else
                    const_cast<LoopCtx&>(L).contJumps.push_back(j);
                break;
            }

            case StKind::If: {
                std::vector<int32_t> endings;
                compileExpr(em, *s.exprs[0]);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                // Each branch gets its own child Env, and each therefore needs
                // its own enter/leave pair -- the diamond cannot share one
                // scope around both sides, or a `var` in the taken branch would
                // still be live in the other.
                compileScopedBlock(em, s.body);
                int32_t jDone = em.here(); em.emit(OP_JUMP, 0);
                endings.push_back(jDone);
                em.at(jF).a = em.here() - jF;
                // NOTE: elifConds/elifBodies are never assigned by the parser
                // (it nests `elif` as a synthetic If in elseBody[0]), so this
                // loop is dead. Kept scoped anyway so it stays correct if the
                // parser ever populates the fields.
                for (size_t i = 0; i < s.elifConds.size(); ++i) {
                    compileExpr(em, *s.elifConds[i]);
                    int32_t jF2 = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                    em.emit(OP_SCOPE_ENTER); em.scopeDepth++;
                    compileStmt(em, *s.elifBodies[i]);
                    em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
                    int32_t jD2 = em.here(); em.emit(OP_JUMP, 0);
                    endings.push_back(jD2);
                    em.at(jF2).a = em.here() - jF2;
                }
                if (!s.elseBody.empty()) {
                    em.emit(OP_SCOPE_ENTER); em.scopeDepth++;
                    compileBlock(em, s.elseBody);
                    em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
                }
                for (int32_t tj : endings) em.at(tj).a = em.here() - tj;
                break;
            }

            case StKind::While: {
                int32_t top = em.here();
                compileExpr(em, *s.exprs[0]);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                LoopCtx L; L.scopeDepthAtLoop = em.scopeDepth; L.top = top; L.label = s.label;
                loops.push_back(L);
                // The scope opens *after* the JUMP_IF_FALSE, so the loop-exit
                // edge needs no matching leave -- and scopeDepthAtLoop above is
                // captured before it, so a `break`/`continue` from inside the
                // body unwinds exactly this one scope. Same arrangement as the
                // `for` case below.
                em.emit(OP_SCOPE_ENTER); em.scopeDepth++;
                compileBlock(em, s.body);
                em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
                auto& LR = loops.back();   // Break/Continue mutate this very element
                for (int32_t cj : LR.contJumps) em.at(cj).a = top - (int32_t)cj;
                em.emit(OP_JUMP, top - em.here());
                em.at(jF).a = em.here() - jF;
                for (int32_t bj : LR.breakJumps) em.at(bj).a = em.here() - bj;
                loops.pop_back();
                break;
            }

            case StKind::For: {
                if (!s.pat || s.pat->kind != ast::PatKind::Bind || s.pat->bindName.empty())
                    { em.f.interpreted = true; break; }
                int32_t bindName = em.strId(s.pat->bindName);
                compileExpr(em, *s.exprs[0]);
                em.emit(OP_ITER_BEGIN);
                int32_t top = em.here();
                LoopCtx L; L.scopeDepthAtLoop = em.scopeDepth; L.top = top; L.label = s.label;
                loops.push_back(L);
                em.emit(OP_SCOPE_ENTER); em.scopeDepth++;
                em.emit(OP_ITER_NEXT);
                int32_t jDone0 = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                int32_t lv = slotUse_ ? slotOf(s.pat->bindName) : -1;
                if (lv >= 0) em.emit(OP_ITER_VALUE_LOCAL, lv);
                else em.emit(OP_ITER_VALUE_VAR, bindName);
                compileBlock(em, s.body);
                em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
                auto& LR = loops.back();   // Break/Continue mutate this very element
                for (int32_t cj : LR.contJumps) em.at(cj).a = top - (int32_t)cj;
                em.emit(OP_JUMP, top - em.here());
                em.at(jDone0).a = em.here() - jDone0;
                em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
                for (int32_t bj : LR.breakJumps) em.at(bj).a = em.here() - bj;
                em.emit(OP_ITER_END);
                loops.pop_back();
                break;
            }

            case StKind::FuncDef:
                // Nested named functions capture by reference and are bound by
                // the tree-walker; out of the VM core slice -> fall back.
                em.f.interpreted = true;
                break;
            case StKind::Try:
                // try/catch is exception-aware control flow: run the
                // tree-walker (same policy as Raise/Match/Select/Defer).
                em.f.interpreted = true;
                break;
            case StKind::With:
                // context-manager enter/exit, exception-aware: tree-walker.
                em.f.interpreted = true;
                break;
            case StKind::Using:
                // block-scoped drop, a scope-exit hook: tree-walker.
                em.f.interpreted = true;
                break;
            default: em.f.interpreted = true; break;
        }
    }

      // Compile the clause chain of a comprehension/generator. `mode` selects
      // the accumulator kind (0=list, 1=set, 2=dict) and is what OP_APPEND uses.
      // The accumulator is pushed before the first clause and is the sole value
      // left when the chain finishes, so no hidden frame local is needed.
      void compileCompClauses(Emitter& em, const Expr& e, size_t idx, int mode) {
          if (em.f.interpreted) return;
          if (idx == e.clauses.size()) {
              if (mode == 2) {
                  if (e.elems.empty() || !e.value) { em.f.interpreted = true; em.emit(OP_NONEZ); return; }
                  compileExpr(em, *e.elems[0]);   // key
                  compileExpr(em, *e.value);      // value
              } else {
                  if (e.elems.empty()) { em.f.interpreted = true; em.emit(OP_NONEZ); return; }
                  compileExpr(em, *e.elems[0]);
              }
              em.emit(OP_APPEND, mode);
              return;
          }
          const ast::CompClause& cl = e.clauses[idx];
          if (cl.isFor) {
              if (!cl.iter) { em.f.interpreted = true; em.emit(OP_NONEZ); return; }
              compileExpr(em, *cl.iter);
              em.emit(OP_ITER_BEGIN);
              int32_t top = em.here();
              em.emit(OP_SCOPE_ENTER); em.scopeDepth++;
              em.emit(OP_ITER_NEXT);
              int32_t jDone = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
              if (!cl.pat || cl.pat->kind != ast::PatKind::Bind || cl.pat->bindName.empty()) {
                  em.f.interpreted = true; em.emit(OP_NONEZ); return;   // destructuring
              }
              int32_t s = slotUse_ ? slotOf(cl.pat->bindName) : -1;
              if (s >= 0) em.emit(OP_ITER_VALUE_LOCAL, s);
              else em.emit(OP_ITER_VALUE_VAR, em.strId(cl.pat->bindName));
              compileCompClauses(em, e, idx + 1, mode);
              if (em.f.interpreted) return;
              em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
              em.emit(OP_JUMP, top - em.here());
              em.at(jDone).a = em.here() - jDone;
              em.emit(OP_SCOPE_LEAVE); em.scopeDepth--;
              em.emit(OP_ITER_END);
          } else {
              if (!cl.cond) { em.f.interpreted = true; em.emit(OP_NONEZ); return; }
              compileExpr(em, *cl.cond);
              int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
              compileCompClauses(em, e, idx + 1, mode);
              if (em.f.interpreted) return;
              em.at(jF).a = em.here() - jF;
          }
      }

      void compileComp(Emitter& em, const Expr& e) {
          if (e.clauses.empty()) { em.f.interpreted = true; em.emit(OP_NONEZ); return; }
          int mode = e.kind == ExKind::SetComp ? 1 : e.kind == ExKind::DictComp ? 2 : 0;
          if (mode == 1) em.emit(OP_MAKE_SET, 0);
          else if (mode == 2) em.emit(OP_MAKE_DICT, 0);
          else em.emit(OP_MAKE_LIST, 0);
          compileCompClauses(em, e, 0, mode);
      }

      // Load/store a plain name, choosing the slot path when the name owns a
      // frame slot and the env path otherwise. Used by the M1/M3 sugar lowering
      // so hidden temps and walrus targets read back consistently.
      void loadName(Emitter& em, const std::string& n) {
          int32_t s = slotUse_ ? slotOf(n) : -1;
          if (s >= 0) em.emit(OP_LOAD_LOCAL, s);
          else em.emit(OP_LOAD, em.strId(n));
      }
      void storeName(Emitter& em, const std::string& n) {
          int32_t s = slotUse_ ? slotOf(n) : -1;
          if (s >= 0) em.emit(OP_STORE_LOCAL, s);
          else em.emit(OP_STORE, em.strId(n));
      }

      void emitStore(Emitter& em, const Expr& t) {
          if (t.kind != ExKind::Ident) { em.f.interpreted = true; return; }
          int32_t s = slotUse_ ? slotOf(t.text) : -1;
          if (s >= 0) em.emit(OP_STORE_LOCAL, s);
          else em.emit(OP_STORE, em.strId(t.text));
      }

      // `var` / `const` / `local` bind in the CURRENT frame; `=` / `+=` write
      // through to the nearest enclosing binding. Conflating them is what made
      // the fast path disagree with the tree-walker on shadowing.
      void emitDefine(Emitter& em, const Expr& t) {
          if (t.kind != ExKind::Ident) { em.f.interpreted = true; return; }
          // Slots are per-function frame storage, so a define into a slot is
          // already "the current binding" and needs no distinct opcode. planSlots
          // disables slots outright when a name is redeclared in a nested scope,
          // so a shadowing define can never reach the slot path.
          int32_t s = slotUse_ ? slotOf(t.text) : -1;
          if (s >= 0) em.emit(OP_STORE_LOCAL, s);
          else em.emit(OP_DEFINE, em.strId(t.text));
      }

    void compileExpr(Emitter& em, const Expr& e) {
        if (em.f.interpreted) return;
        switch (e.kind) {
            case ExKind::Int: em.emit(OP_INT, em.constId(Value::integer(kirn::parseIntLiteral(e.text)))); break;
            case ExKind::Float: em.emit(OP_FLOAT, em.constId(Value::floating(kirn::parseFloatLiteral(e.text)))); break;
            case ExKind::CharLit: em.emit(OP_CHAR, em.constId(Value::chr(decodeCharText(e.text)))); break;
            case ExKind::Str: {
                switch (e.flavor) {
                    case ast::StrFlavor::Raw:  em.emit(OP_STR_RAW,   em.constId(Value::str(e.text))); break;
                    case ast::StrFlavor::Byte: em.emit(OP_STR_BYTES, em.constId(Value::str(decodeEscapes(e.text)))); break;
                    case ast::StrFlavor::C:    em.emit(OP_STR_C,     em.constId(Value::str(decodeEscapes(e.text)))); break;
                    default:                   em.emit(OP_STR,       em.constId(Value::str(decodeEscapes(e.text)))); break;
                }
                break;
            }
            case ExKind::Ident: {
                if (e.text == "true") { em.emit(OP_TRUE); break; }
                if (e.text == "false") { em.emit(OP_FALSE); break; }
                if (e.text == "none") { em.emit(OP_NONEZ); break; }
                int32_t s = slotUse_ ? slotOf(e.text) : -1;
                if (s >= 0) em.emit(OP_LOAD_LOCAL, s);
                else em.emit(OP_LOAD, em.strId(e.text));
                break;
            }
            case ExKind::Unary: {
                if (e.op == "try") { compileExpr(em, *e.rhs); em.emit(OP_TRY); break; }
                if (e.op == "spawn") { em.f.interpreted = true; em.emit(OP_NONEZ); break; }
                compileExpr(em, *e.rhs);
                if (e.op == "-") em.emit(OP_NEG);
                else if (e.op == "not") em.emit(OP_NOT);
                else em.emit(OP_UNARY, em.strId(e.op));
                break;
            }
            case ExKind::Binary: {
                const std::string& op = e.op;
                if (op == "and" || op == "or") {
                    compileExpr(em, *e.lhs);
                    if (op == "and") {
                        int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                        compileExpr(em, *e.rhs);
                        em.emit(OP_BOOL);
                        int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                        em.at(jF).a = em.here() - jF;
                        em.emit(OP_FALSE);
                        em.at(jE).a = em.here() - jE;
                    } else {
                        int32_t jT = em.here(); em.emit(OP_JUMP_IF_TRUE, 0);
                        compileExpr(em, *e.rhs);
                        em.emit(OP_BOOL);
                        int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                        em.at(jT).a = em.here() - jT;
                        em.emit(OP_TRUE);
                        em.at(jE).a = em.here() - jE;
                    }
                    break;
                }
                compileExpr(em, *e.lhs);
                if (op == "is") {
                    if (e.rhs->kind != ExKind::Ident) { em.f.interpreted = true; break; }
                    em.emit(OP_IS, e.rhs->text == "none" ? -1 : em.strId(e.rhs->text));
                    break;
                }
                if (op == "in") { compileExpr(em, *e.rhs); em.emit(OP_IN); break; }
                if ((op == ".." || op == "..=") && !e.rhs) { em.emit(OP_UNARANGE); break; }
                if ((op == ".." || op == "..=")) { compileExpr(em, *e.rhs); em.emit(OP_RANGE, op == "..=" ? 1 : 0); break; }
                if (isCmpOp(op) && e.lhs->kind == ExKind::Binary && isCmpOp(e.lhs->op)) {
                    em.f.interpreted = true; break;   // chained comparison
                }
                {
                    uint16_t spOp = specializedBinaryOp(op);
                    compileExpr(em, *e.rhs);
                    if (spOp) em.emit(spOp);
                    else em.emit(OP_BINARY, em.strId(op));
                }
                break;
            }
            case ExKind::Member: {
                if (isNonLocalReceiver(*e.lhs)) { em.f.interpreted = true; break; }
                compileExpr(em, *e.lhs);
                em.emit(OP_MEMBER, em.strId(e.text), e.nilSafe ? 1 : 0);
                break;
            }
            case ExKind::Index: {
                compileExpr(em, *e.lhs);
                ast::Expr* rhs = e.rhs.get();
                if (rhs->kind == ExKind::Binary && (rhs->op == ".." || rhs->op == "..=")) {
                    compileExpr(em, *rhs->lhs);
                    if (rhs->rhs) compileExpr(em, *rhs->rhs); else em.emit(OP_NONEZ);
                    em.emit(OP_SLICE, rhs->op == "..=" ? 1 : 0);
                    break;
                }
                compileExpr(em, *rhs);
                em.emit(OP_INDEX);
                break;
            }
            case ExKind::Slice: {
                compileExpr(em, *e.lhs);
                for (size_t i = 0; i < 3; ++i) {
                    if (i < e.elems.size() && e.elems[i]) compileExpr(em, *e.elems[i]);
                    else em.emit(OP_NONEZ);
                }
                em.emit(OP_SLICE3);
                break;
            }
            case ExKind::Call: compileCall(em, e); break;
            case ExKind::Cond: {
                compileExpr(em, *e.cond);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                compileExpr(em, *e.lhs);
                int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                em.at(jF).a = em.here() - jF;
                compileExpr(em, *e.rhs);
                em.at(jE).a = em.here() - jE;
                break;
            }
            case ExKind::List: case ExKind::Set: case ExKind::Tuple: {
                for (const auto& el : e.elems) compileExpr(em, *el);
                if (e.kind == ExKind::List) em.emit(OP_MAKE_LIST, (int32_t)e.elems.size());
                else if (e.kind == ExKind::Set) em.emit(OP_MAKE_SET, (int32_t)e.elems.size());
                else em.emit(OP_MAKE_TUPLE, (int32_t)e.elems.size());
                break;
            }
            case ExKind::Dict: {
                for (const auto& pr : e.pairs) { compileExpr(em, *pr.first); compileExpr(em, *pr.second); }
                em.emit(OP_MAKE_DICT, (int32_t)e.pairs.size());
                break;
            }
            case ExKind::ListComp: case ExKind::Generator:
            case ExKind::SetComp: case ExKind::DictComp:
                compileComp(em, e);
                break;
            case ExKind::New: case ExKind::Cast: case ExKind::Lambda:
            case ExKind::FString: case ExKind::Spread:
            case ExKind::Comptime:
                em.f.interpreted = true;
                em.emit(OP_NONEZ);
                break;
            // M1 walrus `(x := E)`: eval once, write the name, leave the value.
            case ExKind::AssignExpr: {
                if (!e.lhs || e.lhs->kind != ExKind::Ident || !e.rhs) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                compileExpr(em, *e.rhs);
                storeName(em, e.lhs->text);
                loadName(em, e.lhs->text);
                break;
            }
            // `%` inside a pipe body: load the enclosing pipe's hidden temp.
            case ExKind::Topic: {
                if (curTopic_.empty()) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                loadName(em, curTopic_);
                break;
            }
            // M1 pipe `a |> body`: bind `%` to a temp, then eval the body.
            case ExKind::Pipe: {
                auto it = tmpOf_.find(&e);
                if (it == tmpOf_.end() || !e.lhs || !e.rhs) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                const std::string& t = it->second;
                compileExpr(em, *e.lhs);
                storeName(em, t);
                std::string saved = curTopic_;
                curTopic_ = t;
                compileExpr(em, *e.rhs);
                curTopic_ = saved;
                break;
            }
            // M1 elvis `a ?? b`: `(t is none) ? b : t`, short-circuiting.
            case ExKind::Coalesce: {
                auto it = tmpOf_.find(&e);
                if (it == tmpOf_.end() || !e.lhs || !e.rhs) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                const std::string& t = it->second;
                compileExpr(em, *e.lhs);
                storeName(em, t);
                loadName(em, t);
                em.emit(OP_IS, -1);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                compileExpr(em, *e.rhs);
                int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                em.at(jF).a = em.here() - jF;
                loadName(em, t);
                em.at(jE).a = em.here() - jE;
                break;
            }
            // M3 `a.?[i]`: `(t is none) ? none : t[i]`.
            case ExKind::SafeIndex: {
                auto it = tmpOf_.find(&e);
                if (it == tmpOf_.end() || !e.lhs || !e.rhs) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                const std::string& t = it->second;
                compileExpr(em, *e.lhs);
                storeName(em, t);
                loadName(em, t);
                em.emit(OP_IS, -1);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                em.emit(OP_NONEZ);
                int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                em.at(jF).a = em.here() - jF;
                loadName(em, t);
                compileExpr(em, *e.rhs);
                em.emit(OP_INDEX);
                em.at(jE).a = em.here() - jE;
                break;
            }
            // M3 `a.?(args)`: `(t is none) ? none : t(args)`.
            case ExKind::SafeCall: {
                auto it = tmpOf_.find(&e);
                if (it == tmpOf_.end() || !e.lhs) {
                    em.f.interpreted = true; em.emit(OP_NONEZ); break;
                }
                const std::string& t = it->second;
                compileExpr(em, *e.lhs);
                storeName(em, t);
                loadName(em, t);
                em.emit(OP_IS, -1);
                int32_t jF = em.here(); em.emit(OP_JUMP_IF_FALSE, 0);
                em.emit(OP_NONEZ);
                int32_t jE = em.here(); em.emit(OP_JUMP, 0);
                em.at(jF).a = em.here() - jF;
                loadName(em, t);
                for (const auto& a : e.args) compileExpr(em, *a.value);
                em.emit(OP_CALL, (int32_t)e.args.size());
                em.at(jE).a = em.here() - jE;
                break;
            }
            default:
                em.f.interpreted = true;
                em.emit(OP_NONEZ);
                break;
        }
    }

    bool isNonLocalReceiver(const Expr& recv) {
        // A bare Ident receiver is only safe when it is a local value (var/param)
        // or a known constant primitive. Module/global-struct receivers are not.
        if (recv.kind == ExKind::Ident) {
            const std::string& n = recv.text;
            if (n == "true" || n == "false" || n == "none") return false;
            if (kModules().count(n)) return true;          // math.* etc.
            if (ls.isLocal(n)) return false;               // local value receiver
            return true;                                   // global/unknown/module
        }
        return false;   // nested expression receiver is a value -> core
    }

    void compileCall(Emitter& em, const Expr& e) {
        if (e.lhs->kind == ExKind::Member) {
            if (isNonLocalReceiver(*e.lhs->lhs)) { em.f.interpreted = true; return; }
            compileExpr(em, *e.lhs->lhs);
            for (const auto& a : e.args) compileExpr(em, *a.value);
            em.emit(OP_MEMBER_METHOD, em.strId(e.lhs->text),
                    (int32_t)e.args.size(), e.lhs->nilSafe ? 1 : 0);
            return;
        }
        if (e.lhs->kind == ExKind::Ident) {
            const std::string& name = e.lhs->text;
            bool safeCall = userFuncs.count(name) > 0 || kBuiltins().count(name) > 0;
            if (!safeCall) { em.f.interpreted = true; return; }   // struct/enum/unknown
            for (const auto& a : e.args) compileExpr(em, *a.value);
            em.emit(OP_CALL_NAME, em.strId(name), (int32_t)e.args.size());
            return;
        }
        compileExpr(em, *e.lhs);
        for (const auto& a : e.args) compileExpr(em, *a.value);
        em.emit(OP_CALL, (int32_t)e.args.size());
    }

};

} // namespace

CompileResult compileProgram(const std::vector<StmtP>& body,
                             const ast::Stmt* entry,
                             const std::unordered_set<std::string>& userFuncs,
                             const std::unordered_set<std::string>& /*builtins*/,
                             const std::unordered_set<std::string>& /*modules*/) {
    CompileResult res;
    Compiler c(res, userFuncs);
    if (entry) {
        int32_t idx = c.compileFunction(entry->name, entry->params, entry->body,
                                        entry->ret != nullptr);
        res.defIdx[entry] = idx;
        res.prog.mainFn = entry;
    }
    for (const auto& sp : body) {
        if (sp->kind == StKind::FuncDef && sp.get() != entry && !sp->externDef) {
            int32_t idx = c.compileFunction(sp->name, sp->params, sp->body,
                                            sp->ret != nullptr);
            res.defIdx[sp.get()] = idx;
        }
    }
    return res;
}

} // namespace vm
} // namespace kirn
