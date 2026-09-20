// Tree-walking interpreter implementation (plan §10.4).
// Divergences from the eventual compiled semantics, all unobservable in the
// corpus: generator views and filter/map chains materialize eagerly; channels
// declared without capacity queue unboundedly instead of rendezvous-syncing;
// weak fields keep their target alive while any strong handle exists.
#include "interp/runtime.h"
#include "interp/escapes.h"
#include "lex/lexer.h"
#include "parser/parser.h"
#include "sema/checker.h"
#include "util/tomlmini.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace kirn {
namespace interp {

namespace vm = kirn::vm;
using namespace kirn::vm;

using ast::CallArg;
using ast::Expr;
using ast::Param;
using ast::Pat;
using ast::Stmt;

void panicHere(const std::string& msg) {
    PanicSignal sig;
    sig.msg = msg;
    sig.frames = g_panicFrames;
    throw sig;
}

// C4996-safe environment lookup (getenv is deprecated on MSVC).
const char* envGet(const char* name) {
#if defined(_MSC_VER)
    size_t needed = 0;
    char* buf = nullptr;
    if (_dupenv_s(&buf, &needed, name) == 0 && buf) return buf;
    return nullptr;
#else
    return std::getenv(name);
#endif
}

thread_local std::vector<std::string> g_panicFrames;

CallFrameGuard::CallFrameGuard(std::string name, uint32_t line, uint32_t col) {
    std::string frame = "in " + name;
    if (line) {
        frame += " (line " + std::to_string(line);
        if (col) frame += ":" + std::to_string(col);
        frame += ")";
    }
    g_panicFrames.push_back(std::move(frame));
}
CallFrameGuard::~CallFrameGuard() {
    g_panicFrames.pop_back();
}

// forward decls (defined near the bottom of this file)
static void mapNamedIntoPos(const std::vector<std::string>& slots,
                            std::vector<Value>& pos,
                            std::vector<std::pair<std::string, Value>>& named);
static Value lockHeap(const Value& v);
static size_t normIndex(int64_t len, int64_t idx);
static Value sliceOf(const Value& obj, int64_t lo, int64_t hi, bool incl,
                     int64_t step);
static bool valEq(const Value& a, const Value& b);
static bool valLess(const Value& a, const Value& b);

// ---------------------------------------------------------------------------
// environments & small helpers
// ---------------------------------------------------------------------------

struct EnvS {
    std::unordered_map<std::string, Value> vars;
    Env parent;

    const Value* find(const std::string& n) const {
        for (const EnvS* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
    Value* findRef(const std::string& n) {
        for (EnvS* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
    // Erase a binding from the frame where it actually lives (walk the chain).
    // Returns true if found+erased.
    bool unbind(const std::string& n) {
        for (EnvS* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) { e->vars.erase(it); return true; }
        }
        return false;
    }
    EnvS* root() {
        EnvS* e = this;
        while (e->parent) e = e->parent.get();
        return e;
    }
};

static Env makeChild(Env parent) {
    auto e = std::make_shared<EnvS>();
    e->parent = std::move(parent);
    return e;
}

// ---- AST deep-clone (used by single-inheritance merging, mergeInheritance) --
// Structs have unique_ptr members (no default copy); these cloners cover
// precisely the node shapes a class/struct declaration can carry.

static ast::TypeP cloneType(const ast::Type* t) {
    if (!t) return nullptr;
    auto c = std::make_unique<ast::Type>();
    c->kind = t->kind;
    c->span = t->span;
    c->name = t->name;
    c->refMut = t->refMut;
    if (t->inner) c->inner = cloneType(t->inner.get());
    for (const auto& g : t->generics) c->generics.push_back(cloneType(g.get()));
    for (const auto& p : t->params) c->params.push_back(cloneType(p.get()));
    if (t->ret) c->ret = cloneType(t->ret.get());
    return c;
}

static ast::TypeP cloneType(const ast::Type* t);
static ast::ExprP cloneExpr(const ast::Expr* e);
static ast::StmtP cloneStmt(const ast::Stmt* s);

static ast::PatP clonePat(const ast::Pat* p) {
    if (!p) return nullptr;
    auto c = std::make_unique<ast::Pat>();
    c->kind = p->kind;
    c->span = p->span;
    c->ctorName = p->ctorName;
    c->bindName = p->bindName;
    c->restName = p->restName;
    c->hasRest = p->hasRest;
    c->inclusive = p->inclusive;
    c->literal = cloneExpr(p->literal.get());
    c->lo = clonePat(p->lo.get());
    c->hi = clonePat(p->hi.get());
    c->bindType = cloneType(p->bindType.get());
    c->aliasSub = clonePat(p->aliasSub.get());
    c->inner = clonePat(p->inner.get());
    for (const auto& el : p->elems) c->elems.push_back(clonePat(el.get()));
    for (const auto& f : p->fields) {
        ast::PatField nf;
        nf.name = f.name;
        nf.pat = clonePat(f.pat.get());
        c->fields.push_back(std::move(nf));
    }
    for (const auto& a : p->alts) c->alts.push_back(clonePat(a.get()));
    return c;
}

static ast::ExprP cloneExpr(const ast::Expr* e) {
    if (!e) return nullptr;
    auto c = std::make_unique<ast::Expr>();
    c->kind = e->kind;
    c->span = e->span;
    c->text = e->text;
    c->flavor = e->flavor;
    c->op = e->op;
    c->nilSafe = e->nilSafe;
    c->lhs = cloneExpr(e->lhs.get());
    c->rhs = cloneExpr(e->rhs.get());
    c->cond = cloneExpr(e->cond.get());
    if (e->pat) c->pat = clonePat(e->pat.get());
    c->newType = cloneType(e->newType.get());
    for (const auto& a : e->args) {
        ast::CallArg ca;
        ca.name = a.name;
        ca.value = cloneExpr(a.value.get());
        c->args.push_back(std::move(ca));
    }
    c->lambdaParams = e->lambdaParams;
    for (const auto& el : e->elems) c->elems.push_back(cloneExpr(el.get()));
    for (const auto& pr : e->pairs)
        c->pairs.emplace_back(cloneExpr(pr.first.get()),
                              cloneExpr(pr.second.get()));
    for (const auto& arm : e->matchArms) {
        ast::MatchArm na;
        na.pat = clonePat(arm.pat.get());
        na.guard = cloneExpr(arm.guard.get());
        for (const auto& b : arm.body) na.body.push_back(cloneStmt(b.get()));
        c->matchArms.push_back(std::move(na));
    }
    return c;
}

static ast::FieldDecl cloneField(const ast::FieldDecl& f) {
    ast::FieldDecl nf;
    nf.weak = f.weak;
    nf.mutable_ = f.mutable_;
    nf.pub = f.pub;
    nf.name = f.name;
    nf.span = f.span;
    nf.type = cloneType(f.type.get());
    nf.defaultValue = cloneExpr(f.defaultValue.get());
    return nf;
}

static ast::StmtP cloneStmt(const ast::Stmt* s) {
    if (!s) return nullptr;
    auto c = std::make_unique<ast::Stmt>();
    c->kind = s->kind;
    c->span = s->span;
    c->name = s->name;
    c->pub = s->pub;
    c->externDef = s->externDef;
    c->baseName = s->baseName;
    c->catchParam = s->catchParam;
    c->ret = cloneType(s->ret.get());
    c->augOp = s->augOp;
    c->label = s->label;
    c->varKw = s->varKw;
    c->moduleName = s->moduleName;
    c->importAlias = s->importAlias;
    c->starImport = s->starImport;
    c->fromImport = s->fromImport;
    c->implTrait = cloneType(s->implTrait.get());
    c->implType = cloneType(s->implType.get());
    c->declType = cloneType(s->declType.get());
    c->value = cloneExpr(s->value.get());
    c->target = cloneExpr(s->target.get());
    for (const auto& p : s->params) {
        ast::Param np;
        np.name = p.name;
        np.mutable_ = p.mutable_;
        np.variadic = p.variadic;
        np.selfParam = p.selfParam;
        np.span = p.span;
        np.type = cloneType(p.type.get());
        np.defaultValue = cloneExpr(p.defaultValue.get());
        c->params.push_back(std::move(np));
    }
    for (const auto& f : s->fields) c->fields.push_back(cloneField(f));
    for (const auto& e : s->exprs) c->exprs.push_back(cloneExpr(e.get()));
    for (const auto& b : s->body) c->body.push_back(cloneStmt(b.get()));
    for (const auto& b : s->elseBody) c->elseBody.push_back(cloneStmt(b.get()));
    for (const auto& b : s->elifBodies) c->elifBodies.push_back(cloneStmt(b.get()));
    c->pat = clonePat(s->pat.get());
    c->cond = cloneExpr(s->cond.get());
    for (const auto& se : s->seedExprs) c->seedExprs.push_back(cloneExpr(se.get()));
    for (const auto& arm : s->arms) {
        ast::MatchArm na;
        na.pat = clonePat(arm.pat.get());
        na.guard = cloneExpr(arm.guard.get());
        for (const auto& b : arm.body) na.body.push_back(cloneStmt(b.get()));
        c->arms.push_back(std::move(na));
    }
    return c;
}

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

static void ipow64(int64_t base, int64_t exp, int64_t& out) {
    out = 1;
    while (exp > 0) {
        if (exp & 1) out *= base;
        exp >>= 1;
        if (exp) base *= base;
    }
}

static std::vector<std::string> splitDots(const std::string& dotted) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t dot = dotted.find('.', start);
        parts.push_back(dotted.substr(start, dot == std::string::npos
                                                ? std::string::npos
                                                : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts;
}

static std::string stripUnderscores(const std::string& s) {
    std::string o;
    for (char c : s)
        if (c != '_') o += c;
    return o;
}

static int64_t parseIntText(const std::string& raw) {
    std::string t = stripUnderscores(raw);
    int base = 10;
    if (t.size() > 2 && t[0] == '0') {
        char p = (char)tolower(t[1]);
        if (p == 'x') { base = 16; t = t.substr(2); }
        else if (p == 'b') { base = 2; t = t.substr(2); }
        else if (p == 'o') { base = 8; t = t.substr(2); }
    }
    return strtoll(t.c_str(), nullptr, base);
}

// Decodes lexer escape sequences (\n \t \r \\ \' \" \0 \xHH \u{HEX}).
// Shared implementation lives in escapes.h (used by tree-walker and VM emitter).

// ---------------------------------------------------------------------------
// value rendering
// ---------------------------------------------------------------------------

static std::string escapeStr(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            default:   out += c;
        }
    }
    return out;
}

std::string repr(const Value& v);

std::string toStr(const Value& v) {
    switch (v.k) {
        case VK::None:   return "none";
        case VK::Bool:   return v.b ? "true" : "false";
        case VK::Int:    return std::to_string(v.i);
        case VK::Char: {
            // encode the unicode codepoint as UTF-8 (avoids MSVC wchar_t
            // warnings from wctomb).
            std::string s;
            uint32_t cp = (uint32_t)v.ch;
            if (cp < 0x80) {
                s.push_back((char)cp);
            } else if (cp < 0x800) {
                s.push_back((char)(0xC0 | (cp >> 6)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                s.push_back((char)(0xE0 | (cp >> 12)));
                s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            } else {
                s.push_back((char)(0xF0 | (cp >> 18)));
                s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                s.push_back((char)(0x80 | (cp & 0x3F)));
            }
            return s;
        }
        case VK::Float: {
            if (std::isnan(v.d)) return "NaN";
            if (std::isinf(v.d)) return v.d > 0 ? "inf" : "-inf";
            for (int prec = 15; prec <= 17; ++prec) {
                char buf[64];
                snprintf(buf, sizeof buf, "%.*g", prec, v.d);
                if (strtod(buf, nullptr) == v.d) {
                    std::string r(buf);
                    if (r.find('.') == std::string::npos &&
                        r.find('e') == std::string::npos)
                        r += ".0";
                    return r;
                }
            }
            return std::to_string(v.d);
        }
        case VK::Str:    return v.s;
        default:         return repr(v);
    }
}

std::string repr(const Value& v) {
    switch (v.k) {
        case VK::Str: return "'" + escapeStr(v.s) + "'";
        case VK::Bytes: {
            std::string o = "b'";
            for (unsigned char c : v.s) {
                if (c >= 0x20 && c < 0x7f && c != '\\' && c != '\'') {
                    o += (char)c;
                } else {
                    char b[6];
                    snprintf(b, sizeof b, "\\x%02x", c);
                    o += b;
                }
            }
            return o + "'";
        }
        case VK::List: {
            std::string o = "[";
            for (size_t i = 0; i < v.vec->size(); ++i)
                o += (i ? ", " : "") + repr((*v.vec)[i]);
            return o + "]";
        }
        case VK::Tuple: {
            std::string o = "(";
            for (size_t i = 0; i < v.vec->size(); ++i)
                o += (i ? ", " : "") + repr((*v.vec)[i]);
            if (v.vec->size() == 1) o += ",";
            return o + ")";
        }
        case VK::Set: {
            std::string o = "{";
            for (size_t i = 0; i < v.vec->size(); ++i)
                o += (i ? ", " : "") + repr((*v.vec)[i]);
            return o + "}";
        }
        case VK::Dict: {
            std::string o = "{";
            for (size_t i = 0; i < v.map->size(); ++i)
                o += (i ? ", " : "") + repr((*v.map)[i].first) + ": " +
                     repr((*v.map)[i].second);
            return o + "}";
        }
        case VK::Range:
            return std::to_string(v.lo) + (v.inclusive ? "..=" : "..") +
                   std::to_string(v.hi);
        case VK::Struct: {
            std::string o = v.typeName + "(";
            for (size_t i = 0; i < v.fields.size(); ++i)
                o += (i ? ", " : "") + v.fields[i].first + ": " +
                     repr(v.fields[i].second);
            return o + ")";
        }
        case VK::Heap: {
            std::string o = "new " + v.heap->typeName + "(";
            auto& f = v.heap->fields;
            for (size_t i = 0; i < f.size(); ++i)
                o += (i ? ", " : "") + f[i].first + ": " + repr(f[i].second);
            return o + ")";
        }
        case VK::EnumV: {
            std::string o = v.typeName + "." + v.variant;
            if (!v.payload.empty()) {
                o += "(";
                for (size_t i = 0; i < v.payload.size(); ++i)
                    o += (i ? ", " : "") + repr(v.payload[i]);
                o += ")";
            }
            return o;
        }
        case VK::Result:
            return v.variant == "ok" ? "ok(" + repr(v.payload[0]) + ")"
                                     : "err(" + repr(v.payload[0]) + ")";
        case VK::Gen: {
            std::string o = "gen(";
            for (size_t i = 0; i < v.vec->size(); ++i)
                o += (i ? ", " : "") + repr((*v.vec)[i]);
            return o + ")";
        }
        default: return toStr(v);
    }
}

bool truthy(const Value& v) {
    switch (v.k) {
        case VK::Bool:  return v.b;
        case VK::None:  return false;
        case VK::Int:   return v.i != 0;
        case VK::Float: return v.d != 0.0;
        default:        panicHere("condition is not a bool");
    }
}

// Human-readable name of a value's runtime type (for the `type()` builtin).
static std::string typeName(const Value& v) {
    switch (v.k) {
        case VK::None:   return "none";
        case VK::Bool:   return "bool";
        case VK::Int:    return "int";
        case VK::Float:  return "float";
        case VK::Str:    return "string";
        case VK::Bytes:  return "bytes";
        case VK::Char:   return "char";
        case VK::List:   return "list";
        case VK::Set:    return "set";
        case VK::Tuple:  return "tuple";
        case VK::Dict:   return "dict";
        case VK::Range:  return "range";
        case VK::Struct: return v.typeName;
        case VK::Heap:   return v.heap ? v.heap->typeName : "heap";
        case VK::EnumV:  return v.typeName;
        case VK::Result: return "result";
        case VK::Fn:     return "fn";
        case VK::Builtin:return "builtin";
        case VK::Gen:    return "generator";
        case VK::Chan:   return "chan";
        case VK::Module: return "module";
        case VK::Ptr:    return "ptr";
        default:         return "value";
    }
}

// Accumulate numeric addition (int + int -> int; float involved -> float).
static void numericAdd(Value& acc, const Value& item) {
    if (acc.k == VK::Int && item.k == VK::Int) {
        acc.i += item.i;
    } else if ((acc.k == VK::Int || acc.k == VK::Float) &&
               (item.k == VK::Int || item.k == VK::Float)) {
        acc.k = VK::Float;
        acc.d = (acc.k == VK::Int ? (double)acc.i : acc.d) +
                (item.k == VK::Int ? (double)item.i : item.d);
    } else {
        panicHere("sum(): elements are not all numbers");
    }
}

// ---------------------------------------------------------------------------
// channels
// ---------------------------------------------------------------------------

void ChanImpl::send(Value v) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return closed || q.size() < cap || cap == 0; });
    if (closed) panicHere("send on closed channel");
    q.push_back(std::move(v));
    cv.notify_all();
}

ChanImpl::Slot ChanImpl::recv() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return !q.empty() || closed; });
    if (q.empty()) return Slot{false, true, Value::none()};
    Slot s{true, false, std::move(q.front())};
    q.pop_front();
    cv.notify_all();
    return s;
}

ChanImpl::Slot ChanImpl::tryRecv() {
    std::lock_guard<std::mutex> lk(m);
    if (!q.empty()) {
        Slot s{true, false, std::move(q.front())};
        q.pop_front();
        cv.notify_all();
        return s;
    }
    return Slot{false, closed, Value::none()};
}

