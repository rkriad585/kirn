// Semantic analysis for Kirn v1. See checker.h for the design overview.
#include "sema/checker.h"
#include "sema/borrow.h"

namespace kirn {
namespace sema {

using namespace kirn::ast;

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
        case TyK::Bool:    return "bool";
        case TyK::Int:     return t.name.empty() ? "int" : t.name;
        case TyK::Float:   return t.name.empty() ? "float" : t.name;
        case TyK::Str:     return "string";
        case TyK::Char:    return "char";
        case TyK::List:    return t.args.empty() ? "list" : "list[" + toString(*t.args[0]) + "]";
        case TyK::Dict:
            return t.args.size() < 2
                ? "dict"
                : "dict[" + toString(*t.args[0]) + ", " + toString(*t.args[1]) + "]";
        case TyK::Set:     return t.args.empty() ? "set" : "set[" + toString(*t.args[0]) + "]";
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
    scope_ = new Scope(scope_);
    return *scope_;
}

void Checker::pop() {
    Scope* p = scope_->parent;
    if (scope_ != p) reportUnused(*scope_, /*topLevel*/ false);
    delete scope_;
    scope_ = p;
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
    if (Symbol* clash = scope_->declare(sym))
        error(line, col, "'" + name + "' is already defined in this scope");
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

    // Free builtins: print(...) len(x) sqrt(x) range(...). User decls may shadow.
    makeBuiltinFunc("print", {unkTy()}, {"x"}, noneTy(), /*variadic*/ true, 0);
    makeBuiltinFunc("len", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("sqrt", {floatTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("range", {intTy(), intTy(), intTy()},
                    {"start", "end", "step"}, rangeTy(intTy()),
                    /*variadic*/ true, 1);
    makeBuiltinFunc("panic", {unkTy()}, {"msg"}, unkTy(),
                    /*variadic*/ true, 0);
    makeBuiltinFunc("catch_panic", {unkTy()}, {"thunk"},
                    nominalWithArgs(TyK::Struct, "result", {unkTy(), unkTy()}),
                    false, 1);
    makeBuiltinFunc("ord", {charTy()}, {"c"}, intTy(), false, 1);
    makeBuiltinFunc("chr", {intTy()}, {"n"}, charTy(), false, 1);
    makeBuiltinFunc("assert", {boolTy()}, {"cond"}, noneTy(), false, 1);
    makeBuiltinFunc("assert_eq", {unkTy(), unkTy()}, {"a", "b"}, noneTy(),
                    false, 2);

    // WHY-1 / SP-8: batteries-included conversion & collection builtins.
    // Polymorphic params use Unknown (dynamic v1) matched at runtime.
    makeBuiltinFunc("str", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("int", {unkTy()}, {"x"}, intTy(), false, 1);
    makeBuiltinFunc("float", {unkTy()}, {"x"}, floatTy(), false, 1);
    makeBuiltinFunc("bool", {unkTy()}, {"x"}, boolTy(), false, 1);
    makeBuiltinFunc("type", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("repr", {unkTy()}, {"x"}, strTy(), false, 1);
    makeBuiltinFunc("sum", {unkTy()}, {"xs"}, unkTy(), false, 1);
    makeBuiltinFunc("min", {unkTy()}, {"xs"}, unkTy(), false, 1);
    makeBuiltinFunc("max", {unkTy()}, {"xs"}, unkTy(), false, 1);
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
    for (const char* mod : {"time", "mem", "io", "os", "json", "math"})
        importRoots_.insert(mod);
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

            // scalar spellings
            if (n == "bool")   return boolTy();
            if (n == "string") return strTy();
            if (n == "char")   return charTy();
            if (n == "none" || n == "unit" || n == "None") return noneTy();
            if (n == "int" || n == "f32" || n == "float")
                return n == "int" ? intTy() : floatTy(n == "f32" ? "f32" : "f64");
            if (n == "f64")    return floatTy("f64");
            static const char* kIntNames[] = {"i8", "i16", "i32", "i64",
                                              "u8", "u16", "u32", "u64",
                                              "usize", nullptr};
            for (int i = 0; kIntNames[i]; ++i)
                if (n == kIntNames[i]) return intTy(n);

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
    out.ret = ret ? resolveType(ret) : unkTy();
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
        case TyK::Range:
        case TyK::Gen:
        case TyK::Chan:
        case TyK::Dict: {
            TyP e = argAt(t, 0);
            if (e && e->isError()) return unkTy();     // silence poisoned cascades
            return e;
        }
        case TyK::Str:
            return charTy();
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

void Checker::checkReassignable(const ast::Expr& tgt) {
    if (tgt.kind != ExKind::Ident) return;      // index/member targets: v1 lenient
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
            if (!ex) {
                declareLocal(SymK::Var, tgt.text, vt, /*mut*/ multi,
                             tgt.span.line, tgt.span.col);
                typeCache_[&tgt] = vt;   // codegen: tag implicit bindings
                return;
            }
            checkReassignable(tgt);
            typeCache_[&tgt] = ex->type ? ex->type : unkTy();
            break;
        }
        case ExKind::Index:
        case ExKind::Member: {
            TyP tt = checkExpr(tgt);
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

        case StKind::If: {
            if (!s.exprs.empty()) requireBool(checkExpr(*s.exprs[0]),
                                              s.span.line, s.span.col,
                                              "if condition");
            push();
            checkBlock(s.body);
            pop();
            // elif chains are nested as synthetic If statements in elseBody[0]
            if (!s.elseBody.empty()) {
                push();
                checkBlock(s.elseBody);
                pop();
            }
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
            for (const auto& arm : s.arms) {
                push();
                if (arm.pat) checkPatternBindings(*arm.pat, subj);
                if (arm.guard) requireBool(checkExpr(*arm.guard),
                                           arm.pat ? arm.pat->span.line
                                                   : s.span.line,
                                           s.span.col, "match guard");
                checkBlock(arm.body);
                pop();
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
            if (e.flavor == StrFlavor::C || e.flavor == StrFlavor::Byte)
                return ptrTy(charTy());
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
                if (!dynish(lt) && !dynish(rt) && !equal(lt, rt) &&
                    !(isNumeric(*lt) && isNumeric(*rt)) &&
                    !assignable(lt, rt) && !assignable(rt, lt))
                    error(e.span,
                          "comparing incompatible types " + toString(*lt) +
                              " and " + toString(*rt));
                return boolTy();
            }
            if (op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
                return boolTy();
            if (op == "in") {
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
                if (op == "+") {
                    auto isStrish = [](const TyP& t) {
                        return t->is(TyK::Str) || t->is(TyK::Char);
                    };
                    bool strcat = isStrish(lt) && isStrish(rt);
                    bool listcat = lt->is(TyK::List) && rt->is(TyK::List);
                    if (strcat) return strTy();
                    if (listcat)
                        return listTy(unify(argAt(lt, 0), argAt(rt, 0),
                                            e.span.line, e.span.col, "'+'"));
                }
                if (dynish(lt)) return rt;
                if (dynish(rt)) return lt;
                if (isNumeric(*lt) && isNumeric(*rt)) {
                    if (lt->is(TyK::Float) || rt->is(TyK::Float)) return floatTy();
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
            if (obj->is(TyK::Str)) {
                if (idx->is(TyK::Int)) return charTy();
                if (idx->is(TyK::Range)) return strTy();
                error(e.span, "string index must be an integer");
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
            TyP et = !e.elems.empty() ? checkExpr(*e.elems[0]) : unkTy();
            pop();
            return listTy(et);
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
            for (const auto& el : e.elems)
                elem = unify(elem, checkExpr(*el), el->span.line, el->span.col,
                             "list elements");
            return listTy(elem ? elem : unkTy());
        }

        case ExKind::Set: {
            TyP elem;
            for (const auto& el : e.elems)
                elem = unify(elem, checkExpr(*el), el->span.line, el->span.col,
                             "set elements");
            return setTy(elem ? elem : unkTy());
        }

        case ExKind::Dict: {
            TyP kt, vt;
            for (const auto& pr : e.pairs) {
                kt = unify(kt, checkExpr(*pr.first), pr.first->span.line,
                           pr.first->span.col, "dict keys");
                vt = unify(vt, checkExpr(*pr.second), pr.second->span.line,
                           pr.second->span.col, "dict values");
            }
            return dictTy(kt ? kt : unkTy(), vt ? vt : unkTy());
        }

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
            TyP result;
            for (const auto& arm : e.matchArms) {
                push();
                if (arm.pat) checkPatternBindings(*arm.pat,
                                                   unwrapOpt(subj));
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
            }
            checkExhaustiveness(subj, e.matchArms, e.span.line, e.span.col);
            return result ? result : unkTy();
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

} // namespace sema
} // namespace kirn
