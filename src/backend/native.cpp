// Implementation of the Phase 8.2 scalar-native backend. See native.h.
#include "backend/native.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace kirn {
namespace backend {

using ast::ExKind;
using ast::StKind;
using sema::TyK;
using sema::TyP;

bool isScalarTy(const TyP& t) {
    if (!t) return false;
    switch (t->k) {
        case TyK::Int:
        case TyK::Float:
        case TyK::Bool:
            return true;
        default:
            return false;
    }
}

static bool isNoneTy(const TyP& t) { return !t || t->k == TyK::None; }

// M11: an optional of a scalar lowers to `std::optional<scalar>`. The checker
// stores an optional's element in `args[0]` (see `optTy`/`unwrapOpt`).
static const TyP& optInner(const TyP& t) {
    static const TyP kNull;
    return (t && t->k == TyK::Opt && !t->args.empty()) ? t->args[0] : kNull;
}
static bool isOptScalarTy(const TyP& t) {
    return t && t->k == TyK::Opt && optInner(t) && isScalarTy(optInner(t));
}

// The set of types the native scalar backend can lower: plain scalars plus
// optionals of scalars (elvis `??` short-circuits to the inner scalar).
static bool isNativeTy(const TyP& t) { return isScalarTy(t) || isOptScalarTy(t); }

// M11: value type `v` is storable into target type `t`. Scalars-to-optional
// widen (and `none` stores into any optional); otherwise kinds must match.
static bool assignCompatible(const TyP& v, const TyP& t) {
    if (!v || !t) return false;
    if (v->k == t->k) return true;
    if (t->k == TyK::Opt) {
        if (v->k == TyK::None) return true;
        const TyP& in = optInner(t);
        return in && v->k == in->k;
    }
    return false;
}

// M11 const-fn folding: a module-level `const NAME = <evaluable>` is folded to a
// literal by the checker. When the folded value is a scalar we inline it at its
// use sites, so a function that reads the constant stays in the native subset
// instead of falling back to the interpreter. Non-scalar (Str/collection/struct)
// constants are not inlined and keep the interpreter fallback.
static const sema::ConstVal* scalarConst(const sema::Checker& chk,
                                         const std::string& name, TyP& out) {
    const sema::ConstVal* cv = chk.constValue(name);
    if (!cv) return nullptr;
    switch (cv->k) {
        case sema::ConstVal::K::Int:   out = sema::intTy();   return cv;
        case sema::ConstVal::K::Float: out = sema::floatTy(); return cv;
        case sema::ConstVal::K::Bool:  out = sema::boolTy();  return cv;
        case sema::ConstVal::K::None:  out = sema::noneTy();  return cv;
        default:                       return nullptr;
    }
}

static std::string scalarConstLiteral(const sema::ConstVal& cv) {
    switch (cv.k) {
        case sema::ConstVal::K::Int:
            return std::to_string(cv.i);
        case sema::ConstVal::K::Float: {
            std::ostringstream os;
            os << std::setprecision(17) << cv.f;
            std::string s = os.str();
            if (s.find_first_of(".eE") == std::string::npos) s += ".0";
            return s;
        }
        case sema::ConstVal::K::Bool:  return cv.b ? "true" : "false";
        case sema::ConstVal::K::None:  return "std::nullopt";
        default:                       return "0";
    }
}

// C++ scalar type name for a scalar/optional-scalar TyP; "" for anything else.
static std::string cppTy(const TyP& t) {
    if (!t) return "";
    switch (t->k) {
        case TyK::Int:   return "int64_t";
        case TyK::Float: return "double";
        case TyK::Bool:  return "bool";
        case TyK::Char:  return "char32_t";
        case TyK::Opt: {
            std::string in = cppTy(optInner(t));
            if (in.empty()) return "";
            return "std::optional<" + in + ">";
        }
        default:         return "";
    }
}

// Resolve a declaration Type node to a scalar (or optional-scalar) TyP (without
// the checker, whose resolveType is private). Only plain names and `T?` map.
static TyP declTy(const ast::TypeP& t) {
    if (!t) return sema::unkTy();
    if (t->kind == ast::TyKind::Optional) return sema::optTy(declTy(t->inner));
    if (t->kind != ast::TyKind::Name) return sema::unkTy();
    const std::string& n = t->name;
    static const char* kInts[] = {"i4", "i8", "i16", "i32", "i64", "i128",
                                  "i256", "u4", "u8", "u16", "u32", "u64",
                                  "u128", "u256", "isize", "usize",
                                  "Int4", "Int8", "Int16", "Int32", "Int64",
                                  "Int128", "Int256", "UInt4", "UInt8",
                                  "UInt16", "UInt32", "UInt64", "UInt128",
                                  "UInt256", "ISize", "USize", nullptr};
    for (int i = 0; kInts[i]; ++i)
        if (n == kInts[i]) return sema::intTy();
    if (n == "Int" || n == "UInt" || n == "uint" || n == "int")
        return sema::intTy();
    static const char* kFloats[] = {"f4", "f8", "f16", "f32", "f64", "f128",
                                    "f256", "uf4", "uf8", "uf16", "uf32",
                                    "uf64", "uf128", "uf256",
                                    "Float4", "Float8", "Float16", "Float32",
                                    "Float64", "Float128", "Float256",
                                    "UFloat4", "UFloat8", "UFloat16",
                                    "UFloat32", "UFloat64", "UFloat128",
                                    "UFloat256", nullptr};
    for (int i = 0; kFloats[i]; ++i)
        if (n == kFloats[i]) return sema::floatTy();
    if (n == "Float" || n == "UFloat" || n == "float") return sema::floatTy();
    if (n == "Bool" || n == "bool") return sema::boolTy();
    static const char* kBools[] = {"b8", "b16", "b32", "Bool8", "Bool16",
                                   "Bool32", nullptr};
    for (int i = 0; kBools[i]; ++i)
        if (n == kBools[i]) return sema::boolTy();
    static const char* kChars[] = {"char8", "char16", "char32", "Char8",
                                   "Char16", "Char32", nullptr};
    for (int i = 0; kChars[i]; ++i)
        if (n == kChars[i]) return sema::charTy();
    if (n == "Str" || n == "str" || n == "String" || n == "string" ||
        n == "strings")
        return sema::strTy();
    if (n == "bytes") return sema::bytesTy();
    return sema::unkTy();
}

// Sanitize a Kirn identifier into a valid C identifier (locals prefixed v_).
static std::string cIdent(const std::string& s, const char* pre) {
    std::string r = pre;
    for (char c : s) r += (isalnum((unsigned char)c) ? c : '_');
    return r.empty() ? std::string("_") : r;
}

namespace {

// ---------------------------------------------------------------------------
// Analysis: fixed-point lowering of scalar functions.
// ---------------------------------------------------------------------------
class Analyzer {
public:
    Analyzer(const std::vector<ast::StmtP>& prog, const sema::Checker& chk)
        : prog_(&prog), chk_(&chk) {
        for (const auto& s : prog) {
            if (s->kind == StKind::FuncDef) topFuncs_[s->name] = s.get();
            // Module-level bindings: a read of one has no function-local to
            // reference, and a write to one is not a local declaration, so a
            // function touching it must fall back to the interpreter.
            if ((s->kind == StKind::VarDecl || s->kind == StKind::ConstDecl) &&
                s->target && s->target->kind == ExKind::Ident)
                moduleNames_.insert(s->target->text);
        }
    }