void ChanImpl::close() {
    { std::lock_guard<std::mutex> lk(m); closed = true; }
    cv.notify_all();
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

Interpreter::Interpreter(const Stmt& program) {
    globals_ = makeChild(nullptr);
    installBuiltins();
    program_ = &program;
}

Interpreter::~Interpreter() { shutdownThreads(); }

Value Interpreter::run() {
    // deferred so hosts can configure module search paths between
    // construction and execution (imports run during collection)
    if (!collected_) {
        collected_ = true;
        collectProgram(*program_);
    }
    auto it = funcs_.find("main");
    if (it == funcs_.end()) panicHere("no main() defined");
    CallFrameGuard mainGuard(it->second->name, it->second->span.line,
                             it->second->span.col);
    Value r = runFunc(it->second, {}, {}, globals_);
    shutdownThreads();
    return r;
}

void Interpreter::collectProgram(const Stmt& program) {
    for (const auto& sp : program.body) {
        const Stmt& s = *sp;
        switch (s.kind) {
            case ast::StKind::FuncDef:
                if (!s.externDef) funcs_[s.name] = &s;
                break;
            case ast::StKind::StructDef: structs_[s.name] = &s; break;
            case ast::StKind::EnumDef:   enums_[s.name] = &s;   break;
            case ast::StKind::TraitDef:  traits_[s.name] = &s;  break;
            case ast::StKind::ImplDef: {
                ImplEntry ent;
                ent.traitName = s.implTrait ? s.implTrait->name : "";
                ent.typeName  = s.implType ? s.implType->name : "";
                for (const auto& m : s.body)
                    if (m->kind == ast::StKind::FuncDef)
                        ent.methods[m->name] = m.get();
                impls_.push_back(std::move(ent));
                break;
            }
            case ast::StKind::ConstDecl:
            case ast::StKind::VarDecl:
                globals_->vars[s.target->text] =
                    s.value ? eval(*s.value, globals_) : Value::none();
                break;
            case ast::StKind::Import: {
                if (!s.fromImport) {
                    // real module file (project package or stdlib) first
                    Value real = loadModuleFile(s.moduleName);
                    if (real.k != VK::None) {
                        // unaliased imports bind the last path segment
                        // ("util.co" -> util, "a/b.co" -> b)
                        std::string bind = s.importAlias;
                        if (bind.empty()) {
                            std::string mn = s.moduleName;
                            if (mn.size() > 3 &&
                                mn.compare(mn.size() - 3, 3, ".co") == 0)
                                mn.erase(mn.size() - 3);
                            size_t cut = mn.find_last_of("/.");
                            bind = cut == std::string::npos
                                       ? mn
                                       : mn.substr(cut + 1);
                        }
                        globals_->vars[bind] = real;
                    } else {
                        auto parts = splitDots(s.moduleName);
                        globals_->vars[s.importAlias.empty() ? parts[0]
                                                             : s.importAlias] =
                            Value::module(parts[0]);
                    }
                } else {
                    Value mod = resolveModulePath(s.moduleName);
                    for (const auto& item : s.importItems) {
                        const std::string& bind =
                            item.alias.empty() ? item.name : item.alias;
                        globals_->vars[bind] =
                            moduleMember(mod.typeName, item.name);
                    }
                }
                break;
            }
            case ast::StKind::TempDecl:
            case ast::StKind::BucketDecl:
            case ast::StKind::LocalDecl:
            case ast::StKind::GlobalDecl:
            case ast::StKind::Del:
            case ast::StKind::Yield:
                if (s.kind == ast::StKind::Yield)
                    panicHere("'yield' outside of a generator function");
                else
                    exec(s, globals_);
                break;
            default: break;
        }
    }
    mergeInheritance();
}

// Materialize single-inheritance: for every StructDef whose `baseName` is set,
// replace its stored node with one carrying the base's fields + methods merged
// in (derived methods override base methods of the same name; derived fields
// override base fields of the same name). Runs after structs_ is populated.
void Interpreter::mergeInheritance() {
    inheritedStructNodes_.clear();

    for (const auto& child : structs_) {
        const Stmt& s = *child.second;
        if (s.baseName.empty()) continue;
        // Resolve the nearest fully-merged *base* by following the chain.
        std::function<const Stmt*(const std::string&)> ancestor =
            [&](const std::string& name) -> const Stmt* {
            auto sit = structs_.find(name);
            if (sit == structs_.end()) return &s;       // base missing: keep as-is
            const Stmt& sd = *sit->second;
            if (sd.baseName.empty()) return &sd;        // fully-resolved base
            return ancestor(sd.baseName);               // resolve ancestor first
        };
        const Stmt* base = ancestor(s.baseName);
        if (!base || base == &s) continue;              // cycle/self: avoid infinite

        auto merged = cloneStmt(&s);                    // start from derived node
        // Fields: base fields first (skipping any overridden by derived), then
        // derived fields (derived name wins for shadowing).
        std::vector<ast::FieldDecl> workFields;
        for (const auto& bf : base->fields) {
            bool overridden = false;
            for (const auto& df : s.fields)
                if (df.name == bf.name) { overridden = true; break; }
            if (!overridden) workFields.push_back(cloneField(bf));
        }
        for (const auto& df : s.fields) workFields.push_back(cloneField(df));
        merged->fields = std::move(workFields);
        // Methods: base methods (excluding names overridden by derived), then
        // derived methods (derived/o virtual dispatch wins).
        std::vector<std::string> derivedNames;
        for (const auto& dm : s.body)
            if (dm->kind == ast::StKind::FuncDef) derivedNames.push_back(dm->name);
        std::vector<ast::StmtP> workBody;
        for (const auto& bm : base->body) {
            if (bm->kind != ast::StKind::FuncDef) continue;
            bool overridden = false;
            for (const auto& dn : derivedNames)
                if (dn == bm->name) { overridden = true; break; }
            if (!overridden) workBody.push_back(cloneStmt(bm.get()));
        }
        for (const auto& dm : s.body) workBody.push_back(cloneStmt(dm.get()));
        merged->body = std::move(workBody);
        merged->baseName.clear();                       // now effective
        inheritedStructNodes_.push_back(std::move(merged));
        structs_[child.first] = inheritedStructNodes_.back().get();
    }
}

// ---------------------------------------------------------------------------
// builtins & pseudo-modules
// ---------------------------------------------------------------------------

static Value biFn(std::vector<std::string> params, BuiltinFn fn) {
    Value v;
    v.k = VK::Builtin;
    v.biParams = std::move(params);
    v.bi = std::move(fn);
    return v;
}

void Interpreter::installBuiltins() {
    globals_->vars["print"] = biFn({}, [](std::vector<Value>& a) -> Value {
        std::string out;
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) out += ' ';
            out += toStr(a[i]);
        }
        std::cout << out << '\n';
        return Value::none();
    });

    // keyword/named constants used as plain identifiers
    globals_->vars["nil"] = Value::none();
    globals_->vars["NaN"] = Value::floating(NAN);
    globals_->vars["inf"] = Value::floating(INFINITY);

    globals_->vars["len"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        switch (v.k) {
            case VK::Str:
            case VK::Bytes:
                return Value::integer((int64_t)v.s.size());
            case VK::List:
            case VK::Tuple:
            case VK::Set:
                return Value::integer((int64_t)v.vec->size());
            case VK::Dict:
                return Value::integer((int64_t)v.map->size());
            case VK::Range: {
                int64_t hi = v.inclusive ? v.hi + 1 : v.hi;
                return Value::integer(hi > v.lo ? hi - v.lo : 0);
            }
            default: panicHere("len() unsupported operand");
        }
    });

    globals_->vars["sqrt"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::floating(std::sqrt(d));
    });

    globals_->vars["ord"] = biFn({"c"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Char) return Value::integer((int64_t)a[0].ch);
        if (a[0].k == VK::Str && !a[0].s.empty())
            return Value::integer((int64_t)(unsigned char)a[0].s[0]);
        panicHere("ord() expects a char");
    });
    globals_->vars["chr"] = biFn({"n"}, [](std::vector<Value>& a) -> Value {
        int64_t n = a[0].k == VK::Int ? a[0].i : 0;
        return Value::chr((char32_t)n);
    });

    globals_->vars["assert"] =
        biFn({"cond"}, [](std::vector<Value>& a) -> Value {
            if (a[0].k != VK::Bool || !a[0].b) panicHere("assertion failed");
            return Value::none();
        });
    globals_->vars["assert_eq"] =
        biFn({"a", "b"}, [this](std::vector<Value>& a) -> Value {
            if (!valuesEqual(a[0], a[1]))
                panicHere("assertion failed: " + toStr(a[0]) + " != " +
                          toStr(a[1]));
            return Value::none();
        });

    globals_->vars["range"] = biFn({"start", "stop"},
                                   [](std::vector<Value>& a) -> Value {
                                       if (a.size() == 1)
                                           return Value::rangeV(0, a[0].i, false);
                                       return Value::rangeV(a[0].i, a[1].i, false);
                                   });

    globals_->vars["panic"] = biFn({"msg"}, [](std::vector<Value>& a) -> Value {
        panicHere(toStr(a[0]));
    });

    globals_->vars["catch_panic"] = biFn({"fn"}, [this](std::vector<Value>& a) -> Value {
        try {
            Value r = callValue(a[0], {}, 0, 0);
            return Value::resultOk(std::move(r));
        } catch (const PanicSignal& p) {
            return Value::resultErr(Value::str(p.msg));
        }
    });

    // extern printf/strlen resolve here (extern FuncDefs are never registered)
    globals_->vars["printf"] = biFn({"fmt"}, [](std::vector<Value>& a) -> Value {
        const Value& fv = a[0];
        std::string fmt = fv.k == VK::Ptr || fv.k == VK::Str ? fv.s : toStr(fv);
        std::string out;
        size_t argi = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i]; continue; }
            if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += '%'; ++i; continue; }
            size_t j = i + 1;
            while (j < fmt.size() &&
                   !strchr("sdifuzgcxX", fmt[j]))
                ++j;
            if (j >= fmt.size()) { out += fmt[i]; break; }
            std::string spec = fmt.substr(i + 1, j - i - 1);
            char conv = fmt[j];
            i = j;
            const Value& av = argi < a.size() ? a[argi++] : Value::none();
            if (conv == 's' || conv == 'c') {
                out += av.k == VK::Ptr ? av.s : toStr(av);
            } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'z' ||
                       conv == 'x' || conv == 'X') {
                long long n = av.k == VK::Int ? av.i : 0;
                char buf[64];
                snprintf(buf, sizeof buf, ("%ll" + spec + "d").c_str(), n);
                out += buf;
            } else if (conv == 'g' || conv == 'f') {
                double d = av.k == VK::Float ? av.d
                           : av.k == VK::Int ? (double)av.i : 0.0;
                char buf[64];
                snprintf(buf, sizeof buf, ("%" + spec + conv).c_str(), d);
                out += buf;
            }
        }
        fputs(out.c_str(), stdout);
        return Value::integer((int64_t)out.size());
    });

    globals_->vars["strlen"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        const Value& p = a[0];
        return Value::integer(p.k == VK::Ptr ? (int64_t)strlen(p.s.c_str())
                                             : (int64_t)p.s.size());
    });

    // ---- WHY-1 / SP-8: batteries-included conversion & type builtins --------
    globals_->vars["str"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(toStr(a[0]));
    });
    globals_->vars["int"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.k == VK::Int) return v;
        if (v.k == VK::Float) return Value::integer((int64_t)v.d);
        if (v.k == VK::Bool) return Value::integer(v.b ? 1 : 0);
        if (v.k == VK::Char) return Value::integer((int64_t)v.ch);
        if (v.k == VK::Str) {
            try { return Value::integer(std::stoll(v.s)); }
            catch (...) { panicHere("int(): cannot parse '" + v.s + "'"); }
        }
        panicHere("int(): unsupported operand");
    });
    globals_->vars["float"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.k == VK::Float) return v;
        if (v.k == VK::Int) return Value::floating((double)v.i);
        if (v.k == VK::Bool) return Value::floating(v.b ? 1.0 : 0.0);
        if (v.k == VK::Str) {
            try { return Value::floating(std::stod(v.s)); }
            catch (...) { panicHere("float(): cannot parse '" + v.s + "'"); }
        }
        panicHere("float(): unsupported operand");
    });
    globals_->vars["bool"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::boolean(truthy(a[0]));
    });
    globals_->vars["type"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(typeName(a[0]));
    });

    // repr(x): debug/typed string form (quotes strings, shows containers/fields).
    globals_->vars["repr"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(repr(a[0]));
    });

    // ---- WHY-1 / SP-8: collection & iteration builtins ----------------------
    globals_->vars["sum"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        Value acc = Value::integer(0);
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            numericAdd(acc, item);
            return true;
        }, globals_);
        return acc;
    });
    globals_->vars["min"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        bool first = true; Value best;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            if (first) { best = item; first = false; }
            else if (valLess(item, best)) best = item;
            return true;
        }, globals_);
        if (first) panicHere("min(): empty sequence");
        return best;
    });
    globals_->vars["max"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        bool first = true; Value best;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            if (first) { best = item; first = false; }
            else if (valLess(best, item)) best = item;
            return true;
        }, globals_);
        if (first) panicHere("max(): empty sequence");
        return best;
    });
    globals_->vars["any"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        bool r = false;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            if (truthy(item)) { r = true; return false; }
            return true;
        }, globals_);
        return Value::boolean(r);
    });
    globals_->vars["all"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        bool r = true;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            if (!truthy(item)) { r = false; return false; }
            return true;
        }, globals_);
        return Value::boolean(r);
    });
    globals_->vars["sorted"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        std::vector<Value> out;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            out.push_back(item); return true;
        }, globals_);
        std::stable_sort(out.begin(), out.end(), [](const Value& x, const Value& y) {
            return valLess(x, y);
        });
        return Value::list(std::move(out));
    });
    globals_->vars["reversed"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        std::vector<Value> out;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            out.push_back(item); return true;
        }, globals_);
        std::reverse(out.begin(), out.end());
        return Value::list(std::move(out));
    });
    globals_->vars["enumerate"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& xs = a[0];
        std::vector<Value> out;
        int64_t i = 0;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            out.push_back(Value::tuple({Value::integer(i++), item}));
            return true;
        }, globals_);
        return Value::list(std::move(out));
    });
    globals_->vars["map"] = biFn({"f", "xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& f = a[0];
        const Value& xs = a[1];
        std::vector<Value> out;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            out.push_back(callValue(f, {item}, 0, 0));
            return true;
        }, globals_);
        return Value::list(std::move(out));
    });
    globals_->vars["filter"] = biFn({"f", "xs"}, [this](std::vector<Value>& a) -> Value {
        const Value& f = a[0];
        const Value& xs = a[1];
        std::vector<Value> out;
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            if (truthy(callValue(f, {item}, 0, 0))) out.push_back(item);
            return true;
        }, globals_);
        return Value::list(std::move(out));
    });
    globals_->vars["reduce"] = biFn({"f", "xs", "init"}, [this](std::vector<Value>& a) -> Value {
        const Value& f = a[0];
        const Value& xs = a[1];
        Value acc = a[2];
        iterateSeq(xs, [&](Value& item, Env&) -> bool {
            acc = callValue(f, {acc, item}, 0, 0);
            return true;
        }, globals_);
        return acc;
    });

    // ---- WHY-1 / SP-8: string operations -----------------------------------
    globals_->vars["upper"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        std::string out = a[0].s;
        for (auto& c : out) c = (char)std::toupper((unsigned char)c);
        return Value::str(out);
    });
    globals_->vars["lower"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        std::string out = a[0].s;
        for (auto& c : out) c = (char)std::tolower((unsigned char)c);
        return Value::str(out);
    });
    globals_->vars["trim"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        const std::string& s = a[0].s;
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return Value::str("");
        size_t e = s.find_last_not_of(" \t\r\n");
        return Value::str(s.substr(b, e - b + 1));
    });
    globals_->vars["contains"] = biFn({"s", "sub"}, [](std::vector<Value>& a) -> Value {
        return Value::boolean(a[0].s.find(a[1].s) != std::string::npos);
    });
    globals_->vars["starts_with"] = biFn({"s", "prefix"}, [](std::vector<Value>& a) -> Value {
        const std::string& s = a[0].s;
        const std::string& p = a[1].s;
        return Value::boolean(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
    });
    globals_->vars["ends_with"] = biFn({"s", "suffix"}, [](std::vector<Value>& a) -> Value {
        const std::string& s = a[0].s;
        const std::string& p = a[1].s;
        return Value::boolean(s.size() >= p.size() &&
                              s.compare(s.size() - p.size(), p.size(), p) == 0);
    });
    globals_->vars["replace"] = biFn({"s", "from", "to"}, [](std::vector<Value>& a) -> Value {
        std::string s = a[0].s;
        const std::string& f = a[1].s;
        const std::string& t = a[2].s;
        if (f.empty()) return Value::str(s);
        size_t pos = 0;
        while ((pos = s.find(f, pos)) != std::string::npos) {
            s.replace(pos, f.size(), t);
            pos += t.size();
        }
        return Value::str(s);
    });
    globals_->vars["split"] = biFn({"s", "sep"}, [](std::vector<Value>& a) -> Value {
        const std::string& s = a[0].s;
        const std::string& sep = a[1].s;
        std::vector<Value> out;
        if (sep.empty()) {
            for (char c : s) out.push_back(Value::str(std::string(1, c)));
            return Value::list(std::move(out));
        }
        size_t pos = 0, start = 0;
        while ((pos = s.find(sep, start)) != std::string::npos) {
            out.push_back(Value::str(s.substr(start, pos - start)));
            start = pos + sep.size();
        }
        out.push_back(Value::str(s.substr(start)));
        return Value::list(std::move(out));
    });
    globals_->vars["join"] = biFn({"sep", "xs"}, [this](std::vector<Value>& a) -> Value {
        const std::string& sep = a[0].s;
        std::string out;
        bool first = true;
        iterateSeq(a[1], [&](Value& item, Env&) -> bool {
            if (!first) out += sep;
            out += toStr(item);
            first = false;
            return true;
        }, globals_);
        return Value::str(out);
    });

    // pseudo-modules are prebound; explicit imports rebind the same values
    for (const char* m :
         {"math", "time", "io", "mem", "json", "text", "os"})
        globals_->vars[m] = Value::module(m);
}

