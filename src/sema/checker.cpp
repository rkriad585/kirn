// Semantic analysis for Kirn v1. See checker.h for the design overview.
#include "sema/checker.h"
#include "sema/borrow.h"
#include "support/literals.h"

#include <functional>

namespace kirn {
namespace sema {

using namespace kirn::ast;

static bool dynish(const TyP& t);

// ============================ type utilities ================================

bool equal(const TyP& a, const TyP& b) {
    if (!a || !b) return a.get() == b.get();
    if (a->k != b->k) return false;
    if (a->name != b->name || a->variant != b->variant) return false;
    if (a->args.size() != b->args.size()) return false;
    for (size_t i = 0; i < a->args.size(); ++i)
        if (!equal(a->args[i], b->args[i])) return false;
    if ((a->inner == nullptr) != (b->inner == nullptr)) return false;
    if (a->inner && !equal(a->inner, b->inner)) return false;
    return true;
}

std::string toString(const Ty& t) {
    switch (t.k) {
        case TyK::Error:   return "<error>";
        case TyK::Unknown: return "<unknown>";
        case TyK::None:    return "none";
        case TyK::Bool:    return t.name.empty() ? "Bool" : t.name;
        case TyK::Int:     return t.name.empty() ? "Int" : t.name;
        case TyK::Float:   return t.name.empty() ? "Float" : t.name;
        case TyK::BigInt:  return t.name.empty() ? "BigInt" : t.name;
        case TyK::Rational: return t.name.empty() ? "Rational" : t.name;
        case TyK::Complex: return t.name.empty() ? "Complex" : t.name;
        case TyK::Decimal: return t.name.empty() ? "Decimal" : t.name;
        case TyK::Str:     return t.name.empty() ? "Str" : t.name;
        case TyK::Char:    return t.name.empty() ? "Char" : t.name;
        case TyK::Bytes:     return t.name.empty() ? "bytes" : t.name;
        case TyK::ByteArray: return t.name.empty() ? "bytearray" : t.name;
        case TyK::MemoryView: return t.name.empty() ? "memoryview" : t.name;
        case TyK::List:    return t.args.empty() ? "list" : "list[" + toString(*t.args[0]) + "]";
        case TyK::Dict:
            return t.args.size() < 2
                ? "dict"
                : "dict[" + toString(*t.args[0]) + ", " + toString(*t.args[1]) + "]";
        case TyK::Set:     return t.args.empty() ? "set" : "set[" + toString(*t.args[0]) + "]";
        case TyK::Deque:   return t.args.empty() ? "deque" : "deque[" + toString(*t.args[0]) + "]";
        case TyK::Counter: return t.args.empty() ? "Counter" : "Counter[" + toString(*t.args[0]) + "]";
        case TyK::Chan:    return t.args.empty() ? "chan" : "chan[" + toString(*t.args[0]) + "]";
        case TyK::Range:   return "range";
        case TyK::Gen:     return t.args.empty() ? "generator" : "generator[" + toString(*t.args[0]) + "]";
        case TyK::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += toString(*t.args[i]);
            }
            return s + ")";
        }
        case TyK::Fn: {
            std::string s = "fn(";
            for (size_t i = 0; i < t.args.size(); ++i) {
                if (i) s += ", ";
                s += toString(*t.args[i]);
            }
            s += ") -> ";
            s += t.inner ? toString(*t.inner) : "none";
            return s;
        }
        case TyK::Opt:      return (t.inner ? toString(*t.inner)
                                    : !t.args.empty() ? toString(*t.args[0])
                                                      : "?") + std::string("?");
        case TyK::Ptr:      return "*" + (t.inner ? toString(*t.inner)
                                    : !t.args.empty() ? toString(*t.args[0])
                                                      : "");
        case TyK::Ref:      return (t.name == "mut" ? "&mut " : "&") +
                                   (t.inner ? toString(*t.inner)
                                    : !t.args.empty() ? toString(*t.args[0])
                                                      : "");
        case TyK::Struct:   return t.name;
        case TyK::EnumName: return t.name;
        case TyK::EnumVal:  return t.name + "." + t.variant;
        case TyK::TraitObj: return t.name;
        case TyK::TypeVar:  return t.name;
    }
    return "<ty>";
}

namespace {

// RAII: bind generic type params while a generic declaration is processed.
struct TvGuard {
    std::map<std::string, TyP>& m;
    std::vector<std::string> names;

    TvGuard(std::map<std::string, TyP>& mm,
            const std::vector<ast::TypeParam>& tps)
        : m(mm) {
        for (const auto& tp : tps) {
            names.push_back(tp.name);
            m[tp.name] = Ty::named(TyK::TypeVar, tp.name);
        }
    }
    ~TvGuard() {
        for (const auto& n : names) {
            auto it = m.find(n);
            if (it != m.end()) m.erase(it);
        }
    }
};

template <typename T>
struct Save {
    T& ref;
    T old;
    explicit Save(T& r) : ref(r), old(r) {}
    ~Save() { ref = old; }
};

TyP nominalWithArgs(TyK k, std::string n, std::vector<TyP> args) {
    auto t = std::shared_ptr<Ty>(new Ty());
    t->k = k;
    t->name = std::move(n);
    t->args = std::move(args);
    return t;
}

TyP argAt(const TyP& t, size_t i) {
    return (t && i < t->args.size()) ? t->args[i] : unkTy();
}

// Recursively replace TypeVars in a generic declaration's return type:
// constraint/default activations win, unconstrained TypeVars without a default
// stay as-is (inference-only). Nodes are rebuilt along the path so the shared
// declaration signature is never mutated.
TyP substGeneric(const TyP& t, const std::map<std::string, TyP>& activ,
                 uint32_t line, uint32_t col) {
    (void)line; (void)col;
    if (!t) return t;
    if (t->is(TyK::TypeVar)) {
        auto it = activ.find(t->name);
        return it != activ.end() ? it->second : t;
    }
    bool changed = false;
    std::vector<TyP> args;
    args.reserve(t->args.size());
    for (const auto& a : t->args) {
        TyP s = substGeneric(a, activ, 0, 0);
        if (s != a) changed = true;
        args.push_back(std::move(s));
    }
    TyP inner;
    if (t->inner) {
        inner = substGeneric(t->inner, activ, 0, 0);
        if (inner != t->inner) changed = true;
    }
    if (!changed) return t;
    auto out = std::shared_ptr<Ty>(new Ty());
    out->k = t->k;
    out->name = t->name;
    out->variant = t->variant;
    out->args = std::move(args);
    out->inner = std::move(inner);
    return out;
}

// Collect the enum variants covered by a single arm pattern (PLAN 5.4).
void collectPatCoverage(const ast::Pat& p, std::set<std::string>& covered,
                        bool& wild, const std::string& enumName, Scope* scope) {
    switch (p.kind) {
        case ast::PatKind::Wild:
            wild = true;
            break;
        case ast::PatKind::Ctor: {   // `case Running(pid)`
            if (Symbol* vs = scope->find(p.ctorName);
                vs && vs->kind == SymK::EnumVariant && vs->enumOf == enumName)
                covered.insert(p.ctorName);
            break;
        }
        case ast::PatKind::Bind: {   // `case Idle` (discriminant match)
            if (Symbol* vs = scope->find(p.bindName);
                vs && vs->kind == SymK::EnumVariant && vs->enumOf == enumName)
                covered.insert(p.bindName);
            break;
        }
        case ast::PatKind::Or:
            for (const auto& alt : p.alts)
                collectPatCoverage(*alt, covered, wild, enumName, scope);
            break;
        case ast::PatKind::Ref:
            if (p.inner)
                collectPatCoverage(*p.inner, covered, wild, enumName, scope);
            break;
        case ast::PatKind::BindAlias:
            if (p.aliasSub)
                collectPatCoverage(*p.aliasSub, covered, wild, enumName, scope);
            break;
        default:
            break;
    }
}

bool isBoolish(const TyP& t) {
    return !t || t->isError() || t->isUnknown() || t->is(TyK::Bool);
}

} // namespace

// ============================== infrastructure ==============================

void Checker::error(uint32_t line, uint32_t col, const std::string& msg) {
    if (quiet_ > 0) return;   // speculative re-walk: real pass already reported
    diags_.report(line, col, msg);
}

void Checker::error(const ast::Span& s, const std::string& msg) {
    if (quiet_ > 0) return;   // speculative re-walk: real pass already reported
    // Richer path: use the full extent when the AST node carries one.
    if (s.endLine || s.endCol) {
        diags_.error(SpanRange{s.line, s.col, s.endLine, s.endCol + 1})
            .msg(msg)
            .emit();
    } else {
        diags_.report(s.line, s.col, msg);
    }
}

void Checker::warning(SpanRange span, const std::string& code,
                      const std::string& msg) {
    if (quiet_ > 0) return;   // speculative re-walk: real pass already reported
    if (lintAllow_.count(code)) return;
    if (lintDeny_.count(code)) {
        diags_.error(span).code(code).msg(msg).emit();
        return;
    }
    diags_.warning(span).code(code).msg(msg).emit();
}

void Checker::narrowDiag(uint32_t line, uint32_t col, const std::string& name) {
    const std::string msg =
        "flow narrowing skipped for mutable binding '" + name +
        "' (bind it with const to enable narrowing)";
    if (strict_) error(line, col, msg);
    else warning(SpanRange::point(line, col), "W-narrow", msg);
}

void Checker::reportUnused(Scope& s, bool topLevel) {
    for (auto& [name, sym] : s.syms) {
        if (sym->used || sym->pub) continue;
        if (topLevel) {
            switch (sym->kind) {
                case SymK::Func:
                    if (name == "main") break;   // entry point
                    warning(SpanRange::point(sym->declLine, sym->declCol),
                            "W0103",
                            "unused function '" + name + "'");
                    break;
                case SymK::ImportRoot:
                    // star imports and re-exports are exempt; the import
                    // statement owns the binding, so point the caret there.
                    if (sym->importStmt) {
                        const ast::Stmt& im = *sym->importStmt;
                        if (im.starImport) break;
                        if (im.pub) break;
                        warning(SpanRange::point(im.span.line, im.span.col),
                                "W0101", "unused import '" + name + "'");
                    }
                    break;
                case SymK::Const:
                    // `pub` consts are part of a module's public API; they
                    // may be unused within the module but consumed externally.
                    if (sym->pub) break;
                    warning(SpanRange::point(sym->declLine, sym->declCol),
                            "W0102", "unused constant '" + name + "'");
                    break;
                default:
                    break;
            }
            continue;
        }
if (sym->kind == SymK::Var || sym->kind == SymK::Param ||
            sym->kind == SymK::Const) {
            if (name == "_" || name == "self") continue;   // idiomatic
            if (name.size() > 1 && name[0] == '_') continue;   // intentional
            warning(SpanRange::point(sym->declLine, sym->declCol),
                    "W0102", "unused variable '" + name + "'");
            if ((sym->kind == SymK::Var || sym->kind == SymK::Param) &&
                sym->mut && sym->used && !sym->mutated)
                warning(SpanRange::point(sym->declLine, sym->declCol),
                        "W0106",
                        "'var' binding '" + name + "' is never mutated");
        }
    }
}

Scope& Checker::push() {
    narrowedStack_.push_back(narrowed_);
    scope_ = new Scope(scope_);
    return *scope_;
}

void Checker::pop() {
    Scope* p = scope_->parent;
    if (scope_ != p) reportUnused(*scope_, /*topLevel*/ false);
    delete scope_;
    scope_ = p;
    if (!narrowedStack_.empty()) {
        narrowed_ = std::move(narrowedStack_.back());
        narrowedStack_.pop_back();
    }
}

SymP Checker::declareLocal(SymK kind, const std::string& name, TyP t, bool mut,
                           uint32_t line, uint32_t col) {
    auto sym = std::make_shared<Symbol>();
    sym->kind = kind;
    sym->name = name;
    sym->type = std::move(t);
    sym->mut = mut;
    sym->homeScope = scope_ ? scope_->id : 0;
    sym->declLine = line;
    sym->declCol = col;
    // W0105: a local that hides a binding from an outer scope.
    if (kind == SymK::Var || kind == SymK::Param) {
        if (Symbol* shadowed = scope_->parent ? scope_->parent->find(name)
                                              : nullptr) {
            if (shadowed->kind == SymK::Var || shadowed->kind == SymK::Param ||
                shadowed->kind == SymK::Const || shadowed->kind == SymK::ImportRoot)
                warning(SpanRange::point(line, col), "W0105",
                        "variable '" + name + "' shadows an outer binding");
        }
    }
    if (Symbol* clash = scope_->declare(sym)) {
        // A prelude builtin may be overridden by a user declaration, exactly as
        // user functions override builtins (see registerTopLevel).
        if (clash->builtin && !sym->builtin) {
            scope_->syms[sym->name] = sym;
        } else {
            error(line, col, "'" + name + "' is already defined in this scope");
        }
    }
    return sym;
}

// ============================== module driver ===============================

void Checker::checkModule(const std::vector<ast::StmtP>& prog) {
    push();                                    // global scope
    rootScopeId_ = scope_->id;
    predeclareBuiltins();
    registerNominals(prog);
    fillNominals(prog);
    registerTopLevel(prog);

    for (const auto& st : prog) {
        switch (st->kind) {
            case StKind::FuncDef:
                if (!st->externDef) checkFuncLike(*st, nullptr, funcs_[st->name]);
                break;

            case StKind::StructDef: {
                Save<TyP> selfGuard(selfTy_);
                selfTy_ = lookupNominal(TyK::Struct, st->name);
                TvGuard tv(typeVars_, st->typeParams);
                for (const auto& m : st->body)
                    if (m->kind == StKind::FuncDef) checkFuncLike(*m, selfTy_, nullptr);
                break;
            }

            case StKind::TraitDef: {
                Save<TyP> selfGuard(selfTy_);
                selfTy_ = Ty::named(TyK::TraitObj, st->name);
                for (const auto& m : st->body)
                    if (m->kind == StKind::FuncDef) checkFuncLike(*m, selfTy_, nullptr);
                break;
            }

            case StKind::ImplDef: {
                Save<TyP> selfGuard(selfTy_);
                selfTy_ = resolveType(st->implType);
                TvGuard tv(typeVars_, st->typeParams);
                for (const auto& m : st->body)
                    if (m->kind == StKind::FuncDef) checkFuncLike(*m, selfTy_, nullptr);
                break;
            }

            case StKind::ConstDecl:      // handled during registration
            case StKind::Import:
            case StKind::Export:
                break;

            default:
                checkStmt(*st);
                break;
        }
    }
    reportUnused(*scope_, /*topLevel*/ true);
    pop();

    // Phase 5.5: conservative borrow-escape analysis.
    BorrowChecker borrow(diags_);
    borrow.checkModule(prog);
}