    std::unordered_map<const ast::Stmt*, NativeFunc> lower() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& s : *prog_) {
                if (s->kind != StKind::FuncDef) continue;
                if (lowered_.count(s.get())) continue;
                if (tryLower(*s)) changed = true;
            }
        }
        return lowered_;
    }

    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered() const {
        return lowered_;
    }

private:
    const std::vector<ast::StmtP>* prog_;
    const sema::Checker* chk_;
    std::unordered_map<std::string, const ast::Stmt*> topFuncs_;
    std::unordered_map<const ast::Stmt*, NativeFunc> lowered_;
    std::set<std::string> moduleNames_;

    TyP typeOf(const ast::Expr& e) const { return chk_->typeOf(e); }

    bool tryLower(const ast::Stmt& s) {
        if (s.typeParams.size() || s.externDef || s.body.empty()) return false;
        if (s.ret && s.ret->kind == ast::TyKind::Name &&
            (s.ret->name == "result" || s.ret->name == "Result"))
            return false;
        TyP rt = s.ret ? declTy(s.ret) : sema::noneTy();
        if (!isNoneTy(rt) && !isNativeTy(rt)) return false;
        for (const auto& p : s.params) {
            if (p.variadic || p.selfParam || p.defaultValue || !p.type)
                return false;
            // Plain scalars and optionals of scalars (`T?`) are both lowerable;
            // an optional scalar becomes `std::optional<scalar>`.
            if (!isNativeTy(declTy(p.type))) return false;
        }
        // Self-recursion tolerance: an in-progress hook is added so the body
        // walk treats self-calls as lowerable. Non-self calls must already be
        // in `lowered_` (previous fixpoint rounds).
        CurFn cur{&s, rt, {}};
        for (const auto& p : s.params) cur.locals.insert(p.name);
        collectLocals(s.body, cur.locals);
        bool ok = bodyLowerable(s.body, cur);
        if (!ok) return false;

        NativeFunc nf;
        nf.fn = &s;
        nf.name = s.name;
        nf.cName = uniqueCName(s.name);
        nf.retCpp = (rt && rt->k != TyK::None) ? cppTy(rt) : std::string();
        for (const auto& p : s.params) {
            nf.params.push_back(p.name);
            nf.pTypes.push_back(cppTy(declTy(p.type)));
        }
        lowered_[&s] = std::move(nf);
        return true;
    }

    struct CurFn {
        const ast::Stmt* fn;
        TyP ret;
        std::set<std::string> locals;   // params + in-function bindings
    };

    // Every name the function binds itself (params are added by the caller).
    // A free identifier (module-level `const`/`var`, builtin, or a function
    // used as a value) is not emit-able by the scalar backend, so such a
    // function must fall back to the interpreter rather than emit C++ that
    // references an undefined `v_<name>`.
    void collectLocals(const std::vector<ast::StmtP>& body,
                       std::set<std::string>& out) const {
        for (const auto& s : body) collectLocalsStmt(*s, out);
    }

    void collectLocalsStmt(const ast::Stmt& s, std::set<std::string>& out) const {
        if ((s.kind == StKind::VarDecl || s.kind == StKind::ConstDecl) &&
            s.target && s.target->kind == ExKind::Ident &&
            !s.target->text.empty())
            out.insert(s.target->text);
        // A bare `x = e;` (or `x += e;`) binds `x` as a local unless it names a
        // module-level binding (which the emitter cannot reference).
        if ((s.kind == StKind::Assign || s.kind == StKind::AugAssign) &&
            !s.exprs.empty() && s.exprs[0] &&
            s.exprs[0]->kind == ExKind::Ident &&
            !moduleNames_.count(s.exprs[0]->text))
            out.insert(s.exprs[0]->text);
        if (s.kind == StKind::For && s.pat &&
            s.pat->kind == ast::PatKind::Bind && !s.pat->bindName.empty())
            out.insert(s.pat->bindName);
        for (const auto& x : s.exprs) if (x) collectLocalsExpr(*x, out);
        if (s.value) collectLocalsExpr(*s.value, out);
        if (s.cond) collectLocalsExpr(*s.cond, out);
        for (const auto& st : s.body) collectLocalsStmt(*st, out);
        for (const auto& st : s.elseBody) collectLocalsStmt(*st, out);
    }

    void collectLocalsExpr(const ast::Expr& e, std::set<std::string>& out) const {
        if (e.kind == ExKind::AssignExpr && e.lhs &&
            e.lhs->kind == ExKind::Ident && !e.lhs->text.empty())
            out.insert(e.lhs->text);
        if (e.lhs) collectLocalsExpr(*e.lhs, out);
        if (e.rhs) collectLocalsExpr(*e.rhs, out);
        if (e.value) collectLocalsExpr(*e.value, out);
        if (e.cond) collectLocalsExpr(*e.cond, out);
        for (const auto& x : e.elems) if (x) collectLocalsExpr(*x, out);
        for (const auto& p : e.pairs) {
            if (p.first) collectLocalsExpr(*p.first, out);
            if (p.second) collectLocalsExpr(*p.second, out);
        }
        for (const auto& a : e.args) if (a.value) collectLocalsExpr(*a.value, out);
        for (const auto& c : e.clauses) {
            if (c.iter) collectLocalsExpr(*c.iter, out);
            if (c.cond) collectLocalsExpr(*c.cond, out);
        }
    }

    bool callLowerable(const ast::Expr& e, const CurFn& cur) const {
        if (!e.lhs || e.lhs->kind != ExKind::Ident) return false;
        const std::string& callee = e.lhs->text;
        // self-call: lowerable if it's the current function (scalar args).
        if (cur.fn && cur.fn->name == callee) {
            for (const auto& a : e.args)
                if (a.name != "" || !a.value || !exprLowerable(*a.value, cur))
                    return false;
            return true;
        }
        auto it = topFuncs_.find(callee);
        if (it == topFuncs_.end()) return false;
        auto lf = lowered_.find(it->second);
        if (lf == lowered_.end()) return false;
        if (e.args.size() != lf->second.params.size()) return false;
        for (const auto& a : e.args)
            if (a.name != "" || !a.value || !exprLowerable(*a.value, cur))
                return false;
        return true;
    }

    bool exprLowerable(const ast::Expr& e, const CurFn& cur) const {
        TyP t = typeOf(e);
        switch (e.kind) {
            case ExKind::Int:
            case ExKind::Float:
                return true;
            case ExKind::Ident:
                // `none` is only lowerable in an optional-scalar slot; the
                // surrounding statement/return checks the target type.
                if (e.text == "none") return isNoneTy(t);
                if (e.text == "true" || e.text == "false") return true;
                // Only a name the function itself binds can be emitted; a free
                // name (module-level const/var, etc.) has no local to reference.
                if (cur.locals.count(e.text)) return isNativeTy(t);
                // M11: a module-level `const` folded to a scalar is inlined at
                // its use site, so a function that reads it can still lower.
                {
                    TyP ct;
                    if (scalarConst(*chk_, e.text, ct)) return isNativeTy(t);
                }
                return false;
            case ExKind::Unary:
                if (e.rhs && !exprLowerable(*e.rhs, cur)) return false;
                return (e.op == "-" || e.op == "+" || e.op == "not") &&
                       isScalarTy(t);
            case ExKind::Binary:
                if (e.lhs && !exprLowerable(*e.lhs, cur)) return false;
                if (e.rhs && !exprLowerable(*e.rhs, cur)) return false;
                return binopLowerable(e, t);
            case ExKind::Call:
                return callLowerable(e, cur);
            // Walrus `(x := E)`: eval-once into the named scalar local. The
            // target is declared by the emitter (see Emitter::collectWalrus).
            case ExKind::AssignExpr:
                return e.lhs && e.lhs->kind == ExKind::Ident && e.rhs &&
                       exprLowerable(*e.rhs, cur) && isScalarTy(t);
            // Pipe `a |> body` with `%` (Topic) naming the piped scalar.
            case ExKind::Topic:
                return isScalarTy(t);
            case ExKind::Pipe:
                return e.lhs && e.rhs && exprLowerable(*e.lhs, cur) &&
                       exprLowerable(*e.rhs, cur) && isScalarTy(t);
            // Elvis `??`: a scalar (or an optional of a scalar) short-circuits
            // to the inner scalar; `b` is only evaluated when `a` is none.
            case ExKind::Coalesce:
                if (!e.lhs || !e.rhs) return false;
                if (!isScalarTy(t)) return false;
                if (!exprLowerable(*e.lhs, cur)) return false;
                if (typeOf(*e.lhs) && typeOf(*e.lhs)->k == TyK::Opt) {
                    if (!exprLowerable(*e.rhs, cur)) return false;
                    if (!assignCompatible(typeOf(*e.rhs), t)) return false;
                }
                return true;
            default:
                return false;
        }
    }

    static bool binopLowerable(const ast::Expr& e, const TyP& t) {
        const std::string& op = e.op;
        if (op == "and" || op == "or" || op == "==" || op == "!=" ||
            op == "<" || op == "<=" || op == ">" || op == ">=")
            return t && t->k == TyK::Bool;
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "//" ||
            op == "%" || op == "**" || op == "<<" || op == ">>" || op == "&" ||
            op == "|" || op == "^")
            return isScalarTy(t);
        return false;
    }

    bool stmtLowerable(const ast::Stmt& s, const CurFn& cur) const {
        switch (s.kind) {
            case StKind::Pass:
            case StKind::Break:
            case StKind::Continue:
                return true;
            case StKind::VarDecl:
            case StKind::ConstDecl: {
                if (!s.value || !exprLowerable(*s.value, cur)) return false;
                TyP vt = typeOf(*s.value);
                if (s.declType) {
                    TyP dt = declTy(s.declType);
                    if (dt && dt->k != TyK::Unknown) vt = dt;
                }
                return isNativeTy(vt) && assignCompatible(typeOf(*s.value), vt);
            }
            case StKind::Assign:
            case StKind::AugAssign:
                return assignLowerable(s, cur);
            case StKind::Return: {
                if (s.exprs.empty()) return isNoneTy(cur.ret);
                if (s.exprs.size() != 1) return false;
                TyP vt = typeOf(*s.exprs[0]);
                bool ok = exprLowerable(*s.exprs[0], cur);
                if (cur.ret && cur.ret->k != TyK::None)
                    ok = ok && assignCompatible(vt, cur.ret);
                return ok;
            }
            case StKind::ExprStmt:
                return s.exprs.size() == 1 && s.exprs[0] &&
                       exprLowerable(*s.exprs[0], cur);
            case StKind::If:
                if (!s.exprs.empty() && s.exprs[0] &&
                    !exprLowerable(*s.exprs[0], cur))
                    return false;
                if (!bodyLowerable(s.body, cur)) return false;
                if (!bodyLowerable(s.elseBody, cur)) return false;
                return true;
            case StKind::While:
                return !s.exprs.empty() && s.exprs[0] &&
                       exprLowerable(*s.exprs[0], cur) &&
                       bodyLowerable(s.body, cur);
            case StKind::For:
                return forLowerable(s, cur);
            default:
                return false;
        }
    }

    bool forLowerable(const ast::Stmt& s, const CurFn& cur) const {
        if (s.exprs.size() != 1 || !s.exprs[0]) return false;
        if (!s.pat || s.pat->kind != ast::PatKind::Bind ||
            s.pat->bindName.empty())
            return false;
        const ast::Expr& it = *s.exprs[0];
        if (it.kind != ExKind::Binary || (it.op != ".." && it.op != "..="))
            return false;
        if (!it.lhs || !it.rhs) return false;
        if (!exprLowerable(*it.lhs, cur) || !exprLowerable(*it.rhs, cur))
            return false;
        if (typeOf(*it.lhs)->k != TyK::Int || typeOf(*it.rhs)->k != TyK::Int)
            return false;
        return bodyLowerable(s.body, cur);
    }

    bool assignLowerable(const ast::Stmt& s, const CurFn& cur) const {
        if (s.exprs.size() != 2) return false;
        const ast::ExprP& tgt = s.exprs[0];
        if (!tgt || tgt->kind != ExKind::Ident) return false;
        // A write to a module-level binding is not a local declaration; the
        // scalar backend has nowhere to put it.
        if (moduleNames_.count(tgt->text)) return false;
        TyP tt = typeOf(*tgt);
        if (!isNativeTy(tt)) return false;
        // `x ??= v` assigns only when the optional `x` is none; the value must
        // fit the element type. This is elvis, so it lowers on the scalar path.
        if (s.kind == StKind::AugAssign && s.augOp == "?" "?=") {
            if (!isOptScalarTy(tt)) return false;
            const ast::ExprP& v = s.exprs[1];
            if (!v || !exprLowerable(*v, cur)) return false;
            return assignCompatible(typeOf(*v), optInner(tt));
        }
        // Compound assignment (`+=`) only makes sense on a plain scalar.
        if (s.kind == StKind::AugAssign && !isScalarTy(tt)) return false;
        const ast::ExprP& val = s.exprs[1];
        if (!val || !exprLowerable(*val, cur)) return false;
        // Same kind keeps C++ semantics; scalars may widen into an optional.
        return assignCompatible(typeOf(*val), tt);
    }

    bool bodyLowerable(const std::vector<ast::StmtP>& body,
                       const CurFn& cur) const {
        for (const auto& s : body)
            if (!stmtLowerable(*s, cur)) return false;
        return true;
    }

    std::string uniqueCName(const std::string& name) {
        unsigned long idx = (unsigned long)(++cNameSeq_);
        return "co_native_" + std::to_string(idx) + "_" + cIdent(name, "");
    }
    int cNameSeq_ = 0;
};