Value Interpreter::resolveModulePath(const std::string& dotted) {
    // real module files win over native pseudo-module stubs
    Value real = loadModuleFile(dotted);
    if (real.k != VK::None) return real;

    auto parts = splitDots(dotted);
    Value cur = Value::module(parts[0]);
    std::string acc = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        Value nxt = moduleMember(acc, parts[i]);
        if (nxt.k != VK::Module) panicHere("'" + parts[i] + "' is not a module");
        acc += "." + parts[i];
        cur = nxt;
        cur.typeName = acc;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// module loader: dotted/slash import paths -> <dir>/<path>.co source files
// ---------------------------------------------------------------------------

static bool readFileIfExists(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// entry-file convention inside an installed package directory:
//   coco.toml [package] main = "..."
//     -> pin.co (package initializer / public-API aggregator)
//     -> mod.co -> <dirname>.co -> lone *.co
static bool resolvePackageEntry(const std::string& dir, std::string& out) {
    std::string manifest;
    if (readFileIfExists(dir + "/coco.toml", manifest)) {
        kirn::tomlmini::Doc doc = kirn::tomlmini::parse(manifest);
        std::string mainf = kirn::tomlmini::get(doc, "package", "main");
        std::string probe;
        if (!mainf.empty() &&
            readFileIfExists(dir + "/" + mainf, probe)) {
            out = dir + "/" + mainf;
            return true;
        }
    }
    const char* defaults[] = {"/pin.co", "/code/pin.co", "/mod.co",
                              "/code/mod.co"};
    for (const char* d : defaults) {
        std::string probe;
        if (readFileIfExists(dir + d, probe)) {
            out = dir + d;
            return true;
        }
    }
    // <dirname>.co then a single top-level source file
    std::string base = dir;
    size_t cut = base.find_last_of("/\\");
    if (cut != std::string::npos) base = base.substr(cut + 1);
    std::string probe;
    std::vector<std::string> tops;
    if (readFileIfExists(dir + "/" + base + ".co", probe))
        tops.push_back(dir + "/" + base + ".co");
    if (tops.empty()) {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec), end;
        if (!ec)
            for (; it != end; it.increment(ec))
                if (!ec && it->is_regular_file(ec) &&
                    it->path().extension() == ".co")
                    tops.push_back(it->path().string());
    }
    if (tops.size() == 1) {
        out = tops[0];
        return true;
    }
    return false;
}

Value Interpreter::loadModuleFile(const std::string& dottedRaw) {
    // accept explicit ".co" suffixes (import "file1.co")
    std::string dotted = dottedRaw;
    if (dotted.size() > 3 &&
        dotted.compare(dotted.size() - 3, 3, ".co") == 0)
        dotted.erase(dotted.size() - 3);
    auto hit = loadedModules_.find(dotted);
    if (hit != loadedModules_.end()) {
        Value m = Value::module(dotted);
        m.typeName = "__loaded__:" + dotted;
        return m;
    }

    // import paths may use '.' or '/' separators (stdlib vs github style)
    std::string rel;
    for (char c : dotted) rel += (c == '.' || c == '/') ? '/' : c;
    std::string key = rel;                 // embedded-source lookup key
    rel += ".co";

    std::string found, src;
    auto emb = embeddedSources_.find(key);
    if (emb != embeddedSources_.end()) {
        src = emb->second;
    } else {
        for (const auto& dir : stdlibDirs_) {
            std::string cand = dir + "/" + rel;
            if (readFileIfExists(cand, src)) {
                found = cand;
                break;
            }
            // package directory (installed under coco_libs/): use entry file
            std::string pkgDir =
                dir + "/" + rel.substr(0, rel.size() - 3);   // strip ".co"
            if (!pkgDir.empty() && std::filesystem::is_directory(pkgDir) &&
                resolvePackageEntry(pkgDir, found)) {
                readFileIfExists(found, src);
                break;
            }
        }
        if (found.empty()) {
            Value missing;
            missing.k = VK::None;      // sentinel: not a file-backed module
            return missing;
        }
    }

    DiagEngine diags;
    auto toks = Lexer(src, found, diags).lexAll();
    std::vector<ast::StmtP> body;
    if (diags.errorCount() == 0) body = Parser(toks, diags).parseProgram();
    if (diags.errorCount() == 0) {
        sema::Checker chk(diags);
        chk.checkModule(body);
    }
    if (diags.errorCount() != 0) {
        for (const auto& d : diags.diags())
            if (d.sev == Sev::Error || d.sev == Sev::InternalError)
                std::cerr << found << ":" << d.line << ":" << d.col
                          << ": error: " << d.message << "\n";
        panicHere("module '" + dotted + "' failed to compile");
    }

    // keep the AST alive for the lifetime of the interpreter: bound function
    // closures point into it
    auto& prog = moduleAstCache_[dotted];
    prog = std::move(body);

    Env modEnv = makeChild(globals_);   // builtins visible inside the module
    for (const auto& sp : prog) {
        const Stmt& s = *sp;
        switch (s.kind) {
            case ast::StKind::FuncDef: {
                if (s.externDef) break;
                const Stmt* fn = &s;
                Value v;
                v.k = VK::Builtin;
                v.bi = [this, fn, modEnv](std::vector<Value>& a) -> Value {
                    CallFrameGuard guard(fn->name, fn->span.line, fn->span.col);
                    return runFunc(fn, a, {}, modEnv, nullptr);
                };
                modEnv->vars[s.name] = v;
                break;
            }
            case ast::StKind::ConstDecl:
            case ast::StKind::VarDecl:
                modEnv->vars[s.target->text] =
                    s.value ? eval(*s.value, modEnv) : Value::none();
                break;
            case ast::StKind::Assign:
            case ast::StKind::AugAssign:
            case ast::StKind::ExprStmt:
                exec(s, modEnv);
                break;
            case ast::StKind::StructDef:
                structs_[s.name] = &s;
                // export `pub struct`/`pub class` through the module namespace
                // so consumers can reference `mod.Type` and construct it.
                if (s.pub && !s.privateDecl)
                    modEnv->vars[s.name] = Value::module("__struct__:" + s.name);
                break;
            case ast::StKind::EnumDef:
                enums_[s.name] = &s;
                if (s.pub && !s.privateDecl)
                    modEnv->vars[s.name] = Value::module("__enum__:" + s.name);
                break;
            case ast::StKind::TraitDef:
                traits_[s.name] = &s;
                if (s.pub && !s.privateDecl)
                    modEnv->vars[s.name] = Value::module("__trait__:" + s.name);
                break;
            case ast::StKind::Import: {
                // modules importing other modules bind into their own namespace
                if (!s.fromImport) {
                    Value dep = loadModuleFile(s.moduleName);
                    if (dep.k != VK::None)
                        modEnv->vars[s.importAlias.empty() ? s.moduleName
                                                           : s.importAlias] =
                            dep;
                    else if (s.importAlias.empty())
                        modEnv->vars[s.moduleName] =
                            Value::module(s.moduleName);
                } else {
                    Value dep = resolveModulePath(s.moduleName);
                    for (const auto& item : s.importItems) {
                        const std::string& bind =
                            item.alias.empty() ? item.name : item.alias;
                        if (dep.typeName.rfind("__loaded__:", 0) == 0) {
                            Value mv = moduleMember(dep.typeName, item.name);
                            modEnv->vars[bind] = mv;
                        } else {
                            modEnv->vars[bind] =
                                moduleMember(dep.typeName, item.name);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    loadedModules_[dotted] = modEnv;
    Value m = Value::module(dotted);
    m.typeName = "__loaded__:" + dotted;
    return m;
}

static std::string slugifyStr(const std::string& in) {
    std::string out;
    bool hyphen = false;
    for (char c : in) {
        unsigned char u = (unsigned char)c;
        if (isalnum(u)) {
            if (hyphen && !out.empty()) out += '-';
            hyphen = false;
            out += (char)tolower(u);
        } else if (!out.empty()) {
            hyphen = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

static void dumpJson(const Value& v, std::string& out) {
    switch (v.k) {
        case VK::None:  out += "null"; break;
        case VK::Bool:  out += v.b ? "true" : "false"; break;
        case VK::Int:   out += std::to_string(v.i); break;
        case VK::Float: out += toStr(v); break;
        case VK::Str: {
            out += '"';
            for (char c : v.s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    default:   out += c;
                }
            }
            out += '"';
            break;
        }
        case VK::List:
        case VK::Tuple: {
            out += '[';
            for (size_t i = 0; i < v.vec->size(); ++i) {
                if (i) out += ", ";
                dumpJson((*v.vec)[i], out);
            }
            out += ']';
            break;
        }
        case VK::Dict: {
            out += '{';
            for (size_t i = 0; i < v.map->size(); ++i) {
                if (i) out += ", ";
                dumpJson((*v.map)[i].first, out);
                out += ": ";
                dumpJson((*v.map)[i].second, out);
            }
            out += '}';
            break;
        }
        default: out += "null";
    }
}

Value Interpreter::moduleMember(const std::string& mod, const std::string& name) {
    // file-backed modules: look the member up in the module's namespace
    if (mod.rfind("__loaded__:", 0) == 0) {
        std::string dotted = mod.substr(11);
        auto it = loadedModules_.find(dotted);
        if (it == loadedModules_.end())
            panicHere("module '" + dotted + "' is not loaded");
        const Value* mv = it->second->find(name);
        if (!mv || name.empty() || name[0] == '_')
            panicHere("module '" + dotted + "' has no exported member '" +
                      name + "'");
        return *mv;
    }

    if (mod == "math") {
        if (name == "pi") return Value::floating(3.14159265358979323846);
        if (name == "e")  return Value::floating(2.71828182845904523536);
        double (*impl)(double) = nullptr;
        if      (name == "sqrt")  impl = &std::sqrt;
        else if (name == "sin")   impl = &std::sin;
        else if (name == "cos")   impl = &std::cos;
        else if (name == "tan")   impl = &std::tan;
        else if (name == "floor") impl = &std::floor;
        else if (name == "ceil")  impl = &std::ceil;
        else if (name == "log")   impl = &std::log;
        else if (name == "exp")   impl = &std::exp;
        if (impl)
            return biFn({"x"}, [impl](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                return Value::floating(impl(d));
            });
        if (name == "abs")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                if (a[0].k == VK::Int)
                    return Value::integer(a[0].i < 0 ? -a[0].i : a[0].i);
                return Value::floating(std::fabs(a[0].d));
            });
        panicHere("math has no member '" + name + "'");
    }

    if (mod == "time") {
        if (name == "sleep")
            return biFn({"ms"}, [](std::vector<Value>& a) -> Value {
                int64_t ms = a.empty() || a[0].k != VK::Int ? 0 : a[0].i;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return Value::none();
            });
        if (name == "after")
            return biFn({"ms"}, [](std::vector<Value>& a) -> Value {
                Value t;
                t.k = VK::Timer;
                t.deadlineMs = nowMs() + (a[0].k == VK::Int ? a[0].i : 0);
                return t;
            });
        if (name == "now")
            return biFn({}, [](std::vector<Value>&) -> Value {
                return Value::integer(nowMs());
            });
        panicHere("time has no member '" + name + "'");
    }

    if (mod == "io") {
        if (name == "open")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                Value f;
                f.k = VK::File;
                f.s = toStr(a[0]);
                return f;
            });
        panicHere("io has no member '" + name + "'");
    }

    if (mod == "mem") {
        if (name == "Arena")
            return biFn({}, [](std::vector<Value>&) -> Value {
                Value a;
                a.k = VK::Arena;
                return a;
            });
        panicHere("mem has no member '" + name + "'");
    }

    if (mod == "json" && name == "dumps")
        return biFn({"value"}, [](std::vector<Value>& a) -> Value {
            std::string out;
            dumpJson(a[0], out);
            return Value::str(out);
        });

    if (mod == "text" && name == "slug") return Value::module("text.slug");

    if (mod == "text.slug" && name == "slugify")
        return biFn({"text"}, [](std::vector<Value>& a) -> Value {
            return Value::str(slugifyStr(toStr(a[0])));
        });

    if (mod == "os") {
        if (name == "exit")
            return biFn({"code"}, [](std::vector<Value>& a) -> Value {
                std::exit(a[0].k == VK::Int ? (int)a[0].i : 0);
            });
        if (name == "args") {
            Value list = Value::list();
            for (const std::string& a : argv_) list.vec->push_back(Value::str(a));
            return biFn({}, [list](std::vector<Value>&) -> Value {
                return list;
            });
        }
        if (name == "getenv")
            return biFn({"name"}, [](std::vector<Value>& a) -> Value {
                const char* v = envGet(toStr(a[0]).c_str());
                return v ? Value::str(v) : Value::none();
            });
        panicHere("os has no member '" + name + "'");
    }

    panicHere("unknown module '" + mod + "'");
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------

static bool isResultRet(const Stmt& fn) {
    return fn.ret && fn.ret->kind == ast::TyKind::Name && fn.ret->name == "result";
}

void Interpreter::execBlock(const std::vector<ast::StmtP>& body, Env env) {
    // Fast path: no Label statements in this block, execute straight through.
    bool hasLabel = false;
    for (const auto& st : body)
        if (st->kind == ast::StKind::Label) { hasLabel = true; break; }
    if (!hasLabel) {
        for (const auto& st : body) exec(*st, env);
        return;
    }
    // Label-aware path: build label -> statement index, then run with jumps.
    std::map<std::string, size_t> labels;
    for (size_t i = 0; i < body.size(); ++i)
        if (body[i]->kind == ast::StKind::Label) {
            const std::string& name = body[i]->label;
            if (labels.count(name)) {
                panicHere("label '" + name + "' defined more than once");
            }
            labels[name] = i;
        }
    size_t i = 0;
    while (i < body.size()) {
        try {
            exec(*body[i], env);
            ++i;
        } catch (SignalGoto& g) {
            auto it = labels.find(g.label);
            if (it == labels.end()) throw;        // not ours → unwind outward
            i = it->second;                       // land on the labeled statement
        }
    }
}

void Interpreter::exec(const Stmt& s, Env env) {
    switch (s.kind) {
        case ast::StKind::Pass:
        case ast::StKind::Export:
            return;

        case ast::StKind::FuncDef: {
            Value f;
            f.k = VK::Fn;
            f.fn = &s;
            f.env = env;
            env->vars[s.name] = std::move(f);
            return;
        }

        case ast::StKind::ConstDecl:
        case ast::StKind::VarDecl: {
            Value v = s.value ? eval(*s.value, env) : Value::none();
            // a declaration ALWAYS creates a new binding in the current frame
            // (shadowing) instead of assigning into an outer binding of the
            // same name — lexical scope semantics, mirrors the checker.
            bindDecl(*s.target, std::move(v), env);
            return;
        }

        case ast::StKind::LocalDecl: {
            // force a NEW binding in the current frame (shadow allowed despite
            // `local` being strict about same-scope re-decl at check time).
            Value v = s.value ? eval(*s.value, env) : Value::none();
            if (!s.target || s.target->kind != ast::ExKind::Ident) return;
            env->vars[s.target->text] = std::move(v);
            return;
        }

        case ast::StKind::GlobalDecl: {
            // write/read the module top-level binding.
            Value v = s.value ? eval(*s.value, env) : Value::none();
            if (!s.target || s.target->kind != ast::ExKind::Ident) return;
            EnvS* root = env->root();
            if (s.value) root->vars[s.target->text] = std::move(v);
            return;
        }

        case ast::StKind::TempDecl: {
            // like a normal var decl, but carries a compile-time use budget
            // (enforced in the checker); runtime treats it as an ordinary binding.
            Value v = s.value ? eval(*s.value, env) : Value::none();
            if (!s.tempVarName.empty()) env->vars[s.tempVarName] = std::move(v);
            return;
        }

        case ast::StKind::BucketDecl: {
            const std::string& n = s.tempVarName;
            if (s.bucketRelease) {
                // move a parked value back to a normal variable.
                auto it = bucket_.find(n);
                if (it == bucket_.end()) panicHere("bucket: '" + n + "' is not parked");
                env->vars[n] = std::move(it->second);
                bucket_.erase(it);
            } else {
                // park: store in the bucket store, remove from normal scope.
                Value v = s.value ? eval(*s.value, env) : Value::none();
                env->unbind(n);
                bucket_[n] = std::move(v);
            }
            return;
        }

        case ast::StKind::Del: {
            if (s.exprs.empty()) return;
            const Expr& tgt = *s.exprs[0];
            switch (tgt.kind) {
                case ast::ExKind::Ident:
                    if (!env->unbind(tgt.text))
                        panicHere("del: undefined variable '" + tgt.text + "'");
                    return;
                case ast::ExKind::Index: {
                    Value cont = eval(*tgt.lhs, env);
                    if (cont.k == VK::Weak) cont = lockHeap(cont);
                    Value idx = eval(*tgt.rhs, env);
                    if (cont.k == VK::Dict) {
                        for (size_t i = 0; i < cont.map->size(); ++i)
                            if (valEq((*cont.map)[i].first, idx)) {
                                cont.map->erase(cont.map->begin() + (long)i);
                                return;
                            }
                        panicHere("del: dict has no key");
                    }
                    if (cont.k == VK::List || cont.k == VK::Tuple) {
                        size_t k = normIndex((int64_t)cont.vec->size(), idx.i);
                        cont.vec->erase(cont.vec->begin() + (long)k);
                        return;
                    }
                    panicHere("del: unsupported index target");
                }
                case ast::ExKind::Member: {
                    Value base = eval(*tgt.lhs, env);
                    if (base.k == VK::Weak) base = lockHeap(base);
                    const std::string& fn = tgt.text;
                    if (base.k == VK::Struct) {
                        for (auto it = base.fields.begin(); it != base.fields.end(); ++it)
                            if (it->first == fn) { base.fields.erase(it); return; }
                    } else if (base.k == VK::Heap) {
                        for (auto it = base.heap->fields.begin(); it != base.heap->fields.end(); ++it)
                            if (it->first == fn) { base.heap->fields.erase(it); return; }
                    }
                    panicHere("del: struct has no field '" + fn + "'");
                }
                default:
                    panicHere("del: unsupported target");
            }
        }

        case ast::StKind::Yield:
            if (yieldSinks_.empty())
                panicHere("'yield' outside of a generator function");
            yieldSinks_.back()->push_back(
                !s.exprs.empty() ? eval(*s.exprs[0], env) : Value::none());
            return;

        case ast::StKind::StructDef: {
            structs_[s.name] = &s;
            env->vars[s.name] = Value::module("__struct__:" + s.name);
            return;
        }

        case ast::StKind::EnumDef: {
            enums_[s.name] = &s;
            env->vars[s.name] = Value::module("__enum__:" + s.name);
            return;
        }

        case ast::StKind::TraitDef:
            traits_[s.name] = &s;
            return;

        case ast::StKind::ExprStmt:
            for (const auto& e : s.exprs) eval(*e, env);
            return;

        case ast::StKind::Assign: {
            size_t n = s.exprs.size();
            size_t nt = (n % 2 == 0) ? n / 2 : n - 1;
            std::vector<Value> vals;
            for (size_t i = nt; i < n; ++i) vals.push_back(eval(*s.exprs[i], env));
            if (n % 2 != 0 && nt > 1) {
                const Value& src = vals[0];
                if ((src.k == VK::Tuple || src.k == VK::List) &&
                    src.vec->size() == nt) {
                    for (size_t i = 0; i < nt; ++i)
                        assignTo(*s.exprs[i], (*src.vec)[i], env);
                    return;
                }
                panicHere("cannot destructure assignment value");
            }
            if (vals.size() != nt) panicHere("assignment target/value mismatch");
            for (size_t i = 0; i < nt; ++i) assignTo(*s.exprs[i], vals[i], env);
            return;
        }

        case ast::StKind::AugAssign: {
            Value cur = eval(*s.exprs[0], env);
            Value rhs = eval(*s.exprs[1], env);
            std::string op =
                s.augOp.empty() ? "+" : s.augOp.substr(0, s.augOp.size() - 1);
            assignTo(*s.exprs[0], binop(op, cur, rhs, s.span.line, s.span.col),
                     env);
            return;
        }

        case ast::StKind::Return:
            throw SignalReturn{s.exprs.empty() ? Value::none()
                                               : eval(*s.exprs[0], env)};

        case ast::StKind::Raise: {
            Value err = eval(*s.exprs[0], env);
            throw SignalRaise{Value::resultErr(std::move(err))};
        }

        case ast::StKind::Break:    throw SignalBreak{s.label};
        case ast::StKind::Continue: throw SignalContinue{s.label};

        case ast::StKind::Defer: {
            Deferred d = prepareDeferred(*s.exprs[0], env);
            std::thread::id tid = std::this_thread::get_id();
            bool immediate = false;
            {
                std::lock_guard<std::mutex> lk(defersM_);
                auto& stack = defers_[tid];
                if (stack.empty())
                    immediate = true;
                else
                    stack.back().push_back(std::move(d));
            }
            if (immediate) {
                try {
                    callValue(d.callee, d.args, 0, 0);
                } catch (...) {
                }
            }
            return;
        }

        case ast::StKind::If:
            // block bodies get a fresh child frame so `var` shadowing matches
            // the checker's lexical scopes (previously the same env was used).
            if (truthy(eval(*s.exprs[0], env)))
                execBlock(s.body, makeChild(env));
            else
                for (const auto& eb : s.elseBody) exec(*eb, makeChild(env));
            return;

        case ast::StKind::Try: {
            try {
                execBlock(s.body, makeChild(env));
            } catch (const SignalRaise& sr) {
                if (s.catchParam.empty()) {
                    if (!s.elseBody.empty()) execBlock(s.elseBody, makeChild(env));
                } else {
                    Env child = makeChild(env);
                    child->vars[s.catchParam] =
                        (sr.errResult.payload.empty())
                            ? Value::none()
                            : sr.errResult.payload[0];
                    execBlock(s.elseBody, child);
                }
            }
            return;
        }

        case ast::StKind::While:
            while (truthy(eval(*s.exprs[0], env))) {
                try {
                    execBlock(s.body, makeChild(env));
                } catch (const SignalBreak& b) {
                    if (!b.label.empty() && b.label != s.label) throw;
                    break;
                } catch (const SignalContinue& c) {
                    if (!c.label.empty() && c.label != s.label) throw;
                    continue;
                }
            }
            return;

        case ast::StKind::DoWhile:
            // `do { body } while cond ;` — body runs once, then cond decides.
            // Search form `do { ... } while (pat = expr) ;` binds the pattern
            // from cond's value in a fresh scope each round.
            {
                bool first = true;
                for (;;) {
                    Env child = makeChild(env);
                    if (!first) {
                        Value v = eval(*s.cond, env);
                        if (s.pat) matchPat(*s.pat, v, child);
                        if (!truthy(v)) break;
                    }
                    first = false;
                    try {
                        execBlock(s.body, child);
                    } catch (const SignalBreak& b) {
                        if (!b.label.empty() && b.label != s.label) throw;
                        break;
                    } catch (const SignalContinue& c) {
                        if (!c.label.empty() && c.label != s.label) throw;
                    }
                }
            }
            return;

        case ast::StKind::Gather:
            // Worklist loop: `gather { seeds } { body }`. Seeds are evaluated
            // first and appended to an empty worklist; each `gather <expr>,`
            // in the body appends an item. The loop runs until the worklist
            // empties. A signal result stops collection (success or not).
            {
                std::vector<Value> work;
                for (const auto& se : s.seedExprs)
                    work.push_back(eval(*se, env));
                bool overflow = false;
                for (size_t idx = 0; !overflow; ++idx) {
                    if (idx >= work.size()) break;
                    Env child = makeChild(env);
                    Value item = work[idx];
                    child->vars["item"] = item;          // each iteration binds `item`
                    try {
                        execBlock(s.body, child);
                    } catch (const SignalGather& g) {
                        if (g.stop) break;
                        work.push_back(g.value);          // schedule another trip
                    } catch (const SignalBreak& b) {
                        if (!b.label.empty() && b.label != s.label) throw;
                        break;
                    } catch (const SignalContinue& c) {
                        if (!c.label.empty() && c.label != s.label) throw;
                        continue;
                    }
                    if (work.size() > static_cast<size_t>(gatherBudget_))
                        overflow = true;
                }
                if (overflow)
                    panicHere("gather: worklist exceeded the iteration budget");
            }
            return;

        case ast::StKind::GatherStmt:
            // `gather <expr>,` — append an item to the enclosing worklist.
            throw SignalGather{false, eval(*s.exprs[0], env)};

        case ast::StKind::Goto:
            // `goto label;` — unwinds to the block that owns `label:` and jumps
            // to it (the labeled statement runs, then execution continues). A
            // Label has no side effect of its own, so jumping to `label_index`
            // is equivalent to C's "land on the labeled statement".
            throw SignalGoto{s.label};

        case ast::StKind::Label:
            if (!s.body.empty()) exec(*s.body[0], env);   // run labeled statement
            return;

        case ast::StKind::For: {
            Value seq = eval(*s.exprs[0], env);
            iterateSeq(seq,
                       [&](Value& item, Env&) {
                           Env child = makeChild(env);
                           matchPat(*s.pat, item, child);
                           try {
                               execBlock(s.body, child);
                               return true;
                           } catch (const SignalBreak& b) {
                               if (!b.label.empty() && b.label != s.label)
                                   throw;
                               return false;
                           } catch (const SignalContinue& c) {
                               if (!c.label.empty() && c.label != s.label)
                                   throw;
                               return true;
                           }
                       },
                       env);
            return;
        }

        case ast::StKind::Match: {
            Value subj = eval(*s.exprs[0], env);
            for (const auto& arm : s.arms) {
                Env armEnv = makeChild(env);
                if (!matchPat(*arm.pat, subj, armEnv)) continue;
                if (arm.guard && !truthy(eval(*arm.guard, armEnv))) continue;
                execBlock(arm.body, armEnv);
                return;
            }
            // no arm matched: fall through silently
            return;
        }

        case ast::StKind::Select:
            selectExec(s, env);
            return;

        case ast::StKind::Unsafe:
            execBlock(s.body, makeChild(env));
            return;

        case ast::StKind::Spawn:
            spawnCall(*s.exprs[0], env);
            return;

        default:
            panicHere("internal: unexpected statement at runtime");
    }
}

// ---------------------------------------------------------------------------
// expressions
// ---------------------------------------------------------------------------

bool Interpreter::isCmpOp(const std::string& op) {
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

static bool typeTestMatches(const Value& v, const std::string& t) {
    if (t == "none") return v.k == VK::None;
    if (t == "string") return v.k == VK::Str;
    if (t == "bytes") return v.k == VK::Bytes;
    if (t == "char") return v.k == VK::Char;
    if (t == "bool") return v.k == VK::Bool;
    if (t == "int" || t == "i8" || t == "i16" || t == "i32" || t == "i64" ||
        t == "u8" || t == "u16" || t == "u32" || t == "u64" || t == "usize")
        return v.k == VK::Int;
    if (t == "float" || t == "f32" || t == "f64") return v.k == VK::Float;
    if (t == "list") return v.k == VK::List;
    if (t == "dict") return v.k == VK::Dict;
    if (t == "set")  return v.k == VK::Set;
    if (t == "tuple") return v.k == VK::Tuple;
    if (t == "result") return v.k == VK::Result;
    if (v.k == VK::Struct && v.typeName == t) return true;
    if (v.k == VK::Heap && v.heap->typeName == t) return true;
    if (v.k == VK::EnumV && v.typeName == t) return true;
    return false;
}

Value Interpreter::eval(const Expr& e, Env env) {
    switch (e.kind) {
        case ast::ExKind::Int:
            return Value::integer(parseIntText(e.text));
        case ast::ExKind::Float:
            return Value::floating(strtod(stripUnderscores(e.text).c_str(), nullptr));
        case ast::ExKind::CharLit:
            return Value::chr(decodeCharText(e.text));

        case ast::ExKind::Str: {
            switch (e.flavor) {
                case ast::StrFlavor::Raw:   return Value::str(e.text);
                case ast::StrFlavor::Normal:return Value::str(decodeEscapes(e.text));
                case ast::StrFlavor::Byte: {
                    Value b = Value::str(decodeEscapes(e.text));
                    b.k = VK::Bytes;
                    return b;
                }
                case ast::StrFlavor::C: {
                    Value p = Value::str(decodeEscapes(e.text));
                    p.k = VK::Ptr;
                    return p;
                }
            }
            break;
        }

        case ast::ExKind::FString:
            return formatFString(e, env);

        case ast::ExKind::Ident: {
            if (e.text == "true") return Value::boolean(true);
            if (e.text == "false") return Value::boolean(false);
            if (e.text == "none") return Value::none();
            if (const Value* v = env->find(e.text)) return *v;
            auto fi = funcs_.find(e.text);
            if (fi != funcs_.end()) {
                Value f;
                f.k = VK::Fn;
                f.fn = fi->second;
                f.env = globals_;
                return f;
            }
            if (structs_.count(e.text)) {
                Value m;
                m.k = VK::Module;
                m.typeName = "__struct__:" + e.text;
                return m;
            }
            if (enums_.count(e.text)) {
                Value m;
                m.k = VK::Module;
                m.typeName = "__enum__:" + e.text;
                return m;
            }
            panicHere("undefined variable '" + e.text + "'");
        }

        case ast::ExKind::Unary: {
            if (e.op == "spawn") return spawnCall(*e.rhs, env);
            if (e.op == "try") {
                Value t = eval(*e.rhs, env);
                if (t.k == VK::Result) {
                    if (t.variant == "err") throw SignalRaise{t};
                    return t.payload[0];
                }
                return t;
            }
            if (e.op == "&" || e.op == "*") return eval(*e.rhs, env);
            Value v = eval(*e.rhs, env);
            if (e.op == "-") {
                if (v.k == VK::Int) { v.i = -v.i; return v; }
                if (v.k == VK::Float) { v.d = -v.d; return v; }
                panicHere("unary '-' expects a number");
            }
            if (e.op == "+") {
                if (v.k == VK::Int || v.k == VK::Float) return v;
                panicHere("unary '+' expects a number");
            }
            if (e.op == "~") {
                if (v.k == VK::Int) { v.i = ~v.i; return v; }
                panicHere("unary '~' expects an integer");
            }
            if (e.op == "not") return Value::boolean(!truthy(v));
            panicHere("unknown unary operator '" + e.op + "'");
        }

        case ast::ExKind::Binary: {
            const std::string& op = e.op;

            if (op == "and") {
                if (!truthy(eval(*e.lhs, env))) return Value::boolean(false);
                return Value::boolean(truthy(eval(*e.rhs, env)));
            }
            if (op == "or") {
                if (truthy(eval(*e.lhs, env))) return Value::boolean(true);
                return Value::boolean(truthy(eval(*e.rhs, env)));
            }
            if ((op == ".." || op == "..=") && !e.rhs) {
                int64_t lo = eval(*e.lhs, env).i;
                Value r;
                r.k = VK::Range;
                r.lo = lo;
                r.hi = -1;                      // open-ended sentinel
                return r;
            }

            if (isCmpOp(op)) {
                if (e.lhs->kind == ast::ExKind::Binary &&
                    isCmpOp(e.lhs->op))
                    return Value::boolean(evalCmpChain(e, env));
                return compareOne(op, eval(*e.lhs, env), eval(*e.rhs, env));
            }

            Value l = eval(*e.lhs, env);

            if (op == "is") {
                if (e.rhs->kind != ast::ExKind::Ident)
                    panicHere("'is' expects a type or none");
                const std::string& tn = e.rhs->text;
                if (tn == "none") return Value::boolean(l.k == VK::None);
                return Value::boolean(typeTestMatches(l, tn));
            }

            Value r = eval(*e.rhs, env);

            if (op == "in") {
                switch (r.k) {
                    case VK::List:
                    case VK::Set:
                    case VK::Tuple:
                        for (const auto& el : *r.vec)
                            if (valuesEqual(l, el)) return Value::boolean(true);
                        return Value::boolean(false);
                    case VK::Dict:
                        for (const auto& kv : *r.map)
                            if (valuesEqual(l, kv.first)) return Value::boolean(true);
                        return Value::boolean(false);
                    case VK::Str:
                        if (l.k != VK::Str)
                            panicHere("'in' on strings needs a string needle");
                        return Value::boolean(r.s.find(l.s) != std::string::npos);
                    case VK::Range:
                        return Value::boolean(
                            l.i >= r.lo &&
                            (r.inclusive ? l.i <= r.hi : l.i < r.hi));
                    default:
                        panicHere("'in' unsupported operand");
                }
            }

            if (op == ".." || op == "..=")
                return Value::rangeV(l.i, r.i, op == "..=");

            return binop(op, l, r, e.span.line, e.span.col);
        }

        case ast::ExKind::Call: {
            // chan[elem](cap: n)
            if (e.lhs->kind == ast::ExKind::Index && e.lhs->lhs &&
                e.lhs->lhs->kind == ast::ExKind::Ident &&
                e.lhs->lhs->text == "chan") {
                Value c;
                c.k = VK::Chan;
                c.chan = std::make_shared<ChanImpl>();
                for (const auto& a : e.args)
                    if (a.name == "cap")
                        c.chan->cap = (size_t)eval(*a.value, env).i;
                return c;
            }

            // same construction when parsed as Call around `New chan[..]`
            if (e.lhs->kind == ast::ExKind::New &&
                e.lhs->newType->name.rfind("chan", 0) == 0) {
                Value c;
                c.k = VK::Chan;
                c.chan = std::make_shared<ChanImpl>();
                for (const auto& a : e.args)
                    if (a.name == "cap" && a.value)
                        c.chan->cap =
                            (size_t)(a.value->kind == ast::ExKind::Int
                                         ? parseIntText(a.value->text)
                                         : eval(*a.value, env).i);
                return c;
            }

            // heap struct: `new X(..)` may parse as Call around New with the
            // field args on the OUTER call
            if (e.lhs->kind == ast::ExKind::New &&
                structs_.count(e.lhs->newType->name))
                return makeHeapValue(e.lhs->newType->name, e.args, env);

            // enum constructor: Color.red(...) / Option.some(...)
            if (e.lhs->kind == ast::ExKind::Member && e.lhs->lhs &&
                e.lhs->lhs->kind == ast::ExKind::Ident) {
                const std::string& ename = e.lhs->lhs->text;
                if (!env->find(ename) && !funcs_.count(ename) &&
                    !structs_.count(ename) && enums_.count(ename))
                    return makeEnumV(ename, e.lhs->text, e.args, env,
                                     e.span.line, e.span.col);
            }

            // struct literal construction: Circle(center: c, radius: r)
            if (e.lhs->kind == ast::ExKind::Ident) {
                const std::string& sname = e.lhs->text;
                const Value* bound = env->find(sname);
                bool marker = bound && bound->k == VK::Module &&
                              bound->typeName.rfind("__struct__:", 0) == 0;
                if ((marker || (!bound && !funcs_.count(sname))) &&
                    structs_.count(sname))
                    return makeStruct(sname, e.args, env);
            }

            std::vector<Value> pos;
            NamedArgs named;
            for (const auto& a : e.args) {
                if (a.name.empty()) pos.push_back(eval(*a.value, env));
                else named.emplace_back(a.name, eval(*a.value, env));
            }

            // method call
            if (e.lhs->kind == ast::ExKind::Member) {
                const Expr& m = *e.lhs;
                Value recv = eval(*m.lhs, env);
                if (m.nilSafe && recv.isNone()) return Value::none();
                // module-qualified type reference: `collections.HashMap(f:..)`
                if (recv.k == VK::Module && !m.nilSafe) {
                    Value mem = memberRead(recv, m.text, e.span.line, e.span.col);
                    if (mem.k == VK::Module &&
                        mem.typeName.rfind("__struct__:", 0) == 0)
                        return makeStruct(mem.typeName.substr(11), e.args, env);
                    if (mem.k == VK::Module &&
                        mem.typeName.rfind("__enum__:", 0) == 0)
                        return makeEnumV(mem.typeName.substr(9), m.text, e.args,
                                         env, e.span.line, e.span.col);
                }
                bool lval = m.lhs->kind == ast::ExKind::Ident ||
                            m.lhs->kind == ast::ExKind::Index ||
                            m.lhs->kind == ast::ExKind::Member;
                Value* ref = lval ? lvaluePtr(*m.lhs, env) : nullptr;
                bool wantSelf = ref != nullptr && recv.k == VK::Struct;
                Value selfOut;
                Value r = invokeMethod(std::move(recv), m.text, std::move(pos),
                                       std::move(named), env, e.span.line,
                                       e.span.col,
                                       wantSelf ? &selfOut : nullptr);
                if (wantSelf && selfOut.k == VK::Struct) *ref = selfOut;
                return r;
            }

            // plain callee
            if (e.lhs->kind == ast::ExKind::Ident) {
                const std::string& name = e.lhs->text;
                if (const Value* v = env->find(name)) {
                    if (v->k == VK::Module &&
                        v->typeName.rfind("__struct__:", 0) == 0)
                        return makeStruct(v->typeName.substr(11), e.args, env);
                    if (v->k == VK::Builtin && !named.empty() && !v->biParams.empty()) {
                        mapNamedIntoPos(v->biParams, pos, named);
                        named.clear();
                    }
                    return callValue(*v, std::move(pos), e.span.line, e.span.col);
                }
auto fi = funcs_.find(name);
                if (fi != funcs_.end()) {
                    CallFrameGuard guard(fi->second->name, e.span.line,
                                         e.span.col);
                    return runFunc(fi->second, std::move(pos), std::move(named),
                                   globals_);
                }
                if (structs_.count(name))
                    return makeStruct(name, e.args, env);
                panicHere("undefined variable '" + name + "'");
            }

Value cv = eval(*e.lhs, env);
            if (cv.k == VK::Fn && cv.fn) {
                CallFrameGuard guard(cv.fn->name, e.span.line, e.span.col);
                return runFunc(cv.fn, std::move(pos), std::move(named),
                               cv.env ? cv.env : globals_);
            }
            if (!named.empty() && cv.k == VK::Builtin && !cv.biParams.empty()) {
                mapNamedIntoPos(cv.biParams, pos, named);
            }
            return callValue(std::move(cv), std::move(pos), e.span.line,
                             e.span.col);
        }

        case ast::ExKind::Index: {
            Value obj = eval(*e.lhs, env);
            if (obj.k == VK::Weak) obj = lockHeap(obj);
            if (e.rhs->kind == ast::ExKind::Binary &&
                (e.rhs->op == ".." || e.rhs->op == "..=")) {
                int64_t lo = eval(*e.rhs->lhs, env).i;
                int64_t hi = e.rhs->rhs ? eval(*e.rhs->rhs, env).i : -1;
                return sliceOf(obj, lo, hi, e.rhs->op == "..=", 1);
            }
            Value idx = eval(*e.rhs, env);
            switch (obj.k) {
                case VK::List:
                case VK::Tuple:
                    return (*obj.vec)[normIndex((int64_t)obj.vec->size(), idx.i)];
                case VK::Dict:
                    for (const auto& kv : *obj.map)
                        if (valuesEqual(kv.first, idx)) return kv.second;
                    panicHere("dict key not found");
                case VK::Str:
                case VK::Bytes: {
                    size_t n = normIndex((int64_t)obj.s.size(), idx.i);
                    if (obj.k == VK::Bytes)
                        return Value::integer((unsigned char)obj.s[n]);
                    return Value::chr(decodeCharText(obj.s.substr(n, 1)));
                }
                default: {
                    // Index trait lowering: g[i] calls user `index` method
                    if (obj.k == VK::Struct || obj.k == VK::Heap)
                        return invokeMethod(obj, "index", {idx}, {}, env,
                                            e.span.line, e.span.col, nullptr);
                    panicHere("value is not indexable");
                }
            }
        }

        case ast::ExKind::Slice: {
            Value obj = eval(*e.lhs, env);
            if (obj.k == VK::Weak) obj = lockHeap(obj);
            int64_t lo = 0, hi = -1, step = 1;
            if (!e.elems.empty() && e.elems[0]) lo = eval(*e.elems[0], env).i;
            if (e.elems.size() > 1 && e.elems[1]) hi = eval(*e.elems[1], env).i;
            if (e.elems.size() > 2 && e.elems[2]) step = eval(*e.elems[2], env).i;
            return sliceOf(obj, lo, hi, false, step);
        }

        case ast::ExKind::Member: {
            Value recv = eval(*e.lhs, env);
            if (recv.k == VK::Weak) recv = lockHeap(recv);
            if (recv.isNone() && e.nilSafe) return Value::none();
            if (recv.k == VK::Module &&
                recv.typeName.rfind("__enum__:", 0) == 0) {
                std::string ename = recv.typeName.substr(9);
                auto ei = enums_.find(ename);
                bool unit = false;
                if (ei != enums_.end())
                    for (const auto& var : ei->second->variants)
                        if (var.name == e.text && var.payload.empty()) {
                            unit = true;
                            break;
                        }
                if (!unit)
                    panicHere("enum variant '" + e.text + "' must be called");
                return makeEnumV(ename, e.text, {}, env, e.span.line,
                                 e.span.col);
            }
            return memberRead(recv, e.text, e.span.line, e.span.col);
        }

        case ast::ExKind::Try: {
            Value v = eval(*e.lhs, env);
            if (v.k == VK::Result) {
                if (v.variant == "err") throw SignalRaise{v};
                return v.payload[0];
            }
            return v;
        }

        case ast::ExKind::Lambda: {
            Value f;
            f.k = VK::Fn;
            f.lam = &e;
            f.env = env;
            return f;
        }

        case ast::ExKind::Cond:
            return truthy(eval(*e.cond, env)) ? eval(*e.lhs, env)
                                              : eval(*e.rhs, env);

        case ast::ExKind::ListComp:
        case ast::ExKind::Generator: {
            std::vector<Value> out;
            std::function<void(size_t, Env)> walk =
                [&](size_t ci, Env cenv) {
                    if (ci >= e.clauses.size()) {
                        out.push_back(eval(*e.elems[0], cenv));
                        return;
                    }
                    const ast::CompClause& cl = e.clauses[ci];
                    if (cl.isFor) {
                        Value seq = eval(*cl.iter, cenv);
                        iterateSeq(
                            seq,
                            [&](Value& item, Env&) {
                                Env inner = makeChild(cenv);
                                bindPat(*cl.pat, item, inner);
                                walk(ci + 1, inner);
                                return true;
                            },
                            cenv);
                    } else {
                        if (truthy(eval(*cl.cond, cenv))) walk(ci + 1, cenv);
                    }
                };
            walk(0, makeChild(env));
            if (e.kind == ast::ExKind::Generator)
                return Value::tuple(std::move(out));
            return Value::list(std::move(out));
        }

        case ast::ExKind::List:
        case ast::ExKind::Set: {
            std::vector<Value> items;
            for (const auto& el : e.elems) items.push_back(eval(*el, env));
            return e.kind == ast::ExKind::Set ? Value::set(std::move(items))
                                              : Value::list(std::move(items));
        }

        case ast::ExKind::Tuple: {
            std::vector<Value> items;
            for (const auto& el : e.elems) items.push_back(eval(*el, env));
            return Value::tuple(std::move(items));
        }

        case ast::ExKind::Dict: {
            Value d = Value::dict();
            for (const auto& pr : e.pairs) {
                Value key = eval(*pr.first, env);
                for (auto& kv : *d.map)
                    if (valuesEqual(kv.first, key)) {
                        panicHere("duplicate dict key in literal");
                    }
                d.map->emplace_back(std::move(key), eval(*pr.second, env));
            }
            return d;
        }

        case ast::ExKind::New: {
            auto si = structs_.find(e.newType->name);
            if (si == structs_.end() &&
                e.newType->name.rfind("chan", 0) == 0) {
                auto ch = std::make_shared<ChanImpl>();
                if (!e.args.empty()) {
                    const auto& a = e.args[0];
                    int64_t cap = 0;
                    if (a.value)
                        cap = a.value->kind == ast::ExKind::Int
                                  ? parseIntText(a.value->text)
                                  : eval(*a.value, env).i;
                    ch->cap = cap > 0 ? (size_t)cap : 0;
                }
                Value cv;
                cv.k = VK::Chan;
                cv.chan = std::move(ch);
                return cv;
            }
            if (si != structs_.end())
                return makeHeapValue(e.newType->name, e.args, env);
            panicHere("'new' of unknown struct '" + e.newType->name + "'");
        }

        case ast::ExKind::Cast: {
            Value v = eval(*e.lhs, env);
            const std::string& t = e.newType->name;
            if (t == "string" || t == "str") return Value::str(toStr(v));
            if (t == "int") {
                if (v.k == VK::Float) return Value::integer((int64_t)v.d);
                if (v.k == VK::Bool) return Value::integer(v.b ? 1 : 0);
                if (v.k == VK::Char) return Value::integer((int64_t)v.ch);
                return v;
            }
            if (t == "float" || t == "f32" || t == "f64")
                return Value::floating(v.k == VK::Int ? (double)v.i
                                                      : v.k == VK::Float ? v.d
                                                                         : 0.0);
            if (t == "bool") return Value::boolean(truthy(v));
            if (v.k == VK::None && t == "none") return v;
            if ((t == "i8" || t == "i16" || t == "i32" || t == "u8" ||
                 t == "u16" || t == "u32") &&
                v.k == VK::Int) {
                int bits = t == "i8" ? 8 : t == "i16" ? 16 : t == "i32" ? 32
                                       : t == "u8" ? 8 : t == "u16" ? 16 : 32;
                uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
                return Value::integer((int64_t)((uint64_t)v.i & mask));
            }
            if (v.k != VK::None && !typeTestMatches(v, t))
                panicHere("invalid cast to '" + t + "'");
            return v;
        }

        case ast::ExKind::Match: {
            Value subj = e.lhs ? eval(*e.lhs, env) : Value::none();
            Value result = Value::none();
            bool matched = false;
            for (const auto& arm : e.matchArms) {
                Env armEnv = makeChild(env);
                if (!arm.pat || !matchPat(*arm.pat, subj, armEnv)) continue;
                if (arm.guard && !truthy(eval(*arm.guard, armEnv))) continue;
                // execute the arm; its trailing expression statement is the value.
                for (const auto& st : arm.body) {
                    if (st == arm.body.back() &&
                        st->kind == ast::StKind::ExprStmt &&
                        st->exprs.size() == 1)
                        result = eval(*st->exprs[0], armEnv);
                    else
                        exec(*st, armEnv);
                }
                matched = true;
                break;
            }
            return matched ? result : Value::none();
        }
    }

    panicHere("internal: unhandled expression kind");
}

// ---------------------------------------------------------------------------
// helpers defined here, forward-declared above
// ---------------------------------------------------------------------------

static void mapNamedIntoPos(const std::vector<std::string>& slots,
                            std::vector<Value>& pos,
                            std::vector<std::pair<std::string, Value>>& named) {
    std::vector<Value> out(slots.size());
    std::vector<bool> filled(slots.size(), false);
    for (size_t si = 0; si < slots.size(); ++si) {
        for (size_t ni = 0; ni < named.size(); ++ni) {
            if (named[ni].first == slots[si]) {
                out[si] = std::move(named[ni].second);
                named.erase(named.begin() + (long)ni);
                filled[si] = true;
                break;
            }
        }
    }
    size_t pi = 0;
    for (size_t si = 0; si < slots.size(); ++si)
        if (!filled[si] && pi < pos.size()) out[si] = std::move(pos[pi++]);
    while (pi < pos.size()) out.push_back(std::move(pos[pi++]));
    pos = std::move(out);
}

static Value lockHeap(const Value& v) {
    auto sp = v.weak.lock();
    if (!sp) return Value::none();
    Value h;
    h.k = VK::Heap;
    h.heap = std::move(sp);
    return h;
}

static size_t normIndex(int64_t len, int64_t idx) {
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) panicHere("index out of range");
    return (size_t)idx;
}

static Value sliceOf(const Value& obj, int64_t lo, int64_t hi, bool incl,
                     int64_t step) {
    if (step <= 0) panicHere("negative slice step unsupported");
    int64_t len = 0;
    switch (obj.k) {
        case VK::List:
        case VK::Tuple:
        case VK::Set:
            len = (int64_t)obj.vec->size();
            break;
        case VK::Str:
        case VK::Bytes:
            len = (int64_t)obj.s.size();
            break;
        default:
            panicHere("cannot slice this value");
    }
    if (hi < 0) hi = len;               // open-ended sentinel
    else if (incl) hi += 1;
    if (lo < 0) lo += len;
    if (hi > len) hi = len;
    if (lo < 0) lo = 0;

    std::vector<Value> items;
    std::string chars;
    bool strMode = obj.k == VK::Str || obj.k == VK::Bytes;
    for (int64_t i = lo; i < hi; i += step) {
        if (strMode) chars += obj.s[(size_t)i];
        else items.push_back((*obj.vec)[(size_t)i]);
    }
    if (strMode) {
        Value r = Value::str(std::move(chars));
        if (obj.k == VK::Bytes) r.k = VK::Bytes;
        return r;
    }
    if (obj.k == VK::Tuple) return Value::tuple(std::move(items));
    if (obj.k == VK::Set)   return Value::set(std::move(items));
    return Value::list(std::move(items));
}

// ---------------------------------------------------------------------------
// calls
// ---------------------------------------------------------------------------

Value Interpreter::callValue(Value callee, std::vector<Value> args, int line,
                             int col) {
    switch (callee.k) {
        case VK::Builtin: {
            if (!callee.boundSelf.empty()) {
                std::vector<Value> full;
                full.reserve(callee.boundSelf.size() + args.size());
                for (const auto& s : callee.boundSelf) full.push_back(s);
                for (auto& a : args) full.push_back(std::move(a));
                return callee.bi(full);
            }
            return callee.bi(args);
        }
case VK::Fn: {
            std::string name = callee.lam ? "<lambda>"
                              : (callee.fn ? callee.fn->name : "?");
            CallFrameGuard guard(std::move(name),
                                 line > 0 ? (uint32_t)line : 0,
                                 col > 0 ? (uint32_t)col : 0);
            if (callee.lam)
                return runLambda(callee.lam, std::move(args),
                                 callee.env ? callee.env : globals_);
            if (callee.fn)
                return runFunc(callee.fn, std::move(args), {},
                               callee.env ? callee.env : globals_);
            break;
        }
        default:
            break;
    }
    (void)line; (void)col;
    panicHere("value is not callable");
}

Value Interpreter::runFunc(const Stmt* fn, std::vector<Value> pos,
                           NamedArgs named, Env closure, Value* selfOut) {
    Env fenv = makeChild(closure ? closure : globals_);
    const auto& ps = fn->params;

    long varIdx = -1;
    for (size_t pi = 0; pi < ps.size(); ++pi)
        if (ps[pi].variadic) varIdx = (long)pi;

    std::vector<bool> placed(ps.size(), false);

    // named arguments first
    for (auto& pr : named) {
        bool ok = false;
        for (size_t pi = 0; pi < ps.size(); ++pi) {
            if (ps[pi].name == pr.first && !placed[pi]) {
                fenv->vars[pr.first] = std::move(pr.second);
                placed[pi] = true;
                ok = true;
                break;
            }
        }
        if (!ok) panicHere("unknown parameter '" + pr.first + "'");
    }

    // positionals fill unset non-variadic slots in order
    size_t k = 0, slot = 0;
    while (k < pos.size() && slot < ps.size()) {
        if (placed[slot] || ps[slot].variadic) { ++slot; continue; }
        fenv->vars[ps[slot].name] = std::move(pos[k]);
        placed[slot] = true;
        ++k;
        ++slot;
    }
    if ((varIdx >= 0 && k < pos.size()) ||
        (varIdx >= 0 && !placed[(size_t)varIdx])) {
        std::vector<Value> extra;
        while (k < pos.size()) extra.push_back(std::move(pos[k++]));
        fenv->vars[ps[(size_t)varIdx].name] =
            placed[(size_t)varIdx]
                ? Value::none()
                : Value::list(std::move(extra));
        if (!placed[(size_t)varIdx]) placed[(size_t)varIdx] = true;
    } else if (k < pos.size()) {
        panicHere("too many positional arguments to '" + fn->name + "'");
    }

    // defaults / missing
    for (size_t pi = 0; pi < ps.size(); ++pi) {
        if (placed[pi]) continue;
        if (ps[pi].defaultValue)
            fenv->vars[ps[pi].name] = eval(*ps[pi].defaultValue, fenv);
        else if (ps[pi].variadic)
            fenv->vars[ps[pi].name] = Value::list();
        else
            panicHere("missing argument '" + ps[pi].name + "'");
    }

    std::thread::id tid = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lk(defersM_);
        defers_[tid].emplace_back();
    }
    auto runDefers = [&]() {
        std::vector<Deferred> mine;
        {
            std::lock_guard<std::mutex> lk(defersM_);
            auto it = defers_.find(tid);
            if (it == defers_.end() || it->second.empty()) return;
            mine = std::move(it->second.back());
            it->second.pop_back();
        }
        while (!mine.empty()) {
            Deferred d = std::move(mine.back());
            mine.pop_back();
            try {
                callValue(d.callee, d.args, 0, 0);
            } catch (...) {
            }
        }
    };

    Value ret = Value::none();
    std::vector<Value> genOut;
    const bool isGen = fn->isGenerator;
    if (isGen) yieldSinks_.push_back(&genOut);
    try {
        if (const NativeBody* nb = nativeFuncFor(fn))
            ret = nativeRunBody(*nb, fenv, fn);
        else if (const vm::VmFunction* vf = vmFuncFor(fn))
            ret = vmRunBody(*vf, fenv);
        else
            execBlock(fn->body, fenv);
    } catch (SignalReturn& sr) {
        ret = std::move(sr.v);
        runDefers();
    } catch (SignalRaise& sraise) {
        runDefers();
        if (isGen) yieldSinks_.pop_back();
        if (isResultRet(*fn)) return sraise.errResult;
        throw;
    } catch (...) {
        runDefers();
        if (isGen) yieldSinks_.pop_back();
        throw;
    }
    if (isGen) yieldSinks_.pop_back();

    if (selfOut) {
        for (const auto& p : ps)
            if (p.name == "self" || p.selfParam) {
                auto it = fenv->vars.find(p.name);
                if (it != fenv->vars.end()) *selfOut = it->second;
                break;
            }
    }

    if (isGen) return Value::gen(std::move(genOut));
    if (isResultRet(*fn)) return Value::resultOk(std::move(ret));
    return ret;
}

Value Interpreter::runLambda(const Expr* lam, std::vector<Value> args,
                             Env closure) {
    Env lenv = makeChild(closure ? closure : globals_);

    if (!lam->lambdaBody.empty()) {
        // block-bodied closure: bind positionally, fill defaults, execute the
        // statement block; `return v;` supplies the value (SignalReturn).
        const auto& cps = lam->closureParams;
        for (size_t i = args.size(); i-- > 0;) {
            if (i >= cps.size())
                panicHere("too many arguments to fn closure");
            lenv->vars[cps[i].name] = std::move(args[i]);
        }
        for (size_t i = args.size(); i < cps.size(); ++i) {
            const auto& p = cps[i];
            if (p.defaultValue)
                lenv->vars[p.name] = eval(*p.defaultValue, lenv);
            else
                panicHere("missing argument '" + p.name + "'");
        }
        Value ret = Value::none();
        try {
            execBlock(lam->lambdaBody, lenv);
        } catch (SignalReturn& sr) {
            ret = std::move(sr.v);
        }
        return ret;
    }

    const auto& lp = lam->lambdaParams;
    if (args.size() > lp.size()) panicHere("too many lambda arguments");
    if (args.size() < lp.size())
        panicHere("missing lambda argument");
    for (size_t i = 0; i < lp.size(); ++i)
        lenv->vars[lp[i]] = std::move(args[i]);
    return eval(*lam->rhs, lenv);
}

// ---------------------------------------------------------------------------
// methods & members
// ---------------------------------------------------------------------------

Value Interpreter::invokeMethod(Value obj, const std::string& name,
                                std::vector<Value> pos, NamedArgs named,
                                Env env, int line, int col, Value* selfOut) {
    (void)env;
    if (obj.k == VK::Weak) obj = lockHeap(obj);

    if (obj.k == VK::EnumV) {
        if (name == "to_int") {
            auto ei = enums_.find(obj.typeName);
            if (ei != enums_.end())
                for (size_t i = 0; i < ei->second->variants.size(); ++i)
                    if (ei->second->variants[i].name == obj.variant)
                        return Value::integer(static_cast<int64_t>(i));
            panicHere("enum '" + obj.typeName + "' variant '" + obj.variant +
                      "' has no discriminant");
        }
        panicHere("enum '" + obj.typeName + "' has no method '" + name + "'");
    }

    if (obj.k == VK::Struct || obj.k == VK::Heap) {
        const std::string& tn =
            obj.k == VK::Heap ? obj.heap->typeName : obj.typeName;
        const Stmt* method = nullptr;
        auto si = structs_.find(tn);
        if (si != structs_.end()) {
            for (const auto& m : si->second->body)
                if (m->kind == ast::StKind::FuncDef && m->name == name) {
                    method = m.get();
                    break;
                }
        }
        if (!method) {
            for (auto it = impls_.rbegin(); it != impls_.rend(); ++it) {
                if (it->typeName != tn) continue;
                auto mi = it->methods.find(name);
                if (mi != it->methods.end()) { method = mi->second; break; }
            }
        }
        if (!method)
            panicHere("type '" + tn + "' has no method '" + name + "'");
std::vector<Value> full;
        full.reserve(1 + pos.size());
        full.push_back(obj);
        for (auto& v : pos) full.push_back(std::move(v));
        CallFrameGuard guard(method->name, line, col);
        return runFunc(method, std::move(full), std::move(named), globals_,
                       selfOut);
    }

    Value b = memberRead(obj, name, line, col);
    if (b.k != VK::Builtin)
        panicHere("'" + name + "' is not callable");
    if (!named.empty() && !b.biParams.empty()) {
        mapNamedIntoPos(b.biParams, pos, named);
        named.clear();
    }
    return callValue(std::move(b), std::move(pos), line, col);
}

Value Interpreter::memberRead(const Value& obj, const std::string& name,
                              int line, int col) {
    (void)line;
    (void)col;
    auto bind = [&](std::vector<std::string> params, BuiltinFn fn) {
        Value b;
        b.k = VK::Builtin;
        b.biParams = std::move(params);
        b.boundSelf.push_back(obj);
        b.bi = std::move(fn);
        return b;
    };

    // field access on struct values and heap objects (locks weak refs)
    {
        Value self = obj;
        if (self.k == VK::Weak) self = lockHeap(self);
        if (self.k == VK::Struct || self.k == VK::Heap) {
            const std::vector<std::pair<std::string, Value>>& fs =
                self.k == VK::Heap ? self.heap->fields : self.fields;
            for (const auto& f : fs)
                if (f.first == name) return f.second;
        }
    }

    switch (obj.k) {
        case VK::None:
            panicHere("cannot read '" + name + "' from none");

        case VK::Module:
            if (obj.typeName.rfind("__struct__:", 0) == 0 ||
                obj.typeName.rfind("__enum__:", 0) == 0)
                panicHere("type reference has no member '" + name + "'");
            return moduleMember(obj.typeName, name);

        case VK::Result: {
            bool isOk = obj.variant == "ok";
            if (name == "is_ok") return bind({}, [](std::vector<Value>& a) {
                return Value::boolean(a[0].variant == "ok");
            });
            if (name == "is_err") return bind({}, [](std::vector<Value>& a) {
                return Value::boolean(a[0].variant == "err");
            });
            if (name == "unwrap") {
                if (!isOk) panicHere("unwrap on err value");
                Value p = obj.payload.empty() ? Value::none() : obj.payload[0];
                return bind({}, [p](std::vector<Value>&) { return p; });
            }
            if (name == "unwrap_err") {
                if (isOk) panicHere("unwrap_err on ok value");
                Value p = obj.payload.empty() ? Value::none() : obj.payload[0];
                return bind({}, [p](std::vector<Value>&) { return p; });
            }
            break;
        }

        case VK::EnumV: {
            auto ei = enums_.find(obj.typeName);
            if (ei != enums_.end())
                for (const auto& var : ei->second->variants) {
                    if (var.name != obj.variant) continue;
                    for (size_t i = 0;
                         i < var.payload.size() && i < obj.payload.size(); ++i)
                        if (var.payload[i].name == name)
                            return obj.payload[i];
                    break;
                }
            panicHere("enum variant has no field '" + name + "'");
        }

        case VK::Chan: {
            if (name == "send")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].chan->send(a.size() > 1 ? a[1] : Value::none());
                    return Value::none();
                });
            if (name == "recv")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto s = a[0].chan->recv();
                    if (!s.got) panicHere("recv on closed channel");
                    return s.v;
                });
            if (name == "try_recv")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto s = a[0].chan->tryRecv();
                    if (!s.got) return Value::none();
                    return s.v;
                });
            if (name == "close")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].chan->close();
                    return Value::none();
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].chan->q.size());
                });
            break;
        }

        case VK::ThreadH:
            if (name == "join")
                return bind({}, [this](std::vector<Value>& a) -> Value {
                    auto t = a[0].thread;
                    if (t && t->th.joinable() &&
                        std::this_thread::get_id() != t->th.get_id()) {
                        t->th.join();
                    } else if (t && t->th.joinable()) {
                        t->th.detach();     // joining self: detach instead
                    }
                    return Value::none();
                });
            break;

        case VK::File:
            if (name == "read")
                return bind({}, [this](std::vector<Value>& a) -> Value {
                    std::ifstream f(a[0].s, std::ios::binary);
                    std::string data((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    return Value::str(data);
                });
            if (name == "write")
                return bind({}, [this](std::vector<Value>& a) -> Value {
                    std::ofstream f(a[0].s, std::ios::binary | std::ios::app);
                    f << toStr(a.size() > 1 ? a[1] : Value::str(""));
                    return Value::none();
                });
            if (name == "close") return bind({}, [](std::vector<Value>&) -> Value {
                return Value::none();
            });
            break;

        case VK::Arena:
            if (name == "alloc")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    // identity allocator: the value is "owned" by the arena
                    return a.size() > 1 ? a[1] : Value::none();
                });
            if (name == "free_all" || name == "reset")
                return bind({}, [](std::vector<Value>&) -> Value {
                    return Value::none();
                });
            break;

        default:
            break;
    }

    // containers ------------------------------------------------------------
    switch (obj.k) {
        case VK::List: {
            if (name == "append")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].vec->push_back(a.size() > 1 ? a[1] : Value::none());
                    return Value::none();
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].vec->size());
                });
            if (name == "extend")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a.size() > 1 && (a[1].k == VK::List ||
                                         a[1].k == VK::Tuple)) {
                        a[0].vec->insert(a[0].vec->end(), a[1].vec->begin(),
                                         a[1].vec->end());
                    }
                    return Value::none();
                });
            if (name == "pop")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].vec->empty()) panicHere("pop from empty list");
                    Value back = a[0].vec->back();
                    a[0].vec->pop_back();
                    return back;
                });
            if (name == "shift")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].vec->empty()) panicHere("shift from empty list");
                    Value front = a[0].vec->front();
                    a[0].vec->erase(a[0].vec->begin());
                    return front;
                });
            if (name == "next")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    // next item (head) without consuming; none when empty
                    if (a[0].vec->empty()) return Value::none();
                    return (*a[0].vec)[0];
                });
            if (name == "fold")
                return bind({"f", "init"}, [this](std::vector<Value>& a) -> Value {
                    // xs.fold(f) folds right-to-left using xs[0] as the initial
                    // accumulator; xs.fold(f, init) starts from init.
                    const std::vector<Value>& v = *a[0].vec;
                    size_t i = 0;
                    Value acc;
                    if (a.size() > 2 && !a[2].isNone()) acc = a[2];
                    else {
                        if (v.empty())
                            panicHere("fold from empty list requires an initial value");
                        acc = v[0];
                        i = 1;
                    }
                    Value f = a[1];
                    for (; i < v.size(); ++i)
                        acc = callValue(f, {acc, v[i]}, 0, 0);
                    return acc;
                });
            if (name == "insert")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    size_t at = normIndex((int64_t)a[0].vec->size() + 1,
                                          a.size() > 1 ? a[1].i : 0);
                    if (at > a[0].vec->size()) at = a[0].vec->size();
                    a[0].vec->insert(a[0].vec->begin() + (long)at,
                                     a.size() > 2 ? a[2] : Value::none());
                    return Value::none();
                });
            if (name == "remove")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& v = *a[0].vec;
                    const Value& target = a.size() > 1 ? a[1] : Value::none();
                    for (size_t i = 0; i < v.size(); ++i)
                        if (valEq(v[i], target)) {
                            Value gone = v[i];
                            v.erase(v.begin() + (long)i);
                            return gone;
                        }
                    panicHere("remove(): value not found");
                });
            if (name == "clear")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].vec->clear();
                    return Value::none();
                });
            if (name == "contains")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    const Value& needle = a.size() > 1 ? a[1] : Value::none();
                    for (const auto& el : *a[0].vec)
                        if (valEq(el, needle)) return Value::boolean(true);
                    return Value::boolean(false);
                });
            if (name == "index")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    const Value& needle = a.size() > 1 ? a[1] : Value::none();
                    for (size_t i = 0; i < a[0].vec->size(); ++i)
                        if (valEq((*a[0].vec)[i], needle))
                            return Value::integer((int64_t)i);
                    return Value::integer(-1);
                });
            if (name == "reverse")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::reverse(a[0].vec->begin(), a[0].vec->end());
                    return Value::none();
                });
            if (name == "join")
                return bind({}, [this](std::vector<Value>& a) -> Value {
                    std::string sep = a.size() > 1 ? toStr(a[1]) : "";
                    std::string out;
                    for (size_t i = 0; i < a[0].vec->size(); ++i) {
                        if (i) out += sep;
                        out += toStr((*a[0].vec)[i]);
                    }
                    return Value::str(out);
                });
            if (name == "filter")
                return bind({"pred"}, [this](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    std::vector<Value> snap = *a[0].vec;
                    for (auto& el : snap)
                        if (truthy(callValue(a[1], {el}, 0, 0)))
                            out.push_back(el);
                    return Value::list(std::move(out));
                });
            if (name == "map")
                return bind({"f"}, [this](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    std::vector<Value> snap = *a[0].vec;
                    for (auto& el : snap)
                        out.push_back(callValue(a[1], {el}, 0, 0));
                    return Value::list(std::move(out));
                });
            if (name == "sum")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    double acc = 0;
                    bool isInt = true;
                    for (const auto& el : *a[0].vec) {
                        if (el.k == VK::Int) acc += (double)el.i;
                        else if (el.k == VK::Float) {
                            acc += el.d;
                            isInt = false;
                        } else panicHere("sum(): non-numeric element");
                    }
                    return isInt ? Value::integer((int64_t)acc)
                                 : Value::floating(acc);
                });
            if (name == "sort")
                return bind({"key", "reverse"},
                            [this](std::vector<Value>& a) -> Value {
                                Value keyFn =
                                    a.size() > 1 && !a[1].isNone()
                                        ? a[1]
                                        : Value::none();
                                bool rev = a.size() > 2 && truthy(a[2]);
                                std::stable_sort(
                                    a[0].vec->begin(), a[0].vec->end(),
                                    [&](const Value& x, const Value& y) {
                                        Value kx = keyFn.k == VK::None
                                                       ? const_cast<Value&>(x)
                                                       : callValue(keyFn,
                                                                   {x}, 0, 0);
                                        Value ky = keyFn.k == VK::None
                                                       ? const_cast<Value&>(y)
                                                       : callValue(keyFn,
                                                                   {y}, 0, 0);
                                        return valLess(kx, ky);
                                    });
                                if (rev)
                                    std::reverse(a[0].vec->begin(),
                                                 a[0].vec->end());
return Value::none();
                             });
            break;
        }

        case VK::Gen: {
            if (name == "map")
                return bind({"f"}, [this](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    std::vector<Value> snap = *a[0].vec;
                    for (auto& el : snap)
                        out.push_back(callValue(a[1], {el}, 0, 0));
                    return Value::gen(std::move(out));
                });
            if (name == "filter")
                return bind({"pred"}, [this](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    std::vector<Value> snap = *a[0].vec;
                    for (auto& el : snap)
                        if (truthy(callValue(a[1], {el}, 0, 0)))
                            out.push_back(el);
                    return Value::gen(std::move(out));
                });
            if (name == "collect" || name == "to_list")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> c = *a[0].vec;
                    return Value::list(std::move(c));
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].vec->size());
                });
            break;
        }

        case VK::Dict: {
            if (name == "keys")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> ks;
                    for (auto& kv : *a[0].map) ks.push_back(kv.first);
                    return Value::list(std::move(ks));
                });
            if (name == "values")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> vs;
                    for (auto& kv : *a[0].map) vs.push_back(kv.second);
                    return Value::list(std::move(vs));
                });
            if (name == "items")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> its;
                    for (auto& kv : *a[0].map)
                        its.push_back(Value::tuple({kv.first, kv.second}));
                    return Value::list(std::move(its));
                });
            if (name == "get")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    for (auto& kv : *a[0].map)
                        if (valEq(kv.first, a.size() > 1 ? a[1]
                                                         : Value::none()))
                            return kv.second;
                    return a.size() > 2 ? a[2] : Value::none();
                });
            if (name == "has" || name == "contains_key")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    for (auto& kv : *a[0].map)
                        if (valEq(kv.first, a.size() > 1 ? a[1]
                                                         : Value::none()))
                            return Value::boolean(true);
                    return Value::boolean(false);
                });
            if (name == "pop")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& m = *a[0].map;
                    for (size_t i = 0; i < m.size(); ++i)
                        if (valEq(m[i].first,
                                  a.size() > 1 ? a[1] : Value::none())) {
                            Value gone = m[i].second;
                            m.erase(m.begin() + (long)i);
                            return gone;
                        }
                    return a.size() > 2 ? a[2] : Value::none();
                });
            if (name == "clear")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].map->clear();
                    return Value::none();
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].map->size());
                });
            if (name == "remove")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& m = *a[0].map;
                    const Value& k = a.size() > 1 ? a[1] : Value::none();
                    for (size_t i = 0; i < m.size(); ++i)
                        if (valEq(m[i].first, k)) {
                            m.erase(m.begin() + (long)i);
                            return Value::none();
                        }
                    panicHere("dict.remove(): key not found");
                });
            if (name == "contains")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    const Value& k = a.size() > 1 ? a[1] : Value::none();
                    for (auto& kv : *a[0].map)
                        if (valEq(kv.first, k)) return Value::boolean(true);
                    return Value::boolean(false);
                });
            if (name == "setdefault")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& m = *a[0].map;
                    const Value& k = a.size() > 1 ? a[1] : Value::none();
                    for (auto& kv : m)
                        if (valEq(kv.first, k)) return kv.second;
                    Value dv = a.size() > 2 ? a[2] : Value::none();
                    m.push_back({k, dv});
                    return dv;
                });
            break;
        }

        case VK::Set: {
            if (name == "add" || name == "insert")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    const Value& nv = a.size() > 1 ? a[1] : Value::none();
                    for (auto& el : *a[0].vec)
                        if (valEq(el, nv)) return Value::none();
                    a[0].vec->push_back(nv);
                    return Value::none();
                });
            if (name == "remove")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& v = *a[0].vec;
                    const Value& target = a.size() > 1 ? a[1] : Value::none();
                    for (size_t i = 0; i < v.size(); ++i)
                        if (valEq(v[i], target)) {
                            v.erase(v.begin() + (long)i);
                            return Value::none();
                        }
                    panicHere("set.remove(): value not found");
                });
            if (name == "contains")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    const Value& needle = a.size() > 1 ? a[1] : Value::none();
                    for (const auto& el : *a[0].vec)
                        if (valEq(el, needle)) return Value::boolean(true);
                    return Value::boolean(false);
                });
            if (name == "union")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> out = *a[0].vec;
                    if (a.size() > 1 && a[1].k == VK::Set)
                        for (auto& el : *a[1].vec) {
                            bool found = false;
                            for (auto& o : out)
                                if (valEq(o, el)) { found = true; break; }
                            if (!found) out.push_back(el);
                        }
                    return Value::set(std::move(out));
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].vec->size());
                });
            break;
        }

        case VK::Str: {
            if (name == "upper")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string s = a[0].s;
                    for (char& c : s) c = (char)toupper((unsigned char)c);
                    return Value::str(s);
                });
            if (name == "lower" || name == "to_lower")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string s = a[0].s;
                    for (char& c : s) c = (char)tolower((unsigned char)c);
                    return Value::str(s);
                });
            if (name == "to_upper")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string s = a[0].s;
                    for (char& c : s) c = (char)toupper((unsigned char)c);
                    return Value::str(s);
                });
            if (name == "to_int")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    try {
                        size_t pos = 0;
                        const std::string& s = a[0].s;
                        long long v = std::stoll(s, &pos);
                        while (pos < s.size() &&
                               isspace((unsigned char)s[pos]))
                            ++pos;
                        if (pos != s.size()) return Value::none();
                        return Value::integer(v);
                    } catch (...) {
                        return Value::none();
                    }
                });
            if (name == "c_ptr")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    Value p;
                    p.k = VK::Ptr;
                    p.s = a[0].s;
                    return p;
                });
            if (name == "trim" || name == "strip")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string s = a[0].s;
                    while (!s.empty() && isspace((unsigned char)s.front()))
                        s.erase(s.begin());
                    while (!s.empty() && isspace((unsigned char)s.back()))
                        s.pop_back();
                    return Value::str(s);
                });
            if (name == "split")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string sep = a.size() > 1 ? toStr(a[1]) : " ";
                    std::vector<Value> parts;
                    std::string s = a[0].s;
                    if (sep.empty()) {
                        for (char c : s)
                            parts.push_back(Value::str(std::string(1, c)));
                        return Value::list(std::move(parts));
                    }
                    size_t start = 0;
                    for (;;) {
                        size_t hit = s.find(sep, start);
                        parts.push_back(Value::str(
                            s.substr(start, hit == std::string::npos
                                                ? std::string::npos
                                                : hit - start)));
                        if (hit == std::string::npos) break;
                        start = hit + sep.size();
                    }
                    return Value::list(std::move(parts));
                });
            if (name == "starts_with" || name == "ends_with") {
                bool starts = name == "starts_with";
                return bind({}, [starts](std::vector<Value>& a) -> Value {
                    std::string pfx =
                        a.size() > 1 ? toStr(a[1]) : "";
                    bool r = starts
                                 ? a[0].s.rfind(pfx, 0) == 0
                                 : (pfx.size() <= a[0].s.size() &&
                                    a[0].s.compare(
                                        a[0].s.size() - pfx.size(),
                                        pfx.size(), pfx) == 0);
                    return Value::boolean(r);
                });
            }
            if (name == "contains")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string sub = a.size() > 1 ? toStr(a[1]) : "";
                    return Value::boolean(
                        a[0].s.find(sub) != std::string::npos);
                });
            if (name == "replace")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string from = a.size() > 1 ? toStr(a[1]) : "";
                    std::string to = a.size() > 2 ? toStr(a[2]) : "";
                    std::string s = a[0].s;
                    if (!from.empty())
                        for (size_t p = 0; (p = s.find(from, p)) !=
                                           std::string::npos;
                             p += to.size())
                            s.replace(p, from.size(), to);
                    return Value::str(s);
                });
            if (name == "chars")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> cs;
                    for (char c : a[0].s)
                        cs.push_back(Value::chr((char32_t)(unsigned char)c));
                    return Value::list(std::move(cs));
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].s.size());
                });
            if (name == "repeat")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t n = a.size() > 1 ? a[1].i : 0;
                    if (n <= 0) return Value::str("");
                    std::string out;
                    out.reserve(a[0].s.size() * (size_t)n);
                    for (int64_t i = 0; i < n; ++i) out += a[0].s;
                    return Value::str(out);
                });
            if (name == "find")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string sub = a.size() > 1 ? toStr(a[1]) : "";
                    size_t hit = a[0].s.find(sub);
                    return Value::integer(hit == std::string::npos
                                              ? -1
                                              : (int64_t)hit);
                });
            if (name == "capitalize")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::string s = a[0].s;
                    if (!s.empty())
                        s[0] = (char)toupper((unsigned char)s[0]);
                    return Value::str(s);
                });
            break;
        }

        default:
            break;
    }

    panicHere("value of this type has no member '" + name + "'");
}