void Checker::predeclareBuiltins() {
    // trait Iterator[T]: next() -> T?
    auto iter = std::make_shared<Symbol>();
    iter->kind = SymK::Trait;
    iter->name = "Iterator";
    FuncSig nextSig;
    nextSig.ret = optTy(Ty::named(TyK::TypeVar, "T"));
    iter->sigs["next"] = nextSig;
    traits_["Iterator"] = iter;
    scope_->declare(iter);

    // result[OkT, ErrT] nominal + ok()/err() variant constructors (patterns).
    auto res = std::make_shared<Symbol>();
    res->kind = SymK::Struct;
    res->name = "result";
    structs_["result"] = res;
    scope_->declare(res);

    auto okSym = std::make_shared<Symbol>();
    okSym->kind = SymK::EnumVariant;
    okSym->name = "ok";
    okSym->enumOf = "result";
    okSym->payloads.emplace_back("value", unkTy());
    scope_->declare(okSym);

    auto errSym = std::make_shared<Symbol>();
    errSym->kind = SymK::EnumVariant;
    errSym->name = "err";
    errSym->enumOf = "result";
    errSym->payloads.emplace_back("err", unkTy());
    scope_->declare(errSym);

    // Free builtins: print(...) len(x) Sqrt(x) range(...). User decls may shadow.
    makeBuiltinFunc("print", {unkTy()}, {"x"}, noneTy(), /*variadic*/ true, 0);
    makeBuiltinFunc("len", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Sqrt", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("range", {intTy(), intTy(), intTy()},
                    {"start", "end", "step"}, rangeTy(intTy()),
                    /*variadic*/ true, 1);
    makeBuiltinFunc("panic", {unkTy()}, {"msg"}, unkTy(),
                    /*variadic*/ true, 0);
    makeBuiltinFunc("catch_panic", {unkTy()}, {"thunk"},
                    nominalWithArgs(TyK::Struct, "result", {unkTy(), unkTy()}),
                    false, 1);
    makeBuiltinFunc("Ord", {charTy()}, {"c"}, intTy(), false, 1);
    makeBuiltinFunc("__Ord", {charTy()}, {"c"}, intTy(), false, 1);
    makeBuiltinFunc("Chr", {intTy()}, {"n"}, charTy(), false, 1);
    makeBuiltinFunc("__Chr", {intTy()}, {"n"}, charTy(), false, 1);
    makeBuiltinFunc("BitLength", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__BitLength", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("BitCount", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__BitCount", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("assert", {boolTy()}, {"cond"}, noneTy(), false, 1);
    makeBuiltinFunc("assert_eq", {unkTy(), unkTy()}, {"a", "b"}, noneTy(),
                    false, 2);
    // Rational and float builtins (Phase 2)
    makeBuiltinFunc("Rational", {intTy(), intTy()}, {"num", "den"}, rationalTy(), false, 1);
    makeBuiltinFunc("__Rational", {intTy(), intTy()}, {"num", "den"}, rationalTy(), false, 1);
    makeBuiltinFunc("Numerator", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Numerator", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Denominator", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Denominator", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("ToFloat", {unkTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__ToFloat", {unkTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("ToInt", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__ToInt", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("FAbs", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__FAbs", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Abs", {unkTy()}, {"x"}, unkTy(), false, 1);
    makeBuiltinFunc("add", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("mul", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("Exp", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Exp", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Log", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Log", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Log2", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Log2", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Log10", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Log10", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Sin", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Sin", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Cos", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Cos", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Tan", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Tan", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Sinh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Sinh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Cosh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Cosh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Tanh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Tanh", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Floor", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Floor", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Ceil", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Ceil", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Round", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Round", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Trunc", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("__Trunc", {floatTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Fract", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("__Fract", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("IsNan", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("__IsNan", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("IsInfinite", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("__IsInfinite", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("IsFinite", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("__IsFinite", {floatTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("BigInt", {unkTy()}, {"s"}, bigIntTy(), false, 1);
    makeBuiltinFunc("__BigInt", {unkTy()}, {"s"}, bigIntTy(), false, 1);
    makeBuiltinFunc("IsPrime", {unkTy()}, {"n"}, boolTy(), false, 1);
    makeBuiltinFunc("__IsPrime", {unkTy()}, {"n"}, boolTy(), false, 1);
    makeBuiltinFunc("PowMod", {unkTy(), unkTy(), unkTy()}, {"b", "e", "m"}, unkTy(), false, 1);
    makeBuiltinFunc("__PowMod", {unkTy(), unkTy(), unkTy()}, {"b", "e", "m"}, unkTy(), false, 1);
    makeBuiltinFunc("ToBytes", {unkTy(), intTy(), strTy()}, {"x", "len", "endian"}, unkTy(), false, 2);
    makeBuiltinFunc("__ToBytes", {unkTy(), intTy(), strTy()}, {"x", "len", "endian"}, unkTy(), false, 2);
    makeBuiltinFunc("FromBytes", {unkTy()}, {"b"}, unkTy(), false, 1);
    makeBuiltinFunc("__FromBytes", {unkTy()}, {"b"}, unkTy(), false, 1);
    makeBuiltinFunc("HexDigits", {unkTy(), intTy()}, {"x", "n"}, strTy(), false, 2);
    makeBuiltinFunc("__HexDigits", {unkTy(), intTy()}, {"x", "n"}, strTy(), false, 2);
    makeBuiltinFunc("ToString", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("__ToString", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("ToRational", {unkTy()}, {"x"}, unkTy(), false, 1);
    makeBuiltinFunc("__ToRational", {unkTy()}, {"x"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalAdd", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("__RationalAdd", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalSub", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("__RationalSub", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalMul", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("__RationalMul", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalDiv", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("__RationalDiv", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalPow", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("__RationalPow", {unkTy(), unkTy()}, {"a", "b"}, unkTy(), false, 1);
    makeBuiltinFunc("RationalEq", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalEq", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("RationalNe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalNe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("RationalLt", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalLt", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("RationalLe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalLe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("RationalGt", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalGt", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("RationalGe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    makeBuiltinFunc("__RationalGe", {unkTy(), unkTy()}, {"a", "b"}, boolTy(), false, 1);
    // Lowercase spelling aliases for the numeric constructors (plan syntax).
    makeBuiltinFunc("rational", {intTy(), intTy()}, {"num", "den"}, rationalTy(), false, 1);
    makeBuiltinFunc("big_int", {unkTy()}, {"s"}, bigIntTy(), false, 1);
    // M6 numeric types: complex (rectangular) and decimal (arbitrary precision).
    makeBuiltinFunc("Complex", {unkTy(), unkTy()}, {"re", "im"}, complexTy(), false, 1);
    makeBuiltinFunc("__Complex", {unkTy(), unkTy()}, {"re", "im"}, complexTy(), false, 1);
    makeBuiltinFunc("complex", {unkTy(), unkTy()}, {"re", "im"}, complexTy(), false, 1);
    makeBuiltinFunc("Decimal", {unkTy()}, {"x"}, decimalTy(), false, 1);
    makeBuiltinFunc("__Decimal", {unkTy()}, {"x"}, decimalTy(), false, 1);
    makeBuiltinFunc("decimal", {unkTy()}, {"x"}, decimalTy(), false, 1);
    makeBuiltinFunc("ComplexReal", {complexTy()}, {"z"}, floatTy(), false, 1);
    makeBuiltinFunc("ComplexImag", {complexTy()}, {"z"}, floatTy(), false, 1);
    makeBuiltinFunc("ComplexConj", {complexTy()}, {"z"}, complexTy(), false, 1);
    makeBuiltinFunc("ComplexAbs", {complexTy()}, {"z"}, floatTy(), false, 1);
    // M7: bytes / bytearray / memoryview construction and helpers.
    makeBuiltinFunc("bytes", {unkTy()}, {"x"}, bytesTy(), false, 1);
    makeBuiltinFunc("__Bytes", {unkTy()}, {"x"}, bytesTy(), false, 1);
    makeBuiltinFunc("bytearray", {unkTy()}, {"x"}, byteArrayTy(), false, 1);
    makeBuiltinFunc("__ByteArray", {unkTy()}, {"x"}, byteArrayTy(), false, 1);
    makeBuiltinFunc("memoryview", {unkTy()}, {"x"}, memoryViewTy(), false, 1);
    makeBuiltinFunc("__MemoryView", {unkTy()}, {"x"}, memoryViewTy(), false, 1);
    makeBuiltinFunc("fromhex", {strTy()}, {"s"}, bytesTy(), false, 1);
    makeBuiltinFunc("__FromHex", {strTy()}, {"s"}, bytesTy(), false, 1);
    // M8: deque / Counter constructors and collection free-functions.
    makeBuiltinFunc("Deque", {unkTy(), intTy()}, {"xs", "maxlen"}, dequeTy(),
                    false, 1);
    makeBuiltinFunc("deque", {unkTy(), intTy()}, {"xs", "maxlen"}, dequeTy(),
                    false, 1);
    makeBuiltinFunc("Counter", {unkTy()}, {"xs"}, counterTy(), false, 1);
    makeBuiltinFunc("counter", {unkTy()}, {"xs"}, counterTy(), false, 1);
    makeBuiltinFunc("group_by", {unkTy(), unkTy()}, {"f", "xs"},
                    unkTy(), false, 2);
    makeBuiltinFunc("chunk", {unkTy(), intTy()}, {"xs", "n"},
                    listTy(listTy(unkTy())), false, 2);
    makeBuiltinFunc("flatten", {unkTy()}, {"xs"}, listTy(unkTy()), false, 1);
    makeBuiltinFunc("count", {unkTy(), unkTy()}, {"xs", "x"}, intTy(), false, 2);
    makeBuiltinFunc("find", {unkTy(), unkTy()}, {"xs", "x"}, intTy(), false, 2);
    makeBuiltinFunc("binary_search", {unkTy(), unkTy()}, {"xs", "x"}, intTy(),
                    false, 2);
    makeBuiltinFunc("partition", {unkTy(), unkTy()}, {"f", "xs"}, unkTy(),
                    false, 2);
    makeBuiltinFunc("heappush", {unkTy(), unkTy()}, {"h", "x"}, noneTy(), false, 2);
    makeBuiltinFunc("heappop", {unkTy()}, {"h"}, unkTy(), false, 1);
    makeBuiltinFunc("heapify", {unkTy()}, {"h"}, noneTy(), false, 1);
    makeBuiltinFunc("heappush_max", {unkTy(), unkTy()}, {"h", "x"}, noneTy(),
                    false, 2);
    makeBuiltinFunc("heappop_max", {unkTy()}, {"h"}, unkTy(), false, 1);
    makeBuiltinFunc("nlargest", {intTy(), unkTy()}, {"n", "xs"},
                    listTy(unkTy()), false, 2);
    makeBuiltinFunc("nsmallest", {intTy(), unkTy()}, {"n", "xs"},
                    listTy(unkTy()), false, 2);
    // M9: `sync` primitives. Handles are dynamically typed; methods resolve at
    // runtime and the sendability gate treats them as opaque shared objects.
    makeBuiltinFunc("Mutex", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("RwMutex", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("RWLock", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("WaitGroup", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("Once", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("Cond", {}, {}, unkTy(), false, 0);
    makeBuiltinFunc("Atomic", {unkTy()}, {"init"}, unkTy(), false, 0);
    makeBuiltinFunc("CopySign", {floatTy(), floatTy()}, {"x", "y"}, floatTy(), false, 1);
    makeBuiltinFunc("__CopySign", {floatTy(), floatTy()}, {"x", "y"}, floatTy(), false, 1);
    makeBuiltinFunc("Pow", {floatTy(), floatTy()}, {"x", "y"}, floatTy(), false, 1);
    makeBuiltinFunc("__Pow", {floatTy(), floatTy()}, {"x", "y"}, floatTy(), false, 1);
    // WHY-1 / SP-8: batteries-included conversion & collection builtins.
    // Polymorphic params use Unknown (dynamic v1) matched at runtime.
    makeBuiltinFunc("Str", {unkTy(), intTy()}, {"x", "base"}, strTy(), false, 1);
    makeBuiltinFunc("Int", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("Float", {unkTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("Bool", {unkTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("type", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("typeof", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("repr", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("sum", {unkTy()}, {"xs"}, unkTy(), false, 1);
    makeBuiltinFunc("Min", {unkTy()}, {"xs"}, unkTy(), false, 1);
    makeBuiltinFunc("Max", {unkTy()}, {"xs"}, unkTy(), false, 1);
    makeBuiltinFunc("any", {unkTy()}, {"xs"}, boolTy(), false, 1);
    makeBuiltinFunc("all", {unkTy()}, {"xs"}, boolTy(), false, 1);
    makeBuiltinFunc("sorted", {unkTy()}, {"xs"}, listTy(unkTy()), false, 1);
    makeBuiltinFunc("reversed", {unkTy()}, {"xs"}, listTy(unkTy()), false, 1);
    makeBuiltinFunc("enumerate", {unkTy()}, {"xs"}, listTy(unkTy()), false, 1);
    makeBuiltinFunc("map", {unkTy(), unkTy()}, {"f", "xs"}, listTy(unkTy()),
                    false, 2);
    makeBuiltinFunc("filter", {unkTy(), unkTy()}, {"f", "xs"}, listTy(unkTy()),
                    false, 2);
    makeBuiltinFunc("reduce", {unkTy(), unkTy(), unkTy()},
                    {"f", "xs", "init"}, unkTy(), false, 3);
    makeBuiltinFunc("upper", {strTy()}, {"s"}, strTy(), false, 1);
    makeBuiltinFunc("lower", {strTy()}, {"s"}, strTy(), false, 1);
    makeBuiltinFunc("trim", {strTy()}, {"s"}, strTy(), false, 1);
    makeBuiltinFunc("contains", {strTy(), strTy()}, {"s", "sub"}, boolTy(),
                    false, 2);
    makeBuiltinFunc("starts_with", {strTy(), strTy()}, {"s", "prefix"},
                    boolTy(), false, 2);
    makeBuiltinFunc("ends_with", {strTy(), strTy()}, {"s", "suffix"}, boolTy(),
                    false, 2);
    makeBuiltinFunc("replace", {strTy(), strTy(), strTy()},
                    {"s", "from", "to"}, strTy(), false, 3);
    makeBuiltinFunc("split", {strTy(), strTy()}, {"s", "sep"}, listTy(strTy()),
                    false, 2);
    makeBuiltinFunc("join", {strTy(), unkTy()}, {"sep", "xs"}, strTy(), false,
                    2);

    // keyword constants
    declareConst("true", boolTy());
    declareConst("false", boolTy());
    declareConst("none", noneTy());
    declareConst("nil", noneTy());          // alias of `none`
    declareConst("NaN", floatTy());
    declareConst("inf", floatTy());

    // pseudo-modules used by the standard environment (dynamically typed v1).
    // Not declared into scope: user `import time` etc. must not clash; the
    // importRoots_ set alone makes bare references resolve dynamically.
    for (const char* mod : {"time", "mem", "io", "os", "json", "math", "sync"})
        importRoots_.insert(mod);

    // Publish every name that resolves at top level without a user declaration,
    // so consumers do not maintain their own list. Two sources, and both are
    // needed:
    //   - the root scope, NOT filtered on the `builtin` flag: `true` / `false` /
    //     `none` go in via declareConst and are not flagged, and filtering them
    //     left the three most-referenced names in the language unresolvable;
    //   - importRoots_, the pseudo-modules (time, mem, io, os, json, math), which
    //     are deliberately NOT in the scope -- see the note at their declaration.
    //     Reading only the scope left `io.open(...)` unresolvable.
    builtinNames_.clear();
    builtinNames_.reserve(scope_->syms.size() + importRoots_.size());
    for (const auto& [n, sym] : scope_->syms) {
        (void)sym;
        builtinNames_.push_back(n);
    }
    for (const auto& n : importRoots_) builtinNames_.push_back(n);
}

SymP Checker::declareConst(std::string name, TyP t) {
    auto sym = std::make_shared<Symbol>();
    sym->kind = SymK::Const;
    sym->name = std::move(name);
    sym->type = std::move(t);
    sym->used = true;    // keyword literals are always "used"
    scope_->declare(sym);
    return sym;
}

SymP Checker::makeBuiltinFunc(std::string name, std::vector<TyP> params,
                              std::vector<std::string> names, TyP ret,
                              bool variadic, size_t required) {
    auto sym = std::make_shared<Symbol>();
    sym->kind = SymK::Func;
    sym->name = std::move(name);
    sym->sig.params = std::move(params);
    sym->sig.names = std::move(names);
    sym->sig.ret = std::move(ret);
    sym->sig.variadic = variadic;
    sym->sig.required = required;
    funcs_[sym->name] = sym;
    sym->used = true;                // builtins are exempt from the unused-func lint
    sym->builtin = true;
    scope_->declare(sym);
    return sym;
}

// ========================== declaration collection ==========================

void Checker::registerNominals(const std::vector<ast::StmtP>& prog) {
    for (const auto& st : prog) {
        switch (st->kind) {
            case StKind::StructDef: {
                auto sym = std::make_shared<Symbol>();
                sym->kind = SymK::Struct;
                sym->name = st->name;
                sym->pub = st->pub;
                structs_[st->name] = sym;
                structDecls_[st->name] = st.get();
                {
                    // Field types, resolved once, while the struct's own type
                    // parameters are in scope. Recorded because nothing else here
                    // resolves them, and a later reader cannot: see
                    // Checker::fieldTypes.
                    TvGuard tv(typeVars_, st->typeParams);
                    std::vector<TyP> fts;
                    fts.reserve(st->fields.size());
                    for (const auto& fd : st->fields)
                        fts.push_back(fd.type ? resolveType(fd.type) : unkTy());
                    structFields_[st.get()] = std::move(fts);
                }
                if (Symbol* clash = scope_->declare(sym))
                    error(st->span.line, st->span.col,
                          "duplicate definition of '" + st->name + "'");
                break;
            }
            case StKind::EnumDef: {
                auto sym = std::make_shared<Symbol>();
                sym->kind = SymK::EnumName;
                sym->name = st->name;
                enums_[st->name] = sym;
                if (Symbol* clash = scope_->declare(sym))
                    error(st->span.line, st->span.col,
                          "duplicate definition of '" + st->name + "'");
                break;
            }
            case StKind::TraitDef: {
                SymP sym;
                auto it = traits_.find(st->name);
                if (it != traits_.end()) {
                    sym = it->second;                 // builtin predeclared
                    sym->pub = st->pub;
                } else {
                    sym = std::make_shared<Symbol>();
                    sym->kind = SymK::Trait;
                    sym->name = st->name;
                    traits_[st->name] = sym;
                }
                if (Symbol* clash = scope_->declare(sym))
                    error(st->span.line, st->span.col,
                          "duplicate definition of '" + st->name + "'");
                break;
            }
            case StKind::ImplDef: {
                std::string traitN =
                    (st->implTrait && st->implTrait->kind == TyKind::Name)
                        ? st->implTrait->name
                        : "";
                std::string typeN =
                    (st->implType && st->implType->kind == TyKind::Name)
                        ? st->implType->name
                        : "";
                if (!traitN.empty() && !typeN.empty())
                    impls_.insert({traitN, typeN});
                // Record the methods each impl block supplies, so protocol
                // checks (`with`/`using`) can see impl-provided methods even
                // though they are not merged into the struct's inherent table.
                if (!typeN.empty())
                    for (const auto& m : st->body)
                        if (m->kind == StKind::FuncDef)
                            implMethods_[typeN].insert(m->name);
                break;
            }
            default:
                break;
        }
    }
}

void Checker::fillNominals(const std::vector<ast::StmtP>& prog) {
    for (const auto& st : prog) {
        switch (st->kind) {
            case StKind::StructDef:
                fillStructBody(structs_[st->name], *st);
                break;

            case StKind::EnumDef: {
                TvGuard tv(typeVars_, st->typeParams);
                auto es = enums_[st->name];
                for (const auto& v : st->variants) {
                    if (es) es->variants.push_back(v.name);
                    auto vs = std::make_shared<Symbol>();
                    vs->kind = SymK::EnumVariant;
                    vs->name = v.name;
                    vs->enumOf = st->name;
                    for (const auto& p : v.payload)
                        vs->payloads.emplace_back(p.name, resolveType(p.type));
                    if (es)
                        for (const auto& p : vs->payloads)
                            es->variantPayloads[v.name].push_back(p);
                    if (Symbol* clash = scope_->declare(vs))
                        error(v.span,
                              "'" + v.name + "' is already defined");
                }
                break;
            }

            case StKind::TraitDef: {
                SymP tr = traits_[st->name];
                TvGuard tv(typeVars_, st->typeParams);
                for (const auto& sg : st->sigs)
                    tr->sigs[sg.name] = resolveSig(sg.params, sg.ret, /*stripSelf*/ true);
                for (const auto& m : st->body)
                    if (m->kind == StKind::FuncDef && !tr->sigs.count(m->name))
                        tr->sigs[m->name] = resolveSig(m->params, m->ret, true);
                break;
            }

            default:
                break;
        }
    }
}

void Checker::fillStructBody(SymP stSym, const ast::Stmt& s) {
    // Single inheritance: fill the base first (recursively), then inherit its
    // fields and non-overridden methods so subtyping/dispatch sees them.
    if (!s.baseName.empty() && s.baseName != s.name) {
        if (auto bd = structDecls_.find(s.baseName); bd != structDecls_.end()) {
            if (auto bs = structs_.find(s.baseName); bs != structs_.end()) {
                fillStructBody(bs->second, *bd->second);   // ensure base filled
                for (const auto& bf : bs->second->fields)
                    stSym->fields.push_back(bf);           // base fields first
                for (const auto& bm : bs->second->methods)
                    stSym->methods[bm.first] = bm.second;  // base methods
            }
        } else {
            error(s.span, "base class '" + s.baseName + "' is not defined");
        }
    }
    TvGuard tv(typeVars_, s.typeParams);
    // Derived fields override base fields of the same name (Shadowing).
    for (const auto& f : s.fields) {
        TyP ft = f.type ? resolveType(f.type) : unkTy();
        bool replaced = false;
        for (auto& ef : stSym->fields)
            if (ef.first == f.name) { ef.second = ft; replaced = true; break; }
        if (!replaced) stSym->fields.emplace_back(f.name, ft);
        if (f.defaultValue) {
            push();
            TyP vt = checkExpr(*f.defaultValue);
            if (!assignable(vt, ft))
                error(f.span,
                      "default for field '" + f.name + "' has type " +
                          toString(*vt) + ", expected " + toString(*ft));
            pop();
        }
    }
    // Derived methods override base methods of the same name (virtual dispatch).
    for (const auto& m : s.body)
        if (m->kind == StKind::FuncDef)
            stSym->methods[m->name] = resolveSig(m->params, m->ret, true);
}

std::string importRootOf(const ast::Stmt& st) {
    if (!st.importAlias.empty()) return st.importAlias;
    std::string mod = st.moduleName;
    if (mod.size() > 3 && mod.compare(mod.size() - 3, 3, ".kn") == 0)
        mod.erase(mod.size() - 3);   // import "util.kn" binds as util
    auto dot = mod.rfind('.');
    if (dot == std::string::npos) return mod;
    return mod.substr(dot + 1);
}

// True when a function's own body contains a `yield` statement, descending into
// its control flow (if/while/for/try/unsafe/match/select) but NOT into nested
// function/type bodies — their yields belong to those nested declarations.
static bool containsYieldStmt(const ast::Stmt& s) {
    if (s.kind == ast::StKind::Yield) return true;
    switch (s.kind) {
        case ast::StKind::FuncDef:
            // yields in the function's OWN body (recursion descends into
            // control flow; nested FuncDefs encountered here self-terminate)
            for (const auto& b : s.body)
                if (containsYieldStmt(*b)) return true;
            return false;
        case ast::StKind::StructDef:
        case ast::StKind::EnumDef:
        case ast::StKind::TraitDef:
        case ast::StKind::ImplDef:
            return false;
        case ast::StKind::If:
            for (const auto& b : s.body)
                if (containsYieldStmt(*b)) return true;
            for (const auto& b : s.elifBodies)
                if (containsYieldStmt(*b)) return true;
            for (const auto& b : s.elseBody)
                if (containsYieldStmt(*b)) return true;
            return false;
        case ast::StKind::While:
        case ast::StKind::For:
        case ast::StKind::DoWhile:
        case ast::StKind::Gather:
        case ast::StKind::Try:
        case ast::StKind::Unsafe:
            for (const auto& b : s.body)
                if (containsYieldStmt(*b)) return true;
            if (s.kind == ast::StKind::Try)
                for (const auto& b : s.elseBody)
                    if (containsYieldStmt(*b)) return true;
            return false;
        case ast::StKind::Match:
            for (const auto& arm : s.arms)
                for (const auto& b : arm.body)
                    if (containsYieldStmt(*b)) return true;
            return false;
        case ast::StKind::Select:
            for (const auto& arm : s.selArms)
                for (const auto& b : arm.body)
                    if (containsYieldStmt(*b)) return true;
            return false;
        default:
            return false;
    }
}

void Checker::registerTopLevel(const std::vector<ast::StmtP>& prog) {
    // M5: collect every `const fn` up front so a const declaration may call a
    // const function defined later in the same file.
    for (const auto& st : prog)
        if (st->kind == StKind::FuncDef && st->isConstFn)
            constFns_[st->name] = st.get();

    for (const auto& st : prog) {
        switch (st->kind) {
            case StKind::FuncDef: {
                auto sym = std::make_shared<Symbol>();
                sym->kind = SymK::Func;
                sym->name = st->name;
                sym->pub = st->pub;
                {
                    TvGuard tv(typeVars_, st->typeParams);
                    sym->sig = resolveSig(st->params, st->ret, /*stripSelf*/ false);
                    // Recorded here, inside the TvGuard scope, so a later reader
                    // gets the signature with type variables resolved. See
                    // Checker::funcSig.
                    funcSigs_[st.get()] = sym->sig;
                    for (const auto& tp : st->typeParams)
                        if (tp.def)
                            sym->typeDefaults[tp.name] = resolveType(tp.def);
                }
                if (containsYieldStmt(*st)) {
                    st->isGenerator = true;
                    if (!st->ret)
                        sym->sig.ret = genTy(unkTy());   // elem resolved later
                }
                funcs_[st->name] = sym;
                if (Symbol* clash = scope_->find(st->name)) {
                    // A user top-level function may shadow a prelude builtin
                    // (dogfooding: stdlib modules redefine join/split/etc.).
                    if (clash->builtin) {
                        scope_->syms.erase(st->name);
                        scope_->declare(sym);
                    } else {
                        scope_->declare(sym);
                        error(st->span.line, st->span.col,
                              "duplicate definition of '" + st->name + "'");
                    }
                } else {
                    scope_->declare(sym);
                }
                break;
            }

            case StKind::ConstDecl: {
                if (!st->target || st->target->kind != ExKind::Ident) break;
                const std::string& n = st->target->text;
                TyP ann = st->declType ? resolveType(st->declType) : nullptr;
                TyP vt = st->value ? checkExpr(*st->value) : unkTy();
                if (ann && !assignable(vt, ann))
                    error(st->span.line, st->span.col,
                          "constant '" + n + "' declared " + toString(*ann) +
                              " but initialized with " + toString(*vt));
                declareLocal(SymK::Const, n,
                             ann ? ann : (vt ? vt : unkTy()), false,
                             st->span.line, st->span.col);
                if (Symbol* cs = scope_->find(n))
                    cs->pub = st->pub;
                // M5: fold a compile-time-evaluable initializer. The value is
                // recorded regardless (so later const/`const fn` expressions can
                // read it); the AST is rewritten to a literal only for computed
                // scalars/collections, so native codegen sees a constant and the
                // `--release` call is eliminated.
                if (st->value) {
                    constSteps_ = 0;
                    ConstVal cv;
                    if (constEvalExpr(*st->value, cv)) {
                        constVals_[n] = cv;
                        bool trivial =
                            st->value->kind == ExKind::Int ||
                            st->value->kind == ExKind::Float ||
                            st->value->kind == ExKind::CharLit ||
                            st->value->kind == ExKind::Str ||
                            st->value->kind == ExKind::List ||
                            st->value->kind == ExKind::Tuple ||
                            st->value->kind == ExKind::Set ||
                            st->value->kind == ExKind::Dict;
                        bool foldable =
                            cv.k == ConstVal::K::Int ||
                            cv.k == ConstVal::K::Bool ||
                            cv.k == ConstVal::K::Str ||
                            cv.k == ConstVal::K::List ||
                            cv.k == ConstVal::K::Tuple;
                        if (!trivial && foldable) {
                            auto lit = constValToExpr(cv, st->value->span);
                            const ast::Expr* lp = lit.get();
                            st->value = std::move(lit);
                            if (vt) typeCache_[lp] = vt;
                        }
                    }
                }
                break;
            }

            case StKind::Import:
            case StKind::Export: {
                if (st->fromImport && !st->importItems.empty()) {
                    // from mod import item1, item2 — bind the imported names
                    for (const auto& item : st->importItems) {
                        std::string n = item.alias.empty() ? item.name : item.alias;
                        importRoots_.insert(n);
                        SymP im = declareLocal(SymK::ImportRoot, n, unkTy(), false,
                                               st->span.line, st->span.col);
                        im->importStmt = st.get();
                    }
                    break;
                }
                std::string root = importRootOf(*st);
                if (root.empty()) break;
                importRoots_.insert(root);
                SymP im = declareLocal(SymK::ImportRoot, root, unkTy(), false,
                                       st->span.line, st->span.col);
                im->importStmt = st.get();
                break;
            }

            default:
                break;
        }
    }

    // M5: purity gate for every `const fn` (reported even if never called).
    for (const auto& kv : constFns_) checkConstFnPurity(*kv.second);
}

// =============================== type resolution ============================

TyP Checker::lookupNominal(TyK k, const std::string& name) const {
    switch (k) {
        case TyK::Struct: {
            auto it = structs_.find(name);
            return it != structs_.end() ? Ty::named(TyK::Struct, name) : errTy();
        }
        case TyK::EnumName: {
            auto it = enums_.find(name);
            return it != enums_.end() ? Ty::named(TyK::EnumName, name) : errTy();
        }
        case TyK::TraitObj: {
            auto it = traits_.find(name);
            return it != traits_.end() ? Ty::named(TyK::TraitObj, name) : errTy();
        }
        default:
            return errTy();
    }
}

TyP Checker::resolveType(const ast::TypeP& t) {
    if (!t) return unkTy();
    switch (t->kind) {
        case TyKind::Name: {
            const std::string& n = t->name;
            auto tvIt = typeVars_.find(n);
            if (tvIt != typeVars_.end()) return tvIt->second;

            // scalar spellings: canonical capitalized primitives plus the numeric
            // width families in both the classic lowercase micro forms and
            // their capitalized forms (Python/Rust/Go-style), plus a plain
            // unsigned UInt/UFloat.
            if (n == "Bool")   return boolTy();
            if (n == "Str" || n == "String") return strTy();
            if (n == "Char")   return charTy();
            if (n == "none" || n == "unit" || n == "None" || n == "void")
                return noneTy();
            if (n == "Int" || n == "ISize" || n == "isize") return intTy("Int64");
            if (n == "UInt" || n == "uint" || n == "USize" || n == "usize")
                return intTy("UInt64");
            if (n == "Float") return floatTy("Float64");
            if (n == "UFloat") return floatTy("UFloat64");
            // Width families are names, not separate kinds: every int width
            // lowers to the Int kind and every float width to Float. The
            // width is the spelling on Ty::name, carried through to
            // diagnostics and the MIR TypeEntry. Semantics stay the trunk
            // int64/double of the existing model; BigInt-backed >64 / unsigned
            // widths and per-width range checks are a later hardening step.
            static const char* kIntNames[] = {
                "i4", "i8", "i16", "i32", "i64", "i128", "i256",
                "u4", "u8", "u16", "u32", "u64", "u128", "u256",
                "Int4", "Int8", "Int16", "Int32", "Int64", "Int128", "Int256",
                "UInt4", "UInt8", "UInt16", "UInt32", "UInt64", "UInt128",
                "UInt256", nullptr};
            for (int i = 0; kIntNames[i]; ++i)
                if (n == kIntNames[i]) return intTy(n);
            static const char* kFloatNames[] = {
                "f4", "f8", "f16", "f32", "f64", "f128", "f256",
                "uf4", "uf8", "uf16", "uf32", "uf64", "uf128", "uf256",
                "Float4", "Float8", "Float16", "Float32", "Float64", "Float128",
                "Float256", "UFloat4", "UFloat8", "UFloat16", "UFloat32",
                "UFloat64", "UFloat128", "UFloat256", nullptr};
            for (int i = 0; kFloatNames[i]; ++i)
                if (n == kFloatNames[i]) return floatTy(n);
            static const char* kBoolNames[] = {
                "b8", "b16", "b32", "Bool8", "Bool16", "Bool32", nullptr};
            for (int i = 0; kBoolNames[i]; ++i)
                if (n == kBoolNames[i]) return boolTy(n);
            static const char* kCharNames[] = {
                "char8", "char16", "char32", "Char8", "Char16", "Char32",
                nullptr};
            for (int i = 0; kCharNames[i]; ++i)
                if (n == kCharNames[i]) return charTy(n);
            if (n == "strings") return strTy("strings");
            if (n == "bytes")   return bytesTy();
            if (n == "bytearray") return byteArrayTy();
            if (n == "memoryview") return memoryViewTy();
            if (n == "deque")
                return t->generics.size() >= 1
                           ? dequeTy(resolveType(t->generics[0])) : dequeTy();
            if (n == "Counter" || n == "counter")
                return t->generics.size() >= 1
                           ? counterTy(resolveType(t->generics[0])) : counterTy();
            if (n == "BigInt" || n == "bigint" || n == "big_int") return bigIntTy();
            if (n == "Rational" || n == "rational") return rationalTy();
            if (n == "Complex" || n == "complex") return complexTy();
            if (n == "Decimal" || n == "decimal") return decimalTy();

            // containers
            size_t ngens = t->generics.size();
            if (n == "list" || n == "set" || n == "chan" || n == "gen") {
                TyP elem = ngens >= 1 ? resolveType(t->generics[0]) : unkTy();
                if (n == "list") return listTy(elem);
                if (n == "set")  return setTy(elem);
                if (n == "chan") return chanTy(elem);
                return genTy(elem);
            }
            if (n == "dict") {
                TyP kk = ngens >= 1 ? resolveType(t->generics[0]) : unkTy();
                TyP vv = ngens >= 2 ? resolveType(t->generics[1]) : unkTy();
                return dictTy(kk, vv);
            }
            if (n == "result") {
                std::vector<TyP> args;
                args.push_back(ngens >= 1 ? resolveType(t->generics[0]) : unkTy());
                args.push_back(ngens >= 2 ? resolveType(t->generics[1]) : unkTy());
                return nominalWithArgs(TyK::Struct, "result", std::move(args));
            }

            // `any` / `dynamic` — the dynamic type. It is a real, usable type:
            // the checker defers anything against it (assignable/unify are
            // already permissive for Unknown), and the runtime dispatches on the
            // value's tag. `dynamic` is spelled as an alias for `any`.
            if (n == "any" || n == "dynamic") return unkTy();

            // nominal user types
            if (structs_.count(n)) {
                std::vector<TyP> args;
                for (const auto& g : t->generics) args.push_back(resolveType(g));
                if (args.empty() && !t->generics.empty()) {}
                return nominalWithArgs(TyK::Struct, n, std::move(args));
            }
            if (enums_.count(n)) {
                std::vector<TyP> args;
                for (const auto& g : t->generics) args.push_back(resolveType(g));
                return nominalWithArgs(TyK::EnumName, n, std::move(args));
            }
            if (traits_.count(n)) {
                std::vector<TyP> args;
                for (const auto& g : t->generics) args.push_back(resolveType(g));
                return nominalWithArgs(TyK::TraitObj, n, std::move(args));
            }

            error(t->span.line, t->span.col, "unknown type '" + n + "'");
            return errTy();
        }
        case TyKind::TypeOf:
            return t->typeExpr ? checkExpr(*t->typeExpr) : unkTy();
        case TyKind::Pointer:
            return ptrTy(resolveType(t->inner));
        case TyKind::Ref:
            return Ty::one(TyK::Ref, resolveType(t->inner));
        case TyKind::Optional:
            return optTy(resolveType(t->inner));
        case TyKind::Fn: {
            std::vector<TyP> ps;
            for (const auto& p : t->params) ps.push_back(resolveType(p));
            return fnTy(std::move(ps),
                        t->ret ? resolveType(t->ret) : noneTy());
        }
        case TyKind::Tuple: {
            std::vector<TyP> items;
            for (const auto& i : t->params) items.push_back(resolveType(i));
            return tupleTy(std::move(items));
        }
    }
    return unkTy();
}

FuncSig Checker::resolveSig(const std::vector<ast::Param>& params,
                            const ast::TypeP& ret, bool stripSelf) {
    FuncSig out;
    size_t required = 0;
    bool seenDefault = false;
    for (const auto& p : params) {
        if (p.selfParam && stripSelf) continue;
        out.params.push_back(p.type ? resolveType(p.type) : unkTy());
        out.names.push_back(p.variadic ? "" : p.name);
        if (p.variadic) {
            out.variadic = true;
        } else {
            if (!seenDefault && !p.defaultValue) required = out.params.size();
            if (p.defaultValue) seenDefault = true;
        }
    }
    // variadic tail is optional; everything before the first default is required
    if (ret && ret->kind == ast::TyKind::Name && ret->name == "TypeGuard" &&
        ret->generics.size() == 1) {
        // M3 TypeGuard: `fn is_int(x: any) -> TypeGuard[Int]`. Callers see a
        // Bool; the guard narrows its first argument to the guarded type.
        out.guardType = resolveType(ret->generics[0]);
        out.ret = boolTy();
    } else {
        out.ret = ret ? resolveType(ret) : unkTy();
    }
    if (out.variadic)
        required = required > 0 ? required - 1 : 0;
    out.required = required;
    return out;
}

// ============================ type compatibility ============================

bool Checker::assignable(const TyP& from, const TyP& to) const {
    if (!from || !to) return true;
    if (from->isError() || to->isError()) return true;
    if (from->isUnknown() || to->isUnknown()) return true;
    if (equal(from, to)) return true;
    if (to->is(TyK::TypeVar) || from->is(TyK::TypeVar)) return true;

    // Single inheritance subtyping: a derived struct is assignable to any of
    // its transitively-present base structs.
    if (from->is(TyK::Struct) && to->is(TyK::Struct)) {
        const std::string derived = from->name, base = to->name;
        std::string cur = derived;
        while (!cur.empty() && cur != base) {
            auto it = structDecls_.find(cur);
            if (it == structDecls_.end()) break;
            cur = it->second->baseName;
        }
        if (cur == base) return true;
    }

    auto kFrom = from->k, kTo = to->k;

    // numeric families are mutually assignable in v1
    if (kFrom == TyK::Int && kTo == TyK::Int) return true;
    if (kFrom == TyK::Float && kTo == TyK::Float) return true;
    if (kFrom == TyK::BigInt && kTo == TyK::BigInt) return true;
    if (kFrom == TyK::Rational && kTo == TyK::Rational) return true;
    if (kFrom == TyK::Complex && kTo == TyK::Complex) return true;
    if (kFrom == TyK::Decimal && kTo == TyK::Decimal) return true;
    // numeric widening: integral/rational values bind into decimal/complex.
    if (kTo == TyK::Decimal &&
        (kFrom == TyK::Int || kFrom == TyK::BigInt || kFrom == TyK::Rational))
        return true;
    if (kTo == TyK::Complex &&
        (kFrom == TyK::Int || kFrom == TyK::Float || kFrom == TyK::BigInt ||
         kFrom == TyK::Rational || kFrom == TyK::Decimal))
        return true;
    if (kTo == TyK::Rational &&
        (kFrom == TyK::Int || kFrom == TyK::BigInt))
        return true;

    // The width families (b8/b16/b32, char8/16/32, string/str/strings,
    // bytes) are distinct spellings of one runtime value kind, so they are
    // mutually assignable: a `b8` variable may bind a `bool` literal, and
    // `var s: strings = "hi";` is the same value kind as `string`.
    if (kFrom == TyK::Bool && kTo == TyK::Bool) return true;
    if (kFrom == TyK::Char && kTo == TyK::Char) return true;
    if (kFrom == TyK::Str && kTo == TyK::Str) return true;
    if (kFrom == TyK::Bytes && kTo == TyK::Bytes) return true;
    if (kFrom == TyK::ByteArray && kTo == TyK::ByteArray) return true;
    if (kFrom == TyK::MemoryView && kTo == TyK::MemoryView) return true;
    // bytes/bytearray/memoryview are buffer-compatible: a memoryview binds a
    // bytes or bytearray source, and either buffer accepts a memoryview rvalue.
    if (kTo == TyK::MemoryView &&
        (kFrom == TyK::Bytes || kFrom == TyK::ByteArray)) return true;
    if (kFrom == TyK::MemoryView &&
        (kTo == TyK::Bytes || kTo == TyK::ByteArray)) return true;

    // none literal widens into an optional
    if (kFrom == TyK::None && kTo == TyK::Opt) return true;

    // enum variant instance into its enum name
    if (kFrom == TyK::EnumVal && kTo == TyK::EnumName)
        return from->name == to->name;

    switch (kTo) {
        case TyK::Opt: {
            if (!to->inner) return true;
            return kFrom == TyK::Opt &&
                   assignable(argAt(from, 0), to->inner);
        }
        case TyK::List:
        case TyK::Set:
        case TyK::Chan:
        case TyK::Range:
        case TyK::Gen:
            if (kFrom != kTo) return false;
            return assignable(from->args.empty() ? unkTy() : from->args[0],
                              to->args.empty() ? unkTy() : to->args[0]);
        case TyK::Dict:
            if (kFrom != kTo) return false;
            return assignable(argAt(from, 0), argAt(to, 0)) &&
                   assignable(argAt(from, 1), argAt(to, 1));
        case TyK::Ptr:
        case TyK::Ref:
            if (kFrom != kTo) return false;
            return assignable(from->inner, to->inner);
        case TyK::Fn: {
            if (kFrom != TyK::Fn) return false;
            if (from->args.size() != to->args.size()) return false;
            for (size_t i = 0; i < to->args.size(); ++i)
                if (!assignable(to->args[i], from->args[i])) return false;   // params contravariant
            return assignable(from->inner, to->inner);
        }
        case TyK::Tuple: {
            if (kFrom != TyK::Tuple || from->args.size() != to->args.size())
                return false;
            for (size_t i = 0; i < to->args.size(); ++i)
                if (!assignable(argAt(from, i), argAt(to, i))) return false;
            return true;
        }
        case TyK::Struct: {
            if (kFrom != TyK::Struct || from->name != to->name ||
                from->args.size() != to->args.size())
                return false;
            for (size_t i = 0; i < to->args.size(); ++i)
                if (!assignable(argAt(from, i), argAt(to, i))) return false;
            return true;
        }
        case TyK::EnumName:
            return kFrom == TyK::EnumName && from->name == to->name;
        case TyK::TraitObj: {
            if (kFrom == TyK::TraitObj) return from->name == to->name;
            if (kFrom == TyK::Struct)
                return impls_.count({to->name, from->name}) != 0;   // impl Trait for Struct
            return false;
        }
        default:
            break;
    }
    return false;
}

TyP Checker::unify(const TyP& a, const TyP& b, uint32_t line, uint32_t col,
                   const char* ctx) {
    if (equal(a, b)) return a;
    if (!a || a->isError()) return errTy();
    if (!b || b->isError()) return errTy();
    if (a->isUnknown()) return b;
    if (b->isUnknown()) return a;
    if (a->is(TyK::TypeVar)) return b;
    if (b->is(TyK::TypeVar)) return a;

    if (a->is(TyK::Int) && b->is(TyK::Int)) return intTy();
    if (a->is(TyK::Float) && b->is(TyK::Float)) return floatTy();
    if ((a->is(TyK::BigInt) || a->is(TyK::Int)) &&
        (b->is(TyK::BigInt) || b->is(TyK::Int)) &&
        (a->is(TyK::BigInt) || b->is(TyK::BigInt)))
        return bigIntTy();
    if (a->is(TyK::Rational) && b->is(TyK::Rational)) return rationalTy();
    if (a->is(TyK::Rational) &&
        (b->is(TyK::Int) || b->is(TyK::BigInt)))
        return rationalTy();
    if (b->is(TyK::Rational) &&
        (a->is(TyK::Int) || a->is(TyK::BigInt)))
        return rationalTy();
    {
        auto isnumT = [](const TyP& t) {
            return t->is(TyK::Int) || t->is(TyK::Float) || t->is(TyK::BigInt) ||
                   t->is(TyK::Rational) || t->is(TyK::Complex);
        };
        if ((a->is(TyK::Complex) || b->is(TyK::Complex)) && isnumT(a) && isnumT(b))
            return complexTy();
    }
    {
        auto isdecT = [](const TyP& t, bool allowDec) {
            return (allowDec && t->is(TyK::Decimal)) || t->is(TyK::Int) ||
                   t->is(TyK::BigInt) || t->is(TyK::Rational);
        };
        if (a->is(TyK::Decimal) && isdecT(b, true)) return decimalTy();
        if (b->is(TyK::Decimal) && isdecT(a, true)) return decimalTy();
    }
    {
        auto isbuf = [](const TyP& t) {
            return t->is(TyK::Bytes) || t->is(TyK::ByteArray) ||
                   t->is(TyK::MemoryView);
        };
        if (isbuf(a) && isbuf(b)) {
            if (a->is(TyK::ByteArray) || b->is(TyK::ByteArray))
                return byteArrayTy();
            if (a->is(TyK::MemoryView) || b->is(TyK::MemoryView))
                return memoryViewTy();
            return bytesTy();
        }
    }
    if (a->is(TyK::None) && !b->is(TyK::None)) return optTy(b);
    if (b->is(TyK::None) && !a->is(TyK::None)) return optTy(a);
    if (a->is(TyK::Opt) && !b->is(TyK::Opt))
        return optTy(unify(argAt(a, 0), b, line, col, ctx));
    if (b->is(TyK::Opt) && !a->is(TyK::Opt))
        return optTy(unify(argAt(b, 0), a, line, col, ctx));

    if ((a->is(TyK::Opt)) && b->is(TyK::Opt))
        return optTy(unify(a->args.empty() ? unkTy() : a->args[0],
                           b->args.empty() ? unkTy() : b->args[0],
                           line, col, ctx));
    if (a->is(TyK::List) && b->is(TyK::List))
        return listTy(unify(a->args.empty() ? unkTy() : a->args[0],
                            b->args.empty() ? unkTy() : b->args[0],
                            line, col, ctx));
    if (a->is(TyK::Gen) && b->is(TyK::Gen))
        return genTy(unify(a->args.empty() ? unkTy() : a->args[0],
                           b->args.empty() ? unkTy() : b->args[0],
                           line, col, ctx));

    error(line, col, std::string(ctx) + ": incompatible types " +
                         toString(*a) + " and " + toString(*b));
    return errTy();
}

TyP Checker::unwrapOpt(const TyP& t) const {
    if (t && t->is(TyK::Opt) && !t->args.empty()) return t->args[0];
    return t;
}

TyP Checker::unwrapResult(const TyP& t) const {
    if (t && t->is(TyK::Struct) && t->name == "result" && !t->args.empty())
        return t->args[0];
    return t;
}

// ============================ iteration & methods ===========================

TyP Checker::iterableElem(const TyP& t) const {
    if (!t || t->isError() || t->isUnknown() || t->is(TyK::TypeVar)) return unkTy();
    switch (t->k) {
        case TyK::List:
        case TyK::Set:
        case TyK::Deque:
        case TyK::Counter:
        case TyK::Range:
        case TyK::Gen:
        case TyK::Chan:
        case TyK::Dict: {
            TyP e = argAt(t, 0);
            if (e && e->isError()) return unkTy();     // silence poisoned cascades
            return e ? e : unkTy();
        }
        case TyK::Str:
            return charTy();
        case TyK::Bytes:
        case TyK::ByteArray:
        case TyK::MemoryView:
            return intTy("UInt8");
        case TyK::Struct:
        case TyK::TraitObj: {
            auto sig = methodLookup(t, "next");
            if (!sig && t->is(TyK::Struct)) {
                // iteration protocol provided via `impl Iterator for X`
                for (const auto& imp : impls_) {
                    if (imp.second != t->name) continue;
                    auto tit = traits_.find(imp.first);
                    if (tit == traits_.end()) continue;
                    auto sit = tit->second->sigs.find("next");
                    if (sit != tit->second->sigs.end()) {
                        sig = sit->second;
                        break;
                    }
                }
            }
            if (!sig) return errTy();               // not iterable
            return unwrapOpt(sig->ret);
        }
        default:
            return errTy();
    }
}

TyP Checker::unpackElem(const ast::Expr& spread, const char* where) {
    TyP src = spread.lhs ? checkExpr(*spread.lhs) : unkTy();
    if (dynish(src)) return unkTy();
    TyP elem = iterableElem(src);
    if (elem && elem->isError()) {
        error(spread.span,
              std::string(where) + " requires an iterable, got " +
                  toString(*src));
        return errTy();
    }
    return elem ? elem : unkTy();
}

bool Checker::typeHasMethod(const TyP& recv, const std::string& name) const {
    if (!recv) return false;
    if (methodLookup(recv, name)) return true;
    if (recv->is(TyK::Struct)) {
        auto it = implMethods_.find(recv->name);
        if (it != implMethods_.end() && it->second.count(name)) return true;
    }
    return false;
}

std::optional<FuncSig> Checker::methodLookup(const TyP& recv,
                                             const std::string& name) const {
    if (!recv) return std::nullopt;

    if (recv->is(TyK::Struct)) {
        auto it = structs_.find(recv->name);
        if (it != structs_.end()) {
            auto mit = it->second->methods.find(name);
            if (mit != it->second->methods.end()) return mit->second;
        }
        // builtin struct protocols
        if (recv->name == "thread") {
            FuncSig s;
            s.ret = noneTy();
            if (name == "join") return s;
            return std::nullopt;
        }
        if (recv->name == "result") {
            FuncSig s;
            if (name == "is_err" || name == "is_ok") { s.ret = boolTy(); return s; }
            if (name == "unwrap")     { s.ret = argAt(recv, 0); return s; }
            if (name == "unwrap_err") { s.ret = argAt(recv, 1); return s; }
            return std::nullopt;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::TraitObj)) {
        auto it = traits_.find(recv->name);
        if (it == traits_.end()) return std::nullopt;
        auto sit = it->second->sigs.find(name);
        if (sit != it->second->sigs.end()) return sit->second;
        return std::nullopt;
    }

    if (recv->is(TyK::Opt)) {
        if (name == "unwrap") {
            FuncSig s;
            s.ret = argAt(recv, 0);
            return s;
        }
        return std::nullopt;
    }

    // Phase 1 integral family (DATA_TYPE_PLAN.md): every int width exposes the
    // same method surface; semantics are decided at the receiver's declared
    // width by the runtime (memberRead case VK::Int).
    if (recv->is(TyK::Int)) {
        static const char* kIntMethods[] = {
            "checked_add", "checked_sub", "checked_mul", "checked_div",
            "checked_rem", "checked_neg", "checked_shl", "checked_shr",
            "checked_pow", "wrapping_add", "wrapping_sub",
            "wrapping_mul", "wrapping_div", "wrapping_rem", "wrapping_neg",
            "wrapping_abs", "wrapping_shl", "wrapping_shr", "wrapping_pow",
            "saturating_add", "saturating_sub",
            "saturating_mul", "saturating_div", "saturating_neg",
            "saturating_abs", "saturating_pow", "div_euclid", "rem_euclid",
            "div_ceil", "count_ones", "count_zeros", "leading_zeros",
            "trailing_zeros", "leading_ones", "trailing_ones",
            "reverse_bits", "rotate_left", "rotate_right", "swap_bytes",
            "is_power_of_two", "next_power_of_two", "abs", "signum",
            "ilog2", "min", "max", "bits", "pow", nullptr};
        for (int i = 0; kIntMethods[i]; ++i) {
            if (name != kIntMethods[i]) continue;
            FuncSig s;
            s.ret = intTy(recv->name.empty() ? "Int64" : recv->name);
            if ((name.rfind("checked_", 0) == 0 ||
                 name.rfind("wrapping_", 0) == 0 ||
                 name.rfind("saturating_", 0) == 0 ||
                 name == "div_euclid" || name == "rem_euclid" ||
                 name == "div_ceil" || name == "rotate_left" ||
                 name == "rotate_right" || name == "pow") &&
                name != "checked_neg" && name != "wrapping_neg" &&
                name != "saturating_neg" && name != "wrapping_abs" &&
                name != "saturating_abs") {
                s.params.push_back(intTy(recv->name.empty() ? "Int64" : recv->name));
                s.names.push_back("rhs");
                s.required = 1;
            }
            return s;
        }
        return std::nullopt;
    }

    // Phase 3 arbitrary-precision integer method surface (DATA_TYPE_PLAN.md).
    if (recv->is(TyK::BigInt)) {
        FuncSig s;
        if (name == "pow" || name == "powmod") {
            s.ret = bigIntTy();
            s.params.push_back(unkTy());
            s.names.push_back("exp");
            s.required = 1;
            if (name == "powmod") {
                s.params.push_back(unkTy());
                s.names.push_back("mod");
                s.required = 2;
            }
            return s;
        }
        if (name == "bit_length" || name == "bit_count") {
            s.ret = intTy("Int64");
            s.required = 0;
            return s;
        }
        if (name == "to_int" || name == "is_prime") {
            s.ret = name == "to_int" ? intTy("Int64") : boolTy();
            s.required = 0;
            return s;
        }
        if (name == "to_string") {
            s.ret = strTy();
            s.params.push_back(intTy("Int64"));
            s.names.push_back("base");
            s.required = 0;
            return s;
        }
        if (name == "to_float") {
            s.ret = floatTy();
            s.required = 0;
            return s;
        }
        if (name == "gcd" || name == "lcm" || name == "mod_inverse" ||
            name == "mod_sqrt") {
            s.ret = (name == "mod_inverse" || name == "mod_sqrt")
                        ? unkTy() : bigIntTy();
            s.params.push_back(unkTy());
            s.names.push_back(name == "mod_inverse" || name == "mod_sqrt"
                                  ? "mod" : "other");
            s.required = 1;
            return s;
        }
        if (name == "to_bytes") {
            s.ret = unkTy();
            s.params.push_back(intTy("Int64"));
            s.names.push_back("len");
            s.params.push_back(strTy());
            s.names.push_back("endian");
            s.required = 0;
            return s;
        }
        if (name == "to_rational") {
            s.ret = unkTy();
            s.required = 0;
            return s;
        }
        return std::nullopt;
    }

    // Phase 2 float method surface (DATA_TYPE_PLAN.md).
    if (recv->is(TyK::Float)) {
        FuncSig s;
        if (name == "floor" || name == "ceil" || name == "trunc") {
            s.ret = intTy("Int64");
            s.required = 0;
            return s;
        }
        if (name == "round") {
            s.ret = intTy("Int64");
            s.params.push_back(intTy("Int64"));
            s.names.push_back("n");
            s.required = 0;
            return s;
        }
        if (name == "fract" || name == "abs" || name == "signum" ||
            name == "sqrt" || name == "exp" || name == "ln" ||
            name == "log2" || name == "log10" || name == "to_degrees" ||
            name == "to_radians") {
            s.ret = floatTy();
            s.required = 0;
            return s;
        }
        if (name == "log" || name == "pow" || name == "copysign" ||
            name == "atan2" || name == "min" || name == "max" ||
            name == "total_cmp") {
            s.ret = name == "total_cmp" ? intTy("Int64") : floatTy();
            s.params.push_back(floatTy());
            s.names.push_back("x");
            s.required = 1;
            return s;
        }
        if (name == "sin" || name == "cos" || name == "tan" ||
            name == "asin" || name == "acos" || name == "atan" ||
            name == "sinh" || name == "cosh" || name == "tanh") {
            s.ret = floatTy();
            s.required = 0;
            return s;
        }
        if (name == "is_nan" || name == "is_infinite" || name == "is_finite") {
            s.ret = boolTy();
            s.required = 0;
            return s;
        }
        return std::nullopt;
    }

    // Phase 2 exact rational method surface (DATA_TYPE_PLAN.md).
    if (recv->is(TyK::Rational)) {
        FuncSig s;
        if (name == "numerator" || name == "denominator" ||
            name == "to_int") {
            s.ret = intTy("Int64");
            s.required = 0;
            return s;
        }
        if (name == "to_float") {
            s.ret = floatTy();
            s.required = 0;
            return s;
        }
        if (name == "abs") {
            s.ret = recv;
            s.required = 0;
            return s;
        }
        return std::nullopt;
    }

    // M6 complex / decimal method surfaces.
    if (recv->is(TyK::Complex)) {
        FuncSig s;
        if (name == "abs" || name == "arg") {
            s.ret = floatTy();
            s.required = 0;
            return s;
        }
        if (name == "conjugate" || name == "conj" || name == "exp" ||
            name == "log" || name == "sqrt" || name == "sin" ||
            name == "cos" || name == "tan") {
            s.ret = complexTy();
            s.required = 0;
            return s;
        }
        if (name == "pow") {
            s.params.push_back(complexTy());
            s.names.push_back("exp");
            s.ret = complexTy();
            s.required = 1;
            return s;
        }
        return std::nullopt;
    }
    if (recv->is(TyK::Decimal)) {
        FuncSig s;
        if (name == "to_float") { s.ret = floatTy(); s.required = 0; return s; }
        if (name == "to_int")   { s.ret = intTy("Int64"); s.required = 0; return s; }
        if (name == "to_string"){ s.ret = strTy(); s.required = 0; return s; }
        if (name == "abs" || name == "sqrt" || name == "ln" ||
            name == "log" || name == "exp") {
            s.ret = decimalTy();
            s.required = 0;
            return s;
        }
        if (name == "quantize") {
            s.params.push_back(intTy("Int64"));
            s.names.push_back("exp");
            s.ret = decimalTy();
            s.required = 1;
            return s;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::List) || recv->is(TyK::Gen)) {
        TyP elem = argAt(recv, 0);
        if (name == "append" || name == "push_back") {
            FuncSig s;
            s.params.push_back(elem);
            s.names.push_back("x");
            s.ret = noneTy();
            return s;
        }
        if (name == "sort") {                       // sort(key:, reverse:) all optional
            FuncSig s;
            s.params.push_back(fnTy({unkTy()}, unkTy()));
            s.names.push_back("key");
            s.params.push_back(boolTy());
            s.names.push_back("reverse");
            s.ret = noneTy();
            s.required = 0;
            return s;
        }
        if (name == "filter") {
            FuncSig s;
            s.params.push_back(fnTy({elem}, boolTy()));
            s.names.push_back("pred");
            s.ret = genTy(elem);
            return s;
        }
        if (name == "map") {
            FuncSig s;
            s.params.push_back(fnTy({elem}, unkTy()));
            s.names.push_back("f");
            s.ret = genTy(unkTy());
            return s;
        }
        if ((name == "collect" || name == "to_list") && recv->is(TyK::Gen)) {
            FuncSig s;
            s.ret = listTy(elem);
            return s;
        }
        if (name == "sum") {
            FuncSig s;
            s.ret = elem && elem->is(TyK::Int) ? intTy()
                    : (elem && elem->is(TyK::Float) ? floatTy() : unkTy());
            return s;
        }
        if (name == "join" && recv->is(TyK::List)) {
            FuncSig s;
            s.params.push_back(strTy());
            s.names.push_back("sep");
            s.ret = strTy();
            return s;
        }
        if (name == "len") {                       // xs.len() -> int
            FuncSig s; s.ret = intTy(); return s;
        }
        if (name == "pop") {                       // xs.pop() -> elem
            FuncSig s; s.ret = elem; return s;
        }
        if (name == "shift") {                     // xs.shift() -> elem
            FuncSig s; s.ret = elem; return s;
        }
        if (name == "next" && recv->is(TyK::List)) {  // xs.next() -> elem?
            FuncSig s; s.ret = optTy(elem); return s;
        }
        if (name == "fold" && recv->is(TyK::List)) {  // xs.fold(f, [init])
            FuncSig s;
            s.params.push_back(fnTy({unkTy(), elem}, unkTy()));
            s.names.push_back("f");
            s.params.push_back(unkTy());          // optional init accumulator
            s.names.push_back("init");
            s.ret = unkTy();
            s.required = 1;
            return s;
        }
        if (name == "extend") {                    // xs.extend(ys)
            FuncSig s;
            s.params.push_back(recv); s.names.push_back("ys");
            s.ret = noneTy(); return s;
        }
        if (name == "reverse") {
            FuncSig s; s.ret = noneTy(); return s;
        }
        if (name == "clear") {
            FuncSig s; s.ret = noneTy(); return s;
        }
        if (name == "contains") {                  // xs.contains(x) -> bool
            FuncSig s;
            s.params.push_back(elem); s.names.push_back("x");
            s.ret = boolTy(); return s;
        }
        if (name == "index") {                     // xs.index(x) -> int (-1 absent)
            FuncSig s;
            s.params.push_back(elem); s.names.push_back("x");
            s.ret = intTy(); return s;
        }
        if (name == "remove") {                    // xs.remove(x)
            FuncSig s;
            s.params.push_back(elem); s.names.push_back("x");
            s.ret = noneTy(); return s;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::Dict)) {
        if (name == "keys") {
            FuncSig s;
            s.ret = listTy(argAt(recv, 0));
            return s;
        }
        if (name == "values") {
            FuncSig s;
            s.ret = listTy(argAt(recv, 1));
            return s;
        }
        if (name == "get") {
            FuncSig s;
            s.params.push_back(argAt(recv, 0));
            s.names.push_back("key");
            s.ret = optTy(argAt(recv, 1));
            return s;
        }
        if (name == "len") {                       // d.len() -> int
            FuncSig s; s.ret = intTy(); return s;
        }
        if (name == "remove") {                    // d.remove(key)
            FuncSig s;
            s.params.push_back(argAt(recv, 0)); s.names.push_back("key");
            s.ret = noneTy(); return s;
        }
        if (name == "contains") {                  // d.contains(key) -> bool
            FuncSig s;
            s.params.push_back(argAt(recv, 0)); s.names.push_back("key");
            s.ret = boolTy(); return s;
        }
        if (name == "setdefault") {                // d.setdefault(k, def) -> val
            FuncSig s;
            s.params.push_back(argAt(recv, 0)); s.names.push_back("key");
            s.params.push_back(argAt(recv, 1)); s.names.push_back("def");
            s.ret = argAt(recv, 1); return s;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::Set)) {
        if (name == "insert" || name == "add") {
            FuncSig s;
            s.params.push_back(argAt(recv, 0));
            s.names.push_back("x");
            s.ret = noneTy();
            return s;
        }
        if (name == "len") {                       // st.len() -> int
            FuncSig s; s.ret = intTy(); return s;
        }
        if (name == "remove") {                    // st.remove(x)
            FuncSig s;
            s.params.push_back(argAt(recv, 0)); s.names.push_back("x");
            s.ret = noneTy(); return s;
        }
        if (name == "contains") {                  // st.contains(x) -> bool
            FuncSig s;
            s.params.push_back(argAt(recv, 0)); s.names.push_back("x");
            s.ret = boolTy(); return s;
        }
        return std::nullopt;
    }

    // M8: deque method surface. The element type is argAt(recv, 0) when known.
    if (recv->is(TyK::Deque)) {
        TyP elem = argAt(recv, 0);
        FuncSig s;
        if (name == "append" || name == "push_back" || name == "append_right" ||
            name == "append_left" || name == "appendleft" ||
            name == "push_front") {
            s.params.push_back(elem); s.names.push_back("x");
            s.ret = noneTy(); s.required = 1; return s;
        }
        if (name == "pop" || name == "pop_back" || name == "pop_right" ||
            name == "pop_left" || name == "popleft" || name == "pop_front") {
            s.ret = elem ? elem : unkTy(); return s;
        }
        if (name == "extend" || name == "extend_right" || name == "extend_left" ||
            name == "extendleft") {
            s.params.push_back(unkTy()); s.names.push_back("xs");
            s.ret = noneTy(); s.required = 1; return s;
        }
        if (name == "rotate") {
            s.params.push_back(intTy()); s.names.push_back("n");
            s.ret = noneTy(); s.required = 0; return s;
        }
        if (name == "clear" || name == "reverse") { s.ret = noneTy(); return s; }
        if (name == "len") { s.ret = intTy(); return s; }
        if (name == "count" || name == "index" || name == "remove") {
            s.params.push_back(elem); s.names.push_back("x");
            s.ret = (name == "count" || name == "index") ? intTy() : noneTy();
            s.required = 1; return s;
        }
        if (name == "copy") { s.ret = recv; return s; }
        if (name == "maxlen") { s.ret = optTy(intTy()); return s; }
        return std::nullopt;
    }

    // M8: Counter method surface.
    if (recv->is(TyK::Counter)) {
        TyP elem = argAt(recv, 0);
        FuncSig s;
        if (name == "total" || name == "len") { s.ret = intTy(); return s; }
        if (name == "elements" || name == "keys") {
            s.ret = listTy(elem); return s;
        }
        if (name == "get") {
            s.params.push_back(elem); s.names.push_back("k");
            s.ret = intTy(); s.required = 1; return s;
        }
        if (name == "most_common") {
            s.params.push_back(intTy()); s.names.push_back("n");
            s.ret = listTy(tupleTy({elem, intTy()}));
            s.required = 0; return s;
        }
        if (name == "update" || name == "subtract") {
            s.params.push_back(unkTy()); s.names.push_back("xs");
            s.ret = noneTy(); s.required = 1; return s;
        }
        if (name == "copy") { s.ret = recv; return s; }
        if (name == "clear") { s.ret = noneTy(); return s; }
        return std::nullopt;
    }

    // M7: bytes / bytearray / memoryview method surface. `bytes` is immutable;
    // `bytearray` adds the mutating methods; `memoryview` is a read-only view.
    if (recv->is(TyK::Bytes) || recv->is(TyK::ByteArray) ||
        recv->is(TyK::MemoryView)) {
        FuncSig s;
        bool mut = recv->is(TyK::ByteArray);
        if (name == "decode") {
            s.params.push_back(strTy()); s.names.push_back("encoding");
            s.ret = strTy(); s.required = 0; return s;
        }
        if (name == "hex" || name == "to_string") { s.ret = strTy(); return s; }
        if (name == "len") { s.ret = intTy(); return s; }
        if (name == "contains") {
            s.params.push_back(intTy()); s.names.push_back("x");
            s.ret = boolTy(); s.required = 1; return s;
        }
        if (name == "count") {
            s.params.push_back(intTy()); s.names.push_back("x");
            s.ret = intTy(); s.required = 1; return s;
        }
        if (name == "to_list" || name == "tolist") {
            s.ret = listTy(intTy()); return s;
        }
        if (name == "to_bytes") { s.ret = bytesTy(); return s; }
        if (mut && (name == "append" || name == "push_back")) {
            s.params.push_back(intTy()); s.names.push_back("x");
            s.ret = noneTy(); s.required = 1; return s;
        }
        if (mut && (name == "extend" || name == "extend_from")) {
            s.params.push_back(recv); s.names.push_back("other");
            s.ret = noneTy(); s.required = 1; return s;
        }
        if (mut && name == "clear") { s.ret = noneTy(); return s; }
        if (mut && (name == "pop" || name == "pop_back")) {
            s.ret = intTy(); return s;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::Str)) {
        if (name == "split") {
            FuncSig s;
            s.params.push_back(strTy());
            s.names.push_back("sep");
            s.ret = listTy(strTy());
            return s;
        }
        if (name == "to_lower" || name == "lower") {
            FuncSig s; s.ret = strTy(); return s;
        }
        if (name == "upper" || name == "to_upper") {
            FuncSig s; s.ret = strTy(); return s;
        }
        if (name == "trim" || name == "strip") {
            FuncSig s; s.ret = strTy(); return s;
        }
        if (name == "c_ptr") {
            FuncSig s; s.ret = ptrTy(charTy()); return s;
        }
        if (name == "to_int") {
            FuncSig s; s.ret = optTy(intTy()); return s;
        }
        if (name == "len") {                       // s.len() -> int
            FuncSig s; s.ret = intTy(); return s;
        }
        if (name == "repeat") {                    // s.repeat(n) -> string
            FuncSig s;
            s.params.push_back(intTy()); s.names.push_back("n");
            s.ret = strTy(); return s;
        }
        if (name == "contains") {                  // s.contains(sub) -> bool
            FuncSig s;
            s.params.push_back(strTy()); s.names.push_back("sub");
            s.ret = boolTy(); return s;
        }
        if (name == "starts_with" || name == "startsWith") {
            FuncSig s;
            s.params.push_back(strTy()); s.names.push_back("prefix");
            s.ret = boolTy(); return s;
        }
        if (name == "ends_with" || name == "endsWith") {
            FuncSig s;
            s.params.push_back(strTy()); s.names.push_back("suffix");
            s.ret = boolTy(); return s;
        }
        if (name == "replace") {                   // s.replace(from, to)
            FuncSig s;
            s.params.push_back(strTy()); s.names.push_back("from");
            s.params.push_back(strTy()); s.names.push_back("to");
            s.ret = strTy(); return s;
        }
        if (name == "find") {                      // s.find(sub) -> int (-1 if absent)
            FuncSig s;
            s.params.push_back(strTy()); s.names.push_back("sub");
            s.ret = intTy(); return s;
        }
        if (name == "capitalize") {
            FuncSig s; s.ret = strTy(); return s;
        }
        return std::nullopt;
    }

    if (recv->is(TyK::EnumVal)) {
        if (name == "to_int") { FuncSig s; s.ret = intTy(); return s; }
        return std::nullopt;
    }

    if (recv->is(TyK::Chan)) {
        TyP elem = argAt(recv, 0);
        if (name == "send") {
            FuncSig s;
            s.params.push_back(elem);
            s.names.push_back("v");
            s.ret = noneTy();
            return s;
        }
        if (name == "recv") {
            // blocking receive: yields the element directly
            FuncSig s;
            s.ret = elem;
            return s;
        }
        if (name == "close") {
            FuncSig s; s.ret = noneTy(); return s;
        }
        return std::nullopt;
    }

    return std::nullopt;
}

// ================================ statements ================================

// A `goto` may not jump over a statement that introduces a binding or a
// noun (declaring statements): the checker rejects forward jumps that cross
// one. Backward jumps re-run the code (loop semantics) and are allowed.
static bool isLegacyGotoDecl(ast::StKind k) {
    switch (k) {
        case ast::StKind::VarDecl:
        case ast::StKind::ConstDecl:
        case ast::StKind::LocalDecl:
        case ast::StKind::TempDecl:
        case ast::StKind::BucketDecl:
        case ast::StKind::FuncDef:
        case ast::StKind::StructDef:
        case ast::StKind::EnumDef:
        case ast::StKind::TraitDef:
        case ast::StKind::ImplDef:
        case ast::StKind::Import:
        case ast::StKind::Export:
            return true;
        default:
            return false;
    }
}

void Checker::checkBlock(const std::vector<ast::StmtP>& body) {
    std::map<std::string, std::size_t> labels;
    for (std::size_t i = 0; i < body.size(); ++i)
        if (body[i]->kind == StKind::Label) {
            auto [it, ok] = labels.emplace(body[i]->label, i);
            if (!ok)
                error(body[i]->span,
                      "label '" + body[i]->label + "' is defined more than once");
        }
    labelScopes_.push_back(std::move(labels));
    bool unreachable = false;
    for (std::size_t idx = 0; idx < body.size(); ++idx) {
        const auto& st = body[idx];
        if (unreachable) {
            warning(SpanRange::point(st->span.line, st->span.col), "W0104",
                    "unreachable statement (dead code)");
            break;   // report once per block
        }
        checkStmt(*st);
        if (st->kind == StKind::Goto) {
            // a goto may target a label in this block or any enclosing block;
            // it may never jump INTO a nested block (labels in nested blocks
            // are not visible here). Declaration-crossing is only checked for
            // same-block forward jumps.
            bool found = false;
            for (auto itl = labelScopes_.rbegin(); itl != labelScopes_.rend();
                 ++itl) {
                auto it = itl->find(st->label);
                if (it == itl->end()) continue;
                found = true;
                if (itl == labelScopes_.rbegin()) {   // same block
                    if (it->second > idx) {           // forward jump
                        bool crossed = false;
                        for (std::size_t j = idx + 1; j < it->second; ++j)
                            if (isLegacyGotoDecl(body[j]->kind)) {
                                crossed = true;
                                break;
                            }
                        if (crossed)
                            error(st->span,
                                  "goto may not jump over a declaration");
                    }
                }
                break;
            }
            if (!found)
                error(st->span,
                      "goto: label '" + st->label +
                          "' is not declared in this block or an enclosing block");
        }
        switch (st->kind) {
            case StKind::Return:
            case StKind::Raise:
            case StKind::Break:
            case StKind::Continue:
                unreachable = true;
                break;
            default:
                break;
        }
    }
    labelScopes_.pop_back();
}

void Checker::requireBool(const TyP& t, uint32_t line, uint32_t col,
                          const char* what) {
    if (!t || t->isError() || t->isUnknown() || t->is(TyK::Bool) ||
        t->is(TyK::TypeVar))
        return;
    error(line, col,
          std::string(what) + " must be a boolean, got " + toString(*t));
}

TyP Checker::collapseOpt(const TyP& t) const {
    if (t && t->is(TyK::Opt)) return t;
    if (!t) return unkTy();
    return optTy(t);
}

bool Checker::alwaysExits(const std::vector<ast::StmtP>& body) const {
    if (body.empty()) return false;
    const ast::Stmt& last = *body.back();
    switch (last.kind) {
        case StKind::Return:
        case StKind::Raise:
        case StKind::Break:
        case StKind::Continue:
            return true;
        case StKind::If:
            return !last.elseBody.empty() && alwaysExits(last.body) &&
                   alwaysExits(last.elseBody);
        case StKind::Match: {
            if (last.arms.empty()) return false;
            for (const auto& arm : last.arms)
                if (!alwaysExits(arm.body)) return false;
            return true;
        }
        default:
            return false;
    }
}

void Checker::narrowApply(const ast::Expr* c, bool positive,
                          std::map<const Symbol*, TyP>& out) {
    if (!c) return;
    if (c->kind == ExKind::Unary && c->op == "not") {
        narrowApply(c->rhs.get(), !positive, out);
        return;
    }
    // M3 TypeGuard: `if is_int(v)` narrows `v` (a stable identifier) to the
    // guard's declared `TypeGuard[T]` in the true branch. The negative branch
    // adds no fact (no "not T" lattice for dynamic values yet).
    if (c->kind == ExKind::Call) {
        const ast::Expr* callee = c->lhs.get();
        if (!callee || callee->kind != ExKind::Ident || c->args.empty()) return;
        Symbol* fn = scope_->find(callee->text);
        if (!fn || fn->kind != SymK::Func || !fn->sig.guardType) return;
        const ast::Expr* arg0 = c->args[0].value.get();
        if (!arg0 || arg0->kind != ExKind::Ident) return;
        Symbol* sym = scope_->find(arg0->text);
        if (!sym) return;
        if (sym->mut) {
            if (narrowWarned_.insert(sym).second)
                narrowDiag(arg0->span.line, arg0->span.col, arg0->text);
            return;
        }
        if (positive) out[sym] = fn->sig.guardType;
        return;
    }
    if (c->kind != ExKind::Binary) return;
    const std::string& op = c->op;
    if (op == "and") {
        if (positive) {
            narrowApply(c->lhs.get(), true, out);
            narrowApply(c->rhs.get(), true, out);
        }
        return;
    }
    if (op == "or") {
        if (!positive) {
            narrowApply(c->lhs.get(), false, out);
            narrowApply(c->rhs.get(), false, out);
        }
        return;
    }
    if (op != "is" && op != "==" && op != "!=") return;
    const ast::Expr* subj = c->lhs.get();
    const ast::Expr* obj = c->rhs.get();
    if (!subj || !obj || subj->kind != ExKind::Ident) return;
    bool pos = (op == "!=") ? !positive : positive;
    bool noneRhs = obj->kind == ExKind::Ident && obj->text == "none";
    if (op == "==" || op == "!=") {
        if (!noneRhs) return;               // only none-equality narrows
    } else if (!noneRhs && obj->kind != ExKind::Ident) {
        return;
    }
    Symbol* sym = scope_->find(subj->text);
    if (!sym) return;
    if (sym->mut) {
        if (narrowWarned_.insert(sym).second)
            narrowDiag(subj->span.line, subj->span.col, subj->text);
        return;
    }
    TyP cur = sym->type ? sym->type : unkTy();
    if (noneRhs) {
        if (pos) out[sym] = noneTy();
        else out[sym] = unwrapOpt(cur);
        return;
    }
    auto tmp = std::make_unique<ast::Type>();
    tmp->kind = ast::TyKind::Name;
    tmp->name = obj->text;
    tmp->span = obj->span;
    ++quiet_;
    TyP narrowedT = resolveType(ast::TypeP(std::move(tmp)));
    --quiet_;
    if (narrowedT && !narrowedT->isError() && !narrowedT->isUnknown()) {
        if (pos) out[sym] = narrowedT;
    }
}

// M3: type facts a pattern establishes for its subject. `positive` selects the
// branch fact (subject matches) vs the fall-through fact (subject failed). Only
// patterns that actually change the subject's type produce a fact; a wildcard
// or an untyped binding says nothing.
TyP Checker::patternFact(const ast::Pat& p, const TyP& subject, bool positive) {
    if (p.kind == ast::PatKind::Literal && p.literal) {
        // `none` is a literal pattern whose type is None; any other literal is
        // a concrete non-none value.
        TyP lt = checkExpr(*p.literal);
        if (lt && lt->is(TyK::None)) {
            // matches `none`; the fall-through knows the subject is non-none.
            return positive ? noneTy() : unwrapOpt(subject);
        }
        // A concrete literal matching an optional subject proves it is not none.
        if (subject && subject->is(TyK::Opt)) {
            if (positive) return unwrapOpt(subject);
        }
        return nullptr;
    }
    if (p.kind == ast::PatKind::Bind && p.bindType) {
        if (!positive) return nullptr;
        TyP t = resolveType(p.bindType);
        return (t && !t->isError()) ? t : nullptr;
    }
    if (p.kind == ast::PatKind::BindAlias && p.aliasSub)
        return patternFact(*p.aliasSub, subject, positive);
    if (p.kind == ast::PatKind::Or) {
        // `none | none` style: only usable when every alternative agrees.
        TyP first = nullptr;
        for (const auto& alt : p.alts) {
            TyP f = patternFact(*alt, subject, positive);
            if (!f) return nullptr;
            if (!first) first = f;
            else if (!equal(first, f)) return nullptr;
        }
        return first;
    }
    return nullptr;
}

void Checker::narrowSubject(Symbol* sym, const TyP& cur, const ast::Pat* pat,
                            uint32_t line, uint32_t col) {
    if (!sym) return;
    // The type the arm body actually sees: the cumulative fall-through fact
    // `cur`, refined by this pattern's own positive fact when it has one.
    TyP fact = pat ? patternFact(*pat, cur ? cur : unkTy(), /*positive*/ true)
                   : nullptr;
    TyP seen = fact ? fact : cur;
    if (!seen || seen->isError()) return;
    // Nothing narrowed (pattern implies no type change) -> no diagnostic and no
    // entry, e.g. a tuple destructure `case (a, b)` leaves the subject alone.
    TyP existing = sym->type;
    auto it = narrowed_.find(sym);
    if (it != narrowed_.end()) existing = it->second;
    if (existing && equal(seen, existing)) return;
    if (sym->mut) {
        if (narrowWarned_.insert(sym).second)
            narrowDiag(line, col, sym->name);
        return;
    }
    narrowed_[sym] = seen;
}

void Checker::checkReassignable(const ast::Expr& tgt) {    if (tgt.kind != ExKind::Ident) return;      // index/member targets: v1 lenient
    Symbol* ex = scope_->find(tgt.text);
    if (!ex) return;                            // fresh binding via assignment
    ex->mutated = true;
    if (!ex->mut && ex->homeScope == scope_->id)
        error(tgt.span,
              "cannot reassign immutable binding '" + tgt.text +
                  "' (declare it with var)");
}

void Checker::assignTarget(const ast::Expr& tgt, const TyP& vt, bool multi) {
    switch (tgt.kind) {
        case ExKind::Ident: {
            Symbol* ex = scope_->find(tgt.text);
            if (!ex || (ex->builtin && ex->kind == SymK::Func &&
                        ex->homeScope != scope_->id)) {
                declareLocal(SymK::Var, tgt.text, vt, /*mut*/ multi,
                             tgt.span.line, tgt.span.col);
                typeCache_[&tgt] = vt;   // codegen: tag implicit bindings
                return;
            }
            checkReassignable(tgt);
            narrowed_.erase(ex);
            typeCache_[&tgt] = ex->type ? ex->type : unkTy();
            break;
        }
        case ExKind::Index:
        case ExKind::Member: {
            TyP tt = checkExpr(tgt);
            if (tgt.kind == ExKind::Index && tgt.lhs) {
                TyP bt = checkExpr(*tgt.lhs);
                if (bt && bt->is(TyK::Bytes))
                    error(tgt.span, "cannot assign into immutable 'bytes'");
            }
            // loose element compatibility when shapes are known on both sides
            if (tt && !tt->isError() && !tt->isUnknown() && vt &&
                !vt->isError() && !vt->isUnknown()) {
                if (tt->is(TyK::List) && !assignable(vt, argAt(tt, 0)))
                    error(tgt.span,
                          "list element expects " + toString(*argAt(tt, 0)) +
                              ", got " + toString(*vt));
                else if (tt->is(TyK::Dict) && !assignable(vt, argAt(tt, 1)))
                    error(tgt.span,
                          "dict value expects " + toString(*argAt(tt, 1)) +
                              ", got " + toString(*vt));
            }
            break;
        }
        case ExKind::Tuple: {
            if (vt && vt->is(TyK::Tuple) && vt->args.size() == tgt.elems.size()) {
                for (size_t i = 0; i < tgt.elems.size(); ++i)
                    assignTarget(*tgt.elems[i], argAt(vt, i), multi);
            } else {
                for (const auto& sub : tgt.elems)
                    assignTarget(*sub, unkTy(), multi);
            }
            break;
        }
        default:
            checkExpr(tgt);
            error(tgt.span, "invalid assignment target");
            break;
    }
}

void Checker::bindWalrus(const ast::Expr& tgt, const TyP& vt) {
    if (tgt.kind != ExKind::Ident) {
        checkExpr(tgt);
        error(tgt.span, "left side of ':=' must be a name");
        return;
    }
    // Kirn binds the walrus target in the enclosing lexical scope (block or
    // comprehension), which is what the tree interpreter does too: `assignTo`
    // writes into the current frame. This diverges from PEP 572's
    // function-scope rule, deliberately, so the checker and interpreter agree.
    assignTarget(tgt, vt, /*multi*/ true);
    // The value a walrus yields is its own use, and the assignment mutates it,
    // so it never trips W0102 (unused) or W0106 (never mutated).
    if (Symbol* sym = scope_->find(tgt.text)) {
        sym->used = true;
        sym->mutated = true;
    }
}

void Checker::checkAssignTargets(const ast::Stmt& s) {
    size_t n = s.exprs.size();
    if (n == 0) return;
    size_t nT, nV;
    if (n % 2 == 0) nT = nV = n / 2;
    else { nV = 1; nT = n - 1; }

    std::vector<TyP> valTys;
    for (size_t i = nT; i < n; ++i)
        valTys.push_back(checkExpr(*s.exprs[i]));

    bool destructure = (nV == 1 && nT > 1);
    if (destructure) {
        const TyP& v = valTys[0];
        if (v && v->is(TyK::Tuple) && v->args.size() != nT)
            error(s.exprs[nT]->span.line, s.exprs[nT]->span.col,
                  "unpacking " + std::to_string(v->args.size()) +
                      " values into " + std::to_string(nT) + " targets");
        for (size_t i = 0; i < nT; ++i)
            assignTarget(*s.exprs[i],
                         v && v->is(TyK::Tuple) && i < v->args.size()
                             ? v->args[i]
                             : unkTy(),
                         /*multi*/ true);
        return;
    }
    if (nT != valTys.size()) {
        error(s.span, "assignment target/value count mismatch");
        return;
    }
    bool multi = nT > 1;                    // parallel assignment: mutable slots
    for (size_t i = 0; i < nT; ++i)
        assignTarget(*s.exprs[i], valTys[i], multi);
}

void Checker::checkFuncLike(const ast::Stmt& s, TyP selfTy, SymP funcSym) {
    Save<TyP> selfG(selfTy_);
    Save<TyP> retG(currentRet_);
    Save<int> loopG(loopDepth_);
    Save<bool> genG(inGen_);
    Save<TyP> elemG(genElem_);
    selfTy_ = std::move(selfTy);

    const bool gen = (s.isGenerator || containsYieldStmt(s)) && !s.externDef;
    if (gen && !s.isGenerator) const_cast<ast::Stmt&>(s).isGenerator = true;
    inGen_ = false;
    genElem_ = nullptr;
    if (gen) {
        if (funcSym) {
            if (!funcSym->sig.ret || !funcSym->sig.ret->is(TyK::Gen)) {
                if (funcSym->sig.ret && !funcSym->sig.ret->isUnknown() &&
                    !funcSym->sig.ret->is(TyK::None))
                    error(s.span, "generator function '" + s.name +
                                      "' must declare return type generator[...], got " +
                                      toString(*funcSym->sig.ret));
                funcSym->sig.ret = genTy(unkTy());
            }
            inGen_ = true;
            genElem_ = funcSym->sig.ret->args.empty()
                           ? unkTy()
                           : funcSym->sig.ret->args[0];
        } else {
            // methods (checked with funcSym == nullptr): adopt Gen; the
            // effective return type is not part of a stored signature here.
            inGen_ = true;
            genElem_ = unkTy();
        }
    }
    currentRet_ = funcSym ? funcSym->sig.ret : nullptr;
    loopDepth_ = 0;

    if (funcSym && funcSym->sig.guardType && s.params.empty())
        error(s.span, "TypeGuard function '" + s.name +
                          "' must take at least one parameter to guard");

    push();
    TvGuard tv(typeVars_, s.typeParams);
    for (const auto& p : s.params) {
        if (p.selfParam) {
            declareLocal(SymK::Param, "self",
                         selfTy_ ? selfTy_ : unkTy(), /*mut*/ true,
                         p.span.line, p.span.col);
            continue;
        }
        TyP pt = p.type ? resolveType(p.type) : unkTy();
        // a variadic parameter is a list inside the function body
        declareLocal(SymK::Param, p.name, p.variadic ? listTy(pt) : pt,
                     p.mutable_, p.span.line, p.span.col);
    }
    if (s.body.empty() && !s.externDef && s.kind == StKind::FuncDef &&
        currentRet_ && !currentRet_->is(TyK::None) &&
        !currentRet_->isUnknown())
        error(s.span,
              "function '" + s.name + "' declares a return type but has no body statements");
    checkBlock(s.body);
    if (inGen_ && funcSym) {
        TyP elem = genElem_ ? genElem_ : noneTy();
        funcSym->sig.ret = genTy(elem);
    }
    pop();
}

void Checker::checkPatternBindings(const ast::Pat& p, const TyP& subject) {
    switch (p.kind) {
        case PatKind::Wild:
            break;

        case PatKind::Literal: {
            TyP lt = checkExpr(*p.literal);
            if (subject && lt && !subject->isUnknown() && !subject->isError() &&
                !subject->is(TyK::TypeVar) && !lt->isUnknown() && !lt->isError()) {
                bool bothNum = isNumeric(*subject) && isNumeric(*lt);
                // `none` literal matches any optional subject
                if (lt->is(TyK::None) && subject->is(TyK::Opt)) break;
                if (!bothNum && !equal(subject, lt) &&
                    !(lt->is(TyK::Str) && subject->is(TyK::Str)))
                    error(p.literal->span.line, p.literal->span.col,
                          "pattern literal of type " + toString(*lt) +
                              " does not match subject of type " +
                              toString(*subject));
            }
            break;
        }

        case PatKind::Range: {
            if (p.lo) checkExpr(*p.lo->literal);
            if (p.hi) checkExpr(*p.hi->literal);
            if (subject && !subject->isUnknown() && !subject->isError() &&
                !subject->is(TyK::TypeVar) && !isNumeric(*subject) &&
                !subject->is(TyK::Char))
                error(p.span,
                      "range pattern requires a numeric or char subject, got " +
                          toString(*subject));
            break;
        }

        case PatKind::Tuple: {
            if (subject && subject->is(TyK::Tuple) && !p.hasRest &&
                subject->args.size() != p.elems.size())
                error(p.span,
                      "tuple pattern has " + std::to_string(p.elems.size()) +
                          " elements but subject has " +
                          std::to_string(subject->args.size()));
            for (size_t i = 0; i < p.elems.size(); ++i) {
                TyP elem = subject && subject->is(TyK::Tuple)
                               ? argAt(subject, i)
                               : unkTy();
                checkPatternBindings(*p.elems[i], elem);
            }
            if (!p.restName.empty())
                declareLocal(SymK::Var, p.restName, unkTy(), /*mut*/ true,
                             p.span.line, p.span.col);
            break;
        }

        case PatKind::Ctor: {
            std::vector<std::pair<std::string, TyP>> payloads;
            bool known = false;
            if (subject && subject->is(TyK::Struct) && subject->name == "result" &&
                (p.ctorName == "ok" || p.ctorName == "err")) {
                if (p.ctorName == "ok")
                    payloads.emplace_back("value", argAt(subject, 0));
                else
                    payloads.emplace_back("err", argAt(subject, 1));
                known = true;
            } else {
                Symbol* vs = scope_->find(p.ctorName);
                if (!vs || vs->kind != SymK::EnumVariant) {
                    // destructuring a struct instance: case Point(x, y)
                    auto sit = structs_.find(p.ctorName);
                    if (sit != structs_.end()) {
                        payloads = sit->second->fields;
                        known = true;
                    } else {
                        error(p.span,
                              "unknown variant '" + p.ctorName + "'");
                        break;
                    }
                } else {
                    if (subject && subject->is(TyK::EnumName) &&
                        subject->name != vs->enumOf)
                        error(p.span,
                              "'" + p.ctorName + "' belongs to enum '" +
                                  vs->enumOf + "', not '" + subject->name + "'");
                    payloads = vs->payloads;
                    known = true;
                }
            }

            if (!known) break;
            std::map<std::string, const ast::PatField*> named;
            std::vector<const ast::PatField*> positional;
            for (const auto& f : p.fields) {
                if (f.name.empty()) positional.push_back(&f);
                else named[f.name] = &f;
            }
            size_t pi = 0;
            for (const auto& pl : payloads) {
                const ast::PatField* f = nullptr;
                auto nit = named.find(pl.first);
                if (nit != named.end()) {
                    f = nit->second;
                    named.erase(nit);
                } else if (pi < positional.size()) {
                    f = positional[pi++];
                }
                if (!f) continue;
                checkPatternBindings(*f->pat, pl.second);
            }
            if (pi < positional.size())
                error(p.span,
                      "too many fields in variant pattern '" + p.ctorName + "'");
            for (const auto& [nm, f] : named)
                error(f->pat->span.line, f->pat->span.col,
                      "'" + nm + "' is not a field of variant '" +
                          p.ctorName + "'");
            break;
        }

        case PatKind::Bind: {
            // binding patterns match the payload: T? subject binds T
            TyP bt = p.bindType ? resolveType(p.bindType) : unwrapOpt(subject);
            SymP bound = declareLocal(SymK::Var, p.bindName, bt, /*mut*/ true,
                                      p.span.line, p.span.col);
            // A bare ident that names an in-scope enum variant / struct is a
            // discriminant match, not a user binding: never warn as unused.
            if (scope_->parent)
                if (Symbol* disc = scope_->parent->find(p.bindName))
                    if (disc->kind == SymK::EnumVariant ||
                        disc->kind == SymK::Struct)
                        bound->used = true;
            break;
        }

        case PatKind::BindAlias: {
            // `pat @ name`: the alias name captures the whole subject; the
            // sub-pattern is validated (and destructures) against it.
            if (p.aliasSub) checkPatternBindings(*p.aliasSub, subject);
            TyP bt = p.bindType ? resolveType(p.bindType) : unwrapOpt(subject);
            SymP bound = declareLocal(SymK::Var, p.bindName, bt, /*mut*/ true,
                                      p.span.line, p.span.col);
            if (scope_->parent)
                if (Symbol* disc = scope_->parent->find(p.bindName))
                    if (disc->kind == SymK::EnumVariant ||
                        disc->kind == SymK::Struct)
                        bound->used = true;
            break;
        }

        case PatKind::Or: {
            // p1 | p2 | ...: every alternative must match the same subject.
            for (const auto& alt : p.alts)
                checkPatternBindings(*alt, subject);
            break;
        }

        case PatKind::Slice: {
            TyP elemTy = subject && subject->is(TyK::List)
                             ? argAt(subject, 0)
                         : subject && subject->is(TyK::Tuple)
                             ? unkTy()
                             : unkTy();
            for (const auto& e : p.elems) checkPatternBindings(*e, elemTy);
            if (!p.restName.empty())
                declareLocal(SymK::Var, p.restName, subject ?
                    (subject->is(TyK::List) ? subject : unkTy()) : unkTy(),
                    /*mut*/ true, p.span.line, p.span.col);
            break;
        }

        case PatKind::Rest:
            break;

        case PatKind::Ref:
            if (p.inner) checkPatternBindings(*p.inner, subject);
            break;
    }
}

void Checker::checkStmt(const ast::Stmt& s) {
    switch (s.kind) {
        case StKind::Pass:
            break;

        case StKind::Del: {
            if (s.exprs.empty()) break;
            const ast::Expr& tgt = *s.exprs[0];
            if (tgt.kind == ExKind::Ident) {
                Symbol* sym = scope_->find(tgt.text);
                if (!sym)
                    error(tgt.span, "del: undefined variable '" + tgt.text + "'");
                else if (sym->kind == SymK::Const)
                    error(tgt.span, "del: cannot delete constant '" + tgt.text + "'");
                else
                    // `x = 55; del x;` intentionally creates then destroys a
                    // binding — that is a legitimate use, so never lint it as
                    // an unused variable.
                    sym->used = true;
            } else {
                checkExpr(tgt);   // del d[k] / del xs[i] / del obj.f: check validity
            }
            break;
        }

        case StKind::LocalDecl: {
            if (!s.target || s.target->kind != ExKind::Ident) {
                if (s.value) checkExpr(*s.value);
                break;
            }
            const std::string& n = s.target->text;
            TyP ann = s.declType ? resolveType(s.declType) : nullptr;
            TyP vt = s.value ? checkExpr(*s.value) : unkTy();
            if (ann && !assignable(vt, ann))
                error(s.span, "local '" + n + "' declared as " + toString(*ann) +
                                  " but initialized with " + toString(*vt));
            // declareLocal errors on same-scope duplicates and warns when
            // shadowing an outer binding — exactly the strict `local` contract.
            declareLocal(SymK::Var, n, ann ? ann : (vt ? vt : unkTy()), true,
                         s.span.line, s.span.col);
            break;
        }

        case StKind::GlobalDecl: {
            if (!s.target || s.target->kind != ExKind::Ident) { break; }
            const std::string& n = s.target->text;
            TyP ann = s.declType ? resolveType(s.declType) : nullptr;
            TyP vt = s.value ? checkExpr(*s.value) : unkTy();
            // Must already exist at module top level (strict).
            Symbol* g = scope_->find(n);
            if (!g || g->homeScope != rootScopeId_)
                error(s.span, "global '" + n + "' is not defined at module top level");
            else if (g->kind != SymK::Var && g->kind != SymK::Const)
                error(s.span, "global '" + n + "' is not a mutable variable");
            if (s.value) {
                if (!g) declareLocal(SymK::Var, n, ann ? ann : (vt ? vt : unkTy()),
                                     true, s.span.line, s.span.col);
                else { g->mut = true; }   // may assign from anywhere
            }
            (void)vt;
            break;
        }

        case StKind::TempDecl: {
            const std::string& n = s.tempVarName;
            int budget = s.tempBudget;
            if (budget < 1 || budget > 10)
                error(s.span, "temp usage limit must be between 1 and 10 (got " +
                                  std::to_string(budget) + ")");
            TyP ann = s.declType ? resolveType(s.declType) : nullptr;
            TyP vt = s.value ? checkExpr(*s.value) : unkTy();
            SymP t = declareLocal(SymK::Var, n, ann ? ann : (vt ? vt : unkTy()),
                                  true, s.span.line, s.span.col);
            t->tempBudget = budget;
            t->tempRemaining = budget;
            break;
        }

        case StKind::BucketDecl: {
            const std::string& n = s.tempVarName;
            if (s.bucketRelease) {
                if (!scope_->find(n) && false) {}
            } else {
                // parking: name becomes unavailable in normal scope
                declareLocal(SymK::Var, n,
                             s.value ? checkExpr(*s.value) : unkTy(), true,
                             s.span.line, s.span.col);
            }
            break;
        }

        case StKind::Yield: {
            if (!inGen_) {
                error(s.span, "'yield' outside of a generator function");
                break;
            }
            TyP et = !s.exprs.empty() ? checkExpr(*s.exprs[0]) : noneTy();
            genElem_ = genElem_ ? unify(genElem_, et, s.span.line, s.span.col,
                                        "yield element type")
                                : et;
            break;
        }

        case StKind::VarDecl:
        case StKind::ConstDecl: {
            if (!s.target || s.target->kind != ExKind::Ident) {
                if (s.value) checkExpr(*s.value);
                break;
            }
            const std::string& n = s.target->text;
            TyP ann = s.declType ? resolveType(s.declType) : nullptr;
            TyP vt = s.value ? checkExpr(*s.value) : unkTy();
            if (ann && !assignable(vt, ann))
                error(s.span,
                      "'" + n + "' declared as " + toString(*ann) +
                          " but initialized with " + toString(*vt));
            declareLocal(s.kind == StKind::ConstDecl ? SymK::Const : SymK::Var,
                         n, ann ? ann : (vt ? vt : unkTy()),
                         /*mut*/ s.kind == StKind::VarDecl && s.varKw,
                         s.span.line, s.span.col);
            break;
        }

        case StKind::ExprStmt:
            if (!s.exprs.empty()) checkExpr(*s.exprs[0]);
            break;

        case StKind::Assign:
            checkAssignTargets(s);
            break;

        case StKind::AugAssign: {
            if (s.exprs.size() < 2) break;
            const ast::Expr& tgt = *s.exprs[0];
            TyP tt = checkExpr(tgt);
            TyP vt = checkExpr(*s.exprs[1]);
            const std::string& op = s.augOp;
            if (op == "??=") {
                // `a ??= b`: only assigns when `a` is none. Target must be an
                // optional; the value must fit the unwrapped element type.
                bool optish = tt && (tt->is(TyK::Opt) || tt->isError() ||
                                     tt->isUnknown());
                if (!optish)
                    error(tgt.span, "'??=' requires an optional target, got " +
                                        toString(*tt));
                TyP base = unwrapOpt(tt);
                if (!vt->isUnknown() && !vt->isError() && base &&
                    !base->isUnknown() && !base->isError() &&
                    !assignable(vt, base))
                    error(tgt.span, "'??=' cannot assign " + toString(*vt) +
                                        " to " + toString(*tt));
                checkReassignable(tgt);
                break;
            }
            if (op == "@=") {
                // In-place matrix product: dispatched through `MatMul` at
                // runtime. Scalars are invalid, exactly as for `@`.
                bool ln = !tt->isUnknown() && !tt->isError() && isNumeric(*tt);
                bool rn = !vt->isUnknown() && !vt->isError() && isNumeric(*vt);
                if (ln || rn)
                    error(tgt.span,
                          "E-MATMUL-SCALAR: '@=' requires matrix/vector "
                          "operands (use '*=' for element-wise multiplication)");
                checkReassignable(tgt);
                break;
            }
            bool numericOp = (op == "-" || op == "*" || op == "/" || op == "%" ||
                              op == "**" || op == "//");
            if (op == "+") {
                bool num = isNumeric(*tt) && isNumeric(*vt);
                bool strcat = tt->is(TyK::Str) && vt->is(TyK::Str);
                bool lists = tt->is(TyK::List) && vt->is(TyK::List);
                if (!num && !strcat && !lists && !tt->isUnknown() &&
                    !vt->isUnknown() && !tt->isError() && !vt->isError())
                    error(tgt.span,
                          "invalid operands to += (" + toString(*tt) + " and " +
                              toString(*vt) + ")");
            } else if (numericOp) {
                if (!isNumeric(*tt) && !tt->isUnknown() && !tt->isError())
                    error(tgt.span,
                          "operator '" + op + "' requires a numeric left operand");
                if (!isNumeric(*vt) && !vt->isUnknown() && !vt->isError())
                    error(tgt.span,
                          "operator '" + op + "' requires a numeric right operand");
            }
            checkReassignable(tgt);
            break;
        }

        case StKind::Return: {
            if (inGen_) {
                if (!s.exprs.empty())
                    error(s.span,
                          "generator functions cannot 'return' a value; use 'yield'");
                break;
            }
            TyP vt;
            if (!s.exprs.empty()) vt = checkExpr(*s.exprs[0]);
            else vt = noneTy();
            if (!currentRet_ || currentRet_->isError() || currentRet_->isUnknown())
                break;
            if (assignable(vt, currentRet_)) break;
            // implicit result-wrap: return T / T? where -> result[T, E]
            if (currentRet_->is(TyK::Struct) && currentRet_->name == "result") {
                TyP okT = argAt(currentRet_, 0);
                if (assignable(vt, okT)) break;
                if (vt && vt->is(TyK::Opt) && assignable(unwrapOpt(vt), okT))
                    break;
            }
            error(s.span,
                  "return type mismatch: got " + toString(*vt) + ", expected " +
                      toString(*currentRet_));
            break;
        }

        case StKind::Raise:
            if (!s.exprs.empty()) checkExpr(*s.exprs[0]);
            break;

        case StKind::Break:
        case StKind::Continue:
            if (loopDepth_ <= 0)
                error(s.span,
                      s.kind == StKind::Break ? "'break' outside of a loop"
                                              : "'continue' outside of a loop");
            break;

        case StKind::Defer:
        case StKind::Spawn:
            if (!s.exprs.empty()) {
                const ast::Expr& call = *s.exprs[0];
                checkExpr(call);
                if (call.kind != ExKind::Call)
                    error(s.span,
                          std::string(s.kind == StKind::Spawn ? "spawn"
                                                              : "defer") +
                              " expects a function call");
                // spawn runs concurrently: every argument crosses threads
                if (s.kind == StKind::Spawn && call.kind == ExKind::Call) {
                    for (const auto& a : call.args) {
                        ++quiet_;
                        TyP at = checkExpr(*a.value);
                        --quiet_;
                        std::string why;
                        if (!isSendable(at, &why))
                            error(a.value->span.line, a.value->span.col,
                                  "cannot pass " + toString(*at) +
                                      " to 'spawn': " + why +
                                      " is not sendable");
                    }
                }
            }
            break;

        case StKind::With: {
            std::vector<TyP> items;
            items.reserve(s.exprs.size());
            for (const auto& e : s.exprs) items.push_back(checkExpr(*e));
            for (size_t i = 0; i < items.size(); ++i) {
                const TyP& t = items[i];
                if (!t || t->isUnknown() || t->isError()) continue;
                bool hasEnter = typeHasMethod(t, "enter") ||
                                typeHasMethod(t, "__enter__");
                bool hasExit = typeHasMethod(t, "exit") ||
                               typeHasMethod(t, "__exit__");
                if (!hasEnter || !hasExit)
                    error(s.exprs[i]->span,
                          "E-CTXMGR-NO-PROTOCOL: type " + toString(*t) +
                              " cannot be used with 'with'; it must define "
                              "enter()/__enter__ and exit()/__exit__");
                if (auto sig = methodLookup(t, "exit"))
                    if (sig->ret && !sig->ret->isUnknown() && !sig->ret->isError() &&
                        !(sig->ret->is(TyK::Bool)))
                        error(s.exprs[i]->span,
                              "context manager 'exit' must return Bool, got " +
                                  toString(*sig->ret));
            }
            push();
            for (size_t i = 0; i < s.withNames.size() && i < items.size(); ++i) {
                if (s.withNames[i].empty()) continue;
                TyP bindTy = items[i];
                if (auto sig = methodLookup(items[i], "enter")) bindTy = sig->ret;
                else if (auto sig = methodLookup(items[i], "__enter__"))
                    bindTy = sig->ret;
                declareLocal(SymK::Var, s.withNames[i],
                             bindTy ? bindTy : unkTy(), /*mut*/ false,
                             s.span.line, s.span.col);
            }
            checkBlock(s.body);
            pop();
            break;
        }

        case StKind::Using: {
            if (!s.target || s.target->kind != ExKind::Ident) {
                if (s.value) checkExpr(*s.value);
                break;
            }
            const std::string& n = s.target->text;
            TyP ann = s.declType ? resolveType(s.declType) : nullptr;
            TyP vt = s.value ? checkExpr(*s.value) : unkTy();
            if (ann && !assignable(vt, ann))
                error(s.span, "'" + n + "' declared as " + toString(*ann) +
                                  " but initialized with " + toString(*vt));
            TyP t = ann ? ann : vt;
            if (t && !t->isUnknown() && !t->isError()) {
                if (!typeHasMethod(t, "drop") && !typeHasMethod(t, "__del__") &&
                    !typeHasMethod(t, "exit") && !typeHasMethod(t, "__exit__"))
                    error(s.span,
                          "E-DROP-NO-PROTOCOL: 'using' requires a type with a "
                          "drop() method (or an exit()/__exit__ context "
                          "manager), got " + toString(*t));
            }
            declareLocal(SymK::Var, n, t ? t : unkTy(), /*mut*/ true,
                         s.span.line, s.span.col);
            break;
        }

        case StKind::ComptimeBlock: {
            // Type-check the body normally, then force compile-time evaluation.
            checkBlock(s.body);
            std::map<std::string, ConstVal> saved = constLocals_;
            constSteps_ = 0;
            ConstVal r;
            if (constEvalStmts(s.body, r) == -1)
                error(s.span,
                      "E-CONST-IMPURE: comptime block contains a statement "
                      "that is not compile-time-evaluable");
            constLocals_ = std::move(saved);
            break;
        }

        case StKind::If: {
            const ast::Expr* cond =
                s.exprs.empty() ? nullptr : s.exprs[0].get();            if (cond) requireBool(checkExpr(*cond), s.span.line, s.span.col,
                                  "if condition");
            std::map<const Symbol*, TyP> thenF, elseF;
            if (cond) {
                narrowWarned_.clear();
                narrowApply(cond, true, thenF);
                narrowApply(cond, false, elseF);
            }
            std::map<const Symbol*, TyP> saved = narrowed_;
            for (const auto& kv : thenF) narrowed_[kv.first] = kv.second;
            push();
            checkBlock(s.body);
            pop();
            bool thenExits = alwaysExits(s.body);
            narrowed_ = saved;
            for (const auto& kv : elseF) narrowed_[kv.first] = kv.second;
            // elif chains are nested as synthetic If statements in elseBody[0]
            if (!s.elseBody.empty()) {
                push();
                checkBlock(s.elseBody);
                pop();
            }
            bool elseExits = !s.elseBody.empty() && alwaysExits(s.elseBody);
            narrowed_ = saved;
            // Early exit: `if !(cond) { return } rest` leaves the then-facts
            // (equivalently, the else facts restricted by the exit) live on the
            // fall-through path.
            if (thenExits && !elseExits)
                for (const auto& kv : elseF) narrowed_[kv.first] = kv.second;
            break;
        }

        case StKind::While: {
            if (!s.exprs.empty()) requireBool(checkExpr(*s.exprs[0]),
                                              s.span.line, s.span.col,
                                              "while condition");
            ++loopDepth_;
            push();
            checkBlock(s.body);
            pop();
            --loopDepth_;
            break;
        }

        case StKind::For: {
            if (!s.pat || s.exprs.empty()) break;
            TyP subj = checkExpr(*s.exprs[0]);
            TyP elem = iterableElem(subj);
            push();
            if (elem && elem->isError() && subj && !subj->isError())
                error(s.span,
                      "expression of type " + toString(*subj) +
                          " is not iterable");
            else
                checkPatternBindings(*s.pat, elem);
            ++loopDepth_;
            checkBlock(s.body);
            --loopDepth_;
            pop();
            break;
        }

        case StKind::Match: {
            if (s.exprs.empty()) break;
            TyP subj = checkExpr(*s.exprs[0]);
            // M3: narrow the subject in each arm. `cur` is the type guaranteed by
            // having failed every preceding arm (e.g. after `case none`, an
            // optional subject is known non-none); the arm's own pattern refines
            // it further. Only a simple Ident subject can be narrowed.
            const ast::Expr* subjE = s.exprs[0].get();
            Symbol* subjSym = (subjE && subjE->kind == ExKind::Ident)
                                  ? scope_->find(subjE->text)
                                  : nullptr;
            narrowWarned_.clear();
            TyP cur = subj;
            for (const auto& arm : s.arms) {
                push();
                if (arm.pat) checkPatternBindings(*arm.pat, cur);
                if (subjSym && arm.pat)
                    narrowSubject(subjSym, cur, arm.pat.get(), arm.pat->span.line,
                                  arm.pat->span.col);
                if (arm.guard) requireBool(checkExpr(*arm.guard),
                                           arm.pat ? arm.pat->span.line
                                                   : s.span.line,
                                           s.span.col, "match guard");
                checkBlock(arm.body);
                pop();
                if (arm.pat) {
                    if (TyP nf = patternFact(*arm.pat, cur, /*positive*/ false))
                        cur = nf;
                }
            }
            checkExhaustiveness(subj, s.arms, s.span.line, s.span.col);
            break;
        }

        case StKind::Select: {
            for (const auto& arm : s.selArms) {
                push();
                if (arm.chanOp) {
                    TyP t = checkExpr(*arm.chanOp);
                    if (!arm.bind.empty()) {
                        TyP bt =
                            t && t->is(TyK::Chan) ? argAt(t, 0) : unwrapOpt(t);
                        declareLocal(SymK::Var, arm.bind, bt, /*mut*/ true,
                                     arm.chanOp->span.line,
                                     arm.chanOp->span.col);
                    }
                }
                checkBlock(arm.body);
                pop();
            }
            break;
        }

        case StKind::DoWhile:
            if (s.pat) {
                // search-driven form: `do { ... } while (pat = expr) ;` —
                // the pattern binds the condition value each round
                TyP c = s.cond ? checkExpr(*s.cond) : errTy();
                ++loopDepth_;
                push();
                if (c && !c->isError())
                    checkPatternBindings(*s.pat, unwrapOpt(c));
                checkBlock(s.body);
                pop();
                --loopDepth_;
            } else {
                if (s.cond) requireBool(checkExpr(*s.cond), s.span.line,
                                        s.span.col, "do-while condition");
                ++loopDepth_;
                push();
                checkBlock(s.body);
                pop();
                --loopDepth_;
            }
            break;

        case StKind::Gather: {
            // worklist loop: `gather { seeds } { body }`; `item` holds each
            // dequeued element, `gather <expr>,` re-queues more.
            TyP elem = unkTy();
            for (size_t i = 0; i < s.seedExprs.size(); ++i) {
                TyP t = checkExpr(*s.seedExprs[i]);
                if (i == 0) elem = t;
            }
            ++gatherDepth_;
            ++loopDepth_;
            push();
            declareLocal(SymK::Var, "item", elem, /*mut*/ true, s.span.line,
                         s.span.col);
            checkBlock(s.body);
            pop();
            --loopDepth_;
            --gatherDepth_;
            break;
        }

        case StKind::GatherStmt:
            if (gatherDepth_ <= 0)
                error(s.span,
                      "'gather <expr>,' may only appear inside a gather { } loop");
            else if (!s.exprs.empty())
                checkExpr(*s.exprs[0]);
            break;

        case StKind::Goto:
            // target existence / decl-crossing are validated in checkBlock
            break;

        case StKind::Label:
            if (!s.body.empty()) checkStmt(*s.body[0]);
            break;

        case StKind::Unsafe:
            push();
            checkBlock(s.body);
            pop();
            break;

        case StKind::Try: {
            push();
            checkBlock(s.body);
            pop();
            push();
            if (!s.catchParam.empty())
                declareLocal(SymK::Var, s.catchParam, unkTy(), /*mut*/ true,
                             s.span.line, s.span.col);
            checkBlock(s.elseBody);
            pop();
            break;
        }

        case StKind::FuncDef: {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymK::Func;
            sym->name = s.name;
            sym->pub = s.pub;
            TvGuard tv(typeVars_, s.typeParams);
            sym->sig = resolveSig(s.params, s.ret, false);
            if (containsYieldStmt(s)) {
                const_cast<ast::Stmt&>(s).isGenerator = true;
                if (!s.ret) sym->sig.ret = genTy(unkTy());
            }
            // Declare the REAL symbol (with its signature) into the current
            // scope. Previously we passed a bare name to declareLocal, creating
            // a second empty-signature Symbol, so calls to a nested function
            // failed "too many arguments". Mirror registerTopLevel.
            if (Symbol* clash = scope_->declare(sym))
                error(s.span.line, s.span.col,
                      "duplicate definition of '" + s.name + "'");
            funcs_[s.name] = sym;
            if (!s.externDef) checkFuncLike(s, nullptr, sym);
            break;
        }

        case StKind::Import:
        case StKind::Export: {
            std::string root = importRootOf(s);
            if (root.empty()) break;
            importRoots_.insert(root);
            SymP im = declareLocal(SymK::ImportRoot, root, unkTy(), false,
                                   s.span.line, s.span.col);
            im->importStmt = &s;
            break;
        }

        case StKind::StructDef: {
            // natively nested class/struct: register into the current scope so
            // construction (Point(...)) and member access work inside blocks.
            // Top-level structs are registered by registerNominals/checkModule.
            if (scope_->id == rootScopeId_) break;
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymK::Struct;
            sym->name = s.name;
            sym->pub = s.pub;
            structs_[s.name] = sym;
            structDecls_[s.name] = &s;
            if (Symbol* clash = scope_->declare(sym))
                error(s.span, "duplicate definition of '" + s.name + "'");
            fillStructBody(sym, s);
            Save<TyP> selfGuard(selfTy_);
            selfTy_ = lookupNominal(TyK::Struct, s.name);
            TvGuard tv(typeVars_, s.typeParams);
            for (const auto& m : s.body)
                if (m->kind == StKind::FuncDef) checkFuncLike(*m, selfTy_, nullptr);
            break;
        }

        case StKind::EnumDef: {
            // natively nested enum: declare the name + variants in this scope.
            // Top-level enums are registered by registerNominals/registerEnums.
            if (scope_->id == rootScopeId_) break;
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymK::EnumName;
            sym->name = s.name;
            enums_[s.name] = sym;
            if (Symbol* clash = scope_->declare(sym))
                error(s.span, "duplicate definition of '" + s.name + "'");
            TvGuard tv(typeVars_, s.typeParams);
            for (const auto& v : s.variants) {
                sym->variants.push_back(v.name);
                auto vs = std::make_shared<Symbol>();
                vs->kind = SymK::EnumVariant;
                vs->name = v.name;
                vs->enumOf = s.name;
                for (const auto& p : v.payload)
                    vs->payloads.emplace_back(p.name, resolveType(p.type));
                for (const auto& p : vs->payloads)
                    sym->variantPayloads[v.name].push_back(p);
                if (Symbol* clash = scope_->declare(vs))
                    error(v.span, "'" + v.name + "' is already defined");
            }
            break;
        }

        case StKind::TraitDef: {
            if (scope_->id == rootScopeId_) break;
            SymP sym;
            auto it = traits_.find(s.name);
            if (it != traits_.end()) {
                sym = it->second;                 // builtin predeclared
                sym->pub = s.pub;
            } else {
                sym = std::make_shared<Symbol>();
                sym->kind = SymK::Trait;
                sym->name = s.name;
                traits_[s.name] = sym;
            }
            if (Symbol* clash = scope_->declare(sym))
                error(s.span, "duplicate definition of '" + s.name + "'");
            TvGuard tv(typeVars_, s.typeParams);
            for (const auto& sg : s.sigs)
                sym->sigs[sg.name] =
                    resolveSig(sg.params, sg.ret, /*stripSelf*/ true);
            for (const auto& m : s.body)
                if (m->kind == StKind::FuncDef && !sym->sigs.count(m->name))
                    sym->sigs[m->name] = resolveSig(m->params, m->ret, true);
            break;
        }

        default:
            break;
    }
}

// =============================== expressions ================================

static bool dynish(const TyP& t) {
    return !t || t->isError() || t->isUnknown() || t->is(TyK::TypeVar);
}

TyP Checker::typeOf(const ast::Expr& e) const {
    auto it = typeCache_.find(&e);
    return it != typeCache_.end() ? it->second : unkTy();
}

TyP Checker::checkExpr(const ast::Expr& e) {
    TyP t = checkExprImpl(e);
    typeCache_[&e] = t;
    return t;
}

TyP Checker::checkExprImpl(const ast::Expr& e) {
    switch (e.kind) {
        case ExKind::Int:   return intTy();
        case ExKind::Float: return floatTy();
        case ExKind::CharLit: return charTy();
        case ExKind::Str:
            if (e.flavor == StrFlavor::C) return ptrTy(charTy());
            if (e.flavor == StrFlavor::Byte) return bytesTy();
            return strTy();

        case ExKind::FString:
            for (const auto& part : e.parts)
                if (part.isExpr && part.expr) checkExpr(*part.expr);
            return strTy();

        case ExKind::Ident: {
            Symbol* sym = scope_->find(e.text);
            if (!sym) {
                if (importRoots_.count(e.text)) return unkTy();
                // `bucket` names live in the separate bucket store; still allow
                // them to be referenced (checked separately).
                error(e.span,
                      "undefined variable '" + e.text + "'");
                return errTy();
            }
            if (sym->tempRemaining > 0) {
                sym->tempRemaining -= 1;
                if (sym->tempRemaining < 0) sym->tempRemaining = 0;
            } else if (sym->tempBudget > 0 && sym->tempRemaining == 0) {
                error(e.span, "temp '" + e.text + "' exhausted (" +
                                  std::to_string(sym->tempBudget) + " uses)");
            }
            sym->used = true;
            if (sym->kind == SymK::Func)
                return fnTy(sym->sig.params, sym->sig.ret);
            if (sym->kind == SymK::EnumName) {
                // bare enum name is not a value; tolerate for now
                return unkTy();
            }
            auto nit = narrowed_.find(sym);
            if (nit != narrowed_.end()) return nit->second;
            return sym->type ? sym->type : unkTy();
        }

        case ExKind::Unary: {
            TyP t = e.rhs ? checkExpr(*e.rhs) : unkTy();
            const std::string& op = e.op;
            if (op == "not") {
                requireBool(t, e.span.line, e.span.col, "operand of 'not'");
                return boolTy();
            }
            if (op == "-" || op == "+") {
                if (!dynish(t) && !isNumeric(*t))
                    error(e.span,
                          "unary '" + op + "' requires a numeric operand, got " +
                              toString(*t));
                return t;
            }
            if (op == "~") {
                if (!dynish(t) && !t->is(TyK::Int))
                    error(e.span,
                          "'~' requires an integer operand");
                return intTy();
            }
            if (op == "&") return ptrTy(t);
            if (op == "*") {
                if (t && (t->is(TyK::Ptr) || t->is(TyK::Ref))) return t->inner;
                if (!dynish(t))
                    error(e.span,
                          "cannot dereference non-pointer type " +
                              toString(*t));
                return unkTy();
            }
            if (op == "spawn") {
                if (!e.rhs || e.rhs->kind != ExKind::Call)
                    error(e.span,
                          "spawn expects a function call");
                if (e.rhs) checkExpr(*e.rhs);
                return nominalWithArgs(TyK::Struct, "thread", {});
            }
            return t;
        }

        case ExKind::Binary: {
            TyP lt = e.lhs ? checkExpr(*e.lhs) : unkTy();
            TyP rt = e.rhs ? checkExpr(*e.rhs) : unkTy();
            const std::string& op = e.op;

            if (op == ".." || op == "..=") {
                bool numPair = (lt && rt && !dynish(lt) && !dynish(rt) &&
                                (isNumeric(*lt) || lt->is(TyK::Char)) &&
                                (isNumeric(*rt) || rt->is(TyK::Char)));
                TyP elem = unify(lt, rt, e.span.line, e.span.col, "range");
                if (numPair) return rangeTy(elem);
                return dynish(lt) || dynish(rt) ? rangeTy(elem) : errTy();
            }
            if (op == "and" || op == "or") {
                requireBool(lt, e.span.line, e.span.col, "left operand");
                requireBool(rt, e.span.line, e.span.col, "right operand");
                return boolTy();
            }
            if (op == "==") {
                auto isbuf = [](const TyP& t) {
                    return t && (t->is(TyK::Bytes) || t->is(TyK::ByteArray) ||
                                 t->is(TyK::MemoryView));
                };
                if (!dynish(lt) && !dynish(rt) && !equal(lt, rt) &&
                    !(isNumeric(*lt) && isNumeric(*rt)) &&
                    !(isbuf(lt) && isbuf(rt)) &&
                    !assignable(lt, rt) && !assignable(rt, lt))
                    error(e.span,
                          "comparing incompatible types " + toString(*lt) +
                              " and " + toString(*rt));
                return boolTy();
            }
            if (op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
                return boolTy();
            if (op == "in") {
                auto isbuf = [](const TyP& t) {
                    return t && (t->is(TyK::Bytes) || t->is(TyK::ByteArray) ||
                                 t->is(TyK::MemoryView));
                };
                if (isbuf(rt)) {
                    if (!isbuf(lt) && !lt->is(TyK::Int) && !dynish(lt))
                        error(e.span,
                              "'" + toString(*lt) + "' cannot appear in " +
                                  toString(*rt));
                    return boolTy();
                }
                TyP elem = iterableElem(rt);
                if (elem && elem->isError() && rt && !rt->isError())
                    error(e.span,
                          "'in' requires an iterable right operand, got " +
                              toString(*rt));
                else if (!assignable(lt, elem))
                    error(e.span,
                          "'" + toString(*lt) + "' cannot appear in " +
                              toString(*rt));
                return boolTy();
            }
            if (op == "is") {
                if (e.rhs && e.rhs->kind == ExKind::Ident && e.rhs->text == "none") {
                    if (!dynish(lt) && !lt->is(TyK::Opt))
                        error(e.span,
                              "'is none' expects an optional, got " +
                                  toString(*lt));
                    return boolTy();
                }
                return boolTy();
            }
            if (op == "<<") { /* shifts */ }
            if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%" ||
                op == "**" || op == "//") {
                if ((op == "+" || op == "-") && lt->is(TyK::Counter) &&
                    rt->is(TyK::Counter))
                    return lt;
                if (op == "+") {
                    auto isStrish = [](const TyP& t) {
                        return t->is(TyK::Str) || t->is(TyK::Char);
                    };
                    bool strcat = isStrish(lt) && isStrish(rt);
                    bool listcat = lt->is(TyK::List) && rt->is(TyK::List);
                    bool bytescat = (lt->is(TyK::Bytes) ||
                                     lt->is(TyK::ByteArray)) &&
                                    (rt->is(TyK::Bytes) ||
                                     rt->is(TyK::ByteArray));
                    if (strcat) return strTy();
                    if (bytescat)
                        return (lt->is(TyK::ByteArray) ||
                                rt->is(TyK::ByteArray))
                                   ? byteArrayTy() : bytesTy();
                    if (listcat)
                        return listTy(unify(argAt(lt, 0), argAt(rt, 0),
                                            e.span.line, e.span.col, "'+'"));
                }
                if (op == "*") {
                    auto byteish = [](const TyP& t) {
                        return t->is(TyK::Bytes) || t->is(TyK::ByteArray);
                    };
                    if (byteish(lt) && rt->is(TyK::Int)) return lt;
                    if (lt->is(TyK::Int) && byteish(rt)) return rt;
                }
                if (dynish(lt)) return rt;
                if (dynish(rt)) return lt;
                if (isNumeric(*lt) && isNumeric(*rt)) {
                    if (lt->is(TyK::Complex) || rt->is(TyK::Complex)) {
                        if (lt->is(TyK::Decimal) || rt->is(TyK::Decimal)) {
                            error(e.span,
                                  "complex and decimal cannot be mixed without "
                                  "an explicit conversion");
                            return errTy();
                        }
                        return complexTy();
                    }
                    if (lt->is(TyK::Decimal) || rt->is(TyK::Decimal)) {
                        auto decok = [](const TyP& t) {
                            return t->is(TyK::Decimal) || t->is(TyK::Int) ||
                                   t->is(TyK::BigInt) || t->is(TyK::Rational);
                        };
                        if (!decok(lt) || !decok(rt)) {
                            error(e.span,
                                  "decimal cannot mix with float without an "
                                  "explicit conversion");
                            return errTy();
                        }
                        return decimalTy();
                    }
                    if (lt->is(TyK::Float) || rt->is(TyK::Float)) return floatTy();
                    if (lt->is(TyK::Rational) || rt->is(TyK::Rational)) {
                        // exact rational arithmetic; `/` and `**` stay rational
                        return rationalTy();
                    }
                    if (lt->is(TyK::BigInt) || rt->is(TyK::BigInt)) {
                        if (op == "/") return floatTy();
                        return bigIntTy();
                    }
                    if (op == "/") return floatTy();
                    return intTy();
                }
                // user-defined operators (v1): same-named structs pass through
                if ((op == "+" || op == "-") && lt->is(TyK::Struct) &&
                    equal(lt, rt))
                    return lt;
                error(e.span,
                      "invalid operands to '" + op + "' (" + toString(*lt) +
                          " and " + toString(*rt) + ")");
                return errTy();
            }
            if (op == "@") {
                // Matrix product (PEP 465). It is dispatched through the
                // `MatMul` trait (`matmul`, alias `__matmul__`) rather than
                // element-wise, so scalar operands are always invalid.
                if ((!dynish(lt) && isNumeric(*lt)) ||
                    (!dynish(rt) && isNumeric(*rt))) {
                    error(e.span,
                          "E-MATMUL-SCALAR: '@' requires matrix/vector operands, "
                          "got " + toString(*lt) + " and " + toString(*rt) +
                              " (use '*' for element-wise multiplication)");
                    return errTy();
                }
                if (dynish(lt) || dynish(rt)) return unkTy();
                // A Matrix/vector type implements `MatMul`. The checker does
                // not track impl-method return types, so the product type is
                // left dynamic and resolved by the interpreter (`binop`).
                if (lt->is(TyK::Struct) || rt->is(TyK::Struct)) return unkTy();
                error(e.span, "invalid operands to '@' (" + toString(*lt) +
                                  " and " + toString(*rt) +
                                  "); '@' is matrix multiplication");
                return errTy();
            }
            return unkTy();
        }

        case ExKind::Call:
            return checkCall(e);

        case ExKind::Index: {
            TyP obj = e.lhs ? checkExpr(*e.lhs) : unkTy();
            TyP idx = e.rhs ? checkExpr(*e.rhs) : unkTy();
            if (dynish(obj) || dynish(idx)) return unkTy();

            if (obj->is(TyK::List)) {
                if (idx->is(TyK::Int)) return argAt(obj, 0);
                if (idx->is(TyK::Range) &&
                    equal(argAt(idx, 0), intTy()))
                    return listTy(argAt(obj, 0));
                error(e.span, "list index must be an integer");
                return errTy();
            }
            if (obj->is(TyK::Dict)) {
                if (!assignable(idx, argAt(obj, 0)))
                    error(e.span,
                          "dict key expects " + toString(*argAt(obj, 0)) +
                              ", got " + toString(*idx));
                return argAt(obj, 1);
            }
            if (obj->is(TyK::Deque)) {
                if (idx->is(TyK::Int)) return argAt(obj, 0);
                if (idx->is(TyK::Range) && equal(argAt(idx, 0), intTy()))
                    return listTy(argAt(obj, 0));
                error(e.span, "deque index must be an integer");
                return errTy();
            }
            if (obj->is(TyK::Counter)) {
                if (!dynish(idx) && !assignable(idx, argAt(obj, 0)))
                    error(e.span,
                          "counter key expects " + toString(*argAt(obj, 0)) +
                              ", got " + toString(*idx));
                return intTy();
            }
            if (obj->is(TyK::Str)) {
                if (idx->is(TyK::Int)) return charTy();
                if (idx->is(TyK::Range)) return strTy();
                error(e.span, "string index must be an integer");
                return errTy();
            }
            if (obj->is(TyK::Bytes) || obj->is(TyK::ByteArray) ||
                obj->is(TyK::MemoryView)) {
                if (idx->is(TyK::Int)) return intTy("UInt8");
                if (idx->is(TyK::Range))
                    return obj->is(TyK::Bytes) ? bytesTy() : byteArrayTy();
                error(e.span, "bytes index must be an integer");
                return errTy();
            }
            if (obj->is(TyK::Tuple)) {
                long i = -1;
                try {
                    i = std::stol(e.rhs->text);
                } catch (...) {}
                if (i < 0 || static_cast<size_t>(i) >= obj->args.size()) {
                    error(e.span,
                          "tuple index out of bounds (" + e.rhs->text + ")");
                    return errTy();
                }
                return obj->args[static_cast<size_t>(i)];
            }
            if (obj->is(TyK::Struct) || obj->is(TyK::TraitObj)) {
                // user-defined index operators (v1): accept, type unknown
                return unkTy();
            }
            error(e.span,
                  "cannot index a value of type " + toString(*obj));
            return errTy();
        }

        case ExKind::SafeIndex: {
            TyP obj = e.lhs ? checkExpr(*e.lhs) : unkTy();
            TyP idx = e.rhs ? checkExpr(*e.rhs) : unkTy();
            if (dynish(obj)) return optTy(unkTy());
            if (!obj->is(TyK::Opt))
                error(e.span,
                      "'.?[' expects an optional, got " + toString(*obj));
            TyP base = unwrapOpt(obj);
            TyP elem = unkTy();
            if (base->is(TyK::List)) {
                if (!dynish(idx) && !idx->is(TyK::Int))
                    error(e.span, "list index must be an integer");
                elem = argAt(base, 0);
            } else if (base->is(TyK::Deque)) {
                if (!dynish(idx) && !idx->is(TyK::Int))
                    error(e.span, "deque index must be an integer");
                elem = argAt(base, 0);
            } else if (base->is(TyK::Counter)) {
                elem = intTy();
            } else if (base->is(TyK::Dict)) {
                elem = argAt(base, 1);
            } else if (base->is(TyK::Str)) {
                elem = charTy();
            } else if (base->is(TyK::Tuple)) {
                long i = -1;
                try {
                    if (e.rhs) i = std::stol(e.rhs->text);
                } catch (...) {}
                if (i >= 0 && static_cast<size_t>(i) < base->args.size())
                    elem = base->args[static_cast<size_t>(i)];
            } else if (!dynish(base)) {
                error(e.span, "cannot safely index a value of type " +
                                  toString(*obj));
                return errTy();
            }
            return collapseOpt(elem);
        }

        case ExKind::SafeCall: {
            TyP ct = e.lhs ? checkExpr(*e.lhs) : unkTy();
            for (const auto& a : e.args) checkExpr(*a.value);
            if (dynish(ct)) return collapseOpt(unkTy());
            if (!ct->is(TyK::Opt))
                error(e.span,
                      "'.?(' expects an optional, got " + toString(*ct));
            TyP base = unwrapOpt(ct);
            TyP ret = unkTy();
            if (base && base->is(TyK::Fn)) {
                FuncSig s;
                s.params = base->args;
                s.names.assign(base->args.size(), "");
                s.ret = base->inner;
                s.required = base->args.size();
                matchArgs(s, e.args, e.span.line, e.span.col, "safe call");
                ret = base->inner ? base->inner : unkTy();
            } else if (!dynish(base)) {
                error(e.span, "expression of type " + toString(*ct) +
                                  " is not safely callable");
                return errTy();
            }
            return collapseOpt(ret);
        }

        case ExKind::Slice: {
            TyP obj = e.lhs ? checkExpr(*e.lhs) : unkTy();
            for (const auto& b : e.elems)
                if (b) {
                    TyP bt = checkExpr(*b);
                    requireIntish(bt, "slice bound", b->span.line, b->span.col);
                }
            if (obj->is(TyK::List)) return listTy(argAt(obj, 0));
            if (obj->is(TyK::Str)) return strTy();
            if (obj->is(TyK::Gen)) return genTy(argAt(obj, 0));
            if (dynish(obj)) return unkTy();
            error(e.span,
                  "cannot slice a value of type " + toString(*obj));
            return errTy();
        }

        case ExKind::Member: {
            TyP obj = e.lhs ? checkExpr(*e.lhs) : unkTy();
            return memberAccess(obj, e.text, e.span.line, e.span.col, e.nilSafe);
        }

        case ExKind::Try: {
            TyP t = e.lhs ? checkExpr(*e.lhs) : unkTy();
            if (t && (t->is(TyK::Opt) || t->isUnknown() || t->isError() ||
                      t->is(TyK::TypeVar)))
                return unwrapOpt(t);
            TyP r = unwrapResult(t);
            if (r.get() != t.get() ||
                (t && t->is(TyK::Struct) && t->name == "result"))
                return r;
            error(e.span,
                  "postfix '?' expects an optional or result, got " +
                      toString(*t));
            return errTy();
        }

        case ExKind::Lambda: {
            if (!e.lambdaBody.empty()) {
                // block-bodied closure: `fn (params) [-> T] { stmts }`
                Save<TyP> retG(currentRet_);
                Save<int> loopG(loopDepth_);
                currentRet_ = e.retType ? resolveType(e.retType) : nullptr;
                loopDepth_ = 0;
                push();
                std::vector<TyP> ps;
                for (const auto& p : e.closureParams) {
                    TyP pt = p.type ? resolveType(p.type) : unkTy();
                    declareLocal(SymK::Param, p.name, pt, p.mutable_,
                                 p.span.line, p.span.col);
                    ps.push_back(std::move(pt));
                }
                checkBlock(e.lambdaBody);
                pop();
                TyP ret = e.retType ? resolveType(e.retType) : unkTy();
                return fnTy(std::move(ps), ret);
            }
            push();
            for (const auto& p : e.lambdaParams)
                declareLocal(SymK::Param, p, unkTy(), true, e.span.line,
                             e.span.col);
            TyP bodyT = e.rhs ? checkExpr(*e.rhs) : unkTy();
            pop();
            std::vector<TyP> ps(e.lambdaParams.size(), unkTy());
            return fnTy(std::move(ps), bodyT);
        }

        case ExKind::Cond: {
            requireBool(checkExpr(*e.cond), e.span.line, e.span.col,
                        "conditional expression");
            TyP lt = e.lhs ? checkExpr(*e.lhs) : unkTy();
            TyP rt = e.rhs ? checkExpr(*e.rhs) : unkTy();
            return unify(lt, rt, e.span.line, e.span.col, "conditional branches");
        }

        case ExKind::ListComp: {
            push();
            for (const auto& cl : e.clauses) compClause(cl, e.span.line);
            TyP et = unkTy();
            if (!e.elems.empty() && e.elems[0]) {
                const ast::Expr& el = *e.elems[0];
                et = (el.kind == ExKind::Spread)
                         ? unpackElem(el, "list comprehension unpacking")
                         : checkExpr(el);
            }
            pop();
            return listTy(et ? et : unkTy());
        }

        case ExKind::SetComp: {
            push();
            for (const auto& cl : e.clauses) compClause(cl, e.span.line);
            TyP et = unkTy();
            if (!e.elems.empty() && e.elems[0]) {
                const ast::Expr& el = *e.elems[0];
                et = el.kind == ExKind::Spread
                         ? unpackElem(el, "set comprehension unpacking")
                         : checkExpr(el);
            }
            pop();
            return setTy(et ? et : unkTy());
        }

        case ExKind::DictComp: {
            push();
            for (const auto& cl : e.clauses) compClause(cl, e.span.line);
            TyP kt, vt;
            if (!e.elems.empty() && e.elems[0] &&
                e.elems[0]->kind == ExKind::Spread) {
                TyP src = checkExpr(*e.elems[0]->lhs);
                if (!dynish(src)) {
                    if (src->is(TyK::Dict)) {
                        kt = argAt(src, 0);
                        vt = argAt(src, 1);
                    } else {
                        error(e.elems[0]->span,
                              "dict unpacking requires a dict, got " +
                                  toString(*src));
                    }
                }
            } else {
                kt = (!e.elems.empty() && e.elems[0]) ? checkExpr(*e.elems[0])
                                                       : unkTy();
                vt = e.value ? checkExpr(*e.value) : unkTy();
            }
            pop();
            return dictTy(kt ? kt : unkTy(), vt ? vt : unkTy());
        }

        case ExKind::Generator: {
            push();
            for (const auto& cl : e.clauses) compClause(cl, e.span.line);
            TyP et = !e.elems.empty() ? checkExpr(*e.elems[0]) : unkTy();
            pop();
            return genTy(et);
        }

        case ExKind::List: {
            TyP elem;
            for (const auto& el : e.elems) {
                TyP et = el->kind == ExKind::Spread
                             ? unpackElem(*el, "list unpacking")
                             : checkExpr(*el);
                elem = unify(elem, et, el->span.line, el->span.col,
                             "list elements");
            }
            return listTy(elem ? elem : unkTy());
        }

        case ExKind::Set: {
            TyP elem;
            for (const auto& el : e.elems) {
                TyP et = el->kind == ExKind::Spread
                             ? unpackElem(*el, "set unpacking")
                             : checkExpr(*el);
                elem = unify(elem, et, el->span.line, el->span.col,
                             "set elements");
            }
            return setTy(elem ? elem : unkTy());
        }

        case ExKind::Dict: {
            TyP kt, vt;
            for (const auto& pr : e.pairs) {
                if (pr.first && pr.first->kind == ExKind::Spread) {
                    TyP src = checkExpr(*pr.first->lhs);
                    if (dynish(src)) continue;
                    if (!src->is(TyK::Dict)) {
                        error(pr.first->span,
                              "dict unpacking requires a dict, got " +
                                  toString(*src));
                        continue;
                    }
                    kt = unify(kt, argAt(src, 0), pr.first->span.line,
                               pr.first->span.col, "dict keys");
                    vt = unify(vt, argAt(src, 1), pr.first->span.line,
                               pr.first->span.col, "dict values");
                    continue;
                }
                kt = unify(kt, checkExpr(*pr.first), pr.first->span.line,
                           pr.first->span.col, "dict keys");
                if (pr.second)
                    vt = unify(vt, checkExpr(*pr.second), pr.second->span.line,
                               pr.second->span.col, "dict values");
            }
            return dictTy(kt ? kt : unkTy(), vt ? vt : unkTy());
        }

        case ExKind::Spread:                          // only valid as an operand
            return e.lhs ? checkExpr(*e.lhs) : unkTy();

        case ExKind::Tuple: {
            std::vector<TyP> items;
            for (const auto& el : e.elems) items.push_back(checkExpr(*el));
            return tupleTy(std::move(items));
        }

        case ExKind::New:
            return resolveType(e.newType);

        case ExKind::Cast: {
            if (e.lhs) checkExpr(*e.lhs);
            return resolveType(e.newType);
        }

        case ExKind::Match: {
            // `match subj { case pat [if g] { body } ... }` (expression form).
            TyP subj = e.lhs ? checkExpr(*e.lhs) : unkTy();
            const ast::Expr* subjE = e.lhs.get();
            Symbol* subjSym = (subjE && subjE->kind == ExKind::Ident)
                                  ? scope_->find(subjE->text)
                                  : nullptr;
            narrowWarned_.clear();
            TyP cur = subj;
            TyP result;
            for (const auto& arm : e.matchArms) {
                push();
                if (arm.pat) checkPatternBindings(*arm.pat, cur);
                if (subjSym && arm.pat)
                    narrowSubject(subjSym, cur, arm.pat.get(), e.span.line,
                                  e.span.col);
                if (arm.guard) requireBool(checkExpr(*arm.guard), e.span.line,
                                           e.span.col, "match guard");
                // type the arm body; trailing expression establishes the arm's
                // value type, unified across arms.
                for (const auto& st : arm.body) {
                    checkStmt(*st);
                    // arm value type: a trailing expression statement yields
                    // its checked type; unified across arms.
                    if (st == arm.body.back() &&
                        st->kind == StKind::ExprStmt && !st->exprs.empty()) {
                        TyP vt = checkExpr(*st->exprs[0]);
                        result = unify(result, vt, e.span.line, e.span.col,
                                       "match arms");
                    }
                }
                pop();
                if (arm.pat) {
                    if (TyP nf = patternFact(*arm.pat, cur, /*positive*/ false))
                        cur = nf;
                }
            }
            checkExhaustiveness(subj, e.matchArms, e.span.line, e.span.col);
            return result ? result : unkTy();
        }

        case ExKind::Comptime: {
            TyP t = e.lhs ? checkExpr(*e.lhs) : unkTy();
            if (e.lhs) {
                constSteps_ = 0;
                ConstVal v;
                if (!constEvalExpr(*e.lhs, v))
                    error(e.span,
                          "E-CONST-IMPURE: '@comptime' expression is not "
                          "compile-time-evaluable");
            }
            return t;
        }

        case ExKind::AssignExpr: {
            // Walrus `(name := value)`: bind/assign and yield the value.
            TyP vt = e.rhs ? checkExpr(*e.rhs) : unkTy();
            if (e.lhs) bindWalrus(*e.lhs, vt);
            return vt;
        }

        case ExKind::Topic:
            if (topicStack_.empty()) {
                error(e.span, "E-PIPE-TOPIC: '%' used outside a pipe");
                return unkTy();
            }
            return topicStack_.back() ? topicStack_.back() : unkTy();

        case ExKind::Pipe: {
            // `a |> body`: `%` in body has the type of `a`; result is body's.
            TyP in = e.lhs ? checkExpr(*e.lhs) : unkTy();
            topicStack_.push_back(in ? in : unkTy());
            TyP out = e.rhs ? checkExpr(*e.rhs) : unkTy();
            topicStack_.pop_back();
            return out;
        }

        case ExKind::Coalesce: {
            // `a ?? b`: unwrap a's optional, unify with b. Result is non-opt.
            TyP lt = e.lhs ? checkExpr(*e.lhs) : unkTy();
            TyP rt = e.rhs ? checkExpr(*e.rhs) : unkTy();
            TyP base = unwrapOpt(lt);
            return unify(base, rt, e.span.line, e.span.col, "'??' operands");
        }
    }
    return unkTy();
}

void Checker::compClause(const ast::CompClause& cl, uint32_t line) {
    if (cl.isFor) {
        if (!cl.iter || !cl.pat) return;
        TyP iterT = checkExpr(*cl.iter);
        TyP elem = iterableElem(iterT);
        if (elem && elem->isError() && iterT && !iterT->isError())
            error(cl.iter->span.line, cl.iter->span.col,
                  "expression of type " + toString(*iterT) +
                      " is not iterable");
        else
            checkPatternBindings(*cl.pat, elem);
    } else if (cl.cond) {
        requireBool(checkExpr(*cl.cond), line, 0, "comprehension filter");
    }
}

void Checker::requireIntish(const TyP& t, const char* what, uint32_t line,
                            uint32_t col) {
    if (!t || t->isError() || t->isUnknown() || t->is(TyK::TypeVar) ||
        t->is(TyK::Int))
        return;
    error(line, col,
          std::string(what) + " must be an integer, got " + toString(*t));
}

// ============================== calls & members =============================

bool Checker::matchArgs(const FuncSig& sig,
                        const std::vector<ast::CallArg>& args, uint32_t line,
                        uint32_t col, const char* what) {
    std::vector<const ast::CallArg*> positional;
    std::map<std::string, const ast::CallArg*> named;
    for (const auto& a : args) {
        if (a.name.empty()) positional.push_back(&a);
        else named[a.name] = &a;
    }

    size_t pi = 0;
    bool ok = true;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const ast::CallArg* arg = nullptr;
        if (!sig.names[i].empty()) {
            auto it = named.find(sig.names[i]);
            if (it != named.end()) {
                arg = it->second;
                named.erase(it);
            }
        }
        if (!arg && pi < positional.size()) arg = positional[pi++];

        if (!arg) {
            if (i >= sig.required) continue;
            std::string nm =
                sig.names[i].empty() ? "(unnamed)" : sig.names[i];
            error(line, col, std::string(what) + ": missing argument '" + nm +
                                 "'");
            ok = false;
            continue;
        }
        TyP at = checkExpr(*arg->value);
        if (!assignable(at, sig.params[i])) {
            std::string nm = sig.names[i].empty()
                                 ? "#" + std::to_string(i + 1)
                                 : "'" + sig.names[i] + "'";
            error(arg->value->span.line, arg->value->span.col,
                  std::string(what) + ": argument " + nm + " expects " +
                      toString(*sig.params[i]) + ", got " + toString(*at));
            ok = false;
        }
    }

    if (pi < positional.size()) {
        if (sig.variadic && !sig.params.empty()) {
            for (; pi < positional.size(); ++pi) {
                TyP at = checkExpr(*positional[pi]->value);
                if (!assignable(at, sig.params.back())) {
                    error(positional[pi]->value->span.line,
                          positional[pi]->value->span.col,
                          std::string(what) + ": variadic argument expects " +
                              toString(*sig.params.back()));
                    ok = false;
                }
            }
        } else {
            error(line, col, std::string(what) + ": too many arguments");
            for (; pi < positional.size(); ++pi) checkExpr(*positional[pi]->value);
            ok = false;
        }
    }
    for (const auto& [nm, a] : named) {
        (void)a;
        error(line, col, std::string(what) + ": unknown argument '" + nm + "'");
        ok = false;
    }
    return ok;
}

TyP Checker::memberAccess(const TyP& obj, const std::string& name,
                          uint32_t line, uint32_t col, bool nilSafe) {
    auto miss = [&]() -> TyP {
        if (nilSafe) return unkTy();
        return errTy();
    };

    if (!obj || obj->isError() || obj->isUnknown() || obj->is(TyK::TypeVar))
        return unkTy();

    if (obj->is(TyK::Opt)) {
        if (name == "unwrap") return argAt(obj, 0);          // method handled in calls
        // v1 corpus semantics: member access through an optional unwraps it
        // (strong paths); `.?.` propagates optionality at the call level.
        return memberAccess(unwrapOpt(obj), name, line, col, nilSafe);
    }

    if (obj->is(TyK::Struct)) {
        auto it = structs_.find(obj->name);
        if (it != structs_.end()) {
            for (const auto& f : it->second->fields)
                if (f.first == name) return f.second;
            auto mit = it->second->methods.find(name);
            if (mit != it->second->methods.end())
                return fnTy(mit->second.params, mit->second.ret);
        }
        // result[T,E] pseudo-struct: expose payload fields
        if (obj->name == "result" && name == "value") return argAt(obj, 0);
        if (obj->name == "result" && name == "err") return argAt(obj, 1);

        // EnumName-style static access on struct? no.
        if (!nilSafe)
            error(line, col, "struct '" + obj->name + "' has no field '" +
                                 name + "'");
        return miss();
    }

    if (obj->is(TyK::EnumVal)) {
        Symbol* es = scope_->find(obj->variant);
        if (es && es->kind == SymK::EnumVariant)
            for (const auto& p : es->payloads)
                if (p.first == name) return p.second;
        if (!nilSafe)
            error(line, col, "enum value has no field '" + name + "'");
        return miss();
    }

    if (obj->is(TyK::EnumName)) {
        Symbol* es = scope_->find(name);
        if (es && es->kind == SymK::EnumVariant && es->enumOf == obj->name) {
            auto et = std::shared_ptr<Ty>(new Ty());
            et->k = TyK::EnumVal;
            et->name = obj->name;
            et->variant = name;
            return et;
        }
        if (!nilSafe)
            error(line, col, "enum '" + obj->name + "' has no variant '" +
                                 name + "'");
        return miss();
    }

    if (obj->is(TyK::Complex)) {
        if (name == "real" || name == "imag") return floatTy();
        if (!nilSafe)
            error(line, col, "type Complex has no member '" + name + "'");
        return miss();
    }

    if (obj->is(TyK::TraitObj)) {
        auto it = traits_.find(obj->name);
        if (it != traits_.end()) {
            auto sit = it->second->sigs.find(name);
            if (sit != it->second->sigs.end())
                return fnTy(sit->second.params, sit->second.ret);
        }
        if (!nilSafe)
            error(line, col, "trait '" + obj->name + "' has no method '" +
                                 name + "'");
        return miss();
    }

    if (!nilSafe)
        error(line, col, "type " + toString(*obj) + " has no member '" +
                             name + "'");
    return miss();
}

TyP Checker::checkMemberCall(const TyP& recv, const std::string& name,
                             const std::vector<ast::CallArg>& args,
                             uint32_t line, uint32_t col) {
    auto sig = methodLookup(recv, name);
    if (!sig) {
        if (recv && !recv->isError() && !recv->isUnknown() &&
            !recv->is(TyK::TypeVar)) {
            error(line, col, "no method '" + name + "' on " + toString(*recv));
            for (const auto& a : args) checkExpr(*a.value);
            return errTy();
        }
        for (const auto& a : args) checkExpr(*a.value);
        return unkTy();
    }
    matchArgs(*sig, args, line, col, ("method '" + name + "'").c_str());

    // sendability: values crossing threads through a channel must not carry
    // raw pointers/references, function values, or trait objects
    if (recv && recv->is(TyK::Chan) && name == "send" && !args.empty()) {
        ++quiet_;
        TyP at = checkExpr(*args[0].value);
        --quiet_;
        std::string why;
        if (!isSendable(at, &why))
            error(args[0].value->span.line, args[0].value->span.col,
                  "cannot send " + toString(*at) +
                      " across threads: " + why + " is not sendable");
    }
    return sig->ret;
}

// plan §7: shared memory is opt-in and checked. A type is sendable when it
// cannot smuggle a thread-foreign address: primitives and immutable containers
// of sendables are fine; pointers, references, function values and trait
// objects are rejected.
bool Checker::isSendable(const TyP& t, std::string* why) {
    std::set<std::string> seen;
    return isSendableVisit(t, why, seen);
}

bool Checker::isSendableVisit(const TyP& t, std::string* why,
                              std::set<std::string>& seeing) {
    auto reject = [&](const char* kind) {
        if (why) *why = kind;
        return false;
    };
    if (!t || t->isError() || t->isUnknown() || t->is(TyK::TypeVar))
        return true;   // don't cascade into poisoned/dynamic types
    switch (t->k) {
        case TyK::None:
        case TyK::Bool:
        case TyK::Int:
        case TyK::Float:
        case TyK::Str:
        case TyK::Char:
            return true;
        case TyK::Ptr:      return reject("raw pointer");
        case TyK::Ref:      return reject("reference");
        case TyK::Fn:       return reject("function value");
        case TyK::TraitObj: return reject("trait object");
        case TyK::Opt:
        case TyK::List:
        case TyK::Set:
        case TyK::Chan:
        case TyK::Range:
        case TyK::Gen:
            return t->args.empty() ||
                   isSendableVisit(t->args[0], why, seeing);
        case TyK::Dict:
            if (t->args.size() < 2) return true;
            return isSendableVisit(t->args[0], why, seeing) &&
                   isSendableVisit(t->args[1], why, seeing);
        case TyK::Tuple: {
            for (const auto& it : t->args)
                if (!isSendableVisit(it, why, seeing)) return false;
            return true;
        }
        case TyK::Struct: {
            if (!seeing.insert(t->name).second) return true;  // cycle: assume ok
            auto it = structs_.find(t->name);
            if (it == structs_.end()) return true;   // unknown: don't cascade
            for (const auto& [fname, fty] : it->second->fields)
                if (!isSendableVisit(fty, why, seeing)) return false;
            return true;
        }
        case TyK::EnumVal: {
            auto it = enums_.find(t->name);
            if (it == enums_.end()) return true;   // unknown: don't cascade
            if (!seeing.insert("enum:" + t->name + "." + t->variant).second)
                return true;
            auto vit = it->second->variantPayloads.find(t->variant);
            if (vit == it->second->variantPayloads.end()) return true;
            for (const auto& pp : vit->second)
                if (!isSendableVisit(pp.second, why, seeing)) return false;
            return true;
        }
        default:
            return true;
    }
}

TyP Checker::checkEnumCtorCall(const Symbol* vs,
                               const std::vector<ast::CallArg>& args) {
    std::map<std::string, TyP> payloads;
    for (const auto& p : vs->payloads) payloads[p.first] = p.second;
    for (const auto& a : args) {
        TyP at = checkExpr(*a.value);
        if (!a.name.empty()) {
            auto pit = payloads.find(a.name);
            if (pit != payloads.end() && !assignable(at, pit->second))
                error(a.value->span.line, a.value->span.col,
                      "payload '" + a.name + "' mismatch");
        }
    }
    auto vt = std::shared_ptr<Ty>(new Ty());
    vt->k = TyK::EnumVal;
    vt->name = vs->enumOf;
    vt->variant = vs->name;
    return vt;
}

void Checker::checkExhaustiveness(const TyP& subj,
                                  const std::vector<ast::MatchArm>& arms,
                                  uint32_t line, uint32_t col) {
    if (!subj) return;
    std::string enumName;
    bool isEnum = false;
    if (subj->is(TyK::EnumName))  { enumName = subj->name; isEnum = true; }
    if (subj->is(TyK::EnumVal))   { enumName = subj->name; isEnum = true; }
    if (isEnum) {
        auto ei = enums_.find(enumName);
        if (ei == enums_.end()) return;
        std::set<std::string> covered;
        bool wild = false;
        for (const auto& arm : arms)
            if (arm.pat)
                collectPatCoverage(*arm.pat, covered, wild, enumName, scope_);
        if (wild) return;
        std::vector<std::string> missing;
        for (const auto& v : ei->second->variants)
            if (!covered.count(v)) missing.push_back(v);
        if (missing.empty()) return;
        std::string msg =
            "non-exhaustive match over enum '" + enumName + "': missing";
        for (const auto& m : missing) msg += " '" + m + "'";
        error(line, col, msg);
        return;
    }
    if (subj->is(TyK::Int) || subj->is(TyK::Char)) {
        bool wild = false;
        for (const auto& arm : arms)
            if (arm.pat && arm.pat->kind == ast::PatKind::Wild) {
                wild = true;
                break;
            }
        if (!wild)
            error(line, col,
                  "match over integers/chars is non-exhaustive: add a "
                  "'case _' wildcard arm");
    }
}

TyP Checker::genericApply(const Symbol* sym,
                          const std::vector<ast::CallArg>& args, uint32_t line,
                          uint32_t col) {
    TyP ret = sym->sig.ret;
    if (sym->typeDefaults.empty()) return ret ? ret : unkTy();

    std::map<std::string, TyP> activ;
    for (const auto& kv : sym->typeDefaults) activ[kv.first] = kv.second;
    size_t pi = 0;
    for (const auto& a : args) {
        if (a.name.empty() && pi < sym->sig.params.size() &&
            sym->sig.params[pi] && sym->sig.params[pi]->is(TyK::TypeVar)) {
            TyP at = checkExpr(*a.value);
            if (at && !at->is(TyK::TypeVar) && !at->isUnknown() &&
                !at->isError())
                activ[sym->sig.params[pi]->name] = at;   // constraint wins
        }
        ++pi;
    }
    return substGeneric(ret, activ, line, col);
}

TyP Checker::checkCall(const ast::Expr& e) {

    const ast::Expr* callee = e.lhs.get();
    if (!callee) return unkTy();

    // method call: recv.method(args)
    if (callee->kind == ExKind::Member) {
        // enum variant construction: E.variant(payloads...)
        if (callee->lhs && callee->lhs->kind == ExKind::Ident) {
            Symbol* es = scope_->find(callee->lhs->text);
            if (es && es->kind == SymK::EnumName) {
                Symbol* vs = scope_->find(callee->text);
                if (vs && vs->kind == SymK::EnumVariant &&
                    vs->enumOf == es->name)
                    return checkEnumCtorCall(vs, e.args);
            }
        }
        TyP recv = checkExpr(*callee->lhs);
        bool nilCall = callee->nilSafe;
        TyP target = nilCall ? unwrapOpt(recv) : recv;
        TyP r = checkMemberCall(target, callee->text, e.args, e.span.line,
                                e.span.col);
        // x?.m(...) propagates none through the call
        if (nilCall && recv && recv->is(TyK::Opt)) return optTy(r);
        return r;
    }

    // chan construction: chan[int](2)
    if (callee->kind == ExKind::New) {
        TyP base = resolveType(callee->newType);
        for (const auto& a : e.args) {
            TyP at = checkExpr(*a.value);
            if (!base || !base->is(TyK::Chan)) break;
            requireIntish(at, "channel capacity", a.value->span.line,
                          a.value->span.col);
        }
        return base ? base : unkTy();
    }

    // sync.Atomic[T](v) / Atomic[T](v): element type lands in the index, value
    // typing is dynamic (matches the runtime, which types by the initial value).
    // The element must be sendable: it is shared mutable state across threads.
    if (callee->kind == ExKind::Index && callee->lhs) {
        const ast::Expr* b = callee->lhs.get();
        bool isAtomic = (b->kind == ExKind::Ident && b->text == "Atomic") ||
                        (b->kind == ExKind::Member && b->text == "Atomic");
        if (isAtomic) {
            TyP elemTy;
            const ast::Expr* tyArg = callee->rhs.get();
            if (tyArg && tyArg->kind == ExKind::Ident) {
                if (tyArg->text == "Bool" || tyArg->text == "bool")
                    elemTy = boolTy();
                else if (tyArg->text == "Float" || tyArg->text == "float" ||
                         tyArg->text == "f64" || tyArg->text == "f32")
                    elemTy = floatTy();
                else
                    elemTy = intTy();
            }
            std::string why;
            if (elemTy && !isSendable(elemTy, &why))
                error(callee->rhs ? callee->rhs->span.line : e.span.line,
                      callee->rhs ? callee->rhs->span.col : e.span.col,
                      "Atomic element type " + toString(*elemTy) +
                          " is not sendable: " + why);
            for (const auto& a : e.args) checkExpr(*a.value);
            return unkTy();
        }
    }

    // direct symbol callee keeps full signature info (variadic/optional)
    if (callee->kind == ExKind::Ident) {
        Symbol* sym = scope_->find(callee->text);
        if (sym && sym->kind == SymK::Func) {
            sym->used = true;
            const std::string& n = sym->name;
            if (builtins_.count(n)) {
                if (n == "print") {
                    for (const auto& a : e.args) checkExpr(*a.value);
                    return noneTy();
                }
                if (n == "len") {
                    if (e.args.size() != 1) {
                        error(e.span,
                              "len() takes exactly one argument");
                        return intTy();
                    }
                    TyP at = checkExpr(*e.args[0].value);
                    if (!at || !(at->is(TyK::List) || at->is(TyK::Dict) ||
                                 at->is(TyK::Set) || at->is(TyK::Str) ||
                                 at->isUnknown() || at->isError()))
                        error(e.args[0].value->span.line,
                              e.args[0].value->span.col,
                              "len() expects list, dict, set or string, got " +
                                  toString(*at));
                    return intTy();
                }
                if (n == "sqrt") {
                    if (e.args.size() != 1) {
                        error(e.span,
                              "sqrt() takes exactly one argument");
                        return floatTy();
                    }
                    TyP at = checkExpr(*e.args[0].value);
                    if (!dynish(at) && !isNumeric(*at))
                        error(e.args[0].value->span.line,
                              e.args[0].value->span.col,
                              "sqrt() expects a number, got " + toString(*at));
                    return floatTy();
                }
            }
            matchArgs(sym->sig, e.args, e.span.line, e.span.col,
                      ("function '" + n + "'").c_str());
            return genericApply(sym, e.args, e.span.line, e.span.col);
        }
        if (!sym) {
            if (importRoots_.count(callee->text)) {
                for (const auto& a : e.args) checkExpr(*a.value);
                return unkTy();
            }
            error(callee->span.line, callee->span.col,
                  "undefined function '" + callee->text + "'");
            for (const auto& a : e.args) checkExpr(*a.value);
            return errTy();
        }
    }

    // struct constructor call: Point(x: 0.0, y: 3.5)
    if (callee->kind == ExKind::Ident) {
        Symbol* sym = scope_->find(callee->text);
        if (sym && sym->kind == SymK::Struct) {
            sym->used = true;
            std::map<std::string, TyP> fieldTys;
            for (const auto& f : sym->fields) fieldTys[f.first] = f.second;
            for (const auto& a : e.args) {
                TyP at = checkExpr(*a.value);
                if (a.name.empty()) {
                    error(a.value->span.line, a.value->span.col,
                          "struct construction requires named arguments");
                    continue;
                }
                auto fit = fieldTys.find(a.name);
                if (fit == fieldTys.end()) {
                    error(a.value->span.line, a.value->span.col,
                          "struct '" + sym->name + "' has no field '" +
                              a.name + "'");
                    continue;
                }
                if (!assignable(at, fit->second))
                    error(a.value->span.line, a.value->span.col,
                          "field '" + a.name + "' expects " +
                              toString(*fit->second) + ", got " +
                              toString(*at));
            }
            return lookupNominal(TyK::Struct, sym->name);
        }
        if (sym && sym->kind == SymK::EnumVariant)
            return checkEnumCtorCall(sym, e.args);
    }

    // generic callee value (lambda / fn-typed / trait-object fn)
    TyP ct = checkExpr(*callee);
    // enum variant constructed through a member callee: Enum.bad(h: x)
    if (ct && ct->is(TyK::EnumVal)) {
        std::map<std::string, TyP> payloads;
        auto ei = enums_.find(ct->name);
        if (ei != enums_.end()) {
            auto vit = ei->second->variantPayloads.find(ct->variant);
            if (vit != ei->second->variantPayloads.end())
                for (const auto& pp : vit->second)
                    payloads[pp.first] = pp.second;
        }
        for (const auto& a : e.args) {
            TyP at = checkExpr(*a.value);
            if (!a.name.empty()) {
                auto pit = payloads.find(a.name);
                if (pit != payloads.end() && !assignable(at, pit->second))
                    error(a.value->span.line, a.value->span.col,
                          "payload '" + a.name + "' mismatch");
            }
        }
        return ct;
    }
    for (const auto& a : e.args) checkExpr(*a.value);
    if (ct && ct->is(TyK::Fn)) {
        FuncSig s;
        s.params = ct->args;
        s.names.assign(ct->args.size(), "");
        s.ret = ct->inner;
        s.required = ct->args.size();
        matchArgs(s, e.args, e.span.line, e.span.col, "call");
        return ct->inner ? ct->inner : unkTy();
    }
    if (!dynish(ct)) {
        error(e.span,
              "expression of type " + toString(*ct) + " is not callable");
        return errTy();
    }
    return unkTy();
}

// ====================== M5: const fn / comptime evaluator ====================

namespace {

bool constTruthy(const ConstVal& v) {
    switch (v.k) {
        case ConstVal::K::Int:   return v.i != 0;
        case ConstVal::K::Float: return v.f != 0.0;
        case ConstVal::K::Bool:  return v.b;
        case ConstVal::K::Str:   return !v.s.empty();
        case ConstVal::K::List:  return !v.elems.empty();
        case ConstVal::K::Tuple: return !v.elems.empty();
        case ConstVal::K::None:  return false;
        default:                 return false;
    }
}

bool constIsNum(const ConstVal& v) {
    return v.k == ConstVal::K::Int || v.k == ConstVal::K::Float;
}
double constNum(const ConstVal& v) {
    return v.k == ConstVal::K::Float ? v.f : (double)v.i;
}

// Builtins that a `const fn` may call: pure, total, and implemented by
// constCall below. Anything else is E-CONST-IMPURE.
bool constCallAllowed(const std::string& n) {
    static const std::set<std::string> k = {
        "abs", "Abs", "__Abs", "len", "Len", "int", "Int", "float", "Float",
        "bool", "Bool", "Ord", "__Ord", "Chr", "__Chr", "min", "Min",
        "max", "Max", "pow", "Pow", "__Pow", "sum",
    };
    return k.count(n) != 0;
}

}  // namespace

bool Checker::constEvalIter(const ast::Expr& iter, ConstVal& out) {
    if (iter.kind == ExKind::Binary &&
        (iter.op == ".." || iter.op == "..=")) {
        ConstVal lo, hi;
        if (!iter.lhs || !iter.rhs) return false;
        if (!constEvalExpr(*iter.lhs, lo) || !constEvalExpr(*iter.rhs, hi))
            return false;
        if (lo.k != ConstVal::K::Int || hi.k != ConstVal::K::Int) return false;
        long long end = hi.i + (iter.op == "..=" ? 1 : 0);
        out = ConstVal{};
        out.k = ConstVal::K::List;
        for (long long i = lo.i; i < end; ++i) {
            ConstVal e;
            e.k = ConstVal::K::Int;
            e.i = i;
            out.elems.push_back(e);
            if (out.elems.size() > 100000) return false;
        }
        return true;
    }
    ConstVal v;
    if (!constEvalExpr(iter, v)) return false;
    if (v.k == ConstVal::K::List || v.k == ConstVal::K::Tuple) {
        out = std::move(v);
        return true;
    }
    return false;
}

bool Checker::constCall(const std::string& name,
                        const std::vector<ConstVal>& args, const ast::Span& sp,
                        ConstVal& out) {
    auto num = [](const ConstVal& v, double& d) {
        if (v.k == ConstVal::K::Int) { d = (double)v.i; return true; }
        if (v.k == ConstVal::K::Float) { d = v.f; return true; }
        return false;
    };

    auto fnIt = constFns_.find(name);
    if (fnIt != constFns_.end()) {
        const ast::Stmt* fn = fnIt->second;
        if (args.size() != fn->params.size()) return false;
        std::map<std::string, ConstVal> saved;
        std::set<std::string> savedPresent;
        for (size_t i = 0; i < fn->params.size(); ++i) {
            const std::string& p = fn->params[i].name;
            auto old = constLocals_.find(p);
            if (old != constLocals_.end()) {
                saved[p] = old->second;
                savedPresent.insert(p);
            }
            constLocals_[p] = args[i];
        }
        ConstVal ret;
        int flow = constEvalStmts(fn->body, ret);
        for (const auto& p : fn->params) {
            auto it = saved.find(p.name);
            if (it != saved.end()) constLocals_[p.name] = it->second;
            else constLocals_.erase(p.name);
        }
        if (flow == -1) return false;
        if (flow == 1) { out = ret; return true; }
        out = ConstVal{};
        out.k = ConstVal::K::None;
        return true;
    }

        if (name == "len" || name == "Len") {
            if (args.size() != 1) return false;
            out = ConstVal{};
            out.k = ConstVal::K::Int;
            if (args[0].k == ConstVal::K::Str) out.i = (long long)args[0].s.size();
            else if (args[0].k == ConstVal::K::List ||
                     args[0].k == ConstVal::K::Tuple)
                out.i = (long long)args[0].elems.size();
            else return false;
            return true;
        }
    if ((name == "abs" || name == "Abs") && args.size() == 1) {
        if (args[0].k == ConstVal::K::Int) {
            out = args[0];
            if (out.i < 0) out.i = -out.i;
            return true;
        }
        if (args[0].k == ConstVal::K::Float) {
            out = args[0];
            out.f = out.f < 0 ? -out.f : out.f;
            return true;
        }
        return false;
    }
    if ((name == "min" || name == "Min" || name == "max" || name == "Max") &&
        args.size() == 2) {
        bool wantMax = (name == "max" || name == "Max");
        double a, b;
        if (!num(args[0], a) || !num(args[1], b)) return false;
        const ConstVal& pick = (wantMax ? (a >= b) : (a <= b)) ? args[0] : args[1];
        out = pick;
        return true;
    }
    if (name == "int" || name == "Int") {
        if (args.size() != 1) return false;
        out = ConstVal{};
        out.k = ConstVal::K::Int;
        if (args[0].k == ConstVal::K::Int) out.i = args[0].i;
        else if (args[0].k == ConstVal::K::Float) out.i = (long long)args[0].f;
        else if (args[0].k == ConstVal::K::Bool) out.i = args[0].b ? 1 : 0;
        else return false;
        return true;
    }
    if (name == "float" || name == "Float") {
        if (args.size() != 1) return false;
        double d;
        if (!num(args[0], d)) return false;
        out = ConstVal{};
        out.k = ConstVal::K::Float;
        out.f = d;
        return true;
    }
    if (name == "bool" || name == "Bool") {
        if (args.size() != 1) return false;
        out = ConstVal{};
        out.k = ConstVal::K::Bool;
        out.b = constTruthy(args[0]);
        return true;
    }
    if (name == "Ord" || name == "__Ord") {
        if (args.size() != 1 || args[0].k != ConstVal::K::Char) return false;
        out = ConstVal{};
        out.k = ConstVal::K::Int;
        out.i = args[0].s.empty() ? 0 : (unsigned char)args[0].s[0];
        return true;
    }
    if (name == "Chr" || name == "__Chr") {
        if (args.size() != 1 || args[0].k != ConstVal::K::Int) return false;
        out = ConstVal{};
        out.k = ConstVal::K::Char;
        out.s = std::string(1, (char)args[0].i);
        return true;
    }
    if (name == "Pow" || name == "__Pow" || name == "pow") {
        if (args.size() != 2 || args[0].k != ConstVal::K::Int ||
            args[1].k != ConstVal::K::Int)
            return false;
        if (args[1].i < 0) return false;
        long long base = args[0].i, e = args[1].i, r = 1;
        while (e > 0) {
            if (e & 1) r *= base;
            base *= base;
            e >>= 1;
            if (++constSteps_ > 100000) return false;
        }
        out = ConstVal{};
        out.k = ConstVal::K::Int;
        out.i = r;
        return true;
    }
    if (name == "sum") {
        if (args.size() != 1 ||
            (args[0].k != ConstVal::K::List && args[0].k != ConstVal::K::Tuple))
            return false;
        long long acc = 0;
        for (const auto& e : args[0].elems) {
            if (e.k != ConstVal::K::Int) return false;
            acc += e.i;
        }
        out = ConstVal{};
        out.k = ConstVal::K::Int;
        out.i = acc;
        return true;
    }
    (void)sp;
    return false;
}

bool Checker::constEvalExpr(const ast::Expr& e, ConstVal& out) {
    if (++constSteps_ > 200000) return false;
    switch (e.kind) {
        case ExKind::Int:
            out = ConstVal{};
            out.k = ConstVal::K::Int;
            out.i = parseIntLiteral(e.text);
            return true;
        case ExKind::Float:
            out = ConstVal{};
            out.k = ConstVal::K::Float;
            out.f = parseFloatLiteral(e.text);
            return true;
        case ExKind::CharLit:
            out = ConstVal{};
            out.k = ConstVal::K::Char;
            out.s = e.text;
            return true;
        case ExKind::Str:
            out = ConstVal{};
            out.k = ConstVal::K::Str;
            out.s = e.text;
            return true;
        case ExKind::Ident: {
            if (e.text == "true" || e.text == "false") {
                out = ConstVal{};
                out.k = ConstVal::K::Bool;
                out.b = e.text == "true";
                return true;
            }
            if (e.text == "none") {
                out = ConstVal{};
                out.k = ConstVal::K::None;
                return true;
            }
            auto lit = constLocals_.find(e.text);
            if (lit != constLocals_.end()) { out = lit->second; return true; }
            auto git = constVals_.find(e.text);
            if (git != constVals_.end()) { out = git->second; return true; }
            return false;
        }
        case ExKind::Unary: {
            ConstVal a;
            if (!e.rhs || !constEvalExpr(*e.rhs, a)) return false;
            if (e.op == "+") { out = a; return true; }
            if (e.op == "-") {
                if (a.k == ConstVal::K::Int) { out = a; out.i = -a.i; return true; }
                if (a.k == ConstVal::K::Float) { out = a; out.f = -a.f; return true; }
                return false;
            }
            if (e.op == "~") {
                if (a.k != ConstVal::K::Int) return false;
                out = a; out.i = ~a.i; return true;
            }
            if (e.op == "not") {
                if (a.k != ConstVal::K::Bool) return false;
                out = a; out.b = !a.b; return true;
            }
            return false;
        }
        case ExKind::Binary: {
            if (e.op == ".." || e.op == "..=") return constEvalIter(e, out);
            if (e.op == "and" || e.op == "or") {
                ConstVal a;
                if (!e.lhs || !constEvalExpr(*e.lhs, a)) return false;
                bool l = constTruthy(a);
                if (e.op == "and" && !l) { out = a; return true; }
                if (e.op == "or" && l) { out = a; return true; }
                if (!e.rhs || !constEvalExpr(*e.rhs, out)) return false;
                return true;
            }
            ConstVal a, b;
            if (!e.lhs || !e.rhs) return false;
            if (!constEvalExpr(*e.lhs, a) || !constEvalExpr(*e.rhs, b))
                return false;
            const std::string& op = e.op;
            // string concatenation
            if (op == "+" && a.k == ConstVal::K::Str && b.k == ConstVal::K::Str) {
                out = a; out.s += b.s; return true;
            }
            // equality / ordering on same-kind scalars
            bool aNum = a.k == ConstVal::K::Int || a.k == ConstVal::K::Float;
            bool bNum = b.k == ConstVal::K::Int || b.k == ConstVal::K::Float;
            if (op == "==" || op == "!=") {
                bool eq;
                if (aNum && bNum) {
                    double x = a.k == ConstVal::K::Int ? (double)a.i : a.f;
                    double y = b.k == ConstVal::K::Int ? (double)b.i : b.f;
                    eq = x == y;
                } else if (a.k == b.k && a.k != ConstVal::K::List &&
                           a.k != ConstVal::K::Tuple) {
                    switch (a.k) {
                        case ConstVal::K::Bool: eq = a.b == b.b; break;
                        case ConstVal::K::Str:
                        case ConstVal::K::Char: eq = a.s == b.s; break;
                        case ConstVal::K::None: eq = true; break;
                        default: eq = false; break;
                    }
                } else {
                    return false;
                }
                out = ConstVal{};
                out.k = ConstVal::K::Bool;
                out.b = (op == "==") ? eq : !eq;
                return true;
            }
            if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                double x, y;
                if (aNum && bNum) {
                    x = a.k == ConstVal::K::Int ? (double)a.i : a.f;
                    y = b.k == ConstVal::K::Int ? (double)b.i : b.f;
                } else if (a.k == ConstVal::K::Str && b.k == ConstVal::K::Str) {
                    bool r = op == "<" ? a.s < b.s
                             : op == "<=" ? a.s <= b.s
                             : op == ">" ? a.s > b.s : a.s >= b.s;
                    out = ConstVal{};
                    out.k = ConstVal::K::Bool;
                    out.b = r;
                    return true;
                } else {
                    return false;
                }
                bool r = op == "<"    ? x < y
                         : op == "<=" ? x <= y
                         : op == ">"  ? x > y
                                      : x >= y;
                out = ConstVal{};
                out.k = ConstVal::K::Bool;
                out.b = r;
                return true;
            }
            if (a.k == ConstVal::K::Int && b.k == ConstVal::K::Int) {
                long long x = a.i, y = b.i;
                out = ConstVal{};
                out.k = ConstVal::K::Int;
                if (op == "+") { out.i = x + y; return true; }
                if (op == "-") { out.i = x - y; return true; }
                if (op == "*") { out.i = x * y; return true; }
                if (op == "/" || op == "//") {
                    if (y == 0) return false;
                    out.i = (op == "/") ? x / y : x / y;   // int division
                    return true;
                }
                if (op == "%") { if (y == 0) return false; out.i = x % y; return true; }
                if (op == "**") {
                    if (y < 0) return false;
                    long long r = 1, base = x, e = y;
                    while (e > 0) {
                        if (e & 1) r *= base;
                        base *= base;
                        e >>= 1;
                        if (++constSteps_ > 100000) return false;
                    }
                    out.i = r; return true;
                }
                if (op == "&") { out.i = x & y; return true; }
                if (op == "|") { out.i = x | y; return true; }
                if (op == "^") { out.i = x ^ y; return true; }
                if (op == "<<") { out.i = x << y; return true; }
                if (op == ">>") { out.i = x >> y; return true; }
                return false;
            }
            if ((a.k == ConstVal::K::Float || a.k == ConstVal::K::Int) &&
                (b.k == ConstVal::K::Float || b.k == ConstVal::K::Int)) {
                double x = a.k == ConstVal::K::Float ? a.f : (double)a.i;
                double y = b.k == ConstVal::K::Float ? b.f : (double)b.i;
                out = ConstVal{};
                out.k = ConstVal::K::Float;
                if (op == "+") { out.f = x + y; return true; }
                if (op == "-") { out.f = x - y; return true; }
                if (op == "*") { out.f = x * y; return true; }
                if (op == "/") { if (y == 0.0) return false; out.f = x / y; return true; }
                return false;
            }
            return false;
        }
        case ExKind::Cond: {
            ConstVal c;
            if (!e.cond || !constEvalExpr(*e.cond, c)) return false;
            if (constTruthy(c))
                return e.lhs && constEvalExpr(*e.lhs, out);
            return e.rhs && constEvalExpr(*e.rhs, out);
        }
        case ExKind::Cast: {
            ConstVal a;
            if (!e.lhs || !constEvalExpr(*e.lhs, a)) return false;
            out = a;
            return true;
        }
        case ExKind::List:
        case ExKind::Tuple: {
            out = ConstVal{};
            out.k = e.kind == ExKind::List ? ConstVal::K::List
                                           : ConstVal::K::Tuple;
            for (const auto& el : e.elems) {
                ConstVal v;
                if (!el || !constEvalExpr(*el, v)) return false;
                out.elems.push_back(std::move(v));
                if (out.elems.size() > 100000) return false;
            }
            return true;
        }
        case ExKind::Index: {
            ConstVal base, idx;
            if (!e.lhs || !e.rhs) return false;
            if (!constEvalExpr(*e.lhs, base) || !constEvalExpr(*e.rhs, idx))
                return false;
            if (base.k == ConstVal::K::List || base.k == ConstVal::K::Tuple) {
                if (idx.k != ConstVal::K::Int) return false;
                long long i = idx.i;
                if (i < 0 || (size_t)i >= base.elems.size()) return false;
                out = base.elems[(size_t)i];
                return true;
            }
            if (base.k == ConstVal::K::Str && idx.k == ConstVal::K::Int) {
                long long i = idx.i;
                if (i < 0 || (size_t)i >= base.s.size()) return false;
                out = ConstVal{};
                out.k = ConstVal::K::Char;
                out.s = std::string(1, base.s[(size_t)i]);
                return true;
            }
            return false;
        }
        case ExKind::Call: {
            if (!e.lhs || e.lhs->kind != ExKind::Ident) return false;
            for (const auto& a : e.args)
                if (!a.name.empty()) return false;   // named args: unsupported
            std::vector<ConstVal> args;
            for (const auto& a : e.args) {
                ConstVal v;
                if (!a.value || !constEvalExpr(*a.value, v)) return false;
                args.push_back(std::move(v));
            }
            return constCall(e.lhs->text, args, e.span, out);
        }
        case ExKind::ListComp: {
            out = ConstVal{};
            out.k = ConstVal::K::List;
            if (e.elems.empty()) return false;
            const ast::Expr* elem = e.elems[0].get();
            std::function<bool(size_t)> run = [&](size_t ci) -> bool {
                if (ci == e.clauses.size()) {
                    ConstVal v;
                    if (!elem || !constEvalExpr(*elem, v)) return false;
                    out.elems.push_back(std::move(v));
                    return out.elems.size() <= 100000;
                }
                const CompClause& cl = e.clauses[ci];
                if (cl.isFor && cl.iter && cl.pat &&
                    cl.pat->kind == PatKind::Bind) {
                    ConstVal seq;
                    if (!constEvalIter(*cl.iter, seq)) return false;
                    const std::string& name = cl.pat->bindName;
                    auto old = constLocals_.find(name);
                    bool had = old != constLocals_.end();
                    ConstVal saved = had ? old->second : ConstVal{};
                    for (const auto& item : seq.elems) {
                        constLocals_[name] = item;
                        if (!run(ci + 1)) return false;
                    }
                    if (had) constLocals_[name] = saved;
                    else constLocals_.erase(name);
                    return true;
                }
                if (!cl.isFor && cl.cond) {
                    ConstVal c;
                    if (!constEvalExpr(*cl.cond, c)) return false;
                    if (!constTruthy(c)) return true;
                    return run(ci + 1);
                }
                return false;
            };
            return run(0);
        }
        default:
            return false;
    }
}

int Checker::constEvalStmts(const std::vector<ast::StmtP>& body,
                            ConstVal& ret) {
    for (const auto& st : body) {
        if (++constSteps_ > 200000) return -1;
        switch (st->kind) {
            case StKind::Pass:
                break;
            case StKind::VarDecl:
            case StKind::ConstDecl: {
                if (!st->target || st->target->kind != ExKind::Ident)
                    return -1;
                ConstVal v;
                if (!st->value || !constEvalExpr(*st->value, v)) return -1;
                constLocals_[st->target->text] = std::move(v);
                break;
            }
            case StKind::Assign: {
                if (st->exprs.size() != 2 ||
                    st->exprs[0]->kind != ExKind::Ident)
                    return -1;
                ConstVal v;
                if (!constEvalExpr(*st->exprs[1], v)) return -1;
                constLocals_[st->exprs[0]->text] = std::move(v);
                break;
            }
            case StKind::AugAssign: {
                if (st->exprs.size() != 2 ||
                    st->exprs[0]->kind != ExKind::Ident)
                    return -1;
                ConstVal cur, rhs;
                auto it = constLocals_.find(st->exprs[0]->text);
                if (it == constLocals_.end()) return -1;
                cur = it->second;
                if (!constEvalExpr(*st->exprs[1], rhs)) return -1;
                ast::Expr bin;
                bin.kind = ExKind::Binary;
                bin.op = st->augOp;
                ConstVal la = cur, lb = rhs;
                // reuse the binary evaluator by simulating literal operands
                if (la.k == ConstVal::K::Int && lb.k == ConstVal::K::Int) {
                    long long x = la.i, y = lb.i;
                    ConstVal r;
                    r.k = ConstVal::K::Int;
                    if (st->augOp == "+") r.i = x + y;
                    else if (st->augOp == "-") r.i = x - y;
                    else if (st->augOp == "*") r.i = x * y;
                    else if ((st->augOp == "/" || st->augOp == "//") && y != 0)
                        r.i = x / y;
                    else if (st->augOp == "%" && y != 0) r.i = x % y;
                    else return -1;
                    constLocals_[st->exprs[0]->text] = r;
                } else {
                    return -1;
                }
                break;
            }
            case StKind::ExprStmt: {
                if (st->exprs.empty()) break;
                ConstVal v;
                if (!constEvalExpr(*st->exprs[0], v)) return -1;
                break;
            }
            case StKind::Return: {
                if (st->exprs.empty()) {
                    ret = ConstVal{};
                    ret.k = ConstVal::K::None;
                    return 1;
                }
                if (!constEvalExpr(*st->exprs[0], ret)) return -1;
                return 1;
            }
            case StKind::Break:
                return 2;
            case StKind::Continue:
                return 3;
            case StKind::Unsafe:
            case StKind::ComptimeBlock: {
                int f = constEvalStmts(st->body, ret);
                if (f == -1) return -1;
                if (f == 1 || f == 2 || f == 3) return f;
                break;
            }
            case StKind::If: {
                ConstVal c;
                if (st->exprs.empty() || !constEvalExpr(*st->exprs[0], c))
                    return -1;
                const std::vector<StmtP>& branch =
                    constTruthy(c) ? st->body : st->elseBody;
                int f = constEvalStmts(branch, ret);
                if (f == -1) return -1;
                if (f == 1 || f == 2 || f == 3) return f;
                break;
            }
            case StKind::While: {
                for (;;) {
                    ConstVal c;
                    if (st->exprs.empty() || !constEvalExpr(*st->exprs[0], c))
                        return -1;
                    if (!constTruthy(c)) break;
                    int f = constEvalStmts(st->body, ret);
                    if (f == -1) return -1;
                    if (f == 1) return 1;
                    if (f == 3) continue;   // continue
                    if (f == 2) break;      // break
                    if (++constSteps_ > 200000) return -1;
                }
                break;
            }
            case StKind::For: {
                if (st->pat && st->pat->kind == PatKind::Bind &&
                    !st->exprs.empty()) {
                    ConstVal seq;
                    if (!constEvalIter(*st->exprs[0], seq)) return -1;
                    const std::string& name = st->pat->bindName;
                    auto old = constLocals_.find(name);
                    bool had = old != constLocals_.end();
                    ConstVal saved = had ? old->second : ConstVal{};
                    for (const auto& item : seq.elems) {
                        constLocals_[name] = item;
                        int f = constEvalStmts(st->body, ret);
                        if (f == -1) return -1;
                        if (f == 1) return 1;
                        if (f == 2) break;
                        if (++constSteps_ > 200000) return -1;
                    }
                    if (had) constLocals_[name] = saved;
                    else constLocals_.erase(name);
                    break;
                }
                return -1;
            }
            default:
                return -1;
        }
    }
    return 0;
}

ast::ExprP Checker::constValToExpr(const ConstVal& v,
                                   const ast::Span& sp) const {
    auto e = std::make_unique<ast::Expr>();
    e->span = sp;
    switch (v.k) {
        case ConstVal::K::Int:
            e->kind = ExKind::Int;
            e->text = std::to_string(v.i);
            break;
        case ConstVal::K::Float:
            e->kind = ExKind::Float;
            e->text = std::to_string(v.f);
            break;
        case ConstVal::K::Bool:
            e->kind = ExKind::Ident;
            e->text = v.b ? "true" : "false";
            break;
        case ConstVal::K::Str:
            e->kind = ExKind::Str;
            e->text = v.s;
            break;
        case ConstVal::K::Char:
            e->kind = ExKind::CharLit;
            e->text = v.s;
            break;
        case ConstVal::K::None:
            e->kind = ExKind::Ident;
            e->text = "none";
            break;
        case ConstVal::K::List:
        case ConstVal::K::Tuple:
            e->kind = v.k == ConstVal::K::List ? ExKind::List : ExKind::Tuple;
            for (const auto& el : v.elems)
                e->elems.push_back(constValToExpr(el, sp));
            break;
        default:
            e->kind = ExKind::Ident;
            e->text = "none";
            break;
    }
    return e;
}

// Structural purity scan for a `const fn` body.
void Checker::checkConstFnPurity(const ast::Stmt& fn) {
    std::function<void(const ast::Expr&)> wExpr;
    std::function<void(const std::vector<ast::StmtP>&)> wBody;

    auto impure = [&](const ast::Span& sp, const char* what) {
        error(sp.line, sp.col, std::string("E-CONST-IMPURE: ") + what +
                                   " is not allowed in a 'const fn'");
    };

    wExpr = [&](const ast::Expr& e) {
        if (e.kind == ExKind::Call) {
            if (e.lhs && e.lhs->kind == ExKind::Ident) {
                const std::string& n = e.lhs->text;
                if (constFns_.count(n) == 0 && !constCallAllowed(n))
                    impure(e.span, "call to runtime function");
            } else if (e.lhs) {
                impure(e.span, "method call");
            }
        } else if (e.kind == ExKind::Lambda) {
            impure(e.span, "closure");
        }
        if (e.lhs) wExpr(*e.lhs);
        if (e.rhs) wExpr(*e.rhs);
        if (e.value) wExpr(*e.value);
        if (e.cond) wExpr(*e.cond);
        for (const auto& el : e.elems)
            if (el) wExpr(*el);
        for (const auto& pr : e.pairs) {
            if (pr.first) wExpr(*pr.first);
            if (pr.second) wExpr(*pr.second);
        }
        for (const auto& a : e.args)
            if (a.value) wExpr(*a.value);
        for (const auto& part : e.parts)
            if (part.expr) wExpr(*part.expr);
        for (const auto& cl : e.clauses) {
            if (cl.iter) wExpr(*cl.iter);
            if (cl.cond) wExpr(*cl.cond);
        }
    };

    wBody = [&](const std::vector<ast::StmtP>& body) {
        for (const auto& st : body) {
            if (!st) continue;
            switch (st->kind) {
                case StKind::Raise:      impure(st->span, "'raise'"); break;
                case StKind::Try:        impure(st->span, "'try'"); break;
                case StKind::Defer:      impure(st->span, "'defer'"); break;
                case StKind::Spawn:      impure(st->span, "'spawn'"); break;
                case StKind::With:       impure(st->span, "'with'"); break;
                case StKind::Using:      impure(st->span, "'using'"); break;
                case StKind::Yield:      impure(st->span, "'yield'"); break;
                case StKind::Gather:
                case StKind::GatherStmt: impure(st->span, "'gather'"); break;
                case StKind::Select:     impure(st->span, "'select'"); break;
                case StKind::Import:
                case StKind::Export:     impure(st->span, "import"); break;
                case StKind::Del:        impure(st->span, "'del'"); break;
                case StKind::Goto:       impure(st->span, "'goto'"); break;
                case StKind::Label:      impure(st->span, "label"); break;
                case StKind::BucketDecl: impure(st->span, "'bucket'"); break;
                case StKind::TempDecl:   impure(st->span, "'temp'"); break;
                default: break;
            }
            if (st->value) wExpr(*st->value);
            if (st->target) wExpr(*st->target);
            for (const auto& x : st->exprs)
                if (x) wExpr(*x);
            for (const auto& c : st->elifConds)
                if (c) wExpr(*c);
            for (const auto& arm : st->arms) {
                if (arm.guard) wExpr(*arm.guard);
                wBody(arm.body);
            }
            for (const auto& sa : st->selArms)
                if (sa.chanOp) wExpr(*sa.chanOp);
            wBody(st->body);
            wBody(st->elseBody);
            wBody(st->elifBodies);
        }
    };

    wBody(fn.body);
}

// ====================== end M5 evaluator ====================

} // namespace sema
} // namespace kirn