// ---------------------------------------------------------------------------
// Emission: write native C++ for the lowered functions.
// ---------------------------------------------------------------------------
class Emitter {
public:
    Emitter(std::ostream& out,
            const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered,
            const sema::Checker& chk)
        : out_(out), lowered_(&lowered), chk_(&chk) {}

    void emitPreamble() {
        out_ << "#include <cmath>\n"
             << "#include <optional>\n"
             << "namespace kirn_native {\n"
             << "static inline void co_panic(const char* m){ "
                "kirn::interp::panicHere(m); }\n"
             << "static inline int64_t co_floordiv(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"division by zero\"); int64_t q=a/b;"
                " if((a%b!=0)&&((a<0)!=(b<0)))--q; return q; }\n"
             << "static inline int64_t co_mod(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"modulo by zero\"); int64_t m=a%b;"
                " if(m!=0&&((m<0)!=(b<0)))m+=b; return m; }\n"
             << "static inline double co_fdiv(double a,double b){"
                " if(b==0.0)co_panic(\"division by zero\"); return a/b; }\n"
             << "static inline double co_fmodm(double a,double b){"
                " return std::fmod(std::fmod(a,b)+b,b); }\n"
             << "static inline int64_t co_ipow(int64_t b,int64_t e){"
                " int64_t o=1; while(e>0){ if(e&1)o*=b; e>>=1; if(e)b*=b;}"
                " return o; }\n"
             << "static inline int64_t co_shl(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return (int64_t)((uint64_t)a<<b); }\n"
             << "static inline int64_t co_shr(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return a>>b; }\n";
    }