// ---------------------------------------------------------------------------
// operators & equality
// ---------------------------------------------------------------------------

static bool valEq(const Value& a, const Value& b) {
    if (a.k != b.k) {
        if ((a.k == VK::Int || a.k == VK::Float) &&
            (b.k == VK::Int || b.k == VK::Float)) {
            double x = a.k == VK::Int ? (double)a.i : a.d;
            double y = b.k == VK::Int ? (double)b.i : b.d;
            return x == y;
        }
        return false;
    }
    switch (a.k) {
        case VK::None:  return true;
        case VK::Bool:  return a.b == b.b;
        case VK::Int:   return a.i == b.i;
        case VK::Float: return a.d == b.d;
        case VK::Char:  return a.ch == b.ch;
        case VK::Str:
        case VK::Bytes: return a.s == b.s;
        case VK::List:
        case VK::Tuple:
        case VK::Set:
            if (a.vec->size() != b.vec->size()) return false;
            for (size_t i = 0; i < a.vec->size(); ++i)
                if (!valEq((*a.vec)[i], (*b.vec)[i])) return false;
            return true;
        case VK::Dict: {
            if (a.map->size() != b.map->size()) return false;
            for (const auto& kv : *a.map) {
                bool found = false;
                for (const auto& kv2 : *b.map)
                    if (valEq(kv.first, kv2.first) &&
                        valEq(kv.second, kv2.second)) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
        case VK::Range:
            return a.lo == b.lo && a.hi == b.hi && a.inclusive == b.inclusive;
        case VK::Struct:
            if (a.typeName != b.typeName || a.fields.size() != b.fields.size())
                return false;
            for (size_t i = 0; i < a.fields.size(); ++i)
                if (!valEq(a.fields[i].second, b.fields[i].second))
                    return false;
            return true;
        case VK::Heap:  return a.heap == b.heap;
        case VK::EnumV: {
            if (a.typeName != b.typeName || a.variant != b.variant)
                return false;
            for (size_t i = 0; i < a.payload.size(); ++i)
                if (!valEq(a.payload[i], b.payload[i])) return false;
            return true;
        }
        case VK::Result:
            if (a.variant != b.variant) return false;
            return valEq(a.payload[0], b.payload[0]);
        default:
            return false;
    }
}

bool Interpreter::valuesEqual(const Value& a, const Value& b) {
    return valEq(a, b);
}

static bool valLess(const Value& l, const Value& r) {
    if (l.k == VK::Int && r.k == VK::Int) return l.i < r.i;
    if ((l.k == VK::Int || l.k == VK::Float) &&
        (r.k == VK::Int || r.k == VK::Float)) {
        double x = l.k == VK::Int ? (double)l.i : l.d;
        double y = r.k == VK::Int ? (double)r.i : r.d;
        return x < y;
    }
    if (l.k == VK::Str && r.k == VK::Str)   return l.s < r.s;
    if (l.k == VK::Char && r.k == VK::Char) return l.ch < r.ch;
    if (l.k == VK::Bool && r.k == VK::Bool) return (int)l.b < (int)r.b;
    panicHere("cannot order these values");
}

Value Interpreter::compareOne(const std::string& op, const Value& l,
                              const Value& r) {
    if (l.k == VK::Struct || l.k == VK::Heap) {
        static const std::
            unordered_map<std::string, std::string>
                CMPM = {{"<", "lt"}, {"<=", "le"}, {">", "gt"},
                        {">=", "ge"}, {"==", "eq"}, {"!=", "neq"}};
        const std::string& tn =
            l.k == VK::Heap ? l.heap->typeName : l.typeName;
        auto nm = CMPM.find(op);
        if (nm != CMPM.end()) {
            bool have = false;
            auto si = structs_.find(tn);
            if (si != structs_.end())
                for (const auto& m : si->second->body)
                    if (m->kind == ast::StKind::FuncDef && m->name == nm->second)
                        have = true;
            if (!have)
                for (const auto& imp : impls_) {
                    if (imp.typeName != tn) continue;
                    if (imp.methods.count(nm->second)) have = true;
                }
            if (have)
                return invokeMethod(l, nm->second, {r}, {}, globals_, 0, 0,
                                    nullptr);
        }
    }
    if (op == "==") return Value::boolean(valEq(l, r));
    if (op == "!=") return Value::boolean(!valEq(l, r));
    if (op == "<")  return Value::boolean(valLess(l, r));
    if (op == ">")  return Value::boolean(valLess(r, l));
    if (op == "<=") return Value::boolean(valLess(l, r) || valEq(l, r));
    if (op == ">=") return Value::boolean(!valLess(l, r));
    panicHere("unknown comparison '" + op + "'");
}

bool Interpreter::evalCmpChain(const Expr& b, Env env) {
    std::vector<const Expr*> operands;
    std::vector<std::string> ops;
    const Expr* cur = &b;
    while (cur->kind == ast::ExKind::Binary && isCmpOp(cur->op)) {
        ops.push_back(cur->op);
        operands.push_back(cur->rhs.get());
        cur = cur->lhs.get();
    }
    operands.push_back(cur);
    std::reverse(operands.begin(), operands.end());
    std::reverse(ops.begin(), ops.end());

    Value prev = eval(*operands[0], env);
    for (size_t k = 0; k < ops.size(); ++k) {
        Value next = eval(*operands[k + 1], env);
        if (!truthy(compareOne(ops[k], prev, next))) return false;
        prev = std::move(next);
    }
    return true;
}

Value Interpreter::binop(const std::string& op, const Value& l, const Value& r,
                         int line, int col) {
    if (l.k == VK::Struct || l.k == VK::Heap) {
        static const std::unordered_map<std::string, std::string> ARITH = {
            {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"},
            {"%", "mod"}};
        auto nm = ARITH.find(op);
        if (nm != ARITH.end()) {
            const std::string& tn =
                l.k == VK::Heap ? l.heap->typeName : l.typeName;
            bool have = false;
            auto si = structs_.find(tn);
            if (si != structs_.end())
                for (const auto& m : si->second->body)
                    if (m->kind == ast::StKind::FuncDef && m->name == nm->second)
                        have = true;
            if (!have)
                for (const auto& imp : impls_)
                    if (imp.typeName == tn && imp.methods.count(nm->second))
                        have = true;
            if (have)
                return invokeMethod(l, nm->second, {r}, {}, globals_, line,
                                    col, nullptr);
        }
    }

    auto numL = [&]() { return l.k == VK::Int ? (double)l.i : l.d; };
    auto numR = [&]() { return r.k == VK::Int ? (double)r.i : r.d; };

    if (op == "+") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i + r.i);
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(numL() + numR());
        if (l.k == VK::Str && r.k == VK::Str) return Value::str(l.s + r.s);
        if (l.k == VK::Str && r.k == VK::Char)
            return Value::str(l.s + toStr(r));
        if (l.k == VK::Char && r.k == VK::Str)
            return Value::str(toStr(l) + r.s);
        if (l.k == VK::Char && r.k == VK::Char)
            return Value::str(toStr(l) + toStr(r));
        if (l.k == VK::List && r.k == VK::List) {
            std::vector<Value> out = *l.vec;
            for (auto& el : *r.vec) out.push_back(el);
            return Value::list(std::move(out));
        }
    }
    if (op == "-") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i - r.i);
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(numL() - numR());
    }
    if (op == "*") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i * r.i);
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(numL() * numR());
        if (l.k == VK::Str && r.k == VK::Int) {
            std::string out;
            for (int64_t n = 0; n < r.i; ++n) out += l.s;
            return Value::str(out);
        }
        if (l.k == VK::List && r.k == VK::Int) {
            std::vector<Value> out;
            for (int64_t n = 0; n < r.i; ++n)
                for (auto& el : *l.vec) out.push_back(el);
            return Value::list(std::move(out));
        }
    }
    if (op == "/") {
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float)) {
            if (numR() == 0.0) panicHere("division by zero");
            return Value::floating(numL() / numR());
        }
    }
    if (op == "//") {
        if (l.k == VK::Int && r.k == VK::Int) {
            if (r.i == 0) panicHere("division by zero");
            int64_t q = l.i / r.i;
            if ((l.i % r.i != 0) && ((l.i < 0) != (r.i < 0))) --q;
            return Value::integer(q);
        }
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float)) {
            double d = std::floor(numL() / numR());
            return Value::floating(d);
        }
    }
    if (op == "%") {
        if (l.k == VK::Int && r.k == VK::Int) {
            if (r.i == 0) panicHere("modulo by zero");
            int64_t mres = l.i % r.i;
            if (mres != 0 && ((mres < 0) != (r.i < 0))) mres += r.i;
            return Value::integer(mres);
        }
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(std::fmod(std::fmod(numL(), numR()) +
                                                 numR(),
                                             numR()));
    }
    if (op == "**") {
        if (l.k == VK::Int && r.k == VK::Int && r.i >= 0) {
            int64_t p;
            ipow64(l.i, r.i, p);
            return Value::integer(p);
        }
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(std::pow(numL(), numR()));
    }
    if (op == "<<") {
        if (l.k == VK::Int && r.k == VK::Int && r.i >= 0 && r.i < 63)
            return Value::integer((int64_t)((uint64_t)l.i << r.i));
    }
    if (op == ">>") {
        if (l.k == VK::Int && r.k == VK::Int && r.i >= 0 && r.i < 63)
            return Value::integer(l.i >> r.i);
    }
    if (op == "&") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i & r.i);
    }
    if (op == "|") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i | r.i);
        if (l.k == VK::Set && r.k == VK::Set) {
            std::vector<Value> out = *l.vec;
            for (auto& el : *r.vec) {
                bool found = false;
                for (auto& o : out)
                    if (valEq(o, el)) { found = true; break; }
                if (!found) out.push_back(el);
            }
            return Value::set(std::move(out));
        }
    }
    if (op == "^") {
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i ^ r.i);
    }

    if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
        op == ">=")
        return compareOne(op, l, r);

    panicHere("unsupported operand types for '" + op + "'");
}

