// Implementation of the Phase 8.2 scalar-native backend. See native.h.
#include "backend/native.h"
#include "interp/escapes.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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
    return t && t->k == TyK::Opt && optInner(t) &&
           (isScalarTy(optInner(t)) || optInner(t)->k == TyK::Str);
}
static bool isOptListTy(const TyP& t) {
    return t && t->k == TyK::Opt && optInner(t) &&
           optInner(t)->k == TyK::List && !optInner(t)->args.empty() &&
           (isScalarTy(optInner(t)->args[0]) ||
            optInner(t)->args[0]->k == TyK::Str);
}
static bool isStringTy(const TyP& t) { return t && t->k == TyK::Str; }
static bool isNativeStructTy(const TyP& t) {
    return t && t->k == TyK::Struct && t->name != "result";
}
static bool isOptStructTy(const TyP& t) {
    return t && t->k == TyK::Opt && isNativeStructTy(optInner(t));
}
static bool isNativeFnTy(const TyP& t) {
    if (!t || t->k != TyK::Fn || !t->inner) return false;
    for (const auto& p : t->args)
        if (!isScalarTy(p) && !isStringTy(p)) return false;
    return t->inner->k == TyK::None || isScalarTy(t->inner) ||
           isStringTy(t->inner);
}
static bool isOptFnTy(const TyP& t) {
    return t && t->k == TyK::Opt && isNativeFnTy(optInner(t));
}