    void emitFunctions(const std::vector<ast::StmtP>& prog) {
        for (const auto& s : prog) {
            if (s->kind != StKind::FuncDef) continue;
            auto it = lowered_->find(s.get());
            if (it != lowered_->end()) emitFunction(*s, it->second);
        }
        out_ << "}\n";  // close namespace kirn_native
    }

private:
    std::ostream& out_;
    const std::unordered_map<const ast::Stmt*, NativeFunc>* lowered_;
    const sema::Checker* chk_;
    const NativeFunc* cur_ = nullptr;
    std::unordered_map<std::string, std::string> localTypes_;  // name -> cpp type
    std::unordered_map<std::string, const NativeFunc*> calleeFunc_;
    int tmpSeq_ = 0;              // unique C++ temp names for sugar lowering
    std::string pipeTmp_;         // current `%` binding while emitting a pipe
    std::vector<std::string> predeclared_;  // walrus targets hoisted to top

    TyP typeOf(const ast::Expr& e) const { return chk_->typeOf(e); }

    // Collect `(x := ...)` binding names reachable from an expression so the
    // emitter can hoist their scalar declarations to the function top. Walrus
    // is an expression in this language, so the binding site may be a condition
    // or an argument; a top-level declaration keeps the emitted assignment in
    // scope without a statement slot.
    void collectWalrusExpr(const ast::Expr& e) {
        if (e.kind == ExKind::AssignExpr && e.lhs &&
            e.lhs->kind == ExKind::Ident) {
            const std::string& n = e.lhs->text;
            if (!localTypes_.count(n)) {
                std::string ty = cppTy(typeOf(*e.rhs));
                if (!ty.empty()) {
                    localTypes_[n] = ty;
                    predeclared_.push_back(n);
                }
            }
        }
        if (e.lhs) collectWalrusExpr(*e.lhs);
        if (e.rhs) collectWalrusExpr(*e.rhs);
        if (e.value) collectWalrusExpr(*e.value);
        if (e.cond) collectWalrusExpr(*e.cond);
        for (const auto& x : e.elems) if (x) collectWalrusExpr(*x);
        for (const auto& p : e.pairs) {
            if (p.first) collectWalrusExpr(*p.first);
            if (p.second) collectWalrusExpr(*p.second);
        }
        for (const auto& a : e.args) if (a.value) collectWalrusExpr(*a.value);
        for (const auto& c : e.clauses) {
            if (c.iter) collectWalrusExpr(*c.iter);
            if (c.cond) collectWalrusExpr(*c.cond);
        }
    }