// ---------------------------------------------------------------------------
// constructors / iteration / patterns
// ---------------------------------------------------------------------------

Value Interpreter::makeStruct(const std::string& name,
                              const std::vector<ast::CallArg>& args, Env env) {
    auto si = structs_.find(name);
    if (si == structs_.end()) panicHere("unknown struct '" + name + "'");
    const Stmt& sd = *si->second;

    std::vector<const ast::CallArg*> positional;
    for (const auto& a : args)
        if (a.name.empty()) positional.push_back(&a);

    Value v = Value::structV(name);
    size_t pi = 0;
    for (const auto& fd : sd.fields) {
        bool done = false;
        for (const auto& a : args) {
            if (!a.name.empty() && a.name == fd.name) {
                v.fields.emplace_back(fd.name, eval(*a.value, env));
                done = true;
                break;
            }
        }
        if (done) continue;
        if (pi < positional.size()) {
            v.fields.emplace_back(fd.name, eval(*positional[pi]->value, env));
            ++pi;
            continue;
        }
        if (fd.defaultValue)
            v.fields.emplace_back(fd.name, eval(*fd.defaultValue, env));
        else if (auto def = containerDefaultFor(fd.type))
            v.fields.emplace_back(fd.name, std::move(*def));
        else
            v.fields.emplace_back(fd.name, Value::none());
    }
    return v;
}