// The set of types the native scalar backend can lower: plain scalars plus
// optionals of scalars (elvis `??` short-circuits to the inner scalar).
static bool isListTy(const TyP& t) { return t && t->k == TyK::List; }
static bool isNativeListElemTy(const TyP& t) {
    return isScalarTy(t) || isStringTy(t);
}
static bool isNativeTy(const TyP& t) {
    return isScalarTy(t) || isOptScalarTy(t) || isOptListTy(t) ||
           isStringTy(t) || isListTy(t) || isNativeStructTy(t) ||
           isOptStructTy(t) ||
           isNativeFnTy(t) || isOptFnTy(t);
}
static bool isNativeMatOperand(const TyP& t) {
    if (!t || t->k != TyK::List || t->args.empty()) return false;
    TyP e = t->args[0];
    if (e->k == TyK::Int || e->k == TyK::Float) return true;
    return e->k == TyK::List && !e->args.empty() &&
           (e->args[0]->k == TyK::Int || e->args[0]->k == TyK::Float);
}
static int nativeMatRank(const TyP& t) {
    return t && t->k == TyK::List && !t->args.empty() &&
                   t->args[0]->k == TyK::List ? 2 : 1;
}

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
        case TyK::Str:   return "std::string";
        case TyK::List:  return "std::vector<kirn::interp::Value>";
        case TyK::Struct:return "kirn::interp::Value";
        case TyK::Fn: {
            std::string ret = t->inner && t->inner->k == TyK::None
                ? "void" : cppTy(t->inner);
            if (ret.empty()) return "";
            std::string out = "std::function<" + ret + "(";
            for (size_t i = 0; i < t->args.size(); ++i) {
                if (i) out += ", ";
                out += cppTy(t->args[i]);
            }
            return out + ")>";
        }
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
    if (t->kind == ast::TyKind::Fn) {
        std::vector<TyP> ps;
        for (const auto& p : t->params) ps.push_back(declTy(p));
        return sema::fnTy(std::move(ps), t->ret ? declTy(t->ret) : sema::noneTy());
    }
    if (t->kind != ast::TyKind::Name) return sema::unkTy();
    const std::string& n = t->name;
    if (n == "list" && t->generics.size() == 1)
        return sema::listTy(declTy(t->generics[0]));
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
    // User-defined nominal names have already been validated by the checker;
    // preserve them so the native registration ABI can carry Value instances.
    return sema::Ty::named(TyK::Struct, n);
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
            if (s->kind == StKind::StructDef)
                for (const auto& m : s->body)
                    if (m->kind == StKind::FuncDef)
                        methods_.emplace_back(s->name, "", m.get());
            if (s->kind == StKind::ImplDef && s->implType && s->implTrait)
                for (const auto& m : s->body)
                    if (m->kind == StKind::FuncDef)
                        methods_.emplace_back(s->implType->name,
                                              s->implTrait->name, m.get());
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
                if (tryLower(*s, "")) changed = true;
            }
            for (const auto& [owner, trait, m] : methods_)
                if (!lowered_.count(m) && tryLower(*m, owner, trait)) changed = true;
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
    std::vector<std::tuple<std::string, std::string, const ast::Stmt*>> methods_;

    TyP typeOf(const ast::Expr& e) const { return chk_->typeOf(e); }

    bool tryLower(const ast::Stmt& s, const std::string& owner,
                  const std::string& trait = "") {
        if (s.typeParams.size() || s.externDef || s.body.empty()) return false;
        if (s.ret && s.ret->kind == ast::TyKind::Name &&
            (s.ret->name == "result" || s.ret->name == "Result"))
            return false;
        TyP rt = s.ret ? declTy(s.ret) : sema::noneTy();
        if (!isNoneTy(rt) && !isNativeTy(rt)) return false;
        if (isOptFnTy(rt)) return false;
        for (const auto& p : s.params) {
            if (p.variadic || p.defaultValue || (!p.type && !p.selfParam))
                return false;
            // Plain scalars and optionals of scalars (`T?`) are both lowerable;
            // an optional scalar becomes `std::optional<scalar>`.
            TyP pt = (owner.size() && p.selfParam)
                ? sema::Ty::named(TyK::Struct, owner) : declTy(p.type);
            if (!isNativeTy(pt)) return false;
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
        nf.ownerType = owner;
        nf.implTrait = trait;
        nf.cName = uniqueCName(s.name);
        nf.retCpp = (rt && rt->k != TyK::None) ? cppTy(rt) : std::string();
        if (isNativeFnTy(rt)) {
            for (const auto& a : rt->args) nf.retFnParamTypes.push_back(cppTy(a));
            nf.retFnReturnType = rt->inner && rt->inner->k != TyK::None
                ? cppTy(rt->inner) : std::string();
        }
        for (const auto& p : s.params) {
            nf.params.push_back(p.name);
            TyP pt = (owner.size() && p.selfParam)
                ? sema::Ty::named(TyK::Struct, owner) : declTy(p.type);
            nf.pTypes.push_back(cppTy(pt));
            TyP fn = pt && pt->k == TyK::Opt ? optInner(pt) : pt;
            if (fn && fn->k == TyK::Fn) {
                std::vector<std::string> ps;
                for (const auto& a : fn->args) ps.push_back(cppTy(a));
                nf.fnParamTypes.push_back(std::move(ps));
                nf.fnReturnTypes.push_back(fn->inner && fn->inner->k != TyK::None
                                               ? cppTy(fn->inner) : std::string());
            } else {
                nf.fnParamTypes.emplace_back();
                nf.fnReturnTypes.emplace_back();
            }
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
        if (!e.lhs) return false;
        if (e.lhs->kind == ExKind::Member && e.lhs->lhs) {
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            const NativeFunc* method = nullptr;
            if (isNativeStructTy(base)) {
                for (const auto& [fn, nf] : lowered_)
                    if (nf.ownerType == base->name && nf.name == e.lhs->text) {
                        method = &nf; break;
                    }
            }
            if (method && (!optional || e.lhs->nilSafe)) {
                TyP result = typeOf(e);
                TyP value = optional && result && result->k == TyK::Opt
                    ? optInner(result) : result;
                if (!value || !isNativeTy(value) || e.args.size() + 1 != method->pTypes.size())
                    return false;
                for (size_t i = 0; i < e.args.size(); ++i)
                    if (e.args[i].name != "" || !e.args[i].value ||
                        !exprLowerable(*e.args[i].value, cur)) return false;
                return true;
            }
        }
        if (e.lhs->kind == ExKind::Ident && cur.locals.count(e.lhs->text)) {
            TyP callee = typeOf(*e.lhs);
            TyP fn = callee && callee->k == TyK::Opt ? optInner(callee) : callee;
            if (fn && fn->k == TyK::Fn) {
                if (callee->k == TyK::Opt) return false;
                if (e.args.size() != fn->args.size() ||
                    (!isNoneTy(typeOf(e)) && !isNativeTy(typeOf(e))))
                    return false;
                for (size_t i = 0; i < e.args.size(); ++i)
                    if (e.args[i].name != "" || !e.args[i].value ||
                        !exprLowerable(*e.args[i].value, cur) ||
                        !assignCompatible(typeOf(*e.args[i].value), fn->args[i]))
                        return false;
                return true;
            }
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "extend") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 1 || e.args[0].name != "" ||
                !e.args[0].value || !exprLowerable(*e.args[0].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            TyP src = typeOf(*e.args[0].value);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            TyP result = typeOf(e);
            return (!optional || e.lhs->nilSafe) && isListTy(base) &&
                   !base->args.empty() && isNativeListElemTy(base->args[0]) &&
                   isListTy(src) && !src->args.empty() &&
                   src->args[0]->k == base->args[0]->k && result &&
                   (result->k == TyK::None ||
                    (result->k == TyK::Opt && optInner(result) &&
                     optInner(result)->k == TyK::None));
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "insert") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 2 || e.args[0].name != "" ||
                e.args[1].name != "" || !e.args[0].value || !e.args[1].value ||
                !exprLowerable(*e.args[0].value, cur) ||
                !exprLowerable(*e.args[1].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) || base->args.empty() ||
                !isNativeListElemTy(base->args[0]) ||
                !typeOf(*e.args[0].value) || typeOf(*e.args[0].value)->k != TyK::Int ||
                !assignCompatible(typeOf(*e.args[1].value), base->args[0]))
                return false;
            TyP result = typeOf(e);
            return result && (result->k == TyK::None ||
                              (result->k == TyK::Opt && optInner(result) &&
                               optInner(result)->k == TyK::None));
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "remove") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 1 || e.args[0].name != "" ||
                !e.args[0].value || !exprLowerable(*e.args[0].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) || base->args.empty() ||
                !isNativeListElemTy(base->args[0]) ||
                !assignCompatible(typeOf(*e.args[0].value), base->args[0]))
                return false;
            TyP result = typeOf(e);
            return result && (result->k == TyK::None ||
                              (result->k == TyK::Opt && optInner(result) &&
                               optInner(result)->k == TyK::None));
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "join") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 1 || e.args[0].name != "" ||
                !e.args[0].value || !exprLowerable(*e.args[0].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) || base->args.empty() ||
                base->args[0]->k != TyK::Str || !typeOf(*e.args[0].value) ||
                typeOf(*e.args[0].value)->k != TyK::Str)
                return false;
            TyP result = typeOf(e);
            TyP value = optional ? optInner(result) : result;
            return value && value->k == TyK::Str;
        }
        if (e.lhs->kind == ExKind::Member &&
            (e.lhs->text == "contains" || e.lhs->text == "index")) {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 1 || e.args[0].name != "" ||
                !e.args[0].value || !exprLowerable(*e.args[0].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) || base->args.empty() ||
                !isScalarTy(base->args[0]) ||
                !assignCompatible(typeOf(*e.args[0].value), base->args[0]))
                return false;
            TyP result = typeOf(e);
            TyP value = optional ? optInner(result) : result;
            return value &&
                   (e.lhs->text == "contains" ? value->k == TyK::Bool
                                                : value->k == TyK::Int);
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "sum") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                !e.args.empty())
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) ||
                base->args.empty() || !isScalarTy(base->args[0]) ||
                (base->args[0]->k != TyK::Int && base->args[0]->k != TyK::Float))
                return false;
            TyP result = typeOf(e);
            TyP value = optional ? optInner(result) : result;
            return value && value->k == base->args[0]->k;
        }
        if (e.lhs->kind == ExKind::Member &&
            (e.lhs->text == "clear" || e.lhs->text == "reverse")) {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                !e.args.empty())
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            TyP result = typeOf(e);
            return (!optional || e.lhs->nilSafe) && isListTy(base) &&
                   !base->args.empty() && isNativeListElemTy(base->args[0]) &&
                   result && (result->k == TyK::None ||
                              (result->k == TyK::Opt && optInner(result) &&
                               optInner(result)->k == TyK::None));
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "shift") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                !e.args.empty())
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            TyP result = typeOf(e);
            TyP value = optional ? optInner(result) : result;
            return (!optional || e.lhs->nilSafe) && isListTy(base) &&
                   !base->args.empty() && isScalarTy(base->args[0]) &&
                   value && value->k == base->args[0]->k;
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "pop") {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                !e.args.empty())
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            TyP result = typeOf(e);
            TyP value = optional ? optInner(result) : result;
            return (!optional || e.lhs->nilSafe) && isListTy(base) &&
                   !base->args.empty() && isScalarTy(base->args[0]) &&
                   value && value->k == base->args[0]->k;
        }
        if (e.lhs->kind == ExKind::Member &&
            (e.lhs->text == "append" || e.lhs->text == "push_back")) {
            if (!e.lhs->lhs || e.lhs->lhs->kind != ExKind::Ident ||
                e.args.size() != 1 || e.args[0].name != "" ||
                !e.args[0].value || !exprLowerable(*e.args[0].value, cur))
                return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if ((optional && !e.lhs->nilSafe) || !isListTy(base) || base->args.empty() ||
                !isNativeListElemTy(base->args[0]))
                return false;
            TyP result = typeOf(e);
            return assignCompatible(typeOf(*e.args[0].value), base->args[0]) &&
                   result && (result->k == TyK::None ||
                              (result->k == TyK::Opt && optInner(result) &&
                               optInner(result)->k == TyK::None));
        }
        if (e.lhs->kind == ExKind::Member && e.lhs->text == "len") {
            if (!e.args.empty() || !e.lhs->lhs) return false;
            TyP recv = typeOf(*e.lhs->lhs);
            bool optional = recv && recv->k == TyK::Opt;
            TyP base = optional ? optInner(recv) : recv;
            if (!base || (base->k != TyK::List && base->k != TyK::Str) ||
                !exprLowerable(*e.lhs->lhs, cur))
                return false;
            if (optional && !e.lhs->nilSafe) return false;
            return true;
        }
        // Struct constructors are value-producing calls, not native function
        // calls. Keep the first ABI slice explicit: named scalar/string fields
        // are boxed into the existing interpreter Value representation.
        if (e.lhs->kind == ExKind::Ident && isNativeStructTy(typeOf(e))) {
            if (e.args.empty()) return false;
            for (const auto& a : e.args)
                if (a.name.empty() || !a.value || !exprLowerable(*a.value, cur) ||
                    (!isScalarTy(typeOf(*a.value)) && !isStringTy(typeOf(*a.value))))
                    return false;
            return true;
        }
        if (e.lhs->kind != ExKind::Ident) return false;
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
            case ExKind::Str:
                return e.flavor == ast::StrFlavor::Normal ||
                       e.flavor == ast::StrFlavor::Raw;
            case ExKind::List:
                for (const auto& item : e.elems)
                    if (!item || !exprLowerable(*item, cur)) return false;
                return isListTy(t);
            case ExKind::ListComp: {
                // Native list comprehensions use one scalar range or list
                // source, optionally followed by a scalar boolean filter. The
                // accumulator is a C++ vector of boxed Kirn values, so it
                // crosses the existing native thunk boundary unchanged.
                if (!isListTy(t) || e.elems.size() != 1 || e.clauses.empty() ||
                    e.clauses.size() > 2 || !e.clauses[0].isFor ||
                    !e.clauses[0].iter || !e.clauses[0].pat ||
                    e.clauses[0].pat->kind != ast::PatKind::Bind ||
                    e.clauses[0].pat->bindName.empty()) return false;
                const ast::Expr& it = *e.clauses[0].iter;
                bool range = it.kind == ExKind::Binary &&
                    (it.op == ".." || it.op == "..=");
                if (range) {
                    if (!it.lhs || !it.rhs || !exprLowerable(*it.lhs, cur) ||
                        !exprLowerable(*it.rhs, cur) || !typeOf(*it.lhs) ||
                        typeOf(*it.lhs)->k != TyK::Int || !typeOf(*it.rhs) ||
                        typeOf(*it.rhs)->k != TyK::Int)
                        return false;
                } else {
                    TyP seq = typeOf(it);
                    if (!exprLowerable(it, cur) || !isListTy(seq) ||
                        seq->args.empty() || !isNativeListElemTy(seq->args[0]))
                        return false;
                }
                CurFn nested = cur;
                nested.locals.insert(e.clauses[0].pat->bindName);
                if (!exprLowerable(*e.elems[0], nested)) return false;
                if (e.clauses.size() == 2) {
                    const auto& filter = e.clauses[1];
                    if (filter.isFor || !filter.cond ||
                        !exprLowerable(*filter.cond, nested) ||
                        !typeOf(*filter.cond) || typeOf(*filter.cond)->k != TyK::Bool)
                        return false;
                }
                return true;
            }
            case ExKind::Ident:
                // `none` is only lowerable in an optional-scalar slot; the
                // surrounding statement/return checks the target type.
                if (e.text == "none") return isNoneTy(t);
                if (e.text == "true" || e.text == "false") return true;
                // Only a name the function itself binds can be emitted; a free
                // name (module-level const/var, etc.) has no local to reference.
                if (cur.locals.count(e.text)) return isNativeTy(t);
                if (topFuncs_.count(e.text) && lowered_.count(topFuncs_.at(e.text)))
                    return isNativeFnTy(t);
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
                if (e.op == "@")
                    return e.lhs && e.rhs &&
                           isNativeMatOperand(typeOf(*e.lhs)) &&
                           isNativeMatOperand(typeOf(*e.rhs)) && isNativeTy(t);
                return binopLowerable(e, t);
            case ExKind::Call:
                return callLowerable(e, cur);
            case ExKind::SafeCall: {
                TyP callee = e.lhs ? typeOf(*e.lhs) : TyP{};
                if (!e.lhs || !exprLowerable(*e.lhs, cur) ||
                    !callee || callee->k != TyK::Opt || !isNativeFnTy(optInner(callee)))
                    return false;
                const TyP& fn = optInner(callee);
                if (e.args.size() != fn->args.size() || !typeOf(e) ||
                    !isOptScalarTy(typeOf(e)))
                    return false;
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (e.args[i].name != "" || !e.args[i].value ||
                        !exprLowerable(*e.args[i].value, cur) ||
                        !assignCompatible(typeOf(*e.args[i].value), fn->args[i]))
                        return false;
                }
                return true;
            }
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
            case ExKind::SafeIndex: {
                // Native safe indexing supports optional scalar lists. The
                // result is an optional scalar, with the same negative-index
                // and bounds behavior as the interpreter.
                if (!e.lhs || !e.rhs || !exprLowerable(*e.lhs, cur) ||
                    !exprLowerable(*e.rhs, cur) || !isOptListTy(typeOf(*e.lhs)) ||
                    !isScalarTy(t ? optInner(t) : TyP{}))
                    return false;
                TyP it = typeOf(*e.rhs);
                return it && it->k == TyK::Int && t && t->k == TyK::Opt;
            }
            case ExKind::Index: {
                // Native aggregate support also includes direct indexing of a
                // list whose elements are scalar values. The list remains a
                // vector<Value> at the thunk boundary; only the selected
                // element is unboxed into the native scalar result.
                TyP obj = e.lhs ? typeOf(*e.lhs) : TyP{};
                TyP idx = e.rhs ? typeOf(*e.rhs) : TyP{};
                if (!e.lhs || !e.rhs || !exprLowerable(*e.lhs, cur) ||
                    !exprLowerable(*e.rhs, cur) || !isListTy(obj) ||
                    obj->args.empty() || !isNativeTy(t) ||
                    obj->args[0]->k != t->k || !idx || idx->k != TyK::Int)
                    return false;
                return true;
            }
            case ExKind::Member: {
                // Nominal struct values use the interpreter's Value layout at
                // the native thunk boundary. Read-only scalar/string fields
                // can therefore lower without inventing a parallel ABI.
                if (!e.lhs || !exprLowerable(*e.lhs, cur) || e.text.empty())
                    return false;
                TyP recv = typeOf(*e.lhs);
                TyP base = recv && recv->k == TyK::Opt ? optInner(recv) : recv;
                if (!isNativeStructTy(base) || !isNativeTy(t)) return false;
                return !t || t->k != TyK::Fn;
            }
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
            return (op == "+" && isStringTy(t)) || isScalarTy(t);
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
        if (it.kind == ExKind::Binary && (it.op == ".." || it.op == "..=")) {
            if (!it.lhs || !it.rhs) return false;
            if (!exprLowerable(*it.lhs, cur) || !exprLowerable(*it.rhs, cur))
                return false;
            if (typeOf(*it.lhs)->k != TyK::Int || typeOf(*it.rhs)->k != TyK::Int)
                return false;
        } else {
            TyP seq = typeOf(it);
            if (!exprLowerable(it, cur) || !isListTy(seq) || seq->args.empty() ||
                !isNativeListElemTy(seq->args[0]))
                return false;
        }
        return bodyLowerable(s.body, cur);
    }

    bool assignLowerable(const ast::Stmt& s, const CurFn& cur) const {
        if (s.exprs.size() != 2) return false;
        const ast::ExprP& tgt = s.exprs[0];
        if (tgt && tgt->kind == ExKind::Member) {
            if (s.kind != StKind::Assign || !tgt->lhs ||
                !exprLowerable(*tgt->lhs, cur)) return false;
            TyP recv = typeOf(*tgt->lhs);
            if (!isNativeStructTy(recv)) return false;
            TyP field = typeOf(*tgt);
            return field && (isScalarTy(field) || isStringTy(field)) &&
                   exprLowerable(*s.exprs[1], cur) &&
                   assignCompatible(typeOf(*s.exprs[1]), field);
        }
        if (tgt && tgt->kind == ExKind::Index) {
            if (s.kind != StKind::Assign || !tgt->lhs || !tgt->rhs ||
                tgt->lhs->kind != ExKind::Ident ||
                !exprLowerable(*tgt->lhs, cur) ||
                !exprLowerable(*tgt->rhs, cur) ||
                !isListTy(typeOf(*tgt->lhs)) ||
                typeOf(*tgt->lhs)->args.empty() ||
                !isScalarTy(typeOf(*tgt->lhs)->args[0]) ||
                !typeOf(*tgt->rhs) || typeOf(*tgt->rhs)->k != TyK::Int)
                return false;
            TyP elem = typeOf(*tgt->lhs)->args[0];
            return assignCompatible(typeOf(*s.exprs[1]), elem) &&
                   exprLowerable(*s.exprs[1], cur);
        }
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
             << "#include <limits>\n"
             << "#include <optional>\n"
             << "#include <string>\n"
             << "#include <utility>\n"
             << "#include <vector>\n"
             << "namespace kirn_native {\n"
             << "static inline void co_panic(const char* m){ "
                "kirn::interp::panicHere(m); }\n"
             << "static inline int64_t co_iadd(int64_t a,int64_t b){"
                " if((b>0&&a>std::numeric_limits<int64_t>::max()-b)||"
                "(b<0&&a<std::numeric_limits<int64_t>::min()-b))"
                "co_panic(\"integer overflow\"); return a+b; }\n"
             << "static inline int64_t co_isub(int64_t a,int64_t b){"
                " if((b<0&&a>std::numeric_limits<int64_t>::max()+b)||"
                "(b>0&&a<std::numeric_limits<int64_t>::min()+b))"
                "co_panic(\"integer overflow\"); return a-b; }\n"
             << "static inline int64_t co_imul(int64_t a,int64_t b){"
                " if(a==0||b==0)return 0;"
                " if((a==std::numeric_limits<int64_t>::min()&&b==-1)||"
                "(b==std::numeric_limits<int64_t>::min()&&a==-1))"
                "co_panic(\"integer overflow\");"
                " if(a>0 ? (b>0 ? a>std::numeric_limits<int64_t>::max()/b : "
                "b<std::numeric_limits<int64_t>::min()/a) : "
                "(b>0 ? a<std::numeric_limits<int64_t>::min()/b : "
                "a!=0&&b<std::numeric_limits<int64_t>::max()/a))"
                "co_panic(\"integer overflow\"); return a*b; }\n"
             << "static inline int64_t co_ineg(int64_t a){"
                " if(a==std::numeric_limits<int64_t>::min())"
                "co_panic(\"integer overflow\"); return -a; }\n"
             << "static inline int64_t co_floordiv(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"division by zero\");"
                " if(a==std::numeric_limits<int64_t>::min()&&b==-1)"
                "co_panic(\"integer overflow\"); int64_t q=a/b;"
                " if((a%b!=0)&&((a<0)!=(b<0)))--q; return q; }\n"
             << "static inline int64_t co_mod(int64_t a,int64_t b){"
                " if(b==0)co_panic(\"modulo by zero\");"
                " if(a==std::numeric_limits<int64_t>::min()&&b==-1)"
                "co_panic(\"integer overflow\"); int64_t m=a%b;"
                " if(m!=0&&((m<0)!=(b<0)))m+=b; return m; }\n"
             << "static inline double co_fdiv(double a,double b){"
                " if(b==0.0)co_panic(\"division by zero\"); return a/b; }\n"
             << "static inline double co_fmodm(double a,double b){"
                " return std::fmod(std::fmod(a,b)+b,b); }\n"
             << "static inline int64_t co_ipow(int64_t b,int64_t e){"
                " int64_t o=1; while(e>0){ if(e&1)o=co_imul(o,b);"
                " e>>=1; if(e)b=co_imul(b,b);}"
                " return o; }\n"
             << "static inline int64_t co_shl(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return (int64_t)((uint64_t)a<<b); }\n"
             << "static inline int64_t co_shr(int64_t a,int64_t b){"
                " if(b<0||b>=63)kirn::interp::panicHere(\"shift\");"
                " return a>>b; }\n"
             << "static inline const kirn::interp::Value& co_field("
                "const kirn::interp::Value& v,const char* n){"
                " const auto* fs = v.k == kirn::interp::VK::Struct ? &v.fields : "
                "(v.k == kirn::interp::VK::Heap && v.heap ? &v.heap->fields : nullptr);"
                " if(!fs) kirn::interp::panicHere(\"native field access on non-struct\");"
                " for(const auto& f:*fs) if(f.first == n) return f.second;"
                " kirn::interp::panicHere(\"native field not found\");"
                " return v; }\n"
             << "static inline kirn::interp::Value& co_field_mut("
                "kirn::interp::Value& v,const char* n){"
                " if(v.k != kirn::interp::VK::Struct)"
                " kirn::interp::panicHere(\"native mutable field access on non-struct\");"
                " for(auto& f:v.fields) if(f.first == n) return f.second;"
                " kirn::interp::panicHere(\"native field not found\");"
                " return v; }\n";
    }

    void emitFunctions(const std::vector<ast::StmtP>& prog) {
        for (const auto& s : prog) {
            if (s->kind == StKind::FuncDef) {
                auto it = lowered_->find(s.get());
                if (it != lowered_->end()) emitFunction(*s, it->second);
            } else if (s->kind == StKind::StructDef || s->kind == StKind::ImplDef) {
                for (const auto& m : s->body) {
                    auto it = lowered_->find(m.get());
                    if (it != lowered_->end()) emitFunction(*m, it->second);
                }
            }
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
               : ty == "std::string" ? "str" : "boolean";
    }
    static const char* envField(const std::string& ty) {
        return ty == "int64_t" ? "i"
             : ty == "double" ? "d"
             : ty == "std::string" ? "s" : "b";
    }
    static const char* zeroOf(const std::string& ty) {
        if (ty.rfind("std::optional<", 0) == 0) return "std::nullopt";
        return ty == "int64_t" ? "0"
             : ty == "double" ? "0.0"
             : ty == "std::string" ? "std::string()"
             : ty == "std::vector<kirn::interp::Value>" ? "{}"
             : ty == "kirn::interp::Value" ? "kirn::interp::Value{}"
             : ty.rfind("std::function<", 0) == 0 ? "{}" : "false";
    }

    std::string boxExpr(const ast::Expr& e) {
        TyP t = typeOf(e);
        if (t && t->k == TyK::Str)
            return "kirn::interp::Value::str(" + emitExpr(e) + ")";
        if (t && t->k == TyK::Float)
            return "kirn::interp::Value::floating(" + emitExpr(e) + ")";
        if (t && t->k == TyK::Bool)
            return "kirn::interp::Value::boolean(" + emitExpr(e) + ")";
        if (t && t->k == TyK::List)
            return "kirn::interp::Value::list(" + emitExpr(e) + ")";
        return "kirn::interp::Value::integer(" + emitExpr(e) + ")";
    }

    static const char* valueField(const TyP& t) {
        if (t && t->k == TyK::Float) return "d";
        if (t && t->k == TyK::Bool) return "b";
        if (t && t->k == TyK::Str) return "s";
        return "i";
    }

    const NativeFunc* findCallee(const std::string& name) const {
        for (auto& [fn, nf] : *lowered_)
            if (nf.name == name) return &nf;
        return nullptr;
    }
    const NativeFunc* findMethod(const std::string& owner,
                                 const std::string& name) const {
        for (const auto& [fn, nf] : *lowered_)
            if (nf.ownerType == owner && nf.name == name && nf.implTrait.empty())
                return &nf;
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
                if (s.exprs[0]->kind == ExKind::Member) {
                    const ast::Expr& tgt = *s.exprs[0];
                    out_ << pad << "kirn_native::co_field_mut(" <<
                        emitExpr(*tgt.lhs) << ", \"" << tgt.text << "\") = " <<
                        boxExpr(*s.exprs[1]) << ";\n";
                    return;
                }
                if (s.exprs[0]->kind == ExKind::Index) {
                    const ast::Expr& tgt = *s.exprs[0];
                    std::string obj = emitExpr(*tgt.lhs);
                    std::string idx = emitExpr(*tgt.rhs);
                    std::string tmp = "__set" + std::to_string(tmpSeq_++);
                    out_ << pad << "{ auto &" << tmp << " = (" << obj
                         << "); int64_t __i = (" << idx << "); if (__i < 0) __i += "
                         << "static_cast<int64_t>(" << tmp << ".size()); " << tmp
                         << ".at(static_cast<size_t>(__i)) = "
                         << boxExpr(*s.exprs[1]) << "; }\n";
                    return;
                }
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
        if (it.kind == ExKind::Binary && (it.op == ".." || it.op == "..=")) {
            bool inclusive = it.op == "..=";
            std::string loopVar = cIdent(s.pat->bindName, "v_");
            std::string lo = emitExpr(*it.lhs);
            std::string hi = emitExpr(*it.rhs);
            std::string cmp = inclusive ? "<=" : "<";
            out_ << pad << "for (int64_t " << loopVar << " = (" << lo << "); "
                 << loopVar << " " << cmp << " (" << hi << "); ++" << loopVar
                 << ") {\n";
            localTypes_[s.pat->bindName] = "int64_t";
        } else {
            TyP elem = typeOf(it)->args[0];
            std::string seq = "__for_seq" + std::to_string(tmpSeq_++);
            std::string loopVar = cIdent(s.pat->bindName, "v_");
            out_ << pad << "{ auto &&" << seq << " = (" << emitExpr(it)
                 << "); for (const auto& __item : " << seq << ") { "
                 << cppTy(elem) << " " << loopVar << " = __item."
                 << valueField(elem) << ";\n";
            localTypes_[s.pat->bindName] = cppTy(elem);
        }
        for (const auto& st : s.body) emitStmt(*st, ind + 1);
        out_ << pad << "}\n";
        if (it.kind != ExKind::Binary || (it.op != ".." && it.op != "..="))
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
            case ExKind::Str: {
                const std::string value = e.flavor == ast::StrFlavor::Raw
                    ? e.text : kirn::decodeEscapes(e.text);
                std::string out = "std::string(\"";
                for (unsigned char c : value) {
                    if (c == '\\' || c == '"') out += '\\';
                    if (c == '\n') out += "\\n";
                    else if (c == '\r') out += "\\r";
                    else if (c == '\t') out += "\\t";
                    else if (c == '\0') out += "\\0";
                    else out += static_cast<char>(c);
                }
                return out + "\")";
            }
            case ExKind::List: {
                std::string out = "std::vector<kirn::interp::Value>{";
                for (size_t i = 0; i < e.elems.size(); ++i) {
                    if (i) out += ", ";
                    out += boxExpr(*e.elems[i]);
                }
                return out + "}";
            }
            case ExKind::ListComp: {
                const auto& clause = e.clauses[0];
                const auto& source = *clause.iter;
                const std::string var = cIdent(clause.pat->bindName, "v_");
                const std::string oldType = localTypes_.count(clause.pat->bindName)
                    ? localTypes_[clause.pat->bindName] : std::string();
                const bool hadType = localTypes_.count(clause.pat->bindName) != 0;
                bool range = source.kind == ExKind::Binary &&
                    (source.op == ".." || source.op == "..=");
                std::string out = "[&]{ std::vector<kirn::interp::Value> __c; ";
                if (range) {
                    localTypes_[clause.pat->bindName] = "int64_t";
                    bool inclusive = source.op == "..=";
                    out += "for (int64_t " + var + " = (" + emitExpr(*source.lhs) + "); " + var +
                           (inclusive ? " <= (" : " < (") + emitExpr(*source.rhs) +
                           "); ++" + var + ") { ";
                } else {
                    TyP elem = typeOf(source)->args[0];
                    localTypes_[clause.pat->bindName] = cppTy(elem);
                    out += "auto &&__src = (" + emitExpr(source) + "); for (const auto& __item : __src) { " +
                           cppTy(elem) + " " + var + " = __item." + valueField(elem) + "; ";
                }
                if (e.clauses.size() == 2)
                    out += "if (" + emitExpr(*e.clauses[1].cond) + ") ";
                out += "__c.push_back(" + boxExpr(*e.elems[0]) + "); } return __c; }()";
                if (hadType) localTypes_[clause.pat->bindName] = oldType;
                else localTypes_.erase(clause.pat->bindName);
                return out;
            }
            case ExKind::Ident:
                if (e.text == "true") return "true";
                if (e.text == "false") return "false";
                if (e.text == "none") return "std::nullopt";
                if (calleeFunc_.count(e.text))
                    return "kirn_native::" + calleeFunc_.at(e.text)->cName;
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
                if (e.op == "-" && t && t->k == TyK::Int)
                    return "kirn_native::co_ineg(" + r + ")";
                if (e.op == "-") return "(-(" + r + "))";
                if (e.op == "+") return "(+(" + r + "))";
                if (e.op == "not") return "(!(" + r + "))";
                return r;
            }
            case ExKind::Binary:
                if (e.op == "@") return emitMatmul(e);
                return emitBinop(e.op, *e.lhs, *e.rhs);
            case ExKind::Member: {
                TyP recv = typeOf(*e.lhs);
                TyP value = t;
                bool optional = recv && recv->k == TyK::Opt;
                std::string obj = emitExpr(*e.lhs);
                auto read = [&](const std::string& v) {
                    if (value && value->k == TyK::Struct) return
                        "kirn_native::co_field(" + v + ", \"" + e.text + "\")";
                    return "kirn_native::co_field(" + v + ", \"" + e.text + "\")." +
                           valueField(optional ? optInner(value) : value);
                };
                if (optional) {
                    TyP inner = optInner(t);
                    std::string tmp = "__mem" + std::to_string(tmpSeq_++);
                    return "[&]{ auto " + tmp + " = (" + obj + "); if (!" + tmp + ") return std::optional<" +
                           cppTy(inner) + ">{ }; return std::optional<" + cppTy(inner) + ">{ " +
                           read("*" + tmp) + " }; }()";
                }
                return read(obj);
            }
            case ExKind::SafeCall: {
                TyP fn = optInner(typeOf(*e.lhs));
                TyP ret = fn->inner;
                std::string retCpp = cppTy(ret);
                std::string call = "(*__fn)(";
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i) call += ", ";
                    call += emitExpr(*e.args[i].value);
                }
                call += ")";
                return "[&]{ auto __fn = (" + emitExpr(*e.lhs) + "); if (!__fn) return std::optional<" +
                       retCpp + ">{ }; return std::optional<" + retCpp + ">{ " + call + " }; }()";
            }
            case ExKind::Call: {
                if (e.lhs->kind == ExKind::Member && e.lhs->lhs) {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP base = optional ? optInner(recv) : recv;
                    const NativeFunc* method = base && base->k == TyK::Struct
                        ? findMethod(base->name, e.lhs->text) : nullptr;
                    if (method) {
                        std::string obj = emitExpr(*e.lhs->lhs);
                        std::string call = "kirn_native::" + method->cName + "(";
                        if (optional) call += "*__recv";
                        else call += obj;
                        for (const auto& a : e.args)
                            call += ", " + emitExpr(*a.value);
                        call += ")";
                        if (optional) {
                            TyP ret = optInner(typeOf(e));
                            return "[&]{ auto __recv = (" + obj + "); if (!__recv) return std::optional<" +
                                   cppTy(ret) + ">{ }; return std::optional<" + cppTy(ret) + ">{ " +
                                   call + " }; }()";
                        }
                        return call;
                    }
                }
                if (e.lhs->kind == ExKind::Ident && localTypes_.count(e.lhs->text) &&
                    typeOf(*e.lhs) && typeOf(*e.lhs)->k == TyK::Fn) {
                    std::string call = cIdent(e.lhs->text, "v_") + "(";
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (i) call += ", ";
                        call += emitExpr(*e.args[i].value);
                    }
                    return call + ")";
                }
                if (e.lhs->kind == ExKind::Ident && isNativeStructTy(typeOf(e))) {
                    std::string out = "[&]{ auto __s = kirn::interp::Value::structV(\"" +
                                      e.lhs->text + "\"); ";
                    for (const auto& a : e.args)
                        out += "__s.fields.emplace_back(\"" + a.name +
                               "\", " + boxExpr(*a.value) + "); ";
                    return out + "return __s; }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "extend") {
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string src = emitExpr(*e.args[0].value);
                    TyP recv = typeOf(*e.lhs->lhs);
                    if (recv && recv->k == TyK::Opt)
                        return "[&]{ auto __opt = (" + obj + "); if (__opt) { auto __src = (" + src + "); __opt->insert(__opt->end(), __src.begin(), __src.end()); } }()";
                    return "[&]{ auto &__dst = (" + obj + "); auto __src = (" +
                           src + "); __dst.insert(__dst.end(), __src.begin(), __src.end()); }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "insert") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP elem = (optional ? optInner(recv) : recv)->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string idx = "__insert_index" + std::to_string(tmpSeq_++);
                    std::string value = boxExpr(*e.args[1].value);
                    if (optional)
                        return "[&]{ auto __opt = (" + obj + "); if (__opt) { int64_t " + idx +
                               " = (" + emitExpr(*e.args[0].value) + "); if (" + idx +
                               " < 0) " + idx + " += static_cast<int64_t>(__opt->size()) + 1; if (" + idx +
                               " < 0 || " + idx + " > static_cast<int64_t>(__opt->size())) kirn_native::co_panic(\"index out of range\"); __opt->insert(__opt->begin() + static_cast<ptrdiff_t>(" + idx +
                               "), " + value + "); } }()";
                    return "[&]{ auto &__seq = (" + obj + "); int64_t " + idx +
                           " = (" + emitExpr(*e.args[0].value) + "); if (" + idx +
                           " < 0) " + idx + " += static_cast<int64_t>(__seq.size()) + 1; if (" + idx +
                           " < 0 || " + idx + " > static_cast<int64_t>(__seq.size())) kirn_native::co_panic(\"index out of range\"); __seq.insert(__seq.begin() + static_cast<ptrdiff_t>(" + idx +
                           "), " + value + "); }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "remove") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP elem = (optional ? optInner(recv) : recv)->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string needle = "__remove_needle" + std::to_string(tmpSeq_++);
                    std::string field = valueField(elem);
                    if (optional)
                        return "[&]{ auto __opt = (" + obj + "); if (__opt) { auto " + needle +
                               " = (" + emitExpr(*e.args[0].value) + "); for (auto __it = __opt->begin(); __it != __opt->end(); ++__it) { if (__it->" +
                               field + " == " + needle + ") { __opt->erase(__it); return; } } kirn_native::co_panic(\"remove(): value not found\"); } }()";
                    return "[&]{ auto &__seq = (" + obj + "); auto " + needle +
                           " = (" + emitExpr(*e.args[0].value) + "); for (auto __it = __seq.begin(); __it != __seq.end(); ++__it) { if (__it->" +
                           field + " == " + needle + ") { __seq.erase(__it); return; } } kirn_native::co_panic(\"remove(): value not found\"); }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "join") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string sep = emitExpr(*e.args[0].value);
                    if (recv && recv->k == TyK::Opt) {
                        return "[&]{ auto __opt = (" + obj + "); if (!__opt) return std::optional<std::string>{}; auto &__seq = *__opt; auto __sep = (" +
                               sep + "); std::string __out; for (size_t __i = 0; __i < __seq.size(); ++__i) { if (__i) __out += __sep; __out += __seq[__i].s; } return std::optional<std::string>{__out}; }()";
                    }
                    return "[&]{ auto &__seq = (" + obj + "); auto __sep = (" +
                           sep + "); std::string __out; for (size_t __i = 0; __i < __seq.size(); ++__i) { if (__i) __out += __sep; __out += __seq[__i].s; } return __out; }()";
                }
                if (e.lhs->kind == ExKind::Member &&
                    (e.lhs->text == "contains" || e.lhs->text == "index")) {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP base = optional ? optInner(recv) : recv;
                    TyP elem = base->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string needle = "__needle" + std::to_string(tmpSeq_++);
                    std::string field = valueField(elem);
                    if (optional) {
                        const char* outTy = e.lhs->text == "contains" ? "bool" : "int64_t";
                        std::string found = e.lhs->text == "contains"
                            ? "if (__v." + field + " == " + needle + ") return std::optional<bool>{true};"
                            : "if (__v." + field + " == " + needle + ") return std::optional<int64_t>{__pos}; ++__pos;";
                        std::string absent = e.lhs->text == "contains"
                            ? "return std::optional<bool>{false};"
                            : "return std::optional<int64_t>{int64_t(-1)};";
                        return "[&]{ auto __opt = (" + obj + "); if (!__opt) return std::optional<" + outTy + ">{ }; auto &__seq = *__opt; auto " + needle +
                               " = (" + emitExpr(*e.args[0].value) + "); int64_t __pos = 0; for (const auto& __v : __seq) { " + found + " } " + absent + " }()";
                    }
                    std::string result = e.lhs->text == "contains"
                        ? "[&]{ auto &__seq = (" + obj + "); auto " + needle +
                          " = (" + emitExpr(*e.args[0].value) + "); for (const auto& __v : __seq) if (__v." +
                          field + " == " + needle + ") return true; return false; }()"
                        : "[&]{ auto &__seq = (" + obj + "); auto " + needle +
                          " = (" + emitExpr(*e.args[0].value) + "); int64_t __pos = 0; for (const auto& __v : __seq) { if (__v." +
                          field + " == " + needle + ") return __pos; ++__pos; } return int64_t(-1); }()";
                    return result;
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "sum") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP base = optional ? optInner(recv) : recv;
                    TyP elem = base->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string tmp = "__sum" + std::to_string(tmpSeq_++);
                    std::string acc = elem->k == TyK::Float ? "double" : "int64_t";
                    std::string zero = elem->k == TyK::Float ? "0.0" : "0";
                    if (optional) {
                        return "[&]{ auto __opt = (" + obj + "); if (!__opt) return std::optional<" + acc + ">{ }; auto &" + tmp + " = *__opt; " + acc +
                               " __acc = " + zero + "; for (const auto& __v : " + tmp + ") __acc += __v." + valueField(elem) + "; return std::optional<" + acc + ">{__acc}; }()";
                    }
                    return "[&]{ auto &" + tmp + " = (" + obj + "); " + acc +
                           " __acc = " + zero + "; for (const auto& __v : " +
                           tmp + ") __acc += __v." + valueField(elem) +
                           "; return __acc; }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "clear") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    std::string obj = emitExpr(*e.lhs->lhs);
                    if (recv && recv->k == TyK::Opt)
                        return "[&]{ auto __opt = (" + obj + "); if (__opt) __opt->clear(); }()";
                    return obj + ".clear()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "reverse") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    std::string obj = emitExpr(*e.lhs->lhs);
                    if (recv && recv->k == TyK::Opt)
                        return "[&]{ auto __opt = (" + obj + "); if (__opt) std::reverse(__opt->begin(), __opt->end()); }()";
                    return "std::reverse(" + obj + ".begin(), " + obj + ".end())";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "shift") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP elem = (optional ? optInner(recv) : recv)->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string tmp = "__shift" + std::to_string(tmpSeq_++);
                    if (optional) {
                        return "[&]{ auto __opt = (" + obj + "); if (!__opt) return std::optional<" + cppTy(elem) + ">{ }; auto &" + tmp +
                               " = *__opt; if (" + tmp + ".empty()) kirn_native::co_panic(\"shift from empty list\"); auto __v = " + tmp + ".front(); " + tmp + ".erase(" + tmp +
                               ".begin()); return std::optional<" + cppTy(elem) + ">{__v." + valueField(elem) + "}; }()";
                    }
                    return "[&]{ auto &" + tmp + " = (" + obj + "); if (" + tmp + ".empty()) kirn_native::co_panic(\"shift from empty list\"); auto __v = " + tmp + ".front(); " + tmp + ".erase(" + tmp +
                           ".begin()); return __v." + valueField(elem) + "; }()";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "pop") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    bool optional = recv && recv->k == TyK::Opt;
                    TyP elem = (optional ? optInner(recv) : recv)->args[0];
                    std::string obj = emitExpr(*e.lhs->lhs);
                    std::string tmp = "__pop" + std::to_string(tmpSeq_++);
                    if (optional) {
                        return "[&]{ auto __opt = (" + obj + "); if (!__opt) return std::optional<" + cppTy(elem) + ">{ }; auto &" + tmp +
                               " = *__opt; if (" + tmp + ".empty()) kirn_native::co_panic(\"pop from empty list\"); auto __v = " + tmp + ".back(); " + tmp + ".pop_back(); return std::optional<" + cppTy(elem) +
                               ">{__v." + valueField(elem) + "}; }()";
                    }
                    return "[&]{ auto &" + tmp + " = (" + obj + "); if (" + tmp + ".empty()) kirn_native::co_panic(\"pop from empty list\"); auto __v = " + tmp + ".back(); " + tmp + ".pop_back(); return __v." +
                           valueField(elem) + "; }()";
                }
                if (e.lhs->kind == ExKind::Member &&
                    (e.lhs->text == "append" || e.lhs->text == "push_back")) {
                    TyP recv = typeOf(*e.lhs->lhs);
                    if (recv && recv->k == TyK::Opt)
                        return "[&]{ auto __opt = (" + emitExpr(*e.lhs->lhs) + "); if (__opt) __opt->push_back(" +
                               boxExpr(*e.args[0].value) + "); }()";
                    return emitExpr(*e.lhs->lhs) + ".push_back(" +
                           boxExpr(*e.args[0].value) + ")";
                }
                if (e.lhs->kind == ExKind::Member && e.lhs->text == "len") {
                    TyP recv = typeOf(*e.lhs->lhs);
                    std::string obj = emitExpr(*e.lhs->lhs);
                    if (recv && recv->k == TyK::Opt) {
                        std::string tmp = "__len" + std::to_string(tmpSeq_++);
                        return "[&]{ auto " + tmp + " = (" + obj +
                               "); if (!" + tmp + ") return std::optional<int64_t>{}; return " +
                               std::string("std::optional<int64_t>{static_cast<int64_t>(") +
                               tmp + "->size())}; }()";
                    }
                    return "static_cast<int64_t>(" + obj + ".size())";
                }
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
            case ExKind::SafeIndex: {
                TyP elem = optInner(t);
                std::string obj = emitExpr(*e.lhs);
                std::string idx = emitExpr(*e.rhs);
                std::string tmp = "__si" + std::to_string(tmpSeq_++);
                std::string field = valueField(elem);
                return "[&]{ auto " + tmp + " = (" + obj + "); if (!" + tmp +
                       ") return std::nullopt; int64_t __i = (" + idx +
                       "); if (__i < 0) __i += static_cast<int64_t>(" + tmp +
                       "->size()); return std::optional<" + cppTy(optInner(t)) +
                       ">{(" + tmp + ")->at(static_cast<size_t>(__i))." +
                       field + "}; }()";
            }
            case ExKind::Index: {
                TyP elem = typeOf(*e.lhs)->args[0];
                std::string obj = emitExpr(*e.lhs);
                std::string idx = emitExpr(*e.rhs);
                std::string tmp = "__ix" + std::to_string(tmpSeq_++);
                std::string access = tmp + ".at(static_cast<size_t>(__i))";
                if (elem->k == TyK::List)
                    return "[&]{ auto " + tmp + " = (" + obj + "); int64_t __i = (" +
                       idx + "); if (__i < 0) __i += static_cast<int64_t>(" +
                       tmp + ".size()); return *" + access + ".vec; }()";
                return "[&]{ auto " + tmp + " = (" + obj + "); int64_t __i = (" +
                       idx + "); if (__i < 0) __i += static_cast<int64_t>(" +
                       tmp + ".size()); return " + access + "." +
                       valueField(elem) + "; }()";
            }
            default:
                return "0";
        }
    }

    std::string emitMatmul(const ast::Expr& e) {
        TyP lt = typeOf(*e.lhs), rt = typeOf(*e.rhs);
        const int lr = nativeMatRank(lt), rr = nativeMatRank(rt);
        TyP le = lr == 1 ? lt->args[0] : lt->args[0]->args[0];
        TyP re = rr == 1 ? rt->args[0] : rt->args[0]->args[0];
        const std::string a = emitExpr(*e.lhs), b = emitExpr(*e.rhs);
        const std::string av = lr == 1 ? "__a[__i]" : "__a[__i].vec->at(__k)";
        const std::string bv = rr == 1 ? "__b[__k]" : "__b[__k].vec->at(__j)";
        auto num = [](const std::string& v, const TyP& t) {
            return t->k == TyK::Float ? v + ".d"
                                      : "static_cast<double>(" + v + ".i)";
        };
        if (lr == 1 && rr == 1) {
            return "[&]{ auto &__a = (" + a + "); auto &__b = (" + b +"); if (__a.size() != __b.size()) kirn_native::co_panic(\"matmul dimension mismatch\"); double __acc = 0.0; for (size_t __i = 0; __i < __a.size(); ++__i) __acc += " +
                   num(av, le) + " * " + num("__b[__i]", re) + "; return __acc; }()";
        }
        if (lr == 2 && rr == 1) {
            return "[&]{ auto &__a = (" + a + "); auto &__b = (" + b + "); std::vector<kirn::interp::Value> __out; for (const auto& __row : __a) { if (__row.vec->size() != __b.size()) kirn_native::co_panic(\"matmul dimension mismatch\"); double __acc = 0.0; for (size_t __k = 0; __k < __b.size(); ++__k) __acc += " +
                   num("__row.vec->at(__k)", le) + " * " + num("__b[__k]", re) + "; __out.push_back(kirn::interp::Value::floating(__acc)); } return __out; }()";
        }
        if (lr == 1 && rr == 2) {
            return "[&]{ auto &__a = (" + a + "); auto &__b = (" + b + "); if (__a.size() != __b.size()) kirn_native::co_panic(\"matmul dimension mismatch\"); size_t __cols = __b.empty() ? 0 : __b[0].vec->size(); std::vector<kirn::interp::Value> __out; for (size_t __j = 0; __j < __cols; ++__j) { double __acc = 0.0; for (size_t __k = 0; __k < __b.size(); ++__k) { if (__b[__k].vec->size() != __cols) kirn_native::co_panic(\"matmul ragged matrix\"); __acc += " +
                   num("__a[__k]", le) + " * " + num("__b[__k].vec->at(__j)", re) + "; } __out.push_back(kirn::interp::Value::floating(__acc)); } return __out; }()";
        }
        return "[&]{ auto &__a = (" + a + "); auto &__b = (" + b + "); size_t __inner = __a.empty() ? 0 : __a[0].vec->size(); if (__b.size() != __inner) kirn_native::co_panic(\"matmul dimension mismatch\"); size_t __cols = __b.empty() ? 0 : __b[0].vec->size(); std::vector<kirn::interp::Value> __out; for (const auto& __row : __a) { if (__row.vec->size() != __inner) kirn_native::co_panic(\"matmul ragged matrix\"); std::vector<kirn::interp::Value> __orow; for (size_t __j = 0; __j < __cols; ++__j) { double __acc = 0.0; for (size_t __k = 0; __k < __inner; ++__k) { if (__b[__k].vec->size() != __cols) kirn_native::co_panic(\"matmul ragged matrix\"); __acc += " +
               num("__row.vec->at(__k)", le) + " * " + num("__b[__k].vec->at(__j)", re) + "; } __orow.push_back(kirn::interp::Value::floating(__acc)); } __out.push_back(kirn::interp::Value::list(std::move(__orow))); } return __out; }()";
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
            if (lint && rint) {
                if (op == "+") return "kirn_native::co_iadd(" + le + "," + re + ")";
                if (op == "-") return "kirn_native::co_isub(" + le + "," + re + ")";
                return "kirn_native::co_imul(" + le + "," + re + ")";
            }
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
        return t == "int64_t" ? "i"
             : t == "double" ? "d"
             : t == "std::string" ? "s" : "b";
    };
    auto valueCtor = [](const std::string& t) -> const char* {
        return t == "int64_t" ? "integer" : t == "double" ? "floating"
               : t == "std::string" ? "str" : "boolean";
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
    auto callbackExpr = [](const std::vector<std::string>& args,
                           const std::string& ret) {
        std::string out = "[&interp, __fv](";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) out += ", ";
            out += args[i] + " __a" + std::to_string(i);
        }
        out += "){ std::vector<kirn::interp::Value> __args; ";
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& t = args[i];
            std::string box = t == "int64_t" ? "integer" :
                              t == "double" ? "floating" :
                              t == "std::string" ? "str" : "boolean";
            out += "__args.push_back(kirn::interp::Value::" + box +
                   "(__a" + std::to_string(i) + ")); ";
        }
        out += "auto __r = interp.nativeInvoke(__fv, std::move(__args)); ";
        if (ret.empty()) out += "return;";
        else {
            std::string field = ret == "int64_t" ? "i" :
                                ret == "double" ? "d" :
                                ret == "std::string" ? "s" : "b";
            out += "return __r." + field + ";";
        }
        return out + " }";
    };
    auto returnedFunctionExpr = [](const NativeFunc& nf, const std::string& call) {
        std::string out = "[&]{ auto __r = " + call + "; return interp.nativeWrapFunction([__r = std::move(__r)](std::vector<kirn::interp::Value>& __args) -> kirn::interp::Value { ";
        for (size_t i = 0; i < nf.retFnParamTypes.size(); ++i) {
            const std::string& t = nf.retFnParamTypes[i];
            std::string field = t == "int64_t" ? "i" :
                                t == "double" ? "d" :
                                t == "std::string" ? "s" : "b";
            out += t + " __a" + std::to_string(i) + " = __args.at(" +
                   std::to_string(i) + ")." + field + "; ";
        }
        out += "auto __v = __r(";
        for (size_t i = 0; i < nf.retFnParamTypes.size(); ++i) {
            if (i) out += ", ";
            out += "__a" + std::to_string(i);
        }
        out += "); ";
        if (nf.retFnReturnType.empty()) out += "return kirn::interp::Value::none();";
        else {
            const std::string& t = nf.retFnReturnType;
            std::string ctor = t == "int64_t" ? "integer" :
                               t == "double" ? "floating" :
                               t == "std::string" ? "str" : "boolean";
            out += "return kirn::interp::Value::" + ctor + "(__v);";
        }
        return out + " }); }()";
    };
    for (const NativeFunc* nfp : ordered) {
        const NativeFunc& nf = *nfp;
        if (nf.ownerType.empty()) {
            out << "    for (const auto& d : prog) if (d->kind == "
                   "kirn::ast::StKind::FuncDef && d->name == \""
                << nf.name
                << "\") { interp.enableNative(); interp.registerNative(d.get(), ";
        } else if (nf.implTrait.empty()) {
            out << "    for (const auto& d : prog) if (d->kind == "
                   "kirn::ast::StKind::StructDef && d->name == \""
                << nf.ownerType
                << "\") for (const auto& m : d->body) if (m->kind == "
                   "kirn::ast::StKind::FuncDef && m->name == \""
                << nf.name
                << "\") { interp.enableNative(); interp.registerNative(m.get(), ";
        } else {
            out << "    for (const auto& d : prog) if (d->kind == "
                   "kirn::ast::StKind::ImplDef && d->implType && d->implTrait && "
                   "d->implType->name == \""
                << nf.ownerType
                << "\" && d->implTrait->name == \""
                << nf.implTrait
                << "\") for (const auto& m : d->body) if (m->kind == "
                   "kirn::ast::StKind::FuncDef && m->name == \""
                << nf.name
                << "\") { interp.enableNative(); interp.registerNative(m.get(), ";
        }
        out << "[&interp](kirn::interp::Env e) -> kirn::interp::Value {\n";
        // read scalar params out of the caller's Env
        for (size_t i = 0; i < nf.params.size(); ++i) {
            std::string v = "v_";
            for (char c : nf.params[i]) v += isalnum((unsigned char)c) ? c : '_';
            const std::string& pt = nf.pTypes[i];
            if (i < nf.fnParamTypes.size() && !nf.fnParamTypes[i].empty()) {
                bool optional = pt.rfind("std::optional<", 0) == 0;
                std::string callback = callbackExpr(
                    nf.fnParamTypes[i], nf.fnReturnTypes[i]);
                out << "        " << pt << " " << v << "; { auto __fv = "
                    "interp.nativeEnvVar(e, \"" << nf.params[i] << "\"); ";
                if (optional)
                    out << "if (!__fv.isNone()) " << v << " = " << callback << "; }\n";
                else
                    out << "if (__fv.isNone()) kirn::interp::panicHere(\"native: missing function argument\"); "
                        << v << " = " << callback << "; }\n";
            } else if (pt.rfind("std::optional<", 0) == 0) {
                // Optional scalar: map `none` -> nullopt, else the inner value.
                std::string inner = pt.substr(14, pt.size() - 14 - 1);
                out << "        " << pt << " " << v << "; { auto __p = "
                       "interp.nativeEnvVar(e, \""
                    << nf.params[i] << "\"); if (!__p.isNone()) ";
                if (inner == "std::vector<kirn::interp::Value>")
                    out << v << " = *__p.vec; }\n";
                else if (inner == "kirn::interp::Value")
                    out << v << " = __p; }\n";
                else
                    out << v << " = __p." << envField(inner) << "; }\n";
            } else if (pt == "kirn::interp::Value") {
                out << "        " << pt << " " << v
                    << " = interp.nativeEnvVar(e, \""
                    << nf.params[i] << "\");\n";
            } else if (pt == "std::vector<kirn::interp::Value>") {
                out << "        " << pt << " " << v
                    << " = *interp.nativeEnvVar(e, \""
                    << nf.params[i] << "\").vec;\n";
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
        else if (!nf.retFnParamTypes.empty()) {
            out << "        return " << returnedFunctionExpr(nf, call) << ";\n";
        } else if (nf.retCpp.rfind("std::optional<", 0) == 0) {
            // Optional scalar return: box none or the inner scalar.
            std::string inner =
                nf.retCpp.substr(14, nf.retCpp.size() - 14 - 1);
            out << "        { auto __r = " << call
                << "; if (!__r) return kirn::interp::Value::none();";
            if (inner == "std::vector<kirn::interp::Value>")
                out << " return kirn::interp::Value::list(*__r); }\n";
            else if (inner == "kirn::interp::Value")
                out << " return *__r; }\n";
            else
                out << " return kirn::interp::Value::" << valueCtor(inner)
                    << "(*__r); }\n";
        } else if (nf.retCpp == "std::vector<kirn::interp::Value>")
            out << "        return kirn::interp::Value::list(" << call
                << ");\n";
        else if (nf.retCpp == "kirn::interp::Value")
            out << "        return " << call << ";\n";
        else
            out << "        return kirn::interp::Value::" << valueCtor(nf.retCpp)
                << "(" << call << ");\n";
        out << "    }); }\n";
    }
    out << "}\n";
}

} // namespace backend
} // namespace kirn