    void collectWalrusStmt(const ast::Stmt& s) {
        for (const auto& x : s.exprs) if (x) collectWalrusExpr(*x);
        if (s.value) collectWalrusExpr(*s.value);
        if (s.cond) collectWalrusExpr(*s.cond);
        for (const auto& st : s.body) collectWalrusStmt(*st);
        for (const auto& st : s.elseBody) collectWalrusStmt(*st);
    }

    void emitFunction(const ast::Stmt& s, const NativeFunc& nf) {
        cur_ = &nf;
        localTypes_.clear();
        tmpSeq_ = 0;
        pipeTmp_.clear();
        predeclared_.clear();
        for (size_t i = 0; i < nf.params.size(); ++i)
            localTypes_[nf.params[i]] = nf.pTypes[i];
        for (auto& [name, fn] : *lowered_) calleeFunc_[fn.name] = &fn;
        // scalar-core signature: plain params -> plain scalar return. The
        // registration thunk (out_registerAll) only boxes/unboxes at the edges;
        // native->native calls pass scalars straight through.
        std::string sig;
        for (size_t i = 0; i < nf.params.size(); ++i)
            sig += (i ? ", " : "") + nf.pTypes[i] + " " +
                   cIdent(nf.params[i], "v_");
        out_ << "static " << (nf.retCpp.empty() ? "void" : nf.retCpp) << " "
             << nf.cName << "(" << sig << ") {\n";
        // Hoist scalar declarations for walrus `(x := E)` bindings so the
        // expression-level assignment in the body has a declared target.
        for (const auto& st : s.body) collectWalrusStmt(*st);
        for (const auto& n : predeclared_)
            out_ << "    " << localTypes_[n] << " " << cIdent(n, "v_")
                 << "{};\n";
        for (const auto& st : s.body) emitStmt(*st, 1);
        // implicit fallthrough return
        if (nf.retCpp.empty())
            out_ << "    return;\n";
        else
            out_ << "    return " << zeroOf(nf.retCpp) << ";\n";
        out_ << "}\n";
        cur_ = nullptr;
    }