std::optional<Value> Interpreter::containerDefaultFor(const ast::TypeP& t) {
    if (!t) return std::nullopt;
    if (t->kind == ast::TyKind::Name) {
        if (t->name == "dict") return Value::dict();
        if (t->name == "list") return Value::list();
        if (t->name == "set") return Value::set({});
    }
    if (t->kind == ast::TyKind::Optional) {
        auto inner = containerDefaultFor(t->inner);
        if (inner) return *inner;
    }
    return std::nullopt;
}

Value Interpreter::makeHeapValue(const std::string& name,
                                 const std::vector<ast::CallArg>& args,
                                 Env env) {
    auto si = structs_.find(name);
    if (si == structs_.end()) panicHere("unknown struct '" + name + "'");
    const Stmt& sd = *si->second;

    auto hobj = std::make_shared<HeapObj>();
    hobj->typeName = name;
    for (const auto& fd : sd.fields) {
        if (fd.weak) hobj->weakFields.push_back(fd.name);
        const ast::CallArg* arg = nullptr;
        for (const auto& a : args)
            if (a.name == fd.name) { arg = &a; break; }
        if (arg)
            hobj->fields.emplace_back(fd.name, eval(*arg->value, env));
        else if (fd.defaultValue)
            hobj->fields.emplace_back(fd.name,
                                      eval(*fd.defaultValue, env));
        else if (auto def = containerDefaultFor(fd.type))
            hobj->fields.emplace_back(fd.name, std::move(*def));
        else
            hobj->fields.emplace_back(fd.name, Value::none());
    }
    Value v;
    v.k = VK::Heap;
    v.heap = std::move(hobj);
    return v;
}

Value Interpreter::makeEnumV(const std::string& enumName,
                             const std::string& variant,
                             const std::vector<ast::CallArg>& args, Env env,
                             int line, int col) {
    (void)line;
    (void)col;
    auto ei = enums_.find(enumName);
    if (ei == enums_.end())
        panicHere("unknown enum '" + enumName + "'");
    const ast::Variant* vd = nullptr;
    for (const auto& var : ei->second->variants)
        if (var.name == variant) { vd = &var; break; }
    if (!vd)
        panicHere("enum '" + enumName + "' has no variant '" + variant + "'");

    std::vector<const ast::CallArg*> positional;
    for (const auto& a : args)
        if (a.name.empty()) positional.push_back(&a);

    Value v = Value::enumV(enumName, variant);
    size_t pi = 0;
    Env eenv = env ? env : globals_;
    for (const auto& pd : vd->payload) {
        bool done = false;
        for (const auto& a : args) {
            if (!a.name.empty() && a.name == pd.name) {
                v.payload.push_back(eval(*a.value, eenv));
                done = true;
                break;
            }
        }
        if (done) continue;
        if (pi < positional.size()) {
            v.payload.push_back(eval(*positional[pi]->value, eenv));
            ++pi;
            continue;
        }
        panicHere("missing payload '" + pd.name + "' for " + enumName + "." +
                  variant);
    }
    return v;
}