    static const char* valueCtor(const std::string& ty) {
        return ty == "int64_t" ? "integer" : ty == "double" ? "floating"
                                                            : "boolean";
    }
    static const char* envField(const std::string& ty) {
        return ty == "int64_t" ? "i" : ty == "double" ? "d" : "b";
    }
    static const char* zeroOf(const std::string& ty) {
        if (ty.rfind("std::optional<", 0) == 0) return "std::nullopt";
        return ty == "int64_t" ? "0" : ty == "double" ? "0.0" : "false";
    }

    const NativeFunc* findCallee(const std::string& name) const {
        for (auto& [fn, nf] : *lowered_)
            if (nf.name == name) return &nf;
        return nullptr;
    }

    // ---- statements --------------------------------------------------------
    void emitStmt(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        switch (s.kind) {
            case StKind::Pass:
                return;
            case StKind::VarDecl:
            case StKind::ConstDecl: {
                TyP vt = typeOf(*s.value);
                if (s.declType) {
                    TyP dt = declTy(s.declType);
                    if (dt && dt->k != TyK::Unknown) vt = dt;
                }
                std::string ty = cppTy(vt);
                std::string name = cIdent(s.target->text, "v_");
                // A walrus binding of the same name was hoisted to the top of
                // the function; reuse it instead of redeclaring.
                bool hoisted = localTypes_.count(s.target->text) &&
                    std::find(predeclared_.begin(), predeclared_.end(),
                              s.target->text) != predeclared_.end();
                if (hoisted)
                    out_ << pad << name << " = " << emitExpr(*s.value)
                         << ";\n";
                else
                    out_ << pad << ty << " " << name << " = "
                         << emitExpr(*s.value) << ";\n";
                localTypes_[s.target->text] = ty;
                return;
            }
            case StKind::Assign:
            case StKind::AugAssign: {
                const std::string& cname = s.exprs[0]->text;
                std::string name = cIdent(cname, "v_");
                // Elvis-assign `x ??= v`: assign only when the optional `x` is
                // none, so the right-hand side is evaluated lazily.
                if (s.kind == StKind::AugAssign && s.augOp == "?" "?=") {
                    if (!localTypes_.count(cname))
                        localTypes_[cname] = cppTy(typeOf(*s.exprs[0]));
                    out_ << pad << "if (!" << name << ") " << name << " = "
                         << emitExpr(*s.exprs[1]) << ";\n";
                    return;
                }
                std::string val;
                if (s.kind == StKind::AugAssign) {
                    // rebuild target op value with binop semantics.
                    // augOp is a compound-assign ("-=" ...); strip the '=' so
                    // the binop dispatcher sees the plain operator. Binop ops
                    // are never comparisons, so a trailing '=' is unambiguous
                    // (and never one of the two-char relops ==/!=/<=/>=).
                    std::string op = s.augOp;
                    if (!op.empty() && op.back() == '=') op.pop_back();
                    val = emitBinop(op, *s.exprs[0], *s.exprs[1]);
                } else {
                    val = emitExpr(*s.exprs[1]);
                }
                auto lf = localTypes_.find(cname);
                if (lf != localTypes_.end()) {
                    out_ << pad << name << " = " << val << ";\n";
                } else {
                    // first (possibly implicit) binding of a mutable local:
                    // emit the declaration so later assignments stay plain.
                    const std::string ty = cppTy(typeOf(*s.exprs[0]));
                    localTypes_[cname] = ty;
                    out_ << pad << ty << " " << name << " = " << val << ";\n";
                }
                return;
            }
            case StKind::Return: {
                if (s.exprs.empty()) {
                    out_ << pad << "return;\n";
                } else {
                    out_ << pad << "return " << emitExpr(*s.exprs[0]) << ";\n";
                }
                return;
            }
            case StKind::ExprStmt: {
                out_ << pad << "(void)" << emitExpr(*s.exprs[0]) << ";\n";
                return;
            }
            case StKind::If:
                emitIf(s, ind);
                return;
            case StKind::While: {
                out_ << pad << "while (" << emitExpr(*s.exprs[0]) << ") {\n";
                for (const auto& st : s.body) emitStmt(*st, ind + 1);
                out_ << pad << "}\n";
                return;
            }
            case StKind::For:
                emitFor(s, ind);
                return;
            case StKind::Break:
                out_ << pad << "break;\n";
                return;
            case StKind::Continue:
                out_ << pad << "continue;\n";
                return;
            default:
                return;
        }
    }

    void emitIf(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        out_ << pad << "if (" << emitExpr(*s.exprs[0]) << ") {\n";
        for (const auto& st : s.body) emitStmt(*st, ind + 1);
        if (!s.elseBody.empty()) {
            out_ << pad << "} else {\n";
            for (const auto& st : s.elseBody) emitStmt(*st, ind + 1);
        }
        out_ << pad << "}\n";
    }

    void emitFor(const ast::Stmt& s, int ind) {
        std::string pad(4 * ind, ' ');
        const ast::Expr& it = *s.exprs[0];
        bool inclusive = it.op == "..=";
        std::string loopVar = cIdent(s.pat->bindName, "v_");
        std::string lo = emitExpr(*it.lhs);
        std::string hi = emitExpr(*it.rhs);
        std::string cmp = inclusive ? "<=" : "<";
        out_ << pad << "for (int64_t " << loopVar << " = (" << lo << "); "
             << loopVar << " " << cmp << " (" << hi << "); ++" << loopVar
             << ") {\n";
        localTypes_[s.pat->bindName] = "int64_t";
        for (const auto& st : s.body) emitStmt(*st, ind + 1);
        out_ << pad << "}\n";
    }

    // ---- expressions -------------------------------------------------------
    std::string emitExpr(const ast::Expr& e) {
        TyP t = typeOf(e);
        switch (e.kind) {
            case ExKind::Int: {
                try {
                    long long v = std::stoll(e.text);
                    return std::to_string(v);
                } catch (...) {
                    return "0";
                }
            }
            case ExKind::Float: {
                std::string s = e.text;
                if (s.find_first_of(".eE") == std::string::npos) s += ".0";
                return s;
            }
            case ExKind::Ident:
                if (e.text == "true") return "true";
                if (e.text == "false") return "false";
                if (e.text == "none") return "std::nullopt";
                // M11: a non-local scalar `const` is inlined as its folded
                // literal (a local of the same name shadows it).
                if (!localTypes_.count(e.text)) {
                    TyP ct;
                    if (const sema::ConstVal* cv =
                            scalarConst(*chk_, e.text, ct))
                        return scalarConstLiteral(*cv);
                }
                return cIdent(e.text, "v_");
            case ExKind::Unary: {
                std::string r = emitExpr(*e.rhs);
                if (e.op == "-") return "(-(" + r + "))";
                if (e.op == "+") return "(+(" + r + "))";
                if (e.op == "not") return "(!(" + r + "))";
                return r;
            }
            case ExKind::Binary:
                return emitBinop(e.op, *e.lhs, *e.rhs);
            case ExKind::Call: {
                const std::string& callee = e.lhs->text;
                const NativeFunc* cf = findCallee(callee);
                std::string s = cf->cName + "(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i) s += ", ";
                    s += emitExpr(*e.args[i].value);
                }
                return s + ")";
            }
            case ExKind::AssignExpr: {
                // `(x := E)` — eval-once into a temp, assign the (hoisted)
                // scalar local, yield the temp. IIFE keeps it expression-shaped.
                std::string tmp = "__w" + std::to_string(tmpSeq_++);
                std::string nm = cIdent(e.lhs->text, "v_");
                return "[&]{ auto " + tmp + " = (" + emitExpr(*e.rhs) +
                       "); " + nm + " = " + tmp + "; return " + tmp + "; }()";
            }
            case ExKind::Topic:
                // `%` — names the piped value bound by the enclosing Pipe IIFE.
                return pipeTmp_.empty() ? "0" : pipeTmp_;
            case ExKind::Pipe: {
                // `a |> body` — bind `%` to a temp, then emit the body with it.
                std::string tmp = "__pipe" + std::to_string(tmpSeq_++);
                std::string in = emitExpr(*e.lhs);
                std::string saved = pipeTmp_;
                pipeTmp_ = tmp;
                std::string body = emitExpr(*e.rhs);
                pipeTmp_ = saved;
                return "[&]{ " + cppTy(t) + " " + tmp + " = (" +
                       in + "); return (" + body + "); }()";
            }
            case ExKind::Coalesce: {
                // `a ?? b` — short-circuit: only evaluate `b` when `a` is
                // none. Non-optional `a` can never be none, so `b` is dropped.
                TyP lt = typeOf(*e.lhs);
                std::string l = emitExpr(*e.lhs);
                if (!lt || lt->k != TyK::Opt) return "(" + l + ")";
                std::string r = emitExpr(*e.rhs);
                std::string tmp = "__co" + std::to_string(tmpSeq_++);
                return "[&]{ auto " + tmp + " = (" + l + "); return (" + tmp +
                       ") ? (*" + tmp + ") : (" + r + "); }()";
            }
            default:
                return "0";
        }
    }

    std::string emitBinop(const std::string& op, const ast::Expr& l,
                          const ast::Expr& r) {
        TyP tl = typeOf(l), tr = typeOf(r);
        std::string le = emitExpr(l), re = emitExpr(r);
        bool lint = tl && tl->k == TyK::Int;
        bool rint = tr && tr->k == TyK::Int;
        bool lflt = tl && tl->k == TyK::Float;
        bool rflt = tr && tr->k == TyK::Float;
        bool mixed = (lint && rflt) || (lflt && rint);

        if (op == "and") return "(" + le + " && " + re + ")";
        if (op == "or") return "(" + le + " || " + re + ")";
        if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
            op == ">=") {
            if (mixed || lflt || rflt)
                return "((" + le + ") " + relOp(op) + " (" + re + "))";
            return "(" + le + " " + relOp(op) + " " + re + ")";
        }

        // arithmetic
        if (op == "+" || op == "-" || op == "*") {
            if (mixed) return "((" + le + ") " + op + " (" + re + "))";
            return "(" + le + " " + op + " " + re + ")";
        }
        if (op == "/") {
            return "kirn_native::co_fdiv((double)(" + le + "),(double)(" + re +
                   "))";
        }
        if (op == "//") {
            if (lint && rint) return "kirn_native::co_floordiv(" + le + "," + re + ")";
            return "std::floor((" + le + ")/(" + re + "))";
        }
        if (op == "%") {
            if (lint && rint) return "kirn_native::co_mod(" + le + "," + re + ")";
            return "kirn_native::co_fmodm((" + le + "),(" + re + "))";
        }
        if (op == "**") {
            if (lint && rint) return "kirn_native::co_ipow(" + le + "," + re + ")";
            return "std::pow((" + le + "),(" + re + "))";
        }
        if (op == "<<") return "kirn_native::co_shl(" + le + "," + re + ")";
        if (op == ">>") return "kirn_native::co_shr(" + le + "," + re + ")";
        if (op == "&") return "(" + le + " & " + re + ")";
        if (op == "|") return "(" + le + " | " + re + ")";
        if (op == "^") return "(" + le + " ^ " + re + ")";
        return "(" + le + ")";
    }

    static const char* relOp(const std::string& op) {
        if (op == "==") return "==";
        if (op == "!=") return "!=";
        if (op == "<=") return "<=";
        if (op == ">=") return ">=";
        if (op == "<") return "<";
        return ">";
    }
};

}  // namespace