void Interpreter::iterateSeq(const Value& seq,
                             const std::function<bool(Value&, Env&)>& body,
                             Env env) {
    switch (seq.k) {
        case VK::Range: {
            int64_t hiEff = seq.hi < 0 ? 9007199254740992LL : seq.hi;
            int64_t end = seq.inclusive ? hiEff + 1 : hiEff;
            for (int64_t x = seq.lo; x < end; ++x) {
                Env child = makeChild(env);
                Value item = Value::integer(x);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::List:
        case VK::Set:
        case VK::Tuple:
        case VK::Gen: {
            std::vector<Value> snapshot = *seq.vec;
            for (auto& item : snapshot) {
                Env child = makeChild(env);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::Dict: {
            std::vector<Value> keys;
            for (auto& kv : *seq.map) keys.push_back(kv.first);
            for (auto& key : keys) {
                Env child = makeChild(env);
                if (!body(key, child)) return;
            }
            return;
        }
        case VK::Str: {
            for (char c : seq.s) {
                Env child = makeChild(env);
                Value item = Value::chr((char32_t)(unsigned char)c);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::Bytes: {
            for (char c : seq.s) {
                Env child = makeChild(env);
                Value item =
                    Value::integer((int64_t)(unsigned char)c);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::Chan: {
            for (;;) {
                auto s = seq.chan->recv();
                if (!s.got) return;
                Env child = makeChild(env);
                if (!body(s.v, child)) return;
            }
        }
        case VK::Struct:
        case VK::Heap: {
            // Iterator protocol: any type with next() works in for-in
            Value selfCopy = seq;
            for (;;) {
                Value nv = invokeMethod(selfCopy, "next", {}, {}, env, 0, 0,
                                        &selfCopy);
                if (nv.isNone()) return;
                Env child = makeChild(env);
                if (!body(nv, child)) return;
            }
        }
        default:
            panicHere("value is not iterable");
    }
}

bool Interpreter::matchPat(const ast::Pat& p, const Value& v, Env env) {
    switch (p.kind) {
        case ast::PatKind::Wild:
            return true;

        case ast::PatKind::Literal: {
            Value lit = eval(*p.literal, env);
            if (lit.k == VK::None) return v.k == VK::None;
            return valEq(lit, v);
        }

        case ast::PatKind::Range: {
            if (v.k != VK::Int && v.k != VK::Float &&
                v.k != VK::Char)
                return false;
            Value lo = eval(*p.lo->literal, env);
            Value hi = eval(*p.hi->literal, env);
            bool geLo, leHi;
            if (v.k == VK::Char) {
                geLo = v.ch >= lo.ch;
                leHi = p.inclusive ? v.ch <= hi.ch : v.ch < hi.ch;
            } else {
                double d = v.k == VK::Int ? (double)v.i : v.d;
                double dl = lo.k == VK::Int ? (double)lo.i : lo.d;
                double dh = hi.k == VK::Int ? (double)hi.i : hi.d;
                geLo = d >= dl;
                leHi = p.inclusive ? d <= dh : d < dh;
            }
            return geLo && leHi;
        }

        case ast::PatKind::Tuple: {
            if (v.k != VK::Tuple && v.k != VK::List) return false;
            if (!p.hasRest && v.vec->size() != p.elems.size())
                return false;
            if (p.hasRest && v.vec->size() < p.elems.size())
                return false;
            for (size_t i = 0; i < p.elems.size(); ++i)
                if (!matchPat(*p.elems[i], (*v.vec)[i], env)) return false;
            if (p.hasRest && !p.restName.empty() && p.restName != "_") {
                Value rest = v.k == VK::List
                                 ? Value::list({v.vec->begin() + (int)p.elems.size(),
                                                v.vec->end()})
                                 : Value::tuple({v.vec->begin() + (int)p.elems.size(),
                                                 v.vec->end()});
                env->vars[p.restName] = rest;
            }
            bindPat(p, v, env);
            return true;
        }

        case ast::PatKind::Ctor: {
            // result ok/err
            if (v.k == VK::Result &&
                (p.ctorName == "ok" || p.ctorName == "err")) {
                if (v.variant != p.ctorName) return false;
                for (size_t i = 0;
                     i < p.fields.size() && i < v.payload.size(); ++i)
                    if (!matchPat(*p.fields[i].pat, v.payload[i], env))
                        return false;
                bindPat(p, v, env);
                return true;
            }
            std::string wantVar = p.ctorName;
            size_t dot = wantVar.rfind('.');
            if (dot != std::string::npos) wantVar = wantVar.substr(dot + 1);
            bool dotted = dot != std::string::npos;

            // struct instance pattern: case Point(x: 0, y)
            if (v.k == VK::Struct || v.k == VK::Heap) {
                const std::string& tn =
                    v.k == VK::Heap ? v.heap->typeName : v.typeName;
                if (tn != wantVar && !(dotted && tn == p.ctorName))
                    return false;
                const std::vector<std::pair<std::string, Value>>& fs =
                    v.k == VK::Heap ? v.heap->fields : v.fields;
                for (size_t i = 0; i < p.fields.size(); ++i) {
                    const auto& pf = p.fields[i];
                    const Value* fv = nullptr;
                    if (!pf.name.empty()) {
                        for (const auto& f : fs)
                            if (f.first == pf.name) { fv = &f.second; break; }
                    } else if (i < fs.size()) {
                        fv = &fs[i].second;
                    }
                    if (!fv || !matchPat(*pf.pat, *fv, env)) return false;
                }
                bindPat(p, v, env);
                return true;
            }

            // enum variant pattern: case Running(pid) / State.Running(pid)
            if (v.k != VK::EnumV) return false;
            auto ei = enums_.find(v.typeName);
            if (ei == enums_.end()) return false;
            const ast::Variant* vd = nullptr;
            for (const auto& var : ei->second->variants)
                if (var.name == v.variant) { vd = &var; break; }
            if (!vd || v.variant != wantVar) return false;
            for (size_t i = 0; i < p.fields.size(); ++i) {
                const auto& pf = p.fields[i];
                size_t slotIdx = SIZE_MAX;
                if (!pf.name.empty()) {
                    for (size_t j = 0; j < vd->payload.size(); ++j)
                        if (vd->payload[j].name == pf.name) {
                            slotIdx = j;
                            break;
                        }
                    if (slotIdx == SIZE_MAX ||
                        slotIdx >= v.payload.size())
                        return false;
                } else {
                    slotIdx = i;
                    if (slotIdx >= v.payload.size()) return false;
                }
                if (!matchPat(*pf.pat, v.payload[slotIdx], env)) return false;
            }
            bindPat(p, v, env);
            return true;
        }

        case ast::PatKind::Slice: {
            // [p1, p2, ..rest] matches a list/tuple with at least the
            // front elements; the rest (if named) binds the remaining items.
            if (v.k != VK::List && v.k != VK::Tuple) return false;
            const auto& items = *v.vec;
            if (!p.hasRest && items.size() != p.elems.size())
                return false;
            if (p.hasRest && items.size() < p.elems.size())
                return false;
            for (size_t i = 0; i < p.elems.size(); ++i)
                if (!matchPat(*p.elems[i], items[i], env)) return false;
            if (p.hasRest && !p.restName.empty() && p.restName != "_") {
                Value rest = v.k == VK::List
                                 ? Value::list({items.begin() + (int)p.elems.size(),
                                                items.end()})
                                 : Value::tuple({items.begin() + (int)p.elems.size(),
                                                 items.end()});
                env->vars[p.restName] = rest;
            }
            return true;
        }

        case ast::PatKind::Rest:
            return true;

        case ast::PatKind::Or: {
            // p1 | p2 | ...: try each alternative; first match wins.
            for (const auto& alt : p.alts) {
                if (matchPat(*alt, v, env)) return true;
            }
            return false;
        }

        case ast::PatKind::BindAlias: {
            // pat @ name: the sub-pattern must match; then bind the whole
            // subject to the alias name.
            if (!p.aliasSub || !matchPat(*p.aliasSub, v, env)) return false;
            env->vars[p.bindName] = v;
            return true;
        }

        case ast::PatKind::Bind: {
            if (p.bindType && !typeTestMatches(v, p.bindType->name))
                return false;
            // a bare identifier that names a known unit variant acts as a
            // constructor pattern (case Idle:), not a capture
            if (!p.bindType && v.k == VK::EnumV) {
                bool known = false;
                for (const auto& kv : enums_) {
                    for (const auto& var : kv.second->variants)
                        if (var.name == p.bindName) { known = true; break; }
                    if (known) break;
                }
                if (known) return v.variant == p.bindName;
            }
            bindPat(p, v, env);
            return true;
        }

        case ast::PatKind::Ref:
            return p.inner ? matchPat(*p.inner, v, env) : false;
    }
    return false;
}

void Interpreter::bindPat(const ast::Pat& p, const Value& v, Env env) {
    switch (p.kind) {
        case ast::PatKind::Bind:
            if (p.bindName != "_") env->vars[p.bindName] = v;
            return;

        case ast::PatKind::Or:
            for (const auto& alt : p.alts)
                if (matchPat(*alt, v, env)) { bindPat(*alt, v, env); return; }
            return;

        case ast::PatKind::Slice:
            for (size_t i = 0; i < p.elems.size() && v.vec &&
                                i < v.vec->size(); ++i)
                bindPat(*p.elems[i], (*v.vec)[i], env);
            if (!p.restName.empty() && p.restName != "_" && v.vec) {
                Value rest = v.k == VK::List
                                 ? Value::list({v.vec->begin() + (int)p.elems.size(),
                                                v.vec->end()})
                                 : Value::tuple({v.vec->begin() + (int)p.elems.size(),
                                                 v.vec->end()});
                env->vars[p.restName] = rest;
            }
            return;

        case ast::PatKind::Rest:
            return;

        case ast::PatKind::BindAlias:
            if (p.aliasSub) bindPat(*p.aliasSub, v, env);
            if (p.bindName != "_") env->vars[p.bindName] = v;
            return;

        case ast::PatKind::Tuple:
            for (size_t i = 0; i < p.elems.size(); ++i)
                if (v.vec && i < v.vec->size())
                    bindPat(*p.elems[i], (*v.vec)[i], env);
            if (!p.restName.empty() && p.restName != "_" && v.vec &&
                p.elems.size() <= v.vec->size()) {
                Value rest = v.k == VK::List
                                 ? Value::list({v.vec->begin() + (int)p.elems.size(),
                                                v.vec->end()})
                                 : Value::tuple({v.vec->begin() + (int)p.elems.size(),
                                                 v.vec->end()});
                env->vars[p.restName] = rest;
            }
            return;

        case ast::PatKind::Ctor: {
            // result payloads are positional
            if (v.k == VK::Result) {
                for (size_t i = 0;
                     i < p.fields.size() && i < v.payload.size(); ++i)
                    bindPat(*p.fields[i].pat, v.payload[i], env);
                return;
            }
            // struct instance fields
            if (v.k == VK::Struct || v.k == VK::Heap) {
                const std::vector<std::pair<std::string, Value>>& fs =
                    v.k == VK::Heap ? v.heap->fields : v.fields;
                size_t posIdx = 0;
                for (const auto& pf : p.fields) {
                    if (!pf.name.empty()) {
                        for (const auto& f : fs)
                            if (f.first == pf.name) {
                                bindPat(*pf.pat, f.second, env);
                                break;
                            }
                    } else if (posIdx < fs.size()) {
                        bindPat(*pf.pat, fs[posIdx].second, env);
                        ++posIdx;
                    }
                }
                return;
            }
            // enum variant payload
            if (v.k == VK::EnumV) {
                auto ei = enums_.find(v.typeName);
                const ast::Variant* vd = nullptr;
                if (ei != enums_.end())
                    for (const auto& var : ei->second->variants)
                        if (var.name == v.variant) { vd = &var; break; }
                size_t posIdx = 0;
                for (const auto& pf : p.fields) {
                    if (!pf.name.empty()) {
                        if (!vd) continue;
                        for (size_t j = 0; j < vd->payload.size(); ++j)
                            if (vd->payload[j].name == pf.name) {
                                if (j < v.payload.size())
                                    bindPat(*pf.pat, v.payload[j], env);
                                break;
                            }
                    } else {
                        if (posIdx < v.payload.size())
                            bindPat(*pf.pat, v.payload[posIdx], env);
                        ++posIdx;
                    }
                }
            }
            return;
        }
        case ast::PatKind::Ref:
            if (p.inner) bindPat(*p.inner, v, env);
            return;
        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// lvalues, assignment, deferral
// ---------------------------------------------------------------------------

Value* Interpreter::lvaluePtr(const Expr& e, Env env) {
    switch (e.kind) {
        case ast::ExKind::Ident:
            return env->findRef(e.text);

        case ast::ExKind::Index: {
            Value* base = lvaluePtr(*e.lhs, env);
            if (!base) return nullptr;
            Value idx = eval(*e.rhs, env);
            if (base->k == VK::Weak) *base = lockHeap(*base);
            if (base->k == VK::List || base->k == VK::Set ||
                base->k == VK::Tuple)
                return &(*base->vec)[normIndex((int64_t)base->vec->size(),
                                               idx.i)];
            if (base->k == VK::Dict) {
                for (auto& kv : *base->map)
                    if (valEq(kv.first, idx)) return &kv.second;
                base->map->emplace_back(idx, Value::none());
                return &base->map->back().second;
            }
            return nullptr;
        }

        case ast::ExKind::Member: {
            Value* base = lvaluePtr(*e.lhs, env);
            if (!base) return nullptr;
            if (base->k == VK::Weak) *base = lockHeap(*base);
            if (base->k == VK::Struct || base->k == VK::Heap) {
                std::vector<std::pair<std::string, Value>>& fs =
                    base->k == VK::Heap ? base->heap->fields : base->fields;
                for (auto& f : fs)
                    if (f.first == e.text) return &f.second;
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

void Interpreter::memberWrite(Value& obj, const std::string& name, Value v,
                              int line, int col) {
    (void)line; (void)col;
    if (obj.k == VK::Struct) {
        for (auto& f : obj.fields)
            if (f.first == name) {
                f.second = std::move(v);
                return;
            }
    } else if (obj.k == VK::Heap) {
        bool weakSlot = false;
        for (const auto& w : obj.heap->weakFields)
            if (w == name) weakSlot = true;
        for (auto& f : obj.heap->fields)
            if (f.first == name) {
                if (weakSlot && v.k == VK::Heap) {
                    Value wv;
                    wv.k = VK::Weak;
                    wv.weak = v.heap;
                    f.second = std::move(wv);
                } else {
                    f.second = std::move(v);
                }
                return;
            }
    }
    panicHere("cannot assign to member '" + name + "'");
}

void Interpreter::bindDecl(const Expr& target, Value v, Env env) {
    switch (target.kind) {
        case ast::ExKind::Ident:
            env->vars[target.text] = std::move(v);
            return;
        case ast::ExKind::Tuple:
        case ast::ExKind::List: {
            if ((v.k == VK::Tuple || v.k == VK::List) &&
                v.vec->size() == target.elems.size()) {
                for (size_t i = 0; i < target.elems.size(); ++i)
                    bindDecl(*target.elems[i], (*v.vec)[i], env);
                return;
            }
            panicHere("cannot destructure declaration value");
        }
        default:
            assignTo(target, std::move(v), env);
            return;
    }
}

void Interpreter::assignTo(const Expr& target, Value v, Env env) {
    switch (target.kind) {
        case ast::ExKind::Ident: {
            if (Value* ref = env->findRef(target.text)) {
                *ref = std::move(v);
                return;
            }
            env->vars[target.text] = std::move(v);
            return;
        }

        case ast::ExKind::Tuple:
        case ast::ExKind::List: {
            if ((v.k == VK::Tuple || v.k == VK::List) &&
                v.vec->size() == target.elems.size()) {
                for (size_t i = 0; i < target.elems.size(); ++i)
                    assignTo(*target.elems[i], (*v.vec)[i], env);
                return;
            }
            panicHere("cannot destructure assignment value");
        }

        case ast::ExKind::Index: {
            // Prefer an in-place lvalue so mutations to a member-backed
            // container (e.g. self.buckets[k] = v with value semantics) land
            // in the stored field instead of a discarded copy.
            if (Value* rp = lvaluePtr(*target.lhs, env)) {
                Value idx2 = eval(*target.rhs, env);
                if (rp->k == VK::List || rp->k == VK::Set ||
                    rp->k == VK::Tuple)
                    (*rp->vec)[normIndex((int64_t)rp->vec->size(), idx2.i)] =
                        std::move(v);
                else if (rp->k == VK::Dict) {
                    for (auto& kv : *rp->map)
                        if (valEq(kv.first, idx2)) {
                            kv.second = std::move(v);
                            return;
                        }
                    rp->map->emplace_back(std::move(idx2), std::move(v));
                }
                return;
            }
            Value cont = eval(*target.lhs, env);
            if (cont.k == VK::Weak) cont = lockHeap(cont);
            Value idx = eval(*target.rhs, env);
            switch (cont.k) {
                case VK::List:
                case VK::Set:
                case VK::Tuple:
                    (*cont.vec)[normIndex((int64_t)cont.vec->size(), idx.i)] =
                        std::move(v);
                    return;
                case VK::Dict:
                    for (auto& kv : *cont.map)
                        if (valEq(kv.first, idx)) {
                            kv.second = std::move(v);
                            return;
                        }
                    cont.map->emplace_back(std::move(idx), std::move(v));
                    return;
                default:
                    panicHere("unsupported index assignment");
            }
        }

        case ast::ExKind::Member: {
            // resolve the BASE as an lvalue so mutations land in the stored
            // struct/heap object itself (value semantics preserved)
            if (Value* ref = lvaluePtr(*target.lhs, env)) {
                memberWrite(*ref, target.text, std::move(v),
                            target.span.line, target.span.col);
                return;
            }
            Value base = eval(*target.lhs, env);
            if (base.k == VK::Weak) base = lockHeap(base);
            memberWrite(base, target.text, std::move(v), target.span.line,
                        target.span.col);
            return;
        }

        default:
            panicHere("invalid assignment target");
    }
}

Deferred Interpreter::prepareDeferred(const Expr& callExpr, Env env) {
    Deferred d;
    if (callExpr.kind != ast::ExKind::Call)
        panicHere("defer expects a call expression");
    for (const auto& a : callExpr.args)
        d.args.push_back(eval(*a.value, env));

    const Expr& ce = *callExpr.lhs;
    if (ce.kind == ast::ExKind::Member) {
        Value recv = eval(*ce.lhs, env);
        d.callee = memberRead(recv, ce.text, ce.span.line, ce.span.col);
    } else if (ce.kind == ast::ExKind::Ident) {
        const std::string& nm = ce.text;
        if (const Value* v = env->find(nm)) {
            d.callee = *v;
        } else {
            auto fi = funcs_.find(nm);
            if (fi == funcs_.end())
                panicHere("undefined function '" + nm + "' in defer");
            d.callee.k = VK::Fn;
            d.callee.fn = fi->second;
            d.callee.env = globals_;
        }
    } else {
        d.callee = eval(ce, env);
    }
    return d;
}

// ---------------------------------------------------------------------------
// formatting
// ---------------------------------------------------------------------------

std::string Interpreter::applySpec(const std::string& text,
                                   const std::string& spec) {
    if (spec.empty()) return text;
    char fill = ' ';
    char align = 0;
    size_t i = 0;
    if (spec.size() > i + 1 && strchr("<^>", spec[i + 1])) {
        fill = spec[i];
        align = spec[i + 1];
        i += 2;
    } else if (i < spec.size() && strchr("<^>", spec[i])) {
        align = spec[i];
        ++i;
    }
    std::string wnum;
    while (i < spec.size() && isdigit((unsigned char)spec[i]))
        wnum += spec[i++];
    int width = wnum.empty() ? 0 : atoi(wnum.c_str());
    (void)i;
    if ((int)text.size() >= width) return text;
    int pad = width - (int)text.size();
    switch (align) {
        case '<': return text + std::string(pad, fill);
        case '>': return std::string(pad, fill) + text;
        case '^': {
            int left = pad / 2;
            return std::string(left, fill) + text +
                   std::string(pad - left, fill);
        }
        default:
            return text + std::string(pad, fill);
    }
}

Value Interpreter::formatFString(const Expr& e, Env env) {
    std::string out;
    for (const auto& part : e.parts) {
        if (!part.isExpr) {
            out += decodeEscapes(part.text);
            continue;
        }
        Value v = eval(*part.expr, env);
        out += applySpec(toStr(v), part.spec);
    }
    return Value::str(out);
}

Value Interpreter::sortList(Value listVal,
                            const std::vector<ast::CallArg>& args, Env env) {
    Value keyFn = Value::none();
    bool rev = false;
    for (const auto& a : args) {
        if (a.name == "key") keyFn = eval(*a.value, env);
        else if (a.name == "reverse") rev = truthy(eval(*a.value, env));
        else if (a.name.empty()) keyFn = eval(*a.value, env);
    }
    std::stable_sort(listVal.vec->begin(), listVal.vec->end(),
                     [&](const Value& x, const Value& y) {
                         Value kx = keyFn.k == VK::None
                                        ? x
                                        : callValue(keyFn, {x}, 0, 0);
                         Value ky = keyFn.k == VK::None
                                        ? y
                                        : callValue(keyFn, {y}, 0, 0);
                         return valLess(kx, ky);
                     });
    if (rev) std::reverse(listVal.vec->begin(), listVal.vec->end());
    return listVal;
}

// ---------------------------------------------------------------------------
// concurrency
// ---------------------------------------------------------------------------

Value Interpreter::spawnCall(const Expr& callExpr, Env env) {
    const Expr* c = &callExpr;
    while (c->kind == ast::ExKind::Try) c = c->lhs.get();

    Value callee;
    std::vector<Value> args;
    std::vector<std::pair<std::string, Value>> named;

    auto resolveCallee = [&](const Expr& ce) {
        if (ce.kind == ast::ExKind::Member) {
            Value recv = eval(*ce.lhs, env);
            callee = memberRead(recv, ce.text, ce.span.line, ce.span.col);
        } else if (ce.kind == ast::ExKind::Ident) {
            const std::string& nm = ce.text;
            if (const Value* v = env->find(nm)) {
                callee = *v;
            } else {
                auto fi = funcs_.find(nm);
                if (fi == funcs_.end())
                    panicHere("undefined function '" + nm + "' in spawn");
                callee.k = VK::Fn;
                callee.fn = fi->second;
                callee.env = globals_;
            }
        } else {
            callee = eval(ce, env);
        }
    };

    if (c->kind == ast::ExKind::Call) {
        resolveCallee(*c->lhs);
        for (const auto& a : c->args) {
            if (a.name.empty()) args.push_back(eval(*a.value, env));
            else named.emplace_back(a.name, eval(*a.value, env));
        }
    } else {
        resolveCallee(*c);                  // `spawn helper` with no parens
    }

    if (!named.empty()) {
        std::vector<std::string> slots;
        if (callee.k == VK::Fn && callee.fn) {
            for (const auto& p : callee.fn->params) slots.push_back(p.name);
        } else if (callee.k == VK::Builtin) {
            slots = callee.biParams;
        }
        if (slots.empty())
            panicHere("named arguments unsupported for spawned callee");
        mapNamedIntoPos(slots, args, named);
    }

    auto t = std::make_shared<ThreadImpl>();
    {
        std::lock_guard<std::mutex> lk(threadsM_);
        threads_.push_back(t);
    }
    Value calleeCopy = callee;
    t->th = std::thread([this, calleeCopy, args]() {
        threadEntry(calleeCopy, args);
    });

    Value h;
    h.k = VK::ThreadH;
    h.thread = t;
    return h;
}

void Interpreter::threadEntry(Value callee, std::vector<Value> args) {
    try {
        callValue(std::move(callee), std::move(args), 0, 0);
} catch (const PanicSignal& p) {
        std::string msg = "panic in spawned thread: " + p.msg;
        for (const auto& f : p.frames) msg += "\n  " + f;
        fputs((msg + "\n").c_str(), stderr);
    } catch (...) {
    }
}

Value Interpreter::selectExec(const Stmt& s, Env env) {
    struct Prepared {
        enum Kind { Recv, Timer, Gate } kind = Kind::Gate;
        Value cachedChan;                      // Recv
        Value cached;                          // Timer/Gate
        const ast::Stmt::SelArm* arm = nullptr;
    };
    std::vector<Prepared> prepared;
    for (const auto& sa : s.selArms) {
        Prepared pr;
        pr.arm = &sa;
        const Expr* op = sa.chanOp.get();
        bool recvCall = op->kind == ast::ExKind::Call && op->lhs &&
                        op->lhs->kind == ast::ExKind::Member &&
                        op->lhs->text == "recv";
        if (recvCall) {
            pr.kind = Prepared::Recv;
            pr.cachedChan = eval(*op->lhs->lhs, env);
        } else {
            Value cv = eval(*op, env);
            if (cv.k == VK::Chan) {
                pr.kind = Prepared::Recv;
                pr.cachedChan = cv;
            } else if (cv.k == VK::Timer) {
                pr.kind = Prepared::Timer;
                pr.cached = cv;
            } else {
                pr.kind = Prepared::Gate;
                pr.cached = cv;
            }
        }
        prepared.push_back(std::move(pr));
    }

    for (;;) {
        for (const auto& pr : prepared) {
            bool ready = false;
            Value payload;
            switch (pr.kind) {
                case Prepared::Recv: {
                    auto slot = pr.cachedChan.chan->tryRecv();
                    if (slot.got) {
                        ready = true;
                        payload = slot.v;
                    }
                    break;
                }
                case Prepared::Timer:
                    ready = nowMs() >= pr.cached.deadlineMs;
                    break;
                case Prepared::Gate:
                    ready = truthy(pr.cached);
                    if (ready) payload = pr.cached;
                    break;
            }
            if (!ready) continue;
            Env aenv = makeChild(env);
            if (!pr.arm->bind.empty())
                aenv->vars[pr.arm->bind] = payload;
            execBlock(pr.arm->body, aenv);
            return Value::none();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Interpreter::shutdownThreads() {
    std::vector<std::shared_ptr<ThreadImpl>> local;
    {
        std::lock_guard<std::mutex> lk(threadsM_);
        local.swap(threads_);
    }
    for (auto& t : local)
        if (t && t->th.joinable()) t->th.detach();
}

// ---------------------------------------------------------------------------
// PLAN Phase 4: bytecode VM (core slice). Defined in this translation unit so
// it can reuse the anonymous-namespace helpers (normIndex, sliceOf,
// decodeCharText, lockHeap, typeTestMatches) and evaluate exactly like eval().
// ---------------------------------------------------------------------------

void Interpreter::enableVm() {
    if (vmEnabled_) return;
    if (!program_) panicHere("internal: enableVm without a program");
    if (!collected_) { collected_ = true; collectProgram(*program_); }
    const Stmt* mainFn = nullptr;
    auto it = funcs_.find("main");
    if (it != funcs_.end()) mainFn = it->second;

    std::unordered_set<std::string> uf;
    for (const auto& kv : funcs_) uf.insert(kv.first);
    std::unordered_set<std::string> bi = {
        "print","len","sqrt","ord","chr","assert","assert_eq","range","panic",
        "catch_panic","printf","strlen","str","int","float","bool","type","sum",
        "min","max","any","all","sorted","reversed","enumerate","map","filter",
        "reduce","upper","lower","trim","contains","starts_with","ends_with",
        "replace","split","join",
    };
    std::unordered_set<std::string> mod = {
        "math","time","io","mem","json","text","os",
    };
    vmProg_ = std::make_unique<vm::CompileResult>(
        vm::compileProgram(program_->body, mainFn, uf, bi, mod));
    if (envGet("COCO_VM_DUMP")) {
        for (size_t i = 0; i < vmProg_->prog.funcs.size(); ++i) {
            const vm::VmFunction& F = vmProg_->prog.funcs[i];
            std::cerr << "[vm fn " << i << "] '" << F.name << "' interp="
                      << F.interpreted << " ins=" << F.code.size() << "\n";
            for (const auto& I : F.code)
                std::cerr << "    " << (int)I.op << " a=" << I.a
                          << " b=" << I.b << " c=" << I.c << "\n";
        }
    }
    vmEnabled_ = true;
}

const vm::VmFunction* Interpreter::vmFuncFor(const Stmt* fn) const {
    if (!vmEnabled_ || !vmProg_) return nullptr;
    auto it = vmProg_->defIdx.find(fn);
    if (it == vmProg_->defIdx.end()) return nullptr;
    if ((size_t)it->second >= vmProg_->prog.funcs.size()) return nullptr;
    const vm::VmFunction& vf = vmProg_->prog.funcs[(size_t)it->second];
    return vf.interpreted ? nullptr : &vf;
}

void Interpreter::enableNative() { nativeEnabled_ = true; }

void Interpreter::registerNative(const Stmt* fn, NativeBody body) {
    nativeFns_[fn] = std::move(body);
}

Value Interpreter::nativeEnvVar(const Env& e, const std::string& name) {
    if (const Value* v = e ? e->find(name) : nullptr) return *v;
    panicHere("native: unbound variable '" + name + "'");
}

const Interpreter::NativeBody* Interpreter::nativeFuncFor(const Stmt* fn) const {
    if (!nativeEnabled_) return nullptr;
    auto it = nativeFns_.find(fn);
    return it != nativeFns_.end() ? &it->second : nullptr;
}

Value Interpreter::nativeRunBody(const NativeBody& nb, Env fenv,
                                 const Stmt* fn) {
    Value r = nb(fenv);
    if (isResultRet(*fn)) return Value::resultOk(std::move(r));
    return r;
}

Value Interpreter::makeEnumVPos(const std::string& enumName,
                                const std::string& variant,
                                std::vector<Value> pos, Env env) {
    (void)env;
    auto ei = enums_.find(enumName);
    if (ei == enums_.end()) panicHere("unknown enum '" + enumName + "'");
    const ast::Variant* vd = nullptr;
    for (const auto& var : ei->second->variants)
        if (var.name == variant) { vd = &var; break; }
    if (!vd)
        panicHere("enum '" + enumName + "' has no variant '" + variant + "'");
    Value v = Value::enumV(enumName, variant);
    size_t pi = 0;
    for (const auto& pd : vd->payload) {
        if (pi < pos.size()) v.payload.push_back(std::move(pos[pi++]));
        else
            panicHere("missing payload '" + pd.name + "' for " + enumName + "." +
                      variant);
    }
    return v;
}

Value Interpreter::vmRunBody(const vm::VmFunction& vf, Env fenv) {
    const auto& code = vf.code;
    const auto& SCC = vf.strConsts;
    const auto& K = vf.constants;

    // Compact 16-byte VM operand. Hot-path values (None/Bool/Int/Float/Char) are
    // stored inline; every other kind is boxed on the heap as a full `Value`.
    // This removes the ~472-byte Value copies that dominated the operand stack.
    struct VmVal {
        VK k = VK::None;
        union { bool b; int64_t i; double d; char32_t ch; Value* box; } u{};
        VmVal() {}
        VmVal(const VmVal& o) : k(o.k) {
            if (k == VK::None) { /* nothing */ }
            else if (k == VK::Bool) u.b = o.u.b;
            else if (k == VK::Int) u.i = o.u.i;
            else if (k == VK::Float) u.d = o.u.d;
            else if (k == VK::Char) u.ch = o.u.ch;
            else u.box = new Value(*o.u.box);
        }
        VmVal(VmVal&& o) noexcept : k(o.k) {
            if (k == VK::None) { /* nothing */ }
            else if (k == VK::Bool) u.b = o.u.b;
            else if (k == VK::Int) u.i = o.u.i;
            else if (k == VK::Float) u.d = o.u.d;
            else if (k == VK::Char) u.ch = o.u.ch;
            else u.box = o.u.box;
            o.k = VK::None;
        }
        VmVal& operator=(const VmVal& o) {
            if (this == &o) return *this;
            if (k != VK::None && k != VK::Bool && k != VK::Int &&
                k != VK::Float && k != VK::Char) delete u.box;
            k = o.k;
            if (k == VK::None) { /* nothing */ }
            else if (k == VK::Bool) u.b = o.u.b;
            else if (k == VK::Int) u.i = o.u.i;
            else if (k == VK::Float) u.d = o.u.d;
            else if (k == VK::Char) u.ch = o.u.ch;
            else u.box = new Value(*o.u.box);
            return *this;
        }
        VmVal& operator=(VmVal&& o) noexcept {
            if (this == &o) return *this;
            if (k != VK::None && k != VK::Bool && k != VK::Int &&
                k != VK::Float && k != VK::Char) delete u.box;
            k = o.k;
            if (k == VK::None) { /* nothing */ }
            else if (k == VK::Bool) u.b = o.u.b;
            else if (k == VK::Int) u.i = o.u.i;
            else if (k == VK::Float) u.d = o.u.d;
            else if (k == VK::Char) u.ch = o.u.ch;
            else u.box = o.u.box;
            o.k = VK::None;
            return *this;
        }
        ~VmVal() {
            if (k != VK::None && k != VK::Bool && k != VK::Int &&
                k != VK::Float && k != VK::Char) delete u.box;
        }
        bool isScalar() const {
            return k == VK::None || k == VK::Bool || k == VK::Int ||
                   k == VK::Float || k == VK::Char;
        }
        Value take() {   // consume: return full Value, leave None
            if (k == VK::None) { return Value::none(); }
            if (k == VK::Bool) { Value v = Value::boolean(u.b); k = VK::None; return v; }
            if (k == VK::Int)  { Value v = Value::integer(u.i); k = VK::None; return v; }
            if (k == VK::Float){ Value v = Value::floating(u.d); k = VK::None; return v; }
            if (k == VK::Char) { Value v = Value::chr(u.ch);    k = VK::None; return v; }
            Value v = std::move(*u.box);
            k = VK::None;
            delete u.box;
            return v;
        }
    };
    auto mkScalar = [](VK kk, int64_t i, double d, bool b, char32_t ch) -> VmVal {
        VmVal v; v.k = kk;
        if (kk == VK::Bool) v.u.b = b; else if (kk == VK::Float) v.u.d = d;
        else if (kk == VK::Char) v.u.ch = ch; else v.u.i = i;
        return v;
    };
    auto mkBox = [](Value&& x) -> VmVal {   // box a non-scalar value
        VmVal v; v.k = x.k; v.u.box = new Value(std::move(x)); return v;
    };
    // Lift a full Value into a VmVal (inline if scalar, else box).
    auto fromVal = [&mkScalar, &mkBox](Value&& x) -> VmVal {
        switch (x.k) {
            case VK::None: return VmVal();
            case VK::Bool: return mkScalar(VK::Bool, 0, 0, x.b, 0);
            case VK::Int:  return mkScalar(VK::Int, x.i, 0, false, 0);
            case VK::Float:return mkScalar(VK::Float, 0, x.d, false, 0);
            case VK::Char: return mkScalar(VK::Char, 0, 0, false, x.ch);
            default:       return mkBox(std::move(x));
        }
    };

    // ---- Specialized numeric binary/unary fast paths (no string compare) ----
    // Opcode encodes the operator as an integer, so the hot arithmetic/comparison
    // loop dispatches on a small int instead of comparing operator strings. Each
    // fast path operates entirely on unboxed VmVal scalars and pushes a scalar
    // result; on any non-scalar operand it returns false so the caller falls back
    // to the exact tree-walker helper (binop / compareOne) via take().
    enum BinK : uint8_t {
        B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_POW,
        B_LT, B_LE, B_GT, B_GE, B_EQ, B_NE
    };
    auto binFast = [](BinK k, const VmVal& l, const VmVal& r, VmVal& out) -> bool {
        if (l.k != VK::Int && l.k != VK::Float) return false;
        if (r.k != VK::Int && r.k != VK::Float) return false;
        switch (k) {
            case B_ADD: case B_SUB: case B_MUL: case B_DIV: case B_MOD: case B_POW: {
                if (l.k == VK::Float || r.k == VK::Float) {
                    double a = l.k == VK::Float ? l.u.d : (double)l.u.i;
                    double b = r.k == VK::Float ? r.u.d : (double)r.u.i;
                    double res;
                    if (k == B_ADD) res = a + b;
                    else if (k == B_SUB) res = a - b;
                    else if (k == B_MUL) res = a * b;
                    else if (k == B_DIV) res = a / b;
                    else if (k == B_MOD) res = std::fmod(a, b);
                    else res = std::pow(a, b);
                    out.k = VK::Float; out.u.d = res; return true;
                }
                int64_t a = l.u.i, b = r.u.i, res;
                if (k == B_ADD) res = a + b;
                else if (k == B_SUB) res = a - b;
                else if (k == B_MUL) res = a * b;
                else if (k == B_DIV) { out.k = VK::Float; out.u.d = (double)a / (double)b; return true; }
                else if (k == B_MOD) res = a % b;
                else { out.k = VK::Float; out.u.d = std::pow((double)a, (double)b); return true; }
                out.k = VK::Int; out.u.i = res; return true;
            }
            default: { // comparisons
                bool res;
                if (l.k == VK::Float || r.k == VK::Float) {
                    double a = l.k == VK::Float ? l.u.d : (double)l.u.i;
                    double b = r.k == VK::Float ? r.u.d : (double)r.u.i;
                    if (k == B_LT) res = a < b; else if (k == B_LE) res = a <= b;
                    else if (k == B_GT) res = a > b; else if (k == B_GE) res = a >= b;
                    else if (k == B_EQ) res = a == b; else res = a != b;
                } else {
                    int64_t a = l.u.i, b = r.u.i;
                    if (k == B_LT) res = a < b; else if (k == B_LE) res = a <= b;
                    else if (k == B_GT) res = a > b; else if (k == B_GE) res = a >= b;
                    else if (k == B_EQ) res = a == b; else res = a != b;
                }
                out.k = VK::Bool; out.u.b = res; return true;
            }
        }
    };
    auto slowBin = [this, &fromVal](const char* op, VmVal l, VmVal r) -> VmVal {
        Value lv = l.take(); Value rv = r.take();
        std::string so(op);
        if (so == "<" || so == "<=" || so == ">" || so == ">=")
            return fromVal(compareOne(so, lv, rv));
        return fromVal(binop(so, lv, rv, 0, 0));
    };

    // Flat operand stack with an explicit stack pointer (index-SP). The vector
    // is pre-sized once so push/pop reuse slots via `st[sp]`/`st[sp-1]` instead
    // of issuing push_back/pop_back (with their size/capacity bookkeeping and
    // element destruction) per op. It still grows on the heap if ever overflowed,
    // so it is safe for deep recursion (no fixed call-stack buffer).
    std::vector<VmVal> st;
    size_t sp = 0;
    struct VmIter {
        enum Kind { Vec, Range } kind = Vec;
        std::vector<Value> vec;   // Vec mode: materialized elements
        size_t idx = 0;           // Vec mode: next read position
        int64_t cur = 0, end = 0; // Range mode: next value / exclusive end
    };
    std::vector<VmIter> iters;
    size_t pc = 0;
    Env env = fenv;
    st.reserve(1024);
    // Frame-level slot storage. Params are bound into fenv by runFunc before
    // entering here; preload them so OP_LOAD_LOCAL/OP_STORE_LOCAL hit locals[].
    std::vector<VmVal> locals;
    if (vf.useSlots) {
        locals.resize(vf.slotNames.size());
        for (size_t i = 0; i < vf.slotNames.size(); ++i) {
            auto it = fenv->vars.find(vf.slotNames[i]);
            if (it != fenv->vars.end()) locals[i] = fromVal(std::move(it->second));
        }
    }

    auto pop = [&]() -> VmVal {
        VmVal v = std::move(st[sp - 1]);
        --sp;
        return v;
    };
    auto push = [&](VmVal v) {
        if (sp < st.size()) st[sp] = std::move(v);
        else st.push_back(std::move(v));
        ++sp;
    };
    // Compiler emits relative offsets measured from the jump instruction's own
    // index (a = target - here()). The dispatch loop has already advanced `pc`
    // past it (code[pc++]), so subtract one to land exactly on the target.
    auto jmp = [&](int32_t off) { pc = (size_t)((int64_t)pc + (int64_t)off - 1); };

    while (pc < code.size()) {
        const Ins& ins = code[pc++];
        switch (ins.op) {
            case OP_NONEZ: push(VmVal()); break;
            case OP_TRUE: push(mkScalar(VK::Bool, 0, 0, true, 0)); break;
            case OP_FALSE: push(mkScalar(VK::Bool, 0, 0, false, 0)); break;
            case OP_BOOL: { VmVal v = pop(); Value fv = v.take(); push(fromVal(Value::boolean(truthy(fv)))); break; }
            case OP_PUSH_CONST_STR: push(fromVal(Value::str(SCC[(size_t)ins.a]))); break;
            case OP_INT: case OP_FLOAT: case OP_CHAR:
            case OP_STR: case OP_STR_RAW: case OP_STR_BYTES: case OP_STR_C:
                push(fromVal(Value(K[(size_t)ins.a]))); break;

            case OP_LOAD_LOCAL: push(locals[(size_t)ins.a]); break;

            case OP_LOAD: {
                const std::string& n = SCC[(size_t)ins.a];
                if (const Value* v = env->find(n)) { push(fromVal(Value(*v))); break; }
                auto fi = funcs_.find(n);
                if (fi != funcs_.end()) {
                    Value f; f.k = VK::Fn; f.fn = fi->second; f.env = globals_;
                    push(mkBox(std::move(f))); break;
                }
                if (structs_.count(n)) {
                    Value m; m.k = VK::Module; m.typeName = "__struct__:" + n;
                    push(mkBox(std::move(m))); break;
                }
                if (enums_.count(n)) {
                    Value m; m.k = VK::Module; m.typeName = "__enum__:" + n;
                    push(mkBox(std::move(m))); break;
                }
                panicHere("undefined variable '" + n + "'");
                break;
            }

            case OP_STORE: {
                const std::string& n = SCC[(size_t)ins.a];
                Value v = pop().take();
                if (Value* r = env->findRef(n)) *r = std::move(v);
                else env->vars[n] = std::move(v);
                break;
            }

            case OP_STORE_LOCAL: locals[(size_t)ins.a] = pop(); break;

            case OP_UNARY: {
                const std::string& op = SCC[(size_t)ins.a];
                VmVal v = pop();
                if (v.k == VK::Int || v.k == VK::Float) {
                    if (op == "-") { if (v.k==VK::Int) v.u.i=-v.u.i; else v.u.d=-v.u.d; push(v); break; }
                    if (op == "+") { push(v); break; }
                    if (op == "~" ) { if (v.k==VK::Int) { v.u.i = ~v.u.i; push(v); break; } }
                    if (op == "not") { push(fromVal(Value::boolean(v.k!=VK::Int ? (v.u.d!=0.0) : (v.u.i!=0)))); break; }
                } else if (op == "not") {
                    Value ft = pop().take();  // consume dv (was 'v'? v already popped) -> use truthy
                    push(fromVal(Value::boolean(!truthy(ft)))); break;
                }
                if (op == "-" || op == "+" || op == "~") panicHere("unary operator expects a number");
                if (op == "&" || op == "*") { push(v); break; }
                panicHere("unknown unary operator '" + op + "'");
                break;
            }

            case OP_UNARANGE: {
                VmVal lo = pop();
                VmVal r; r.k = VK::Range; r.u.box = new Value(Value::rangeV(lo.u.i, -1, false));
                push(r);
                break;
            }

            case OP_BINARY: {
                const std::string& op = SCC[(size_t)ins.a];
                VmVal r = pop();
                VmVal l = pop();
                if (op == ".." || op == "..=") {
                    push(fromVal(Value::rangeV(l.u.i, r.u.i, op == "..=")));
                    break;
                }
                push(slowBin(op.c_str(), l, r));
                break;
            }

            case OP_RANGE: {
                VmVal r = pop();
                VmVal l = pop();
                push(fromVal(Value::rangeV(l.u.i, r.u.i, ins.a != 0)));
                break;
            }

            // Specialized arithmetic/comparison fast paths (no string compare).
            case OP_BINARY_ADD: case OP_BINARY_SUB: case OP_BINARY_MUL:
            case OP_BINARY_DIV: case OP_BINARY_MOD: case OP_BINARY_POW:
            case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
                VmVal r = pop();
                VmVal l = pop();
                VmVal o;
                BinK k;
                const char* fb;
                switch (ins.op) {
                    case OP_BINARY_ADD: k = B_ADD; fb = "+"; break;
                    case OP_BINARY_SUB: k = B_SUB; fb = "-"; break;
                    case OP_BINARY_MUL: k = B_MUL; fb = "*"; break;
                    case OP_BINARY_DIV: k = B_DIV; fb = "/"; break;
                    case OP_BINARY_MOD: k = B_MOD; fb = "%"; break;
                    case OP_BINARY_POW: k = B_POW; fb = "**"; break;
                    case OP_LT: k = B_LT; fb = "<"; break;
                    case OP_LE: k = B_LE; fb = "<="; break;
                    case OP_GT: k = B_GT; fb = ">"; break;
                    case OP_GE: k = B_GE; fb = ">="; break;
                    case OP_EQ: k = B_EQ; fb = "=="; break;
                    default:   k = B_NE; fb = "!="; break;
                }
                if (binFast(k, l, r, o)) push(std::move(o));
                else push(slowBin(fb, l, r));
                break;
            }

            case OP_NEG: {
                VmVal v = pop();
                if (v.k == VK::Int) { v.u.i = -v.u.i; push(std::move(v)); break; }
                if (v.k == VK::Float) { v.u.d = -v.u.d; push(std::move(v)); break; }
                panicHere("unary operator expects a number");
                break;
            }

            case OP_NOT: {
                VmVal v = pop();
                bool t;
                if (v.k == VK::None) t = false;
                else if (v.k == VK::Bool) t = v.u.b;
                else if (v.k == VK::Int) t = v.u.i != 0;
                else if (v.k == VK::Float) t = v.u.d != 0.0;
                else { Value fv = v.take(); t = truthy(fv); }
                push(mkScalar(VK::Bool, 0, 0, !t, 0));
                break;
            }

            case OP_IS: {
                VmVal l = pop();
                if (ins.a < 0) push(mkScalar(VK::Bool,0,0,l.k == VK::None,0));
                else { Value lv = l.take(); push(fromVal(Value::boolean(typeTestMatches(lv, SCC[(size_t)ins.a])))); }
                break;
            }

            case OP_IN: {
                Value rv = pop().take();
                Value lv = pop().take();
                switch (rv.k) {
                    case VK::List: case VK::Set: case VK::Tuple: {
                        bool found = false;
                        for (const auto& el : *rv.vec)
                            if (valuesEqual(lv, el)) { found = true; break; }
                        push(fromVal(Value::boolean(found)));
                        break;
                    }
                    case VK::Dict: {
                        bool found = false;
                        for (const auto& kv : *rv.map)
                            if (valuesEqual(lv, kv.first)) { found = true; break; }
                        push(fromVal(Value::boolean(found)));
                        break;
                    }
                    case VK::Str: {
                        if (lv.k != VK::Str)
                            panicHere("'in' on strings needs a string needle");
                        push(fromVal(Value::boolean(
                            rv.s.find(lv.s) != std::string::npos)));
                        break;
                    }
                    case VK::Range:
                        push(fromVal(Value::boolean(lv.i >= rv.lo &&
                            (rv.inclusive ? lv.i <= rv.hi : lv.i < rv.hi))));
                        break;
                    default:
                        panicHere("'in' unsupported operand");
                }
                break;
            }

            case OP_MAKE_LIST: case OP_MAKE_SET: case OP_MAKE_TUPLE: {
                std::vector<Value> vs((size_t)ins.a);
                for (int32_t i = ins.a - 1; i >= 0; --i) vs[(size_t)i] = pop().take();
                if (ins.op == OP_MAKE_LIST) push(fromVal(Value::list(std::move(vs))));
                else if (ins.op == OP_MAKE_SET) push(fromVal(Value::set(std::move(vs))));
                else push(fromVal(Value::tuple(std::move(vs))));
                break;
            }

            case OP_MAKE_DICT: {
                Value d = Value::dict();
                for (int32_t i = 0; i < ins.a; ++i) {
                    Value val = pop().take();
                    Value key = pop().take();
                    d.map->emplace_back(std::move(key), std::move(val));
                }
                push(fromVal(std::move(d)));
                break;
            }

            case OP_INDEX: {
                Value idx = pop().take();
                Value obj = pop().take();
                if (obj.k == VK::Weak) obj = lockHeap(obj);
                switch (obj.k) {
                    case VK::List: case VK::Tuple:
                        push(fromVal(Value((*obj.vec)[normIndex((int64_t)obj.vec->size(), idx.i)])));
                        break;
                    case VK::Dict: {
                        bool hit = false; Value rv;
                        for (const auto& kv : *obj.map)
                            if (valuesEqual(kv.first, idx)) { rv = kv.second; hit = true; break; }
                        if (!hit) panicHere("dict key not found");
                        push(fromVal(std::move(rv)));
                        break;
                    }
                    case VK::Str: case VK::Bytes: {
                        size_t n = normIndex((int64_t)obj.s.size(), idx.i);
                        if (obj.k == VK::Bytes)
                            push(fromVal(Value::integer((unsigned char)obj.s[n])));
                        else
                            push(fromVal(Value::chr(decodeCharText(obj.s.substr(n, 1)))));
                        break;
                    }
                    default:
                        if (obj.k == VK::Struct || obj.k == VK::Heap)
                            push(fromVal(invokeMethod(obj, "index", {idx}, {}, env, 0, 0, nullptr)));
                        else
                            panicHere("value is not indexable");
                        break;
                }
                break;
            }

            case OP_SLICE: {
                Value hi = pop().take();
                Value lo = pop().take();
                Value obj = pop().take();
                if (obj.k == VK::Weak) obj = lockHeap(obj);
                push(fromVal(sliceOf(obj, lo.i, hi.i, ins.a != 0, 1)));
                break;
            }
            case OP_SLICE3: {
                Value step = pop().take();
                Value hi = pop().take();
                Value lo = pop().take();
                Value obj = pop().take();
                if (obj.k == VK::Weak) obj = lockHeap(obj);
                push(fromVal(sliceOf(obj, lo.i, hi.i, false, step.i)));
                break;
            }

            case OP_MEMBER: {
                const std::string& name = SCC[(size_t)ins.a];
                Value recv = pop().take();
                if (recv.k == VK::Weak) recv = lockHeap(recv);
                if (recv.isNone() && ins.b) { push(VmVal()); break; }
                if (recv.k == VK::Module &&
                    recv.typeName.rfind("__enum__:", 0) == 0) {
                    push(fromVal(makeEnumVPos(recv.typeName.substr(9), name, {}, env)));
                    break;
                }
                push(fromVal(memberRead(recv, name, 0, 0)));
                break;
            }

            case OP_MEMBER_METHOD: {
                const std::string& name = SCC[(size_t)ins.a];
                std::vector<Value> args((size_t)ins.b);
                for (int32_t i = ins.b - 1; i >= 0; --i) args[(size_t)i] = pop().take();
                Value recv = pop().take();
                if (recv.k == VK::Weak) recv = lockHeap(recv);
                if (recv.isNone() && ins.c) { push(VmVal()); break; }
                if (recv.k == VK::Module &&
                    recv.typeName.rfind("__enum__:", 0) == 0) {
                    push(fromVal(makeEnumVPos(recv.typeName.substr(9), name,
                                       std::move(args), env)));
                    break;
                }
                push(fromVal(invokeMethod(std::move(recv), name, std::move(args), {},
                                   env, 0, 0, nullptr)));
                break;
            }

            case OP_CALL_NAME: {
                const std::string& n = SCC[(size_t)ins.a];
                std::vector<Value> args((size_t)ins.b);
                for (int32_t i = ins.b - 1; i >= 0; --i) args[(size_t)i] = pop().take();
                Value callee;
                if (const Value* v = env->find(n)) callee = *v;
                else if (funcs_.count(n)) {
                    callee.k = VK::Fn;
                    callee.fn = funcs_[n];
                    callee.env = globals_;
                } else {
                    panicHere("undefined function '" + n + "'");
                }
                push(fromVal(callValue(std::move(callee), std::move(args), 0, 0)));
                break;
            }

            case OP_CALL: {
                std::vector<Value> args((size_t)ins.a);
                for (int32_t i = ins.a - 1; i >= 0; --i) args[(size_t)i] = pop().take();
                Value callee = pop().take();
                push(fromVal(callValue(std::move(callee), std::move(args), 0, 0)));
                break;
            }

            case OP_TRY: {
                VmVal v = pop();
                if (v.k == VK::Result) {
                    Value fv = v.take();
                    if (fv.variant == "err") throw SignalRaise{std::move(fv)};
                    push(fromVal(Value(fv.payload[0])));
                } else push(std::move(v));
                break;
            }

            case OP_JUMP: jmp(ins.a); break;
            case OP_JUMP_IF_FALSE: {
                VmVal c = pop();
                bool t;
                if (c.k == VK::None) t = false;
                else if (c.k == VK::Bool) t = c.u.b;
                else if (c.k == VK::Int) t = c.u.i != 0;
                else if (c.k == VK::Float) t = c.u.d != 0.0;
                else { Value fv = c.take(); t = truthy(fv); }
                if (!t) jmp(ins.a);
                break;
            }
            case OP_JUMP_IF_TRUE: {
                VmVal c = pop();
                bool t;
                if (c.k == VK::None) t = false;
                else if (c.k == VK::Bool) t = c.u.b;
                else if (c.k == VK::Int) t = c.u.i != 0;
                else if (c.k == VK::Float) t = c.u.d != 0.0;
                else { Value fv = c.take(); t = truthy(fv); }
                if (t) jmp(ins.a);
                break;
            }

            case OP_SCOPE_ENTER: env = makeChild(env); break;
            case OP_SCOPE_LEAVE: env = env->parent; break;

            case OP_EXPR: pop(); break;

            case OP_ITER_BEGIN: {
                Value seq = pop().take();
                VmIter it;
                if (seq.k == VK::Range) {
                    // Stream ranges (like the tree-walker) instead of
                    // materialising: for `0..n` this avoids a giant Vector.
                    int64_t h = seq.hi < 0 ? 9007199254740992LL : seq.hi;
                    it.kind = VmIter::Range;
                    it.cur = seq.lo;
                    it.end = seq.inclusive ? h + 1 : h;
                    iters.push_back(std::move(it));
                    break;
                }
                std::vector<Value> items;
                switch (seq.k) {
                    case VK::List: case VK::Set: case VK::Tuple: case VK::Gen:
                        items = *seq.vec;
                        break;
                    case VK::Dict:
                        for (const auto& kv : *seq.map) items.push_back(kv.first);
                        break;
                    case VK::Str:
                        for (char c : seq.s)
                            items.push_back(Value::chr((char32_t)(unsigned char)c));
                        break;
                    case VK::Bytes:
                        for (char c : seq.s)
                            items.push_back(Value::integer((int64_t)(unsigned char)c));
                        break;
                    case VK::Chan: {
                        for (;;) {
                            auto s = seq.chan->recv();
                            if (!s.got) break;
                            items.push_back(std::move(s.v));
                        }
                        break;
                    }
                    case VK::Struct: case VK::Heap: {
                        Value selfCopy = seq;
                        for (;;) {
                            Value nv = invokeMethod(selfCopy, "next", {}, {}, env,
                                                    0, 0, &selfCopy);
                            if (nv.isNone()) break;
                            items.push_back(nv);
                        }
                        break;
                    }
                    default:
                        panicHere("value is not iterable");
                }
                it.kind = VmIter::Vec;
                it.vec = std::move(items);
                it.idx = 0;
                iters.push_back(std::move(it));
                break;
            }

            case OP_ITER_NEXT: {
                auto& it = iters.back();
                if (it.kind == VmIter::Range) {
                    if (it.cur < it.end) {
                        push(mkScalar(VK::Int, it.cur++, 0, false, 0)); // value first
                        push(mkScalar(VK::Bool,0,0,true,0));            // hasNext on top
                    } else {
                        push(mkScalar(VK::Bool,0,0,false,0));
                    }
                } else {
                    if (it.idx < it.vec.size()) {
                        push(fromVal(std::move(it.vec[it.idx++])));
                        push(mkScalar(VK::Bool,0,0,true,0));
                    } else {
                        push(mkScalar(VK::Bool,0,0,false,0));
                    }
                }
                break;
            }

            case OP_ITER_VALUE_VAR: {
                const std::string& n = SCC[(size_t)ins.a];
                Value v = pop().take();
                env->vars[n] = std::move(v);
                break;
            }

            case OP_ITER_VALUE_LOCAL:
                locals[(size_t)ins.a] = pop();
                break;

            case OP_ITER_END: iters.pop_back(); break;

            case OP_RETURN: return pop().take();
            case OP_RETURN_NONE: return Value::none();

            case OP_HALT:
            default:
                panicHere("internal: unexpected VM opcode");
                break;
        }
    }
    return Value::none();
}

} // namespace interp
} // namespace kirn