NativeProgram funcsFrom(
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered);
void out_registerAll(
    std::ostream& out,
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered);

NativeProgram emitNative(std::ostream& out,
                         const std::vector<ast::StmtP>& prog,
                         const sema::Checker& chk) {
    Analyzer an(prog, chk);
    auto lowered = an.lower();
    if (lowered.empty())
        return { {}, false };

    Emitter em(out, lowered, chk);
    em.emitPreamble();
    em.emitFunctions(prog);
    // Registration helper that wires each lowered function into an Interpreter.
    out_registerAll(out, lowered);
    return funcsFrom(lowered);
}

NativeProgram funcsFrom(
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered) {
    NativeProgram np;
    np.any = !lowered.empty();
    for (auto& [fn, nf] : lowered) np.funcs.push_back(nf);
    // `lowered` is pointer-keyed and iterated in an unspecified order; sort so
    // callers and codegen goldens are reproducible.
    std::sort(np.funcs.begin(), np.funcs.end(),
              [](const NativeFunc& a, const NativeFunc& b) {
                  return a.name < b.name;
              });
    return np;
}

void out_registerAll(
    std::ostream& out,
    const std::unordered_map<const ast::Stmt*, NativeFunc>& lowered) {
    auto envField = [](const std::string& t) -> const char* {
        return t == "int64_t" ? "i" : t == "double" ? "d" : "b";
    };
    auto valueCtor = [](const std::string& t) -> const char* {
        return t == "int64_t" ? "integer" : t == "double" ? "floating"
                                                          : "boolean";
    };
    out << "inline void kirn_native_register(kirn::interp::Interpreter& interp,"
           " const std::vector<kirn::ast::StmtP>& prog) {\n";
    // Stable order: `lowered` is pointer-keyed (unspecified iteration order), so
    // emit registration by function name to keep the generated TU reproducible.
    std::vector<const NativeFunc*> ordered;
    ordered.reserve(lowered.size());
    for (auto& [fn, nf] : lowered) ordered.push_back(&nf);
    std::sort(ordered.begin(), ordered.end(),
              [](const NativeFunc* a, const NativeFunc* b) {
                  return a->name < b->name;
              });
    for (const NativeFunc* nfp : ordered) {
        const NativeFunc& nf = *nfp;
        out << "    for (const auto& d : prog) if (d->kind == "
               "kirn::ast::StKind::FuncDef && d->name == \""
            << nf.name
            << "\") { interp.enableNative(); interp.registerNative(d.get(), "
               "[&interp](kirn::interp::Env e) -> kirn::interp::Value {\n";
        // read scalar params out of the caller's Env
        for (size_t i = 0; i < nf.params.size(); ++i) {
            std::string v = "v_";
            for (char c : nf.params[i]) v += isalnum((unsigned char)c) ? c : '_';
            const std::string& pt = nf.pTypes[i];
            if (pt.rfind("std::optional<", 0) == 0) {
                // Optional scalar: map `none` -> nullopt, else the inner value.
                std::string inner = pt.substr(14, pt.size() - 14 - 1);
                out << "        " << pt << " " << v << "; { auto __p = "
                       "interp.nativeEnvVar(e, \""
                    << nf.params[i] << "\"); if (!__p.isNone()) " << v
                    << " = __p." << envField(inner) << "; }\n";
            } else {
                out << "        " << pt << " " << v
                    << " = interp.nativeEnvVar(e, \"" << nf.params[i]
                    << "\")." << envField(pt) << ";\n";
            }
        }
        std::string call = "kirn_native::" + nf.cName + "(";
        for (size_t i = 0; i < nf.params.size(); ++i) {
            if (i) call += ", ";
            std::string v = "v_";
            for (char c : nf.params[i]) v += isalnum((unsigned char)c) ? c : '_';
            call += v;
        }
        call += ")";
        if (nf.retCpp.empty())
            out << "        " << call << "; return kirn::interp::Value::none();\n";
        else if (nf.retCpp.rfind("std::optional<", 0) == 0) {
            // Optional scalar return: box none or the inner scalar.
            std::string inner =
                nf.retCpp.substr(14, nf.retCpp.size() - 14 - 1);
            out << "        { auto __r = " << call
                << "; if (!__r) return kirn::interp::Value::none();"
                   " return kirn::interp::Value::"
                << valueCtor(inner) << "(*__r); }\n";
        } else
            out << "        return kirn::interp::Value::" << valueCtor(nf.retCpp)
                << "(" << call << ");\n";
        out << "    }); }\n";
    }
    out << "}\n";
}

} // namespace backend
} // namespace kirn
