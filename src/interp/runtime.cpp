// Tree-walking interpreter implementation (plan §10.4).
// Divergences from the eventual compiled semantics, all unobservable in the
// corpus: generator views and filter/map chains materialize eagerly; channels
// declared without capacity queue unboundedly instead of rendezvous-syncing;
// weak fields keep their target alive while any strong handle exists.
#include "interp/runtime.h"
#include "interp/escapes.h"
#include "interp/bigint.h"
#include "interp/wide.h"
#include "lex/lexer.h"
#include "parser/parser.h"
#include "sema/checker.h"
#include "util/tomlmini.h"

#include <numeric>
#include <climits>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>

#ifndef _WIN32
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace kirn {
namespace interp {

namespace vm = kirn::vm;
using namespace kirn::vm;

using kirn::i128;
using kirn::u128;

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

// M3: sets are true sets — every construction path dedupes (previously only
// `add`/`insert` did, so literals/comprehensions kept duplicates). O(n^2) but
// set construction is not a hot loop.
static Value makeSet(std::vector<Value> items) {
    std::vector<Value> out;
    out.reserve(items.size());
    for (auto& v : items) {
        bool dup = false;
        for (auto& e : out)
            if (valEq(e, v)) { dup = true; break; }
        if (!dup) out.push_back(std::move(v));
    }
    return Value::set(std::move(out));
}

// M3: dict insertion is last-wins (Python/PEP 448), preserving the first
// insertion position. Replaces the old tree-walker panic on duplicate literal
// keys, which also diverged from the VM (which kept both).
static void dictPutInPlace(Value& d, Value key, Value val) {
    for (auto& kv : *d.map)
        if (valEq(kv.first, key)) { kv.second = std::move(val); return; }
    d.map->emplace_back(std::move(key), std::move(val));
}

// ---------------------------------------------------------------------------
// environments & small helpers
// ---------------------------------------------------------------------------

// Hidden frame slot that carries a pipe body's topic value. Not a valid user
// identifier, so it cannot collide with a real binding.
static const char kPipeTopic[] = "%";

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
    c->selBiased = s->selBiased;
    for (const auto& arm : s->selArms) {
        ast::Stmt::SelArm na;
        na.bind = arm.bind;
        na.chanOp = cloneExpr(arm.chanOp.get());
        for (const auto& b : arm.body) na.body.push_back(cloneStmt(b.get()));
        c->selArms.push_back(std::move(na));
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

// ---------------------------------------------------------------------------
// M6 numeric types: complex (rectangular, backed by std::complex<double>) and
// decimal (arbitrary-precision sign/mantissa/exponent; exact +,-,* and a
// precision-bounded division). Helpers live here so both the string formatters
// and the operator/eval paths share them.
// ---------------------------------------------------------------------------
static std::string fmtDouble(double d);

// Decimal canonical form: mant holds ASCII digits (no sign, no dot), value is
// sign * mant * 10^exp. Zero is mant "0", exp 0, dneg false.
static Value decNorm(std::string mant, int64_t exp, bool dneg) {
    size_t i = 0;
    while (i + 1 < mant.size() && mant[i] == '0') ++i;
    if (i) mant.erase(0, i);
    if (mant.empty()) mant = "0";
    if (mant == "0") { exp = 0; dneg = false; }
    return Value::decimalNum(std::move(mant), exp, dneg);
}

// Parse a signed digit string produced by bignum::Big::toString().
static Value decFromSigned(std::string digits, int64_t exp) {
    bool dneg = false;
    if (!digits.empty() && (digits[0] == '-' || digits[0] == '+')) {
        dneg = digits[0] == '-';
        digits.erase(0, 1);
    }
    return decNorm(std::move(digits), exp, dneg);
}

static Value decParse(const std::string& in) {
    std::string s = in;
    bool dneg = false;
    size_t p = 0;
    while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        dneg = s[p] == '-';
        ++p;
    }
    std::string digits;
    int64_t frac = 0;
    bool seenDot = false, hasE = false;
    for (; p < s.size(); ++p) {
        char c = s[p];
        if (c == '.') {
            if (seenDot) panicHere("decimal: invalid literal");
            seenDot = true;
        } else if (c == 'e' || c == 'E') {
            hasE = true;
            ++p;
            break;
        } else if (std::isdigit((unsigned char)c)) {
            digits.push_back(c);
            if (seenDot) ++frac;
        } else {
            panicHere("decimal: invalid literal");
        }
    }
    int64_t e = 0;
    if (hasE) {
        bool eneg = false;
        if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
            eneg = s[p] == '-';
            ++p;
        }
        if (p >= s.size()) panicHere("decimal: invalid exponent");
        for (; p < s.size(); ++p) {
            if (!std::isdigit((unsigned char)s[p]))
                panicHere("decimal: invalid exponent");
            e = e * 10 + (s[p] - '0');
        }
        if (eneg) e = -e;
    }
    if (digits.empty()) panicHere("decimal: invalid literal");
    return decNorm(digits, e - frac, dneg);
}

static std::string decToString(const Value& v) {
    const std::string& m = v.mant;
    std::string out;
    if (v.exp >= 0) {
        out = m + std::string((size_t)v.exp, '0');
    } else {
        int64_t frac = -v.exp;
        if ((int64_t)m.size() > frac)
            out = m.substr(0, m.size() - (size_t)frac) + "." +
                  m.substr(m.size() - (size_t)frac);
        else
            out = "0." + std::string((size_t)(frac - (int64_t)m.size()), '0') +
                  m;
    }
    if (v.dneg) out = "-" + out;
    return out;
}

static Value decAdd(const Value& x, const Value& y) {
    int64_t e = std::min(x.exp, y.exp);
    std::string xm = x.mant + std::string((size_t)(x.exp - e), '0');
    std::string ym = y.mant + std::string((size_t)(y.exp - e), '0');
    bignum::Big a = bignum::fromStr((x.dneg ? "-" : "") + xm);
    bignum::Big b = bignum::fromStr((y.dneg ? "-" : "") + ym);
    return decFromSigned(a.add(b).toString(), e);
}

static Value decNeg(const Value& v) {
    Value r = v;
    if (r.mant != "0") r.dneg = !r.dneg;
    return r;
}

static Value decSub(const Value& x, const Value& y) { return decAdd(x, decNeg(y)); }

static Value decMul(const Value& x, const Value& y) {
    bignum::Big a = bignum::fromStr((x.dneg ? "-" : "") + x.mant);
    bignum::Big b = bignum::fromStr((y.dneg ? "-" : "") + y.mant);
    return decFromSigned(a.mul(b).toString(), x.exp + y.exp);
}

static int decCmp(const Value& x, const Value& y) {
    bool xz = x.mant == "0", yz = y.mant == "0";
    if (xz && yz) return 0;
    if (x.dneg != y.dneg) return x.dneg ? -1 : 1;
    int64_t e = std::min(x.exp, y.exp);
    std::string xm = x.mant + std::string((size_t)(x.exp - e), '0');
    std::string ym = y.mant + std::string((size_t)(y.exp - e), '0');
    int c;
    if (xm.size() != ym.size()) c = xm.size() < ym.size() ? -1 : 1;
    else if (xm < ym) c = -1;
    else if (xm > ym) c = 1;
    else c = 0;
    return x.dneg ? -c : c;
}

// Round `digits` (no sign) with exponent `exp` to `prec` significant digits
// using half-up rounding.
static Value decRoundSig(std::string digits, int64_t exp, bool dneg, int prec) {
    if ((int64_t)digits.size() <= prec)
        return decNorm(std::move(digits), exp, dneg);
    int64_t drop = (int64_t)digits.size() - prec;
    bool roundUp = digits[(size_t)prec] >= '5';
    std::string keep = digits.substr(0, (size_t)prec);
    if (roundUp) {
        int i = (int)keep.size() - 1;
        for (; i >= 0; --i) {
            if (keep[(size_t)i] == '9') keep[(size_t)i] = '0';
            else { keep[(size_t)i] += 1; break; }
        }
        if (i < 0) { keep = "1" + keep; ++drop; }
    }
    // drop insignificant trailing zeros (the value is unchanged) so 1/4 reads
    // as 0.25 rather than 0.2500...0.
    while (keep.size() > 1 && keep.back() == '0') { keep.pop_back(); ++drop; }
    return decNorm(std::move(keep), exp + drop, dneg);
}

static Value decDiv(const Value& x, const Value& y) {
    if (y.mant == "0") panicHere("decimal division by zero");
    if (x.mant == "0") return decNorm("0", 0, false);
    const int prec = 28;
    int64_t K = (int64_t)prec + (int64_t)y.mant.size();
    bignum::Big scale = bignum::fromI64(10).pow((uint64_t)K);
    bignum::Big a = bignum::fromStr(x.mant);
    bignum::Big b = bignum::fromStr(y.mant);
    bignum::Big q = a.mul(scale).div(b);
    return decRoundSig(q.toString(), x.exp - y.exp - K, x.dneg ^ y.dneg, prec);
}

static Value decPowInt(const Value& base, int64_t e) {
    bool neg = e < 0;
    uint64_t ue = neg ? (uint64_t)(-(e + 1)) + 1 : (uint64_t)e;
    bignum::Big r = bignum::fromStr(base.mant).pow(ue);
    Value out = decFromSigned(r.toString(), base.exp * (int64_t)ue);
    if (base.dneg && (ue & 1)) out.dneg = !out.dneg;
    if (neg) {
        Value one = decNorm("1", 0, false);
        return decDiv(one, out);
    }
    return out;
}

static Value decFromDouble(double x) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", x);
    return decParse(buf);
}

static double decToDouble(const Value& v) { return std::stod(decToString(v)); }

static int64_t decToI64Trunc(const Value& v) {
    bignum::Big m = bignum::fromStr(v.mant);
    if (v.exp >= 0) {
        m = m.mul(bignum::fromI64(10).pow((uint64_t)v.exp));
    } else {
        int64_t drop = -v.exp;
        bignum::Big p10 = bignum::fromI64(10).pow((uint64_t)drop);
        m.limbs = bignum::divmodMag(m.limbs, p10.limbs).first;
    }
    std::string s = m.toString();
    if (v.dneg && s != "0") s = "-" + s;
    try {
        return std::stoll(s);
    } catch (...) {
        panicHere("decimal: integer value out of range");
    }
    return 0;
}

static Value decQuantize(const Value& x, int64_t targetExp) {
    if (x.mant == "0")
        return decNorm("0", targetExp, false);
    if (x.exp >= targetExp)
        return decNorm(x.mant + std::string((size_t)(x.exp - targetExp), '0'),
                       targetExp, x.dneg);
    int64_t drop = targetExp - x.exp;
    bignum::Big m = bignum::fromStr(x.mant);
    bignum::Big p10 = bignum::fromI64(10).pow((uint64_t)drop);
    auto qr = bignum::divmodMag(m.limbs, p10.limbs);
    bignum::Big q;
    q.limbs = qr.first;
    bignum::Big r;
    r.limbs = qr.second;
    bignum::Big two;
    two.limbs = bignum::fromInt(2);
    if (r.mul(two).cmpAbs(p10) >= 0)
        q.limbs = bignum::addMag(q.limbs, bignum::fromInt(1));
    return decNorm(q.toString(), targetExp, x.dneg);
}

static std::string fmtComplex(const Value& v) {
    std::string re = fmtDouble(v.re);
    std::string im = fmtDouble(std::fabs(v.im));
    return "(" + re + (v.im < 0 ? "-" : "+") + im + "i)";
}

[[maybe_unused]] static bool isCplxOperand(const Value& v) {
    return v.k == VK::Complex || v.k == VK::Int || v.k == VK::Float ||
           v.k == VK::Rational;
}

static std::complex<double> toCplx(const Value& v) {
    if (v.k == VK::Complex) return {v.re, v.im};
    if (v.k == VK::Int) return {(double)v.i, 0.0};
    if (v.k == VK::Float) return {v.d, 0.0};
    if (v.k == VK::Rational) return {(double)v.num / (double)v.den, 0.0};
    panicHere("complex: unsupported operand");
    return {};
}

static Value fromCplx(const std::complex<double>& z) {
    return Value::complexNum(z.real(), z.imag());
}

std::string repr(const Value& v);

static std::string fmtDouble(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    for (int prec = 15; prec <= 17; ++prec) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (strtod(buf, nullptr) == d) {
            std::string r(buf);
            if (r.find('.') == std::string::npos &&
                r.find('e') == std::string::npos)
                r += ".0";
            return r;
        }
    }
    return std::to_string(d);
}

// POWER_PLAN §5: `pet.error` dynamic error (anyhow-style). An Error is a
// struct value with fields:
//   msg    (Str) - this layer's Display message
//   source (any) - the next cause (an Error struct) or none
//   origin (any) - the wrapped concrete value for downcast/is, or none
std::string toStr(const Value& v);   // forward decl (defined below)
static const char* kErrorType = "error.Error";
static bool isErrorStruct(const std::string& tn) {
    return tn == kErrorType || tn == "error.Error" || tn == "Error";
}
static Value mkError(std::string msg, Value source, Value origin) {
    Value v = Value::structV(kErrorType);
    v.fields.emplace_back("msg", Value::str(std::move(msg)));
    v.fields.emplace_back("source", std::move(source));
    v.fields.emplace_back("origin", std::move(origin));
    return v;
}
// True when v is a pet.error Error built by mkError (precise: not a user
// struct that merely happens to be named "Error").
static bool isPetError(const Value& v) {
    if (v.k == VK::Struct) return v.typeName == kErrorType;
    if (v.k == VK::Heap && v.heap) return v.heap->typeName == kErrorType;
    return false;
}

// POWER_PLAN §5.3: enums annotated `@[derive(Error)]`, registered at load time
// so the free function toStr() can render their Display without Interpreter
// state. Value: enum type name -> its EnumDef AST (for variant @[error("…")]).
static std::unordered_map<std::string, const ast::Stmt*> g_errDerivedEnums;

static bool decoratorDerivesError(const ast::Decorator& d) {
    if (d.name != "derive") return false;
    for (const auto& a : d.args)
        if (a.value && a.value->kind == ast::ExKind::Ident && a.value->text == "Error")
            return true;
    return false;
}
static void registerEnumDerive(const ast::Stmt& s) {
    for (const auto& d : s.decorators)
        if (decoratorDerivesError(d)) { g_errDerivedEnums[s.name] = &s; return; }
}

// Minimal `@[derive(Error)]` Display (thiserror): render the variant's
// `@[error("…")]` template — `{field}`, `{0}`, `{field:?}`, `{{`/`}}` — or, for
// `@[error(transparent)]`, delegate to the sole field's Display.
static std::string deriveErrorDisplay(const Value& v, const ast::Stmt* en) {
    const ast::Variant* var = nullptr;
    for (const auto& vr : en->variants)
        if (vr.name == v.variant) { var = &vr; break; }
    if (!var) return repr(v);
    std::string tmpl;
    bool transparent = false, found = false;
    for (const auto& d : var->decorators) {
        if (d.name != "error") continue;
        for (const auto& a : d.args)
            if (a.value && a.value->kind == ast::ExKind::Ident &&
                a.value->text == "transparent")
                transparent = true;
        if (!d.args.empty() && d.args[0].value &&
            d.args[0].value->kind == ast::ExKind::Str) {
            tmpl = d.args[0].value->text;
            found = true;
        }
    }
    if (transparent) return v.payload.empty() ? repr(v) : toStr(v.payload[0]);
    if (!found) return repr(v);

    auto fieldAt = [&](const std::string& spec, bool& dbg) -> const Value* {
        dbg = false;
        std::string key = spec;
        if (key.size() > 2 && key[key.size() - 2] == ':' && key.back() == '?') {
            dbg = true;
            key = key.substr(0, key.size() - 2);
        }
        if (key.empty()) return nullptr;
        long idx = -1;
        bool numeric = key.find_first_not_of("0123456789") == std::string::npos;
        if (numeric) idx = std::stol(key);
        else {
            for (size_t i = 0; i < var->payload.size(); ++i)
                if (var->payload[i].name == key) { idx = (long)i; break; }
        }
        if (idx < 0 || (size_t)idx >= v.payload.size()) return nullptr;
        return &v.payload[(size_t)idx];
    };

    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
            out += '{'; i += 2; continue;
        }
        if (tmpl[i] == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
            out += '}'; i += 2; continue;
        }
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i);
            if (close == std::string::npos) { out += tmpl[i]; ++i; continue; }
            bool dbg = false;
            const Value* fv = fieldAt(tmpl.substr(i + 1, close - i - 1), dbg);
            if (fv) out += dbg ? repr(*fv) : toStr(*fv);
            i = close + 1;
            continue;
        }
        out += tmpl[i];
        ++i;
    }
    return out;
}
static const Value* structField(const Value& v, const std::string& n) {
    const std::vector<std::pair<std::string, Value>>* fs = nullptr;
    if (v.k == VK::Struct) fs = &v.fields;
    else if (v.k == VK::Heap && v.heap) fs = &v.heap->fields;
    if (fs)
        for (const auto& f : *fs)
            if (f.first == n) return &f.second;
    return nullptr;
}
static std::string errorFieldStr(const Value& v, const std::string& n) {
    const Value* f = structField(v, n);
    return f ? toStr(*f) : std::string();
}
// anyhow `{:#}` Display: "top: cause1: cause2" walking the source chain.
static std::string errorChainStr(const Value& v) {
    std::string out;
    const Value* cur = &v;
    bool first = true;
    while (cur && isErrorStruct(cur->typeName)) {
        if (!first) out += ": ";
        out += errorFieldStr(*cur, "msg");
        first = false;
        const Value* src = structField(*cur, "source");
        cur = (src && isErrorStruct(src->typeName)) ? src : nullptr;
    }
    return out;
}

// `.context(c)` on a result: ok passes through; err gets `c` pushed on top of
// the cause chain as a new Error layer (anyhow Context). A raw (non-Error) err
// payload is first wrapped in an Error so it can be a source.
static Value errorWithContext(const Value& r, const std::string& ctx) {
    if (r.k != VK::Result) return r;
    if (r.variant == "ok") return r;
    Value payload = r.payload.empty() ? Value::none() : r.payload[0];
    if (payload.k == VK::Struct && isErrorStruct(payload.typeName)) {
        const Value* origin = structField(payload, "origin");
        return Value::resultErr(
            mkError(ctx, payload, origin ? *origin : Value::none()));
    }
    Value inner = mkError(toStr(payload), Value::none(), payload);
    return Value::resultErr(mkError(ctx, inner, payload));
}

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
        case VK::Float:
            return fmtDouble(v.d);
        case VK::Rational: {
            return std::to_string(v.num) + "/" + std::to_string(v.den);
        }
        case VK::BigInt: {
            bignum::Big b;
            b.neg = v.neg;
            if (v.limbs) b.limbs = *v.limbs;
            return b.toString();
        }
        case VK::Complex: return fmtComplex(v);
        case VK::Decimal: return decToString(v);
        case VK::Str:    return v.s;
        case VK::Module: {
            // A module / type-reference value. `toStr` and `repr` would call
            // each other forever on this kind, so name it here (POWER_PLAN §3
            // exposes type references via `reflect.type_ref`).
            static const char* prefixes[] = {"__struct__:", "__enum__:",
                                             "__trait__:", "__loaded__:"};
            for (const char* p : prefixes) {
                size_t n = std::strlen(p);
                if (v.typeName.rfind(p, 0) == 0) return v.typeName.substr(n);
            }
            return v.typeName;
        }
        case VK::Struct:
            // pet.error: print(e) shows the Display chain, not the raw struct.
            if (isErrorStruct(v.typeName)) return errorChainStr(v);
            return repr(v);
        case VK::EnumV: {
            // POWER_PLAN §5.3: `@[derive(Error)]` enums print their @[error(…)]
            // Display; every other enum keeps the repr form.
            auto it = g_errDerivedEnums.find(v.typeName);
            if (it != g_errDerivedEnums.end()) return deriveErrorDisplay(v, it->second);
            return repr(v);
        }
        // `toStr`'s default delegates to `repr`, whose default delegates back
        // here, so a kind neither enumerates recursed until the stack blew
        // (`print(<fn|chan|task|timer|…>)` segfaulted). Every remaining value
        // kind is listed below, which makes both delegations terminate.
        case VK::Fn:
            return v.lam ? "<lambda>"
                         : (v.fn && !v.fn->name.empty() ? "<fn " + v.fn->name + ">"
                                                        : "<fn>");
        case VK::Builtin:   return "<builtin>";
        case VK::Chan:      return "<chan>";
        case VK::ThreadH:   return "<task>";
        case VK::Timer:     return "<timer>";
        case VK::File:      return "<file>";
        case VK::Arena:     return "<arena>";
        case VK::Ptr:       return "<ptr>";
        case VK::Mutex:     return "<mutex>";
        case VK::RwMutex:   return "<rwmutex>";
        case VK::WaitGroup: return "<waitgroup>";
        case VK::Once:      return "<once>";
        case VK::Cond:      return "<cond>";
        case VK::Atomic:    return "<atomic>";
        case VK::Weak:      return "<weak>";
        default:         return repr(v);
    }
}

static bool isBuffer(const Value& v) {
    return v.k == VK::Bytes || v.k == VK::ByteArray || v.k == VK::View;
}

// Raw bytes behind a buffer value. VK::Bytes is immutable (`s`); bytearray and
// views share a mutable buffer (`bstr`), with a view narrowed by off/len.
static std::string bytesData(const Value& v) {
    if (v.k == VK::ByteArray) return v.bstr ? *v.bstr : std::string();
    if (v.k == VK::View)
        return v.bstr ? v.bstr->substr(v.voff, v.vlen) : std::string();
    if (v.k == VK::Bytes) return v.s;
    return std::string();
}

std::string repr(const Value& v) {
    auto escBytes = [](const std::string& s) {
        std::string o = "b'";
        for (unsigned char c : s) {
            if (c >= 0x20 && c < 0x7f && c != '\\' && c != '\'') {
                o += (char)c;
            } else {
                char b[6];
                snprintf(b, sizeof b, "\\x%02x", c);
                o += b;
            }
        }
        return o + "'";
    };
    switch (v.k) {
        case VK::Str: return "'" + escapeStr(v.s) + "'";
        case VK::Bytes: return escBytes(v.s);
        case VK::ByteArray: return "bytearray(" + escBytes(v.bstr ? *v.bstr : std::string()) + ")";
        case VK::View: return "memoryview(" + escBytes(bytesData(v)) + ")";
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
        case VK::Deque: {
            std::string o = "deque([";
            for (size_t i = 0; i < v.vec->size(); ++i)
                o += (i ? ", " : "") + repr((*v.vec)[i]);
            o += "]";
            if (v.i >= 0) o += ", maxlen=" + std::to_string(v.i);
            return o + ")";
        }
        case VK::Counter: {
            std::string o = "Counter({";
            bool first = true;
            for (auto& kv : *v.map) {
                if (kv.second.k == VK::Int && kv.second.i == 0) continue;
                if (!first) o += ", ";
                first = false;
                o += repr(kv.first) + ": " + repr(kv.second);
            }
            return o + "})";
        }
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
        case VK::BigInt: return v.limbs && !v.limbs->empty();
        case VK::Complex: return v.re != 0.0 || v.im != 0.0;
        case VK::Decimal: return v.mant != "0";
        case VK::Bytes:  return !v.s.empty();
        case VK::ByteArray: return v.bstr && !v.bstr->empty();
        case VK::View:   return v.vlen != 0;
        case VK::Deque:  return v.vec && !v.vec->empty();
        case VK::Counter: {
            if (!v.map) return false;
            for (auto& kv : *v.map)
                if (kv.second.k == VK::Int && kv.second.i != 0) return true;
            return false;
        }
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
        case VK::BigInt: return "bigint";
        case VK::Complex: return "complex";
        case VK::Decimal: return "decimal";
        case VK::Str:    return "string";
        case VK::Bytes:  return "bytes";
        case VK::ByteArray: return "bytearray";
        case VK::View:   return "memoryview";
        case VK::Char:   return "char";
        case VK::List:   return "list";
        case VK::Set:    return "set";
        case VK::Tuple:  return "tuple";
        case VK::Dict:   return "dict";
        case VK::Deque:  return "deque";
        case VK::Counter: return "counter";
        case VK::Range:  return "range";
        case VK::Struct: return v.typeName;
        case VK::Heap:   return v.heap ? v.heap->typeName : "heap";
        case VK::EnumV:  return v.typeName;
        case VK::Result: return "result";
        case VK::Fn:     return "fn";
        case VK::Builtin:return "builtin";
        case VK::Gen:    return "generator";
        case VK::Chan:   return "chan";
        case VK::Mutex:  return "Mutex";
        case VK::RwMutex: return "RwMutex";
        case VK::WaitGroup: return "WaitGroup";
        case VK::Once:   return "Once";
        case VK::Cond:   return "Cond";
        case VK::Atomic: return "Atomic";
        case VK::Module: return "module";
        case VK::Ptr:    return "ptr";
        default:         return "value";
    }
}

// M10: coarse reflect kind — the category a value belongs to, independent of
// its concrete type name (so BigInt/Int share "int", Decimal/Float "float").
static std::string kindName(const Value& v) {
    switch (v.k) {
        case VK::Bool:   return "bool";
        case VK::Int:
        case VK::BigInt: return "int";
        case VK::Float:
        case VK::Decimal: return "float";
        case VK::Rational:return "rational";
        case VK::Complex: return "complex";
        case VK::Str:    return "string";
        case VK::Char:   return "char";
        case VK::Bytes:  return "bytes";
        case VK::ByteArray: return "bytearray";
        case VK::View:   return "memoryview";
        case VK::List:   return "list";
        case VK::Tuple:  return "tuple";
        case VK::Set:    return "set";
        case VK::Dict:   return "dict";
        case VK::Deque:  return "deque";
        case VK::Counter:return "counter";
        case VK::Range:  return "range";
        case VK::Struct: return "struct";
        case VK::Heap:   return "struct";
        case VK::EnumV:  return "enum";
        case VK::Result: return "result";
        case VK::Fn:
        case VK::Builtin: return "function";
        case VK::Gen:    return "generator";
        case VK::None:   return "none";
        default:         return typeName(v);
    }
}

// M10: name carried by a type reference. `Point` evaluates to a module value
// tagged "__struct__:Point"; a string name is accepted directly (for the
// dynamic path); a value falls back to its own type name.
static std::string typeRefName(const Value& v) {
    if (v.k == VK::Module &&
        v.typeName.rfind("__struct__:", 0) == 0)
        return v.typeName.substr(11);
    if (v.k == VK::Module &&
        v.typeName.rfind("__enum__:", 0) == 0)
        return v.typeName.substr(9);
    if (v.k == VK::Str) return v.s;
    return typeName(v);
}

// POWER_PLAN §3: base name of a declared type as written in source. Containers
// are Name nodes ("list", "dict", "chan", ...) carrying generic args; optional
// (`T?`) unwraps to the underlying name. Returns "" for structural types.
static std::string typeBaseName(const ast::Type& t) {
    switch (t.kind) {
        case ast::TyKind::Name: return t.name;
        case ast::TyKind::Optional:
            return t.inner ? typeBaseName(*t.inner) : "";
        case ast::TyKind::Pointer:
        case ast::TyKind::Ref:
            return t.inner ? typeBaseName(*t.inner) : "";
        default: return "";
    }
}

// POWER_PLAN §3: first-level generic argument base names (`list[User]` ->
// ["User"], `dict[Str, Int]` -> ["Str", "Int"]).
static std::vector<std::string> typeGenericNames(const ast::Type& t) {
    std::vector<std::string> out;
    const ast::Type* cur = &t;
    if (cur->kind == ast::TyKind::Optional && cur->inner)
        cur = cur->inner.get();
    for (const auto& g : cur->generics)
        if (g) out.push_back(typeBaseName(*g));
    return out;
}

// POWER_PLAN §3.2: a literal attribute argument (`rename = "id"`, `skip`, …)
// as a runtime value. Only literals are meaningful for attributes; anything
// else becomes `none` so reflection never evaluates arbitrary user code.
static Value attrLiteralValue(const ast::Expr& e) {
    switch (e.kind) {
        case ast::ExKind::Str:
        case ast::ExKind::CharLit:
            return Value::str(e.text);
        case ast::ExKind::Int: {
            errno = 0;
            long long n = std::strtoll(e.text.c_str(), nullptr, 0);
            if (errno == 0) return Value::integer((int64_t)n);
            return Value::none();
        }
        case ast::ExKind::Float:
            return Value::floating(std::strtod(e.text.c_str(), nullptr));
        case ast::ExKind::Ident:
            if (e.text == "true") return Value::boolean(true);
            if (e.text == "false") return Value::boolean(false);
            if (e.text == "none") return Value::none();
            // `skip_serializing_if = is_empty` — keep the bare path name.
            return Value::str(e.text);
        case ast::ExKind::List: {
            Value out = Value::list();
            for (const auto& el : e.elems)
                if (el) out.vec->push_back(attrLiteralValue(*el));
            return out;
        }
        default:
            return Value::none();
    }
}

// Attributes as `{attrName: [{name, value}, …]}`. A list of args (rather than a
// map) preserves repeated options such as `@[serde(rename = "a", rename = "b")]`
// and keeps positional args (`name == ""`) distinct.
static Value decoratorsValue(const std::vector<ast::Decorator>& ds) {
    Value out = Value::dict();
    for (const auto& d : ds) {
        Value args = Value::list();
        for (const auto& a : d.args) {
            Value av = Value::dict();
            std::string aname = a.name;
            Value aval =
                a.value ? attrLiteralValue(*a.value) : Value::none();
            // Bare flags inside an attribute list (`@[serde(skip)]`,
            // `@[serde(untagged)]`) parse as positional ident arguments; expose
            // them as named booleans so option lookup is uniform.
            if (aname.empty() && a.value &&
                a.value->kind == ast::ExKind::Ident &&
                a.value->text != "true" && a.value->text != "false" &&
                a.value->text != "none") {
                aname = a.value->text;
                aval = Value::boolean(true);
            }
            av.map->push_back({Value::str("name"), Value::str(aname)});
            av.map->push_back({Value::str("value"), std::move(aval)});
            args.vec->push_back(std::move(av));
        }
        out.map->push_back({Value::str(d.name), std::move(args)});
    }
    return out;
}

// POWER_PLAN §3.2: serde's rename_all rules, byte-for-byte the field/variant
// behaviour of serde_derive_internals::case (ASCII, like serde).
static std::string serdeCase(const std::string& s, const std::string& rule,
                             bool variant) {
    auto asciiLower = [](std::string x) {
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        return x;
    };
    auto asciiUpper = [](std::string x) {
        for (char& c : x) c = (char)std::toupper((unsigned char)c);
        return x;
    };
    auto snakeVariant = [&](const std::string& v) {
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            unsigned char c = (unsigned char)v[i];
            if (i > 0 && std::isupper(c)) out += '_';
            out += (char)std::tolower(c);
        }
        return out;
    };
    auto pascalField = [&](const std::string& f) {
        std::string out;
        bool cap = true;
        for (char c : f) {
            if (c == '_') {
                cap = true;
            } else if (cap) {
                out += (char)std::toupper((unsigned char)c);
                cap = false;
            } else {
                out += c;
            }
        }
        return out;
    };
    auto camelField = [&](const std::string& f) {
        std::string p = pascalField(f);
        if (!p.empty()) p[0] = (char)std::tolower((unsigned char)p[0]);
        return p;
    };
    auto kebab = [](std::string x) {
        for (char& c : x)
            if (c == '_') c = '-';
        return x;
    };
    if (rule.empty()) return s;
    if (variant) {
        if (rule == "PascalCase") return s;
        if (rule == "lowercase") return asciiLower(s);
        if (rule == "UPPERCASE") return asciiUpper(s);
        if (rule == "camelCase") {
            std::string t = s;
            if (!t.empty()) t[0] = (char)std::tolower((unsigned char)t[0]);
            return t;
        }
        if (rule == "snake_case") return snakeVariant(s);
        if (rule == "SCREAMING_SNAKE_CASE")
            return asciiUpper(snakeVariant(s));
        if (rule == "kebab-case") return kebab(snakeVariant(s));
        if (rule == "SCREAMING-KEBAB-CASE")
            return asciiUpper(kebab(snakeVariant(s)));
    } else {
        if (rule == "lowercase" || rule == "snake_case") return s;
        if (rule == "UPPERCASE") return asciiUpper(s);
        if (rule == "PascalCase") return pascalField(s);
        if (rule == "camelCase") return camelField(s);
        if (rule == "SCREAMING_SNAKE_CASE") return asciiUpper(s);
        if (rule == "kebab-case") return kebab(s);
        if (rule == "SCREAMING-KEBAB-CASE") return asciiUpper(kebab(s));
    }
    panicHere("serde: unknown rename rule '" + rule + "'");
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

// ---- sizeof / alignof static layout ---------------------------------------
// Kirn values are heap handles in the interpreter/VM, so a *static* byte size
// is only well defined for scalar primitives and for structs laid out C-style:
// fields in declaration order, each aligned to its natural alignment, the
// struct padded to its largest field alignment. Every non-scalar field
// (list/dict/set/str/optional/fn/ptr/ref) is a handle and counts as one
// machine word (8 bytes) with 8-byte alignment.
using StructTable = std::unordered_map<std::string, const Stmt*>;

static int alignUpBytes(int off, int a) { return a > 1 ? (off + a - 1) / a * a : off; }

static int layoutAlignBytes(const ast::TypeP& t, const StructTable& structs,
                            int depth);

static int layoutSizeBytes(const ast::TypeP& t, const StructTable& structs,
                           int depth) {
    if (depth > 64) return 8;                 // recursive/opaque: treat as handle
    if (t && t->kind == ast::TyKind::Name) {
        int s = sema::typeSizeBytes(t->name);
        if (s >= 0) return s;
        auto it = structs.find(t->name);
        if (it != structs.end()) {
            int off = 0, maxA = 1;
            for (const auto& f : it->second->fields) {
                int fa = layoutAlignBytes(f.type, structs, depth + 1);
                off = alignUpBytes(off, fa);
                off += layoutSizeBytes(f.type, structs, depth + 1);
                if (fa > maxA) maxA = fa;
            }
            return alignUpBytes(off, maxA);
        }
    }
    return 8;                                 // handle / opaque
}

static int layoutAlignBytes(const ast::TypeP& t, const StructTable& structs,
                            int depth) {
    if (depth > 64) return 8;
    if (t && t->kind == ast::TyKind::Name) {
        int a = sema::typeAlignBytes(t->name);
        if (a >= 0) return a;
        auto it = structs.find(t->name);
        if (it != structs.end()) {
            int maxA = 1;
            for (const auto& f : it->second->fields) {
                int fa = layoutAlignBytes(f.type, structs, depth + 1);
                if (fa > maxA) maxA = fa;
            }
            return maxA;
        }
    }
    return 8;
}

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

// True for the native/prebound pseudo-modules. A bare `import m;` for one of
// these must never be satisfied by a same-named *.kn file: a stdlib module such
// as pet/parallel.kn starts with `import parallel;` (the native module), and if
// the entry file lives in that same directory the file resolver finds
// parallel.kn and binds the module to *itself* - `parallel.current_num_threads()`
// then recurses until the stack overflows.
static bool isPreboundModule(const std::string& name) {
    for (const char* m :
         {"math", "time", "io", "mem", "json", "text", "os", "parallel",
          "error", "sync", "reflect", "net"})
        if (name == m) return true;
    return false;
}

void Interpreter::collectProgram(const Stmt& program) {
    for (const auto& sp : program.body) {
        const Stmt& s = *sp;
        switch (s.kind) {
            case ast::StKind::FuncDef:
                if (!s.externDef) {
                    funcs_[s.name] = &s;
                    // A user function shadows a prelude builtin of the same
                    // name (checker already allows the override).
                    globals_->vars.erase(s.name);
                }
                break;
            case ast::StKind::StructDef:
                structs_[s.name] = &s;
                structEnv_[s.name] = globals_;
                break;
            case ast::StKind::EnumDef:   enums_[s.name] = &s;   registerEnumDerive(s); break;
            case ast::StKind::TraitDef:  traits_[s.name] = &s;  break;
            case ast::StKind::ImplDef: {
                ImplEntry ent;
                ent.traitName = s.implTrait ? s.implTrait->name : "";
                ent.typeName  = s.implType ? s.implType->name : "";
                ent.env = globals_;
                for (const auto& m : s.body)
                    if (m->kind == ast::StKind::FuncDef)
                        ent.methods[m->name] = m.get();
                impls_.push_back(std::move(ent));
                break;
            }
            case ast::StKind::ConstDecl:
            case ast::StKind::VarDecl: {
                Value v = s.value ? eval(*s.value, globals_) : Value::none();
                if (s.declType && s.declType->kind == ast::TyKind::Name)
                    v.declTyName = s.declType->name;
                globals_->vars[s.target->text] = std::move(v);
                break;
            }
            case ast::StKind::Import: {
                if (!s.fromImport) {
                    // real module file (project package or stdlib) first, but a
                    // bare prebound-module name always binds the native module
                    // (see isPreboundModule).
                    Value real = isPreboundModule(s.moduleName)
                                     ? Value::none()
                                     : loadModuleFile(s.moduleName);
                    if (real.k != VK::None) {
                        // unaliased imports bind the last path segment
                        // ("util.kn" -> util, "a/b.kn" -> b)
                        std::string bind = s.importAlias;
                        if (bind.empty()) {
                            std::string mn = s.moduleName;
                            if (mn.size() > 3 &&
                                mn.compare(mn.size() - 3, 3, ".kn") == 0)
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
    // POWER_PLAN §1: apply Tier-2 (`@name`) decorators to top-level function
    // declarations once every function is collected. A decorator is an ordinary
    // callable; `@a @b fn f` applies bottom-up (b(f), then a(b(f))), as in
    // Python. The final value is bound in globals so it shadows the raw `funcs_`
    // entry on name lookup.
    for (const auto& sp : program.body) {
        const Stmt& s = *sp;
        if (s.kind != ast::StKind::FuncDef || s.externDef) continue;
        bool bare = false;
        for (const auto& d : s.decorators)
            if (!d.bracket) { bare = true; break; }
        if (!bare) continue;
        Value fn;
        fn.k = VK::Fn;
        fn.fn = &s;
        fn.env = globals_;
        for (auto it = s.decorators.rbegin(); it != s.decorators.rend(); ++it) {
            if (it->bracket) continue;
            ast::Expr id;
            id.kind = ast::ExKind::Ident;
            id.text = it->name;
            id.span = it->span;
            Value dec = eval(id, globals_);
            // Python semantics: @deco(args...) is deco(args...)(f); @deco is deco(f)
            if (!it->args.empty()) {
                std::vector<Value> dargs;
                for (const auto& a : it->args)
                    dargs.push_back(eval(*a.value, globals_));
                dec = callValue(std::move(dec), std::move(dargs), it->span.line,
                                it->span.col);
            }
            std::vector<Value> argv;
            argv.push_back(std::move(fn));
            fn = callValue(std::move(dec), std::move(argv), it->span.line,
                           it->span.col);
        }
        globals_->vars[s.name] = std::move(fn);
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

// ---- M9: `sync` primitives -------------------------------------------------
// One impl allocation per Mutex/RwMutex/WaitGroup/Once/Cond/Atomic value; the
// VK tag selects which fields and surface are live. Mutex is a plain std::mutex
// so `Cond.wait` can adopt it; RwMutex tracks readers/writer explicitly; Atomic
// maps to std::atomic with sequential consistency.
struct SyncImpl {
    std::mutex m;
    std::condition_variable cv;
    // Mutex
    std::thread::id owner;
    bool locked = false;
    // Once
    bool done = false;
    // WaitGroup
    int64_t count = 0;
    // RwMutex (readers may share; a writer is exclusive)
    int readers = 0;
    bool writer = false;
    // Atomic
    int elem = 0;                              // 0 = int, 1 = bool, 2 = float
    std::atomic<int64_t> ai{0};
    std::atomic<bool> ab{false};
    std::atomic<double> ad{0.0};
};

static Value makeSync(VK kind) {
    Value v;
    v.k = kind;
    v.sync = std::make_shared<SyncImpl>();
    return v;
}

static Value syncCtorMutex() {
    return biFn({}, [](std::vector<Value>&) { return makeSync(VK::Mutex); });
}
static Value syncCtorRwMutex() {
    return biFn({}, [](std::vector<Value>&) { return makeSync(VK::RwMutex); });
}
static Value syncCtorWaitGroup() {
    return biFn({}, [](std::vector<Value>&) { return makeSync(VK::WaitGroup); });
}
static Value syncCtorOnce() {
    return biFn({}, [](std::vector<Value>&) -> Value {
        return makeSync(VK::Once);
    });
}
static Value syncCtorCond() {
    return biFn({}, [](std::vector<Value>&) -> Value {
        return makeSync(VK::Cond);
    });
}
static Value syncCtorAtomic() {
    return biFn({"init"}, [](std::vector<Value>& a) -> Value {
        Value v = makeSync(VK::Atomic);
        if (!a.empty() && a[0].k != VK::None) {
            const Value& init = a[0];
            if (init.k == VK::Bool) {
                v.sync->elem = 1;
                v.sync->ab.store(init.b);
            } else if (init.k == VK::Float) {
                v.sync->elem = 2;
                v.sync->ad.store(init.d);
            } else if (init.k == VK::Int) {
                v.sync->elem = 0;
                v.sync->ai.store(init.i);
            } else {
                panicHere("Atomic requires an Int, Bool, or Float initial value");
            }
        }
        return v;
    });
}

// ---- M8: heap helpers (a heap is just a list kept in heap order) ----------
static bool heapLt(const Value& x, const Value& y, bool max) {
    return max ? valLess(y, x) : valLess(x, y);
}
static void heapSiftUp(std::vector<Value>& h, size_t i, bool max) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!heapLt(h[i], h[p], max)) break;
        std::swap(h[i], h[p]);
        i = p;
    }
}
static void heapSiftDown(std::vector<Value>& h, size_t i, bool max) {
    size_t n = h.size();
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, best = i;
        if (l < n && heapLt(h[l], h[best], max)) best = l;
        if (r < n && heapLt(h[r], h[best], max)) best = r;
        if (best == i) break;
        std::swap(h[i], h[best]);
        i = best;
    }
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
            case VK::ByteArray:
                return Value::integer((int64_t)(v.bstr ? v.bstr->size() : 0));
            case VK::View:
                return Value::integer((int64_t)v.vlen);
            case VK::List:
            case VK::Tuple:
            case VK::Set:
            case VK::Deque:
                return Value::integer((int64_t)v.vec->size());
            case VK::Counter: {
                int64_t n = 0;
                for (auto& kv : *v.map)
                    if (kv.second.k == VK::Int && kv.second.i != 0) ++n;
                return Value::integer(n);
            }
            case VK::Dict:
                return Value::integer((int64_t)v.map->size());
            case VK::Range: {
                int64_t hi = v.inclusive ? v.hi + 1 : v.hi;
                return Value::integer(hi > v.lo ? hi - v.lo : 0);
            }
            default: panicHere("len() unsupported operand");
        }
    });

    globals_->vars["__Sqrt"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::floating(std::sqrt(d));
    });
    globals_->vars["Rational"] = biFn({"num", "den"}, [](std::vector<Value>& a) -> Value {
        int64_t n = a[0].i;
        int64_t d = a[1].i;
        if (d == 0) panicHere("rational denominator cannot be zero");
        // normalize by gcd (Euclidean algorithm)
        int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
        while (y) { int64_t t = y; y = x % y; x = t; }
        int64_t g = x;
        n /= g;
        d /= g;
        if (d < 0) { n = -n; d = -d; }
        Value v;
        v.k = VK::Rational;
        v.num = n;
        v.den = d;
        return v;
    });
    globals_->vars["Numerator"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Rational) panicHere("numerator: argument is not a rational");
        return Value::integer(a[0].num);
    });
    globals_->vars["Denominator"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Rational) panicHere("denominator: argument is not a rational");
        return Value::integer(a[0].den);
    });
    globals_->vars["ToFloat"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Rational) panicHere("to_float: argument is not a rational");
        return Value::floating((double)a[0].num / (double)a[0].den);
    });
globals_->vars["ToInt"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Rational) panicHere("to_int: argument is not a rational");
        // truncation toward zero
        return Value::integer((a[0].num >= 0 ? a[0].num : -a[0].num) / a[0].den * (a[0].num >= 0 ? 1 : -1));
    });
    globals_->vars["ToRational"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.k == VK::Int) {
            Value r; r.k = VK::Rational; r.num = v.i; r.den = 1; return r;
        }
        if (v.k == VK::Rational) return v;
        if (v.k == VK::BigInt) {
            if (!v.limbs || v.limbs->empty()) {
                Value r; r.k = VK::Rational; r.num = 0; r.den = 1; return r;
            }
            for (size_t i = 1; i < v.limbs->size(); ++i)
                if ((*v.limbs)[i] != 0) panicHere("to_rational: numerator out of range");
            if ((*v.limbs)[0] > (uint64_t)INT64_MAX)
                panicHere("to_rational: numerator out of range");
            Value r; r.k = VK::Rational;
            r.num = (int64_t)(*v.limbs)[0];
            r.den = 1;
            if (v.neg) r.num = -r.num;
            return r;
        }
        panicHere("to_rational: unsupported operand");
        return Value::none();
    });
    globals_->vars["Complex"] = biFn({"re", "im"}, [](std::vector<Value>& a) -> Value {
        auto d = [](const Value& v) -> double {
            if (v.k == VK::Float) return v.d;
            if (v.k == VK::Int) return (double)v.i;
            if (v.k == VK::Rational) return (double)v.num / (double)v.den;
            if (v.k == VK::Complex) return v.re;
            panicHere("complex: bad component");
            return 0.0;
        };
        double re = d(a[0]);
        double im = a.size() > 1 ? d(a[1]) : 0.0;
        return Value::complexNum(re, im);
    });
    globals_->vars["__Complex"] = globals_->vars["Complex"];
    globals_->vars["Decimal"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Str) return decParse(a[0].s);
        if (a[0].k == VK::Int) return decFromSigned(std::to_string(a[0].i), 0);
        if (a[0].k == VK::Float) return decFromDouble(a[0].d);
        if (a[0].k == VK::Decimal) return a[0];
        if (a[0].k == VK::Rational)
            return decDiv(decFromSigned(std::to_string(a[0].num), 0),
                          decFromSigned(std::to_string(a[0].den), 0));
        panicHere("decimal: unsupported argument");
        return Value::none();
    });
    globals_->vars["__Decimal"] = globals_->vars["Decimal"];
    globals_->vars["ComplexReal"] = biFn({"z"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Complex) panicHere("complex_real: argument is not a complex");
        return Value::floating(a[0].re);
    });
    globals_->vars["ComplexImag"] = biFn({"z"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Complex) panicHere("complex_imag: argument is not a complex");
        return Value::floating(a[0].im);
    });
    globals_->vars["ComplexConj"] = biFn({"z"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Complex) panicHere("complex_conj: argument is not a complex");
        return Value::complexNum(a[0].re, -a[0].im);
    });
    globals_->vars["ComplexAbs"] = biFn({"z"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Complex) panicHere("complex_abs: argument is not a complex");
        return Value::floating(std::hypot(a[0].re, a[0].im));
    });
    globals_->vars["FAbs"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("fabs: argument is not a float");
        return Value::floating(std::fabs(a[0].d));
    });
    globals_->vars["__Abs"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Int)
            return Value::integer(a[0].i < 0 ? -a[0].i : a[0].i);
        if (a[0].k == VK::Float) return Value::floating(std::fabs(a[0].d));
        panicHere("abs: argument is not a number");
        return Value::none();
    });
    globals_->vars["Floor"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("floor: argument is not a float");
        return Value::integer((int64_t)std::floor(a[0].d));
    });
    globals_->vars["Ceil"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("ceil: argument is not a float");
        return Value::integer((int64_t)std::ceil(a[0].d));
    });
    globals_->vars["Round"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("round: argument is not a float");
        double d = std::round(a[0].d);
        return Value::integer((int64_t)d);
    });
    globals_->vars["RationalAdd"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n = a[0].num * a[1].den + a[1].num * a[0].den;
            int64_t d = a[0].den * a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[0].k == VK::Rational) {
            int64_t n = a[0].num + a[1].i * a[0].den;
            int64_t d = a[0].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[1].k == VK::Rational) {
            int64_t n = a[0].i * a[1].den + a[1].num;
            int64_t d = a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        // float + float → float
        double da = a[0].k == VK::Float ? a[0].d : (double)a[0].i;
        double db = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
        return Value::floating(da + db);
    });
    globals_->vars["RationalSub"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n = a[0].num * a[1].den - a[1].num * a[0].den;
            int64_t d = a[0].den * a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[0].k == VK::Rational) {
            int64_t n = a[0].num - a[1].i * a[0].den;
            int64_t d = a[0].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[1].k == VK::Rational) {
            int64_t n = a[0].i * a[1].den - a[1].num;
            int64_t d = a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        double da = a[0].k == VK::Float ? a[0].d : (double)a[0].i;
        double db = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
        return Value::floating(da - db);
    });
    globals_->vars["RationalMul"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n = a[0].num * a[1].num;
            int64_t d = a[0].den * a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[0].k == VK::Rational) {
            int64_t n = a[0].num * a[1].i;
            int64_t d = a[0].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[1].k == VK::Rational) {
            int64_t n = a[0].i * a[1].num;
            int64_t d = a[1].den;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        double da = a[0].k == VK::Float ? a[0].d : (double)a[0].i;
        double db = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
        return Value::floating(da * db);
    });
    globals_->vars["RationalDiv"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            if (a[1].num == 0) panicHere("rational division by zero");
            int64_t n = a[0].num * a[1].den;
            int64_t d = a[0].den * a[1].num;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[0].k == VK::Rational) {
            if (a[1].i == 0) panicHere("rational division by zero");
            int64_t n = a[0].num;
            int64_t d = a[0].den * a[1].i;
            int64_t x = (n < 0 ? -n : n), y = (d < 0 ? -d : d);
            while (y) { int64_t t = y; y = x % y; x = t; }
            int64_t g = (x == 0 ? 1 : std::gcd(std::abs(n), std::abs(d)));
            n /= g; d /= g;
            if (d < 0) { n = -n; d = -d; }
            Value v; v.k = VK::Rational; v.num = n; v.den = d; return v;
        }
        if (a[1].k == VK::Rational) {
            // int / rational → float
            double da = (double)a[0].i;
            double db = (double)a[1].num / a[1].den;
            return Value::floating(da / db);
        }
        // float / float
        double da = a[0].k == VK::Float ? a[0].d : (double)a[0].i;
        double db = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
        if (db == 0.0) panicHere("float division by zero");
        return Value::floating(da / db);
    });
    globals_->vars["RationalPow"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Int) {
            int64_t e = a[1].i;
            i128 n = a[0].num, d = a[0].den;
            bool neg = e < 0;
            uint64_t ue = neg ? (uint64_t)(-(e + 1)) + 1 : (uint64_t)e;
            i128 rn = 1, rd = 1;
            for (uint64_t i = 0; i < ue; ++i) { rn *= n; rd *= d; }
            if (neg) { i128 t = rn; rn = rd; rd = t; }
            if (rd < 0) { rn = -rn; rd = -rd; }
            i128 x = rn < 0 ? -rn : rn, y = rd;
            while (y) { i128 t = y; y = x % y; x = t; }
            if (x) { rn /= x; rd /= x; }
            if (rn > INT64_MAX || rn < INT64_MIN || rd > INT64_MAX)
                panicHere("rational_pow: result out of range");
            Value v; v.k = VK::Rational;
            v.num = (int64_t)rn;
            v.den = (int64_t)rd;
            return v;
        }
        if (a[0].k == VK::Float && a[1].k == VK::Int) {
            return Value::floating(std::pow(a[0].d, (double)a[1].i));
        }
        if (a[0].k == VK::Float && a[1].k == VK::Float) {
            return Value::floating(std::pow(a[0].d, a[1].d));
        }
        double da = a[0].k == VK::Float ? a[0].d : (double)a[0].i;
        double db = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
        return Value::floating(std::pow(da, db));
    });
    globals_->vars["RationalEq"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            // compare normalized
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            // cross-multiply
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs == rhs);
        }
        if (a[0].k == VK::Rational) {
            // compare rational with float → convert rational to float
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv == a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d == rv);
        }
        // float == float
        return Value::boolean(a[0].d == a[1].d);
    });
    globals_->vars["RationalNe"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs != rhs);
        }
        if (a[0].k == VK::Rational) {
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv != a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d != rv);
        }
        return Value::boolean(a[0].d != a[1].d);
    });
    globals_->vars["RationalLt"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            // a < b  ⇔  n1/d1 < n2/d2  ⇔  n1*d2 < n2*d1  (denoms > 0 guaranteed)
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs < rhs);
        }
        if (a[0].k == VK::Rational) {
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv < a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d < rv);
        }
        return Value::boolean(a[0].d < a[1].d);
    });
    globals_->vars["RationalLe"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs <= rhs);
        }
        if (a[0].k == VK::Rational) {
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv <= a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d <= rv);
        }
        return Value::boolean(a[0].d <= a[1].d);
    });
    globals_->vars["RationalGt"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs > rhs);
        }
        if (a[0].k == VK::Rational) {
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv > a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d > rv);
        }
        return Value::boolean(a[0].d > a[1].d);
    });
    globals_->vars["RationalGe"] = biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Rational && a[1].k == VK::Rational) {
            int64_t n1 = a[0].num, d1 = a[0].den;
            int64_t n2 = a[1].num, d2 = a[1].den;
            int64_t lhs = n1 * d2;
            int64_t rhs = n2 * d1;
            return Value::boolean(lhs >= rhs);
        }
        if (a[0].k == VK::Rational) {
            double rv = (double)a[0].num / a[0].den;
            return Value::boolean(rv >= a[1].d);
        }
        if (a[1].k == VK::Rational) {
            double rv = (double)a[1].num / a[1].den;
            return Value::boolean(a[0].d >= rv);
        }
        return Value::boolean(a[0].d >= a[1].d);
    });
    globals_->vars["Exp"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("exp: argument is not a float");
        return Value::floating(std::exp(a[0].d));
    });
    globals_->vars["Log"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("log: argument is not a float");
        return Value::floating(std::log(a[0].d));
    });
    globals_->vars["Log2"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("log2: argument is not a float");
        return Value::floating(std::log2(a[0].d));
    });
    globals_->vars["Log10"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("log10: argument is not a float");
        return Value::floating(std::log10(a[0].d));
    });
    globals_->vars["Sin"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("sin: argument is not a float");
        return Value::floating(std::sin(a[0].d));
    });
    globals_->vars["Cos"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("cos: argument is not a float");
        return Value::floating(std::cos(a[0].d));
    });
    globals_->vars["Tan"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("tan: argument is not a float");
        return Value::floating(std::tan(a[0].d));
    });
    globals_->vars["Sinh"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("sinh: argument is not a float");
        return Value::floating(std::sinh(a[0].d));
    });
    globals_->vars["Cosh"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("cosh: argument is not a float");
        return Value::floating(std::cosh(a[0].d));
    });
    globals_->vars["Tanh"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float) panicHere("tanh: argument is not a float");
        return Value::floating(std::tanh(a[0].d));
    });
    globals_->vars["CopySign"] = biFn({"x", "y"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float || a[1].k != VK::Float) panicHere("copysign: both arguments must be floats");
        double sign = (a[1].d >= 0.0) ? 1.0 : -1.0;
        return Value::floating(std::copysign(a[0].d, sign));
    });
    globals_->vars["Pow"] = biFn({"x", "y"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Float || a[1].k != VK::Float) panicHere("pow: both arguments must be floats");
        return Value::floating(std::pow(a[0].d, a[1].d));
    });
    globals_->vars["Ord"] = biFn({"c"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Char) return Value::integer((int64_t)a[0].ch);
        if (a[0].k == VK::Str && !a[0].s.empty())
            return Value::integer((int64_t)(unsigned char)a[0].s[0]);
        panicHere("ord() expects a char");
    });
    globals_->vars["typeof"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(typeName(a[0]));
    });
    globals_->vars["Chr"] = biFn({"n"}, [](std::vector<Value>& a) -> Value {
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

    // POWER_PLAN §6 prelude: reachable with no `import pet.error;`. bail/ensure
    // mirror error.bail / error.ensure exactly (same SignalRaise payload), and
    // ok/err make the checker's predeclared `result` variants constructible.
    globals_->vars["bail"] = biFn({"msg"}, [](std::vector<Value>& a) -> Value {
        throw SignalRaise{Value::resultErr(
            mkError(toStr(a.empty() ? Value::none() : a[0]),
                    Value::none(), Value::none()))};
    });
    globals_->vars["ensure"] = biFn({"cond", "msg"},
                                    [](std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !truthy(a[0]))
            throw SignalRaise{Value::resultErr(
                mkError(toStr(a.size() > 1 ? a[1]
                                           : (a.empty() ? Value::none() : a[0])),
                        Value::none(), Value::none()))};
        return Value::none();
    });
    globals_->vars["ok"] = biFn({"v"}, [](std::vector<Value>& a) -> Value {
        return Value::resultOk(a.empty() ? Value::none() : a[0]);
    });
    globals_->vars["err"] = biFn({"v"}, [](std::vector<Value>& a) -> Value {
        return Value::resultErr(a.empty() ? Value::none() : a[0]);
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
    globals_->vars["Str"] = biFn({"x", "base"}, [](std::vector<Value>& a) -> Value {
        if (a.size() >= 2 && a[0].k != VK::Float && a[1].k == VK::Int) {
            int64_t base = a[1].i;
            if (base < 2 || base > 36) panicHere("str(): base must be in 2..36");
            bignum::Big b = (a[0].k == VK::BigInt)
                                ? bignum::Big{}
                                : bignum::fromI64(a[0].i);
            if (a[0].k == VK::BigInt) {
                b.neg = a[0].neg;
                if (a[0].limbs) b.limbs = *a[0].limbs;
            }
            return Value::str(b.toStringBase((unsigned)base));
        }
        return Value::str(toStr(a[0]));
    });
    globals_->vars["Int"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.k == VK::Int) return v;
        if (v.k == VK::Float) return Value::integer((int64_t)v.d);
        if (v.k == VK::Bool) return Value::integer(v.b ? 1 : 0);
        if (v.k == VK::Char) return Value::integer((int64_t)v.ch);
        if (v.k == VK::BigInt) {
            if (!v.limbs || v.limbs->empty()) return Value::integer(0);
            uint64_t lo = (*v.limbs)[0];
            int64_t r = (int64_t)lo;
            if (v.neg) r = -r;
            return Value::integer(r);
        }
        if (v.k == VK::Rational) {
            int64_t q = (v.num >= 0 ? v.num : -v.num) / v.den;
            return Value::integer(v.num >= 0 ? q : -q);
        }
        if (v.k == VK::Str) {
            try { return Value::integer(std::stoll(v.s)); }
            catch (...) { panicHere("int(): cannot parse '" + v.s + "'"); }
        }
        panicHere("int(): unsupported operand");
    });
    globals_->vars["Float"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        const Value& v = a[0];
        if (v.k == VK::Float) return v;
        if (v.k == VK::Int) return Value::floating((double)v.i);
        if (v.k == VK::Bool) return Value::floating(v.b ? 1.0 : 0.0);
        if (v.k == VK::BigInt) {
            bignum::Big b;
            b.neg = v.neg;
            if (v.limbs) b.limbs = *v.limbs;
            return Value::floating(std::stod(b.toString()));
        }
        if (v.k == VK::Rational)
            return Value::floating((double)v.num / (double)v.den);
        if (v.k == VK::Str) {
            try { return Value::floating(std::stod(v.s)); }
            catch (...) { panicHere("float(): cannot parse '" + v.s + "'"); }
        }
        panicHere("float(): unsupported operand");
    });
    globals_->vars["HexDigits"] = biFn({"x", "n"}, [](std::vector<Value>& a) -> Value {
        int64_t n = a[1].i;
        uint64_t u = 0;
        if (a[0].k == VK::BigInt) {
            u = (a[0].limbs && !a[0].limbs->empty()) ? (*a[0].limbs)[0] : 0;
        } else {
            u = (uint64_t)a[0].i;
        }
        std::string out;
        for (int64_t i = n - 1; i >= 0; --i) {
            int nib = (i >= 0 && i < 16) ? (int)((u >> (4 * i)) & 0xf) : 0;
            out += "0123456789abcdef"[nib];
        }
        return Value::str(out);
    });
    globals_->vars["ToBytes"] = biFn({"x", "len", "endian"}, [](std::vector<Value>& a) -> Value {
        if (a.size() < 2) panicHere("to_bytes: missing length argument");
        int64_t n = a[1].i;
        if (n < 0) panicHere("to_bytes: length must be non-negative");
        std::vector<uint8_t> bytes((size_t)n, 0);
        if (a[0].k == VK::BigInt) {
            std::vector<uint64_t> L =
                a[0].limbs ? *a[0].limbs : std::vector<uint64_t>();
            std::vector<uint8_t> mag;
            for (uint64_t l : L)
                for (int i = 0; i < 8; ++i) mag.push_back((uint8_t)((l >> (8 * i)) & 0xff));
            for (int64_t i = 0; i < n && i < (int64_t)mag.size(); ++i) bytes[i] = mag[i];
            if (a[0].neg) {
                int carry = 1;
                for (int64_t i = 0; i < n; ++i) {
                    int x = (uint8_t)~bytes[i] + carry;
                    bytes[i] = (uint8_t)x;
                    carry = x >> 8;
                }
            }
        } else {
            uint64_t u = (uint64_t)a[0].i;
            for (int64_t i = 0; i < n; ++i) bytes[i] = (uint8_t)((u >> (8 * i)) & 0xff);
        }
        if (a.size() > 2 && a[2].k == VK::Str && a[2].s == "big")
            std::reverse(bytes.begin(), bytes.end());
        std::string s(bytes.begin(), bytes.end());
        Value b = Value::str(s);
        b.k = VK::Bytes;
        return b;
    });
    globals_->vars["FromBytes"] = biFn({"b"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k != VK::Bytes && a[0].k != VK::Str)
            panicHere("from_bytes: expected bytes");
        const std::string& s = a[0].s;
        size_t nb = s.size();
        bool neg = nb > 0 && ((uint8_t)s[nb - 1] & 0x80);
        if (nb == 0) return Value::integer(0);
        if (nb <= 8) {
            std::vector<uint8_t> tmp(s.begin(), s.end());
            if (neg) {
                int carry = 1;
                for (size_t i = 0; i < nb; ++i) {
                    int x = (uint8_t)~tmp[i] + carry;
                    tmp[i] = (uint8_t)x;
                    carry = x >> 8;
                }
            }
            uint64_t u = 0;
            for (size_t i = 0; i < nb; ++i)
                u |= (uint64_t)tmp[i] << (8 * i);
            int64_t r = (int64_t)u;
            if (neg) r = -r;
            return Value::integer(r);
        }
        std::vector<uint8_t> tmp(s.begin(), s.end());
        if (neg) {
            int carry = 1;
            for (size_t i = 0; i < nb; ++i) {
                int x = (uint8_t)~tmp[i] + carry;
                tmp[i] = (uint8_t)x;
                carry = x >> 8;
            }
        }
        std::vector<uint64_t> limbs;
        for (size_t i = 0; i < nb; i += 8) {
            uint64_t l = 0;
            for (size_t j = 0; j < 8 && i + j < nb; ++j)
                l |= (uint64_t)tmp[i + j] << (8 * j);
            limbs.push_back(l);
        }
        while (!limbs.empty() && limbs.back() == 0) limbs.pop_back();
        Value v;
        v.k = VK::BigInt;
        v.neg = neg && !limbs.empty();
        v.limbs = std::make_shared<std::vector<uint64_t>>(std::move(limbs));
        return v;
    });
    // M7: bytes / bytearray / memoryview construction + hex helpers.
    globals_->vars["bytes"] = globals_->vars["__Bytes"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k == VK::None) {
            Value v = Value::str(""); v.k = VK::Bytes; return v;
        }
        if (a[0].k == VK::Bytes) return a[0];
        std::string s;
        if (a[0].k == VK::ByteArray) s = a[0].bstr ? *a[0].bstr : "";
        else if (a[0].k == VK::View) s = bytesData(a[0]);
        else if (a[0].k == VK::Str) s = a[0].s;
        else if (a[0].k == VK::List) {
            for (auto& e : *a[0].vec) s.push_back((char)(e.i & 0xff));
        } else {
            panicHere("bytes(): unsupported operand");
        }
        Value v = Value::str(std::move(s)); v.k = VK::Bytes; return v;
    });
    globals_->vars["bytearray"] = globals_->vars["__ByteArray"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        std::string s;
        if (!a.empty()) {
            if (a[0].k == VK::Bytes) s = a[0].s;
            else if (a[0].k == VK::ByteArray) s = a[0].bstr ? *a[0].bstr : "";
            else if (a[0].k == VK::View) s = bytesData(a[0]);
            else if (a[0].k == VK::Str) s = a[0].s;
            else if (a[0].k == VK::Int) s = std::string((size_t)a[0].i, '\0');
            else if (a[0].k == VK::List) {
                for (auto& e : *a[0].vec) s.push_back((char)(e.i & 0xff));
            } else {
                panicHere("bytearray(): unsupported operand");
            }
        }
        return Value::byteArray(std::move(s));
    });
    globals_->vars["memoryview"] = globals_->vars["__MemoryView"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a.empty()) panicHere("memoryview() expects an argument");
        if (a[0].k == VK::Bytes) {
            auto buf = std::make_shared<std::string>(a[0].s);
            return Value::view(std::move(buf), 0, a[0].s.size(), true);
        }
        if (a[0].k == VK::ByteArray) {
            if (!a[0].bstr)
                return Value::view(std::make_shared<std::string>(), 0, 0, false);
            return Value::view(a[0].bstr, 0, a[0].bstr->size(), false);
        }
        if (a[0].k == VK::View) return a[0];
        panicHere("memoryview(): expected bytes or bytearray");
    });
    globals_->vars["fromhex"] = globals_->vars["FromHex"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        std::string s = a[0].k == VK::Str ? a[0].s
                      : a[0].k == VK::Bytes ? a[0].s : "";
        std::string out;
        int hi = -1;
        for (char c : s) {
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            else panicHere("fromhex(): invalid hex digit");
            if (hi < 0) hi = d;
            else { out.push_back((char)((hi << 4) | d)); hi = -1; }
        }
        if (hi >= 0) panicHere("fromhex(): odd number of hex digits");
        Value v = Value::str(std::move(out)); v.k = VK::Bytes; return v;
    });
    globals_->vars["Bool"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::boolean(truthy(a[0]));
    });
    globals_->vars["type"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(typeName(a[0]));
    });

    // sizeof(T) / alignof(T): T is a type reference (a struct/enum type name
    // resolved to a `__struct__:`/`__enum__:` module, or a plain string name).
    // Scalar spellings are baked by the parser; struct layouts are computed
    // from the declaration table (see layoutSizeBytes).
    globals_->vars["sizeof"] = biFn({"t"}, [this](std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::integer(0);
        std::string tn = typeRefName(a[0]);
        auto ty = ast::Type::makeName(tn, {});
        return Value::integer(layoutSizeBytes(ty, structs_, 0));
    });
    globals_->vars["alignof"] = biFn({"t"}, [this](std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::integer(0);
        std::string tn = typeRefName(a[0]);
        auto ty = ast::Type::makeName(tn, {});
        return Value::integer(layoutAlignBytes(ty, structs_, 0));
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
    globals_->vars["Min"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
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
    globals_->vars["Max"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
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

    // ---- M8: deque / Counter + collection free-functions --------------------
    auto counterBump = [](Value& c, const Value& key, int64_t delta) {
        for (auto& kv : *c.map)
            if (valEq(kv.first, key)) {
                kv.second = Value::integer(kv.second.i + delta);
                return;
            }
        c.map->push_back({key, Value::integer(delta)});
    };
    globals_->vars["Deque"] = globals_->vars["deque"] =
        biFn({"xs", "maxlen"}, [this](std::vector<Value>& a) -> Value {
            int64_t maxlen = -1;
            if (a.size() > 1 && a[1].k == VK::Int) maxlen = a[1].i;
            std::vector<Value> out;
            if (!a.empty() && a[0].k != VK::None)
                iterateSeq(a[0], [&](Value& it, Env&) -> bool {
                    out.push_back(it); return true;
                }, globals_);
            if (maxlen >= 0 && (int64_t)out.size() > maxlen)
                out.erase(out.begin(),
                          out.begin() + ((int64_t)out.size() - maxlen));
            return Value::deque(std::move(out), maxlen);
        });
    globals_->vars["Counter"] = globals_->vars["counter"] =
        biFn({"xs"}, [this, counterBump](std::vector<Value>& a) -> Value {
            Value c = Value::counter();
            if (a.empty() || a[0].k == VK::None) return c;
            const Value& xs = a[0];
            if (xs.k == VK::Counter) { c.map = xs.map; return c; }
            if (xs.k == VK::Dict) {
                for (auto& kv : *xs.map)
                    c.map->push_back({kv.first,
                                      Value::integer(kv.second.k == VK::Int
                                                         ? kv.second.i : 1)});
                return c;
            }
            iterateSeq(xs, [&](Value& it, Env&) -> bool {
                counterBump(c, it, 1); return true;
            }, globals_);
            return c;
        });
    globals_->vars["group_by"] = biFn({"f", "xs"}, [this](std::vector<Value>& a) -> Value {
        Value out = Value::dict();
        if (a.size() < 2) return out;
        const Value& f = a[0];
        iterateSeq(a[1], [&](Value& it, Env&) -> bool {
            Value key = callValue(f, {it}, 0, 0);
            for (auto& kv : *out.map)
                if (valEq(kv.first, key)) { kv.second.vec->push_back(it); return true; }
            out.map->push_back({key, Value::list({it})});
            return true;
        }, globals_);
        return out;
    });
    globals_->vars["chunk"] = biFn({"xs", "n"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> out;
        if (a.size() < 2 || a[1].k != VK::Int || a[1].i <= 0) return Value::list(out);
        int64_t n = a[1].i;
        std::vector<Value> cur;
        iterateSeq(a[0], [&](Value& it, Env&) -> bool {
            cur.push_back(it);
            if ((int64_t)cur.size() == n) {
                out.push_back(Value::list(std::move(cur)));
                cur = {};
            }
            return true;
        }, globals_);
        if (!cur.empty()) out.push_back(Value::list(std::move(cur)));
        return Value::list(std::move(out));
    });
    globals_->vars["flatten"] = biFn({"xs"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> out;
        if (a.empty()) return Value::list(std::move(out));
        iterateSeq(a[0], [&](Value& it, Env&) -> bool {
            if (it.k == VK::List || it.k == VK::Tuple || it.k == VK::Set ||
                it.k == VK::Deque)
                for (auto& e : *it.vec) out.push_back(e);
            else
                out.push_back(it);
            return true;
        }, globals_);
        return Value::list(std::move(out));
    });
    globals_->vars["count"] = biFn({"xs", "x"}, [this](std::vector<Value>& a) -> Value {
        int64_t n = 0;
        if (a.size() < 2) return Value::integer(0);
        const Value& needle = a[1];
        iterateSeq(a[0], [&](Value& it, Env&) -> bool {
            if (valEq(it, needle)) ++n;
            return true;
        }, globals_);
        return Value::integer(n);
    });
    globals_->vars["find"] = biFn({"xs", "x"}, [this](std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::integer(-1);
        const Value& needle = a[1];
        int64_t i = 0, found = -1;
        iterateSeq(a[0], [&](Value& it, Env&) -> bool {
            if (valEq(it, needle)) { found = i; return false; }
            ++i; return true;
        }, globals_);
        return Value::integer(found);
    });
    globals_->vars["binary_search"] = biFn({"xs", "x"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> v;
        if (a.empty()) return Value::integer(-1);
        iterateSeq(a[0], [&](Value& it, Env&) -> bool {
            v.push_back(it); return true;
        }, globals_);
        const Value& needle = a.size() > 1 ? a[1] : Value::none();
        size_t lo = 0, hi = v.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (valEq(v[mid], needle)) return Value::integer((int64_t)mid);
            if (valLess(v[mid], needle)) lo = mid + 1; else hi = mid;
        }
        return Value::integer(-1);
    });
    globals_->vars["partition"] = biFn({"f", "xs"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> yes, no;
        if (a.size() >= 2) {
            const Value& f = a[0];
            iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                if (truthy(callValue(f, {it}, 0, 0))) yes.push_back(it);
                else no.push_back(it);
                return true;
            }, globals_);
        }
        return Value::tuple({Value::list(std::move(yes)), Value::list(std::move(no))});
    });
    globals_->vars["heappush"] = biFn({"h", "x"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k != VK::List) panicHere("heappush expects a list");
        a[0].vec->push_back(a.size() > 1 ? a[1] : Value::none());
        heapSiftUp(*a[0].vec, a[0].vec->size() - 1, false);
        return Value::none();
    });
    globals_->vars["heappop"] = biFn({"h"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k != VK::List || a[0].vec->empty())
            panicHere("heappop from empty heap");
        auto& h = *a[0].vec;
        Value top = h.front();
        h.front() = h.back();
        h.pop_back();
        if (!h.empty()) heapSiftDown(h, 0, false);
        return top;
    });
    globals_->vars["heapify"] = biFn({"h"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k != VK::List) panicHere("heapify expects a list");
        auto& h = *a[0].vec;
        if (h.size() > 1)
            for (size_t i = h.size() / 2; i-- > 0;) heapSiftDown(h, i, false);
        return Value::none();
    });
    globals_->vars["heappush_max"] = biFn({"h", "x"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k != VK::List) panicHere("heappush_max expects a list");
        a[0].vec->push_back(a.size() > 1 ? a[1] : Value::none());
        heapSiftUp(*a[0].vec, a[0].vec->size() - 1, true);
        return Value::none();
    });
    globals_->vars["heappop_max"] = biFn({"h"}, [](std::vector<Value>& a) -> Value {
        if (a.empty() || a[0].k != VK::List || a[0].vec->empty())
            panicHere("heappop_max from empty heap");
        auto& h = *a[0].vec;
        Value top = h.front();
        h.front() = h.back();
        h.pop_back();
        if (!h.empty()) heapSiftDown(h, 0, true);
        return top;
    });
    globals_->vars["nlargest"] = biFn({"n", "xs"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> v;
        if (a.size() > 1)
            iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                v.push_back(it); return true;
            }, globals_);
        std::stable_sort(v.begin(), v.end(),
                         [](const Value& x, const Value& y) { return valLess(y, x); });
        int64_t n = (!a.empty() && a[0].k == VK::Int) ? a[0].i : (int64_t)v.size();
        if (n < 0) n = 0;
        if ((size_t)n < v.size()) v.resize((size_t)n);
        return Value::list(std::move(v));
    });
    globals_->vars["nsmallest"] = biFn({"n", "xs"}, [this](std::vector<Value>& a) -> Value {
        std::vector<Value> v;
        if (a.size() > 1)
            iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                v.push_back(it); return true;
            }, globals_);
        std::stable_sort(v.begin(), v.end(),
                         [](const Value& x, const Value& y) { return valLess(x, y); });
        int64_t n = (!a.empty() && a[0].k == VK::Int) ? a[0].i : (int64_t)v.size();
        if (n < 0) n = 0;
        if ((size_t)n < v.size()) v.resize((size_t)n);
        return Value::list(std::move(v));
    });

    // ---- M9: `sync` primitives ---------------------------------------------
    globals_->vars["Mutex"] = syncCtorMutex();
    globals_->vars["RwMutex"] = syncCtorRwMutex();
    globals_->vars["RWLock"] = syncCtorRwMutex();
    globals_->vars["WaitGroup"] = syncCtorWaitGroup();
    globals_->vars["Once"] = syncCtorOnce();
    globals_->vars["Cond"] = syncCtorCond();
    globals_->vars["Atomic"] = syncCtorAtomic();

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

    // Phase-3 bigint surface ------------------------------------------------
    globals_->vars["BigInt"] = biFn({"s"}, [](std::vector<Value>& a) -> Value {
        bignum::Big b;
        if (a[0].k == VK::Int) {
            b = bignum::fromI64(a[0].i);
        } else if (a[0].k == VK::Str) {
            b = bignum::fromStr(a[0].s);
        } else if (a[0].k == VK::BigInt) {
            return a[0];
        } else {
            panicHere("big_int() expects an int or string");
        }
        Value v;
        v.k = VK::BigInt;
        v.neg = b.neg;
        v.limbs = std::make_shared<std::vector<uint64_t>>(std::move(b.limbs));
        return v;
    });
    globals_->vars["big_int"] = globals_->vars["BigInt"];
    globals_->vars["BitLength"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::BigInt) {
            if (!a[0].limbs) return Value::integer(0);
            return Value::integer(bignum::bitLength(*a[0].limbs));
        }
        // existing int path
        int64_t v = a[0].i;
        uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
        if (u == 0) return Value::integer(0);
        int n = 0; while (u) { n++; u >>= 1; }
        return Value::integer(n);
    });
    globals_->vars["BitCount"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::BigInt) {
            if (!a[0].limbs) return Value::integer(0);
            return Value::integer(bignum::bitCount(*a[0].limbs));
        }
        int64_t v = a[0].i;
        unsigned c = 0;
        uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
        while (u) { c += (u & 1); u >>= 1; }
        return Value::integer(c);
    });
    globals_->vars["IsPrime"] = biFn({"n"}, [](std::vector<Value>& a) -> Value {
        if (a[0].k == VK::Int) {
            int64_t n = a[0].i;
            if (n < 2) return Value::boolean(false);
            for (int64_t d = 2; d * d <= n; ++d)
                if (n % d == 0) return Value::boolean(false);
            return Value::boolean(true);
        }
        if (a[0].k == VK::BigInt) {
            bignum::Big b;
            b.neg = a[0].neg;
            if (a[0].limbs) b.limbs = *a[0].limbs;
            return Value::boolean(bignum::isPrime(b));
        }
        panicHere("is_prime: expected int or bigint");
    });
    globals_->vars["PowMod"] = biFn({"b", "e", "m"}, [](std::vector<Value>& a) -> Value {
        bignum::Big base = bignum::fromStr(toStr(a[0]));
        bignum::Big exp  = bignum::fromStr(toStr(a[1]));
        bignum::Big mod  = bignum::fromStr(toStr(a[2]));
        bignum::Big r = base.powmod(exp, mod);
        Value v; v.k = VK::BigInt; v.neg = r.neg;
        v.limbs = std::make_shared<std::vector<uint64_t>>(std::move(r.limbs));
        return v;
    });

    // Kirn-style: previously checker-only free builtins that the runtime did
    // not bind. Implemented here so free Trunc/Fract/IsNan/IsInfinite/IsFinite
    // and ToString work (they used to fail at runtime).
    globals_->vars["Trunc"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::integer((int64_t)std::trunc(d));
    });
    globals_->vars["Fract"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::floating(d - std::trunc(d));
    });
    globals_->vars["IsNan"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::boolean(std::isnan(d));
    });
    globals_->vars["IsInfinite"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::boolean(std::isinf(d));
    });
    globals_->vars["IsFinite"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
        return Value::boolean(std::isfinite(d));
    });
    globals_->vars["ToString"] = biFn({"x"}, [](std::vector<Value>& a) -> Value {
        return Value::str(toStr(a[0]));
    });

    // Kirn-style dual-named free builtins: each CapCase name also binds the
    // __CapCase form (e.g. Rational / __Rational). The old snake_case forms
    // are gone.
    for (const char* n : {
             "Ord", "Chr", "BitLength", "BitCount",
             "Rational", "Numerator", "Denominator",
             "ToFloat", "ToInt", "ToRational", "FAbs",
             "Exp", "Log", "Log2", "Log10",
             "Sin", "Cos", "Tan", "Sinh", "Cosh", "Tanh",
             "Floor", "Ceil", "Round", "Trunc", "Fract",
             "IsNan", "IsInfinite", "IsFinite",
             "BigInt", "IsPrime", "PowMod", "ToBytes", "FromBytes",
             "HexDigits", "ToString",
             "RationalAdd", "RationalSub", "RationalMul", "RationalDiv",
             "RationalPow", "RationalEq", "RationalNe", "RationalLt",
             "RationalLe", "RationalGt", "RationalGe",
             "CopySign", "Pow"}) {
        auto it = globals_->vars.find(n);
        if (it != globals_->vars.end())
            globals_->vars[std::string("__") + n] = it->second;
    }

    // pseudo-modules are prebound; explicit imports rebind the same values
    for (const char* m :
         {"math", "time", "io", "mem", "json", "text", "os", "parallel",
          "error", "sync", "reflect", "net"})
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
// module loader: dotted/slash import paths -> <dir>/<path>.kn source files
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
//   kirn.toml [package] main = "..."
//     -> pin.kn (package initializer / public-API aggregator)
//     -> mod.kn -> <dirname>.kn -> lone *.kn
static bool resolvePackageEntry(const std::string& dir, std::string& out) {
    std::string manifest;
    if (readFileIfExists(dir + "/kirn.toml", manifest)) {
        // current manifest name
    } else if (readFileIfExists(dir + "/kirn.toml", manifest)) {
        std::cerr << "note: found 'kirn.toml' in '" << dir
                  << "' but expected 'kirn.toml' (manifest renamed in this "
                     "release); run:  mv kirn.toml kirn.toml\n";
    }
    if (!manifest.empty()) {
        kirn::tomlmini::Doc doc = kirn::tomlmini::parse(manifest);
        std::string mainf = kirn::tomlmini::get(doc, "package", "main");
        std::string probe;
        if (!mainf.empty() &&
            readFileIfExists(dir + "/" + mainf, probe)) {
            out = dir + "/" + mainf;
            return true;
        }
    }
    const char* defaults[] = {"/pin.kn", "/code/pin.kn", "/mod.kn",
                              "/code/mod.kn"};
    for (const char* d : defaults) {
        std::string probe;
        if (readFileIfExists(dir + d, probe)) {
            out = dir + d;
            return true;
        }
    }
    // <dirname>.kn then a single top-level source file
    std::string base = dir;
    size_t cut = base.find_last_of("/\\");
    if (cut != std::string::npos) base = base.substr(cut + 1);
    std::string probe;
    std::vector<std::string> tops;
    if (readFileIfExists(dir + "/" + base + ".kn", probe))
        tops.push_back(dir + "/" + base + ".kn");
    if (tops.empty()) {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec), end;
        if (!ec)
            for (; it != end; it.increment(ec))
                if (!ec && it->is_regular_file(ec) &&
                    it->path().extension() == ".kn")
                    tops.push_back(it->path().string());
    }
    if (tops.size() == 1) {
        out = tops[0];
        return true;
    }
    return false;
}

Value Interpreter::loadModuleFile(const std::string& dottedRaw) {
    // accept explicit ".kn" suffixes (import "file1.kn")
    std::string dotted = dottedRaw;
    if (dotted.size() > 3 &&
        dotted.compare(dotted.size() - 3, 3, ".kn") == 0)
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
    rel += ".kn";

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
            // package directory (installed under pets/): use entry file
            std::string pkgDir =
                dir + "/" + rel.substr(0, rel.size() - 3);   // strip ".kn"
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
    // Register before executing the body: a module whose evaluation re-enters
    // loadModuleFile for the same dotted name (directly, or through a value it
    // builds) would otherwise recurse until the stack overflows, because the
    // loadedModules_ entry is only written on the way out.
    loadedModules_[dotted] = modEnv;
    for (const auto& sp : prog) {
        const Stmt& s = *sp;
        switch (s.kind) {
            case ast::StKind::FuncDef: {
                if (s.externDef) break;
                const Stmt* fn = &s;
                Value v;
                v.k = VK::Builtin;
                // Carry parameter names so callers can pass named arguments
                // (`mod.f(x: 1)`); without this, named args are dropped and the
                // callee panics with "missing argument".
                v.biParams.reserve(fn->params.size());
                for (const auto& p : fn->params) v.biParams.push_back(p.name);
                v.bi = [this, fn, modEnv](std::vector<Value>& a) -> Value {
                    // POWER_PLAN §2: an `async fn` exported from a module
                    // schedules a task and returns its handle.
                    if (fn->isAsync) {
                        Value callee;
                        callee.k = VK::Fn;
                        callee.fn = fn;
                        callee.env = modEnv;
                        return spawnTask(std::move(callee), std::move(a));
                    }
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
                structEnv_[s.name] = modEnv;
                // export `pub struct`/`pub class` through the module namespace
                // so consumers can reference `mod.Type` and construct it.
                if (s.pub && !s.privateDecl)
                    modEnv->vars[s.name] = Value::module("__struct__:" + s.name);
                break;
            case ast::StKind::EnumDef:
                enums_[s.name] = &s;
                registerEnumDerive(s);
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
                    // A bare prebound-module name binds the native module, never
                    // a same-named file (see isPreboundModule).
                    Value dep;
                    if (isPreboundModule(s.moduleName))
                        dep = Value::none();
                    else
                        dep = loadModuleFile(s.moduleName);
                    // A prebound module must bind under the alias too: the old
                    // code only bound it when `importAlias` was empty, so
                    // `import sync as sysnc;` inside a module left `sysnc`
                    // undefined at call time.
                    std::string bind = s.importAlias.empty()
                                           ? s.moduleName
                                           : s.importAlias;
                    if (dep.k != VK::None)
                        modEnv->vars[bind] = dep;
                    else
                        modEnv->vars[bind] = Value::module(s.moduleName);
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

// ---- process spawning (POWER_PLAN §2.9) ------------------------------------
// A tiny POSIX-free-standing launcher: create temp files for stdin/stdout/stderr,
// fork+exec the argv, and hand back the pid and paths. `os.run` waits and reads
// them; `os.spawn` leaves the child running for `Child.wait`/`kill`.
namespace {
struct SpawnedProc {
    int pid = -1;
    std::string outPath, errPath, inPath;
};

// Set by the SIGINT handler; `sig_atomic_t` keeps the handler async-signal-safe.
static volatile sig_atomic_t g_ctrlC = 0;
#ifndef _WIN32
static void kirnCtrlCHandler(int) { g_ctrlC = 1; }
#endif

static bool launchProcess(const std::vector<std::string>& argv,
                          SpawnedProc& sp, std::string& err) {
#ifndef _WIN32
    if (argv.empty() || argv[0].empty()) {
        err = "empty command";
        return false;
    }
    static std::atomic<uint64_t> seq{0};
    auto stamp = std::to_string((long long)::getpid()) + "_" +
                 std::to_string((unsigned long long)seq.fetch_add(1));
    auto base = std::filesystem::temp_directory_path() / ("kirn_proc_" + stamp);
    sp.outPath = base.string() + ".out";
    sp.errPath = base.string() + ".err";
    sp.inPath  = base.string() + ".in";
    for (const auto& p : {sp.outPath, sp.errPath, sp.inPath}) {
        std::ofstream touch(p, std::ios::binary | std::ios::trunc);
        if (!touch) { err = "cannot create temp file '" + p + "'"; return false; }
    }
    // Build argv before fork: the child of a multithreaded process may only call
    // async-signal-safe functions, and allocating there could deadlock.
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) { err = "fork failed"; return false; }
    if (pid == 0) {
        int in  = ::open(sp.inPath.c_str(),  O_RDONLY);
        int out = ::open(sp.outPath.c_str(), O_WRONLY | O_TRUNC);
        int er  = ::open(sp.errPath.c_str(), O_WRONLY | O_TRUNC);
        if (in  >= 0) ::dup2(in,  0);
        if (out >= 0) ::dup2(out, 1);
        if (er  >= 0) ::dup2(er,  2);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    sp.pid = (int)pid;
    return true;
#else
    (void)argv; (void)sp;
    err = "process spawn is not supported on this platform";
    return false;
#endif
}

static int waitProcess(int pid) {
#ifndef _WIN32
    int status = 0;
    if (::waitpid((pid_t)pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#else
    (void)pid;
    return -1;
#endif
}

static std::string slurpFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static Value runProcess(const std::vector<std::string>& argv) {
    SpawnedProc sp;
    std::string err;
    if (!launchProcess(argv, sp, err)) panicHere(err);
    int code = waitProcess(sp.pid);
    std::string out = slurpFile(sp.outPath);
    std::string errOut = slurpFile(sp.errPath);
    std::error_code ec;
    std::filesystem::remove(sp.outPath, ec);
    std::filesystem::remove(sp.errPath, ec);
    std::filesystem::remove(sp.inPath, ec);
    Value d = Value::dict();
    dictPutInPlace(d, Value::str("code"), Value::integer(code));
    dictPutInPlace(d, Value::str("stdout"), Value::str(std::move(out)));
    dictPutInPlace(d, Value::str("stderr"), Value::str(std::move(errOut)));
    return d;
}

// ---- sockets (POWER_PLAN §2.7) ---------------------------------------------
#ifndef _WIN32
static bool splitHostPort(const std::string& s, std::string& host,
                          std::string& port) {
    if (!s.empty() && s[0] == '[') {
        auto close = s.find(']');
        if (close == std::string::npos || close + 1 >= s.size() ||
            s[close + 1] != ':')
            return false;
        host = s.substr(1, close - 1);
        port = s.substr(close + 2);
        return true;
    }
    auto pos = s.rfind(':');
    if (pos == std::string::npos) return false;
    host = s.substr(0, pos);
    port = s.substr(pos + 1);
    if (host.empty()) host = "0.0.0.0";
    return true;
}

static std::string sockAddrStr(const sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    if (getnameinfo(sa, len, host, sizeof host, serv, sizeof serv,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return "?";
    std::string h = host;
    if (h.find(':') != std::string::npos) h = "[" + h + "]";
    return h + ":" + serv;
}

static bool resolveAddr(const std::string& addr, int socktype, bool passive,
                        addrinfo*& out, std::string& err) {
    std::string host, port;
    if (!splitHostPort(addr, host, port)) {
        err = "address must be host:port (got '" + addr + "')";
        return false;
    }
    addrinfo hints{};
    hints.ai_socktype = socktype;
    hints.ai_family = AF_UNSPEC;
    if (passive) hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(host == "0.0.0.0" ? nullptr : host.c_str(),
                         port.c_str(), &hints, &out);
    if (rc != 0) {
        err = gai_strerror(rc);
        return false;
    }
    return true;
}

static int bindSocket(const std::string& addr, int socktype, std::string& bound,
                      std::string& err) {
    addrinfo* res = nullptr;
    if (!resolveAddr(addr, socktype, true, res, err)) return -1;
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            if (socktype == SOCK_STREAM && ::listen(fd, 128) != 0) {
                err = std::string("listen: ") + std::strerror(errno);
                ::close(fd);
                fd = -1;
                continue;
            }
            sockaddr_storage ss{};
            socklen_t sl = sizeof ss;
            if (::getsockname(fd, (sockaddr*)&ss, &sl) == 0)
                bound = sockAddrStr((sockaddr*)&ss, sl);
            break;
        }
        err = std::string("bind: ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && err.empty()) err = "bind failed";
    return fd;
}

static int connectSocket(const std::string& addr, int socktype,
                         std::string& peer, std::string& err) {
    addrinfo* res = nullptr;
    if (!resolveAddr(addr, socktype, false, res, err)) return -1;
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            peer = addr;
            break;
        }
        err = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0 && err.empty()) err = "connect failed";
    return fd;
}
#endif
}  // namespace

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

    if (mod == "sync") {
        if (name == "Mutex") return syncCtorMutex();
        if (name == "RwMutex" || name == "RWLock") return syncCtorRwMutex();
        if (name == "WaitGroup") return syncCtorWaitGroup();
        if (name == "Once") return syncCtorOnce();
        if (name == "Cond") return syncCtorCond();
        if (name == "Atomic") return syncCtorAtomic();
        panicHere("sync has no member '" + name + "'");
    }

    // M10: runtime reflection. `typeof` is the same name query; these members
    // expose the shape of a value (kind/type/fields/methods) and deep equality.
    if (mod == "reflect") {
        if (name == "type" || name == "typeof")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                return Value::str(a.empty() ? "none" : typeName(a[0]));
            });
        if (name == "kind_of" || name == "kind")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                return Value::str(a.empty() ? "none" : kindName(a[0]));
            });
        if (name == "fields")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                Value out = Value::dict();
                if (a.empty()) return out;
                const Value& x = a[0];
                const std::vector<std::pair<std::string, Value>>* fs = nullptr;
                if (x.k == VK::Struct) fs = &x.fields;
                else if (x.k == VK::Heap && x.heap) fs = &x.heap->fields;
                if (fs)
                    for (const auto& f : *fs)
                        out.map->push_back({Value::str(f.first), f.second});
                return out;
            });
        if (name == "field_names")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                std::vector<Value> names;
                if (a.empty()) return Value::list(std::move(names));
                const Value& x = a[0];
                const std::vector<std::pair<std::string, Value>>* fs = nullptr;
                if (x.k == VK::Struct) fs = &x.fields;
                else if (x.k == VK::Heap && x.heap) fs = &x.heap->fields;
                if (fs)
                    for (const auto& f : *fs)
                        names.push_back(Value::str(f.first));
                return Value::list(std::move(names));
            });
        if (name == "methods")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                std::vector<Value> out;
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si != structs_.end())
                    for (const auto& m : si->second->body)
                        if (m->kind == ast::StKind::FuncDef)
                            out.push_back(Value::str(m->name));
                for (const auto& imp : impls_)
                    if (imp.typeName == tn)
                        for (const auto& kv : imp.methods)
                            out.push_back(Value::str(kv.first));
                return Value::list(std::move(out));
            });
        if (name == "is_of_type")
            return biFn({"v", "t"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 2) return Value::boolean(false);
                return Value::boolean(typeName(a[0]) == typeRefName(a[1]));
            });
        if (name == "get_field")
            return biFn({"x", "name"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 2 || a[1].k != VK::Str)
                    panicHere("reflect.get_field expects (value, name)");
                const Value& x = a[0];
                const std::vector<std::pair<std::string, Value>>* fs = nullptr;
                if (x.k == VK::Struct) fs = &x.fields;
                else if (x.k == VK::Heap && x.heap) fs = &x.heap->fields;
                if (fs)
                    for (const auto& f : *fs)
                        if (f.first == a[1].s) return f.second;
                panicHere("reflect.get_field: no field '" + a[1].s + "' on " +
                          typeName(x));
            });
        if (name == "set_field")
            return biFn({"x", "name", "value"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 3 || a[1].k != VK::Str)
                    panicHere("reflect.set_field expects (value, name, value)");
                if (a[0].k == VK::Heap && a[0].heap)
                    for (auto& f : a[0].heap->fields)
                        if (f.first == a[1].s) {
                            f.second = a[2];
                            return Value::none();
                        }
                panicHere("reflect.set_field: '" + a[1].s +
                          "' is not a settable field on " + typeName(a[0]));
            });
        if (name == "has_field")
            return biFn({"x", "name"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 2 || a[1].k != VK::Str)
                    return Value::boolean(false);
                const Value& x = a[0];
                const std::vector<std::pair<std::string, Value>>* fs = nullptr;
                if (x.k == VK::Struct) fs = &x.fields;
                else if (x.k == VK::Heap && x.heap) fs = &x.heap->fields;
                if (fs)
                    for (const auto& f : *fs)
                        if (f.first == a[1].s) return Value::boolean(true);
                return Value::boolean(false);
            });
        if (name == "deep_equal")
            return biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 2) return Value::boolean(false);
                return Value::boolean(valEq(a[0], a[1]));
            });
        if (name == "new")
            return biFn({"t", "fields"}, [this](std::vector<Value>& a) -> Value {
                if (a.empty())
                    panicHere("reflect.new expects a type reference");
                std::string tn = typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si == structs_.end())
                    panicHere("reflect.new: unknown struct '" + tn + "'");
                Value v = Value::structV(tn);
                for (const auto& fd : si->second->fields) {
                    if (fd.defaultValue)
                        v.fields.emplace_back(fd.name, eval(*fd.defaultValue,
                                                            globals_));
                    else if (auto def = containerDefaultFor(fd.type))
                        v.fields.emplace_back(fd.name, std::move(*def));
                    else if (fd.type && fd.type->kind == ast::TyKind::Name) {
                        const std::string& ft = fd.type->name;
                        if (ft == "Int" || ft == "int" || ft == "i64" ||
                            ft == "i32" || ft == "i16" || ft == "i8" ||
                            ft == "u64" || ft == "u32" || ft == "u16" ||
                            ft == "u8")
                            v.fields.emplace_back(fd.name, Value::integer(0));
                        else if (ft == "f64" || ft == "f32" || ft == "float" ||
                                 ft == "Float")
                            v.fields.emplace_back(fd.name, Value::floating(0.0));
                        else if (ft == "bool" || ft == "Bool")
                            v.fields.emplace_back(fd.name, Value::boolean(false));
                        else if (ft == "str" || ft == "String" ||
                                 ft == "string" || ft == "Str")
                            v.fields.emplace_back(fd.name, Value::str(""));
                        else
                            v.fields.emplace_back(fd.name, Value::none());
                    } else
                        v.fields.emplace_back(fd.name, Value::none());
                }
                // POWER_PLAN §3: optional overrides for reflection serde —
                // `reflect.new(T, {field: value, ...})`.
                if (a.size() > 1 && a[1].k == VK::Dict && a[1].map)
                    for (auto& f : v.fields)
                        for (const auto& kv : *a[1].map)
                            if (kv.first.k == VK::Str && kv.first.s == f.first)
                                f.second = kv.second;
                return v;
            });
        if (name == "sizeof" || name == "size_of")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                if (a.empty()) return Value::integer(0);
                const Value& x = a[0];
                if (x.k == VK::List || x.k == VK::Tuple || x.k == VK::Set ||
                    x.k == VK::Gen || x.k == VK::Deque)
                    return Value::integer(x.vec ? (int64_t)x.vec->size() : 0);
                if (x.k == VK::Dict || x.k == VK::Counter)
                    return Value::integer(x.map ? (int64_t)x.map->size() : 0);
                if (x.k == VK::Str) return Value::integer((int64_t)x.s.size());
                return Value::integer(0);
            });
        // POWER_PLAN §3: enum introspection + construction for reflection-based
        // serde.
        if (name == "variant")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                if (a.empty() || a[0].k != VK::EnumV)
                    panicHere("reflect.variant expects an enum value");
                return Value::str(a[0].variant);
            });
        if (name == "payload")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                Value out = Value::list();
                if (!a.empty() && a[0].k == VK::EnumV)
                    for (const auto& p : a[0].payload) out.vec->push_back(p);
                return out;
            });
        if (name == "variants")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                std::vector<Value> out;
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    for (const auto& var : ei->second->variants)
                        out.push_back(Value::str(var.name));
                return Value::list(std::move(out));
            });
        if (name == "enum_payload_names")
            return biFn({"t", "variant"}, [this](std::vector<Value>& a) -> Value {
                std::vector<Value> out;
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                std::string var = a.size() > 1 && a[1].k == VK::Str ? a[1].s : "";
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    for (const auto& v : ei->second->variants)
                        if (v.name == var) {
                            for (const auto& pd : v.payload)
                                out.push_back(Value::str(pd.name));
                            break;
                        }
                return Value::list(std::move(out));
            });
        if (name == "enum_payload_type_names")
            return biFn({"t", "variant"}, [this](std::vector<Value>& a) -> Value {
                std::vector<Value> out;
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                std::string var = a.size() > 1 && a[1].k == VK::Str ? a[1].s : "";
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    for (const auto& v : ei->second->variants)
                        if (v.name == var) {
                            for (const auto& pd : v.payload)
                                out.push_back(Value::str(
                                    pd.type ? typeBaseName(*pd.type) : ""));
                            break;
                        }
                return Value::list(std::move(out));
            });
        if (name == "enum_new")
            return biFn({"t", "variant", "payload"},
                        [this](std::vector<Value>& a) -> Value {
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                std::string var =
                    a.size() > 1 && a[1].k == VK::Str ? a[1].s : "";
                auto ei = enums_.find(tn);
                if (ei == enums_.end())
                    panicHere("reflect.enum_new: unknown enum '" + tn + "'");
                const ast::Variant* vd = nullptr;
                for (const auto& v : ei->second->variants)
                    if (v.name == var) { vd = &v; break; }
                if (!vd)
                    panicHere("reflect.enum_new: enum '" + tn +
                              "' has no variant '" + var + "'");
                Value ev = Value::enumV(tn, var);
                std::vector<Value> pls;
                if (a.size() > 2 && a[2].k == VK::List && a[2].vec)
                    pls = *a[2].vec;
                for (size_t i = 0; i < vd->payload.size(); ++i)
                    ev.payload.push_back(i < pls.size() ? pls[i] : Value::none());
                return ev;
            });
        // POWER_PLAN §3: resolve a struct/enum name to a type reference value.
        if (name == "type_ref")
            return biFn({"name"}, [this](std::vector<Value>& a) -> Value {
                std::string n = (!a.empty() && a[0].k == VK::Str) ? a[0].s : "";
                if (structs_.count(n)) return Value::module("__struct__:" + n);
                if (enums_.count(n)) return Value::module("__enum__:" + n);
                return Value::none();
            });
        // POWER_PLAN §3: declared field type names for a struct type reference.
        if (name == "field_type_names")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                Value out = Value::dict();
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si != structs_.end())
                    for (const auto& fd : si->second->fields)
                        out.map->push_back(
                            {Value::str(fd.name),
                             Value::str(fd.type ? typeBaseName(*fd.type) : "")});
                return out;
            });
        // POWER_PLAN §3: first-level generic arg names per field
        // (`tags: list[Str]` -> tags => ["Str"]).
        if (name == "field_generics")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                Value out = Value::dict();
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si != structs_.end())
                    for (const auto& fd : si->second->fields) {
                        Value g = Value::list();
                        if (fd.type)
                            for (const auto& gn : typeGenericNames(*fd.type))
                                g.vec->push_back(Value::str(gn));
                        out.map->push_back({Value::str(fd.name), std::move(g)});
                    }
                return out;
            });
        // POWER_PLAN §3.2: attribute introspection for serde options.
        // `type_attrs`/`field_attrs`/`variant_attrs` expose the parsed
        // `@[serde(...)]` decorators as data so `pet.codec` can honour
        // rename/rename_all/skip/flatten/tag/deny_unknown_fields/transparent.
        if (name == "type_attrs")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si != structs_.end())
                    return decoratorsValue(si->second->decorators);
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    return decoratorsValue(ei->second->decorators);
                return Value::dict();
            });
        if (name == "field_attrs")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                Value out = Value::dict();
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto si = structs_.find(tn);
                if (si != structs_.end())
                    for (const auto& fd : si->second->fields)
                        out.map->push_back(
                            {Value::str(fd.name),
                             decoratorsValue(fd.decorators)});
                return out;
            });
        if (name == "variant_attrs")
            return biFn({"t"}, [this](std::vector<Value>& a) -> Value {
                Value out = Value::dict();
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    for (const auto& var : ei->second->variants)
                        out.map->push_back(
                            {Value::str(var.name),
                             decoratorsValue(var.decorators)});
                return out;
            });
        if (name == "payload_attrs")
            return biFn({"t", "variant"},
                        [this](std::vector<Value>& a) -> Value {
                Value out = Value::list();
                std::string tn = a.empty() ? std::string() : typeRefName(a[0]);
                std::string var =
                    a.size() > 1 && a[1].k == VK::Str ? a[1].s : "";
                auto ei = enums_.find(tn);
                if (ei != enums_.end())
                    for (const auto& v : ei->second->variants)
                        if (v.name == var) {
                            for (const auto& pd : v.payload)
                                out.vec->push_back(
                                    decoratorsValue(pd.decorators));
                            break;
                        }
                return out;
            });
        // `case(name, rule, "field"|"variant")` — serde rename_all rules.
        if (name == "case")
            return biFn({"name", "rule", "kind"},
                        [](std::vector<Value>& a) -> Value {
                if (a.empty() || a[0].k != VK::Str)
                    panicHere("reflect.case expects (name, rule, kind)");
                std::string rule = a.size() > 1 ? toStr(a[1]) : "";
                std::string kind = a.size() > 2 ? toStr(a[2]) : "field";
                return Value::str(
                    serdeCase(a[0].s, rule, kind == "variant"));
            });
        panicHere("reflect has no member '" + name + "'");
    }

    if (mod == "math") {
        if (name == "pi") return Value::floating(3.14159265358979323846);
        if (name == "e")  return Value::floating(2.71828182845904523536);
        if (name == "tau") return Value::floating(6.28318530717958647692);
        if (name == "inf") return Value::floating(HUGE_VAL);
        if (name == "nan") return Value::floating(NAN);
        if (name == "min" || name == "max") {
            bool isMin = name == "min";
            return biFn({"a", "b"}, [isMin](std::vector<Value>& a) -> Value {
                double x = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double y = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                return Value::floating(isMin ? std::fmin(x, y) : std::fmax(x, y));
            });
        }
        if (name == "copysign")
            return biFn({"x", "sign"}, [](std::vector<Value>& a) -> Value {
                double x = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double s = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                return Value::floating(std::copysign(x, s));
            });
        if (name == "fmod")
            return biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
                double x = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double y = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                return Value::floating(std::fmod(x, y));
            });
        if (name == "floor_div")
            return biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
                int64_t x = a[0].k == VK::Int ? a[0].i : (int64_t)a[0].d;
                int64_t y = a[1].k == VK::Int ? a[1].i : (int64_t)a[1].d;
                if (y == 0) panicHere("math.floor_div: division by zero");
                int64_t q = x / y, r = x % y;
                if (r != 0 && ((r < 0) != (y < 0))) --q;
                return Value::integer(q);
            });
        if (name == "pow")
            return biFn({"a", "b"}, [](std::vector<Value>& a) -> Value {
                double x = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double y = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                return Value::floating(std::pow(x, y));
            });
        if (name == "atan2")
            return biFn({"y", "x"}, [](std::vector<Value>& a) -> Value {
                double y = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double x = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                return Value::floating(std::atan2(y, x));
            });
        if (name == "fma")
            return biFn({"a", "b", "c"}, [](std::vector<Value>& a) -> Value {
                double x = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                double y = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                double z = a[2].k == VK::Int ? (double)a[2].i : a[2].d;
                return Value::floating(std::fma(x, y, z));
            });
        if (name == "gamma")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                return Value::floating(std::tgamma(d));
            });
        if (name == "lgamma")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                return Value::floating(std::lgamma(d));
            });
        if (name == "log")
            return biFn({"x", "base"}, [](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                if (a.size() > 1 && (a[1].k == VK::Int || a[1].k == VK::Float)) {
                    double b = a[1].k == VK::Int ? (double)a[1].i : a[1].d;
                    return Value::floating(std::log(d) / std::log(b));
                }
                return Value::floating(std::log(d));
            });
        if (name == "round")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                return Value::integer((int64_t)std::nearbyint(d));
            });
        if (name == "trunc")
            return biFn({"x"}, [](std::vector<Value>& a) -> Value {
                double d = a[0].k == VK::Int ? (double)a[0].i : a[0].d;
                return Value::integer((int64_t)std::trunc(d));
            });
        double (*impl)(double) = nullptr;
        if      (name == "sqrt")  impl = &std::sqrt;
        else if (name == "sin")   impl = &std::sin;
        else if (name == "cos")   impl = &std::cos;
        else if (name == "tan")   impl = &std::tan;
        else if (name == "asin")  impl = &std::asin;
        else if (name == "acos")  impl = &std::acos;
        else if (name == "atan")  impl = &std::atan;
        else if (name == "sinh")  impl = &std::sinh;
        else if (name == "cosh")  impl = &std::cosh;
        else if (name == "tanh")  impl = &std::tanh;
        else if (name == "log2")  impl = &std::log2;
        else if (name == "log10") impl = &std::log10;
        else if (name == "ln")    impl = &std::log;
        else if (name == "floor") impl = &std::floor;
        else if (name == "ceil")  impl = &std::ceil;
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
        // A timer that fires at an absolute epoch-millisecond deadline. Backs
        // `sleep_until`/`timeout_at` (POWER_PLAN §2.8), which name an Instant.
        if (name == "after_at")
            return biFn({"deadline_ms"}, [](std::vector<Value>& a) -> Value {
                Value t;
                t.k = VK::Timer;
                t.deadlineMs =
                    a.empty() || a[0].k != VK::Int ? nowMs() : a[0].i;
                return t;
            });
        if (name == "now")
            return biFn({}, [](std::vector<Value>&) -> Value {
                return Value::integer(nowMs());
            });
        // Wait on a task handle until it finishes or `ms` elapses: returns true
        // if the task completed (ready), false on timeout. `pet.async.timeout`
        // is built on this - it cannot be a plain `await` because the whole
        // point is to stop waiting before the task does.
        if (name == "wait") {
            return biFn({"handle", "ms"}, [](std::vector<Value>& a) -> Value {
                if (a.empty() || a[0].k != VK::ThreadH) return Value::boolean(true);
                auto t = a[0].thread;
                if (!t) return Value::boolean(true);
                int64_t ms = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 0;
                std::unique_lock<std::mutex> lk(t->m);
                if (t->done) return Value::boolean(true);
                if (ms <= 0) return Value::boolean(t->done);
                bool ok =
                    t->cv.wait_for(lk, std::chrono::milliseconds(ms),
                                   [&] { return t->done; });
                return Value::boolean(ok);
            });
        }
        panicHere("time has no member '" + name + "'");
    }

    // POWER_PLAN §4: rayon-like data parallelism. `join`/`invoke` run Kirn
    // closures on OS threads and block until the results are ready (reusing the
    // task machinery that backs `spawn`/`await`), and the thread-info queries
    // back `pet.parallel`'s pool sizing.
    if (mod == "parallel") {
        if (name == "join")
            return biFn({"a", "b"}, [this](std::vector<Value>& a) -> Value {
                if (a.size() < 2)
                    panicHere("parallel.join expects (fn, fn)");
                Value h = spawnTask(a[0], {});
                Value rb = callValue(a[1], {}, 0, 0);
                Value ra = awaitTask(h);
                Value out = Value::list();
                out.vec->push_back(ra);
                out.vec->push_back(rb);
                return out;
            });
        if (name == "invoke")
            return biFn({"fns"}, [this](std::vector<Value>& a) -> Value {
                Value out = Value::list();
                if (a.empty() || a[0].k != VK::List) return out;
                const auto& fns = *a[0].vec;
                std::vector<Value> handles;
                for (size_t i = 0; i + 1 < fns.size(); ++i)
                    handles.push_back(spawnTask(fns[i], {}));
                Value last = Value::none();
                if (!fns.empty()) last = callValue(fns.back(), {}, 0, 0);
                for (auto& h : handles)
                    out.vec->push_back(awaitTask(h));
                out.vec->push_back(last);
                return out;
            });
        if (name == "current_num_threads")
            return biFn({}, [](std::vector<Value>&) -> Value {
                const char* e = envGet("KIRN_NUM_THREADS");
                if (e && *e) {
                    char* end = nullptr;
                    long n = std::strtol(e, &end, 10);
                    if (end != e && n >= 1 && n <= 4096)
                        return Value::integer((int64_t)n);
                }
                unsigned n = std::thread::hardware_concurrency();
                return Value::integer(n ? (int64_t)n : 1);
            });
        if (name == "current_thread_index")
            return biFn({}, [](std::vector<Value>&) -> Value {
                static std::atomic<int> next{0};
                static thread_local int idx = next.fetch_add(1);
                return Value::integer(idx);
            });
        panicHere("parallel has no member '" + name + "'");
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
        // ---- filesystem primitives (POWER_PLAN §2.9) -----------------------
        // These are the blocking syscalls. `pet.async.fs` wraps each in an
        // `async fn`, so the call runs on a task thread instead of the caller
        // (the same spawn-blocking shape tokio::fs uses).
        if (name == "read_file" || name == "read_bytes") {
            bool asBytes = name == "read_bytes";
            return biFn({"path"}, [asBytes](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                std::ifstream f(path, std::ios::binary);
                if (!f) panicHere("cannot open '" + path + "'");
                std::string data((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                Value v = Value::str(std::move(data));
                if (asBytes) v.k = VK::Bytes;
                return v;
            });
        }
        if (name == "write_file")
            return biFn({"path", "data"}, [](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (!f) panicHere("cannot write '" + path + "'");
                f << (a.size() > 1 && isBuffer(a[1]) ? bytesData(a[1])
                                                     : toStr(a.size() > 1 ? a[1] : Value()));
                f.close();
                return Value::none();
            });
        // Positional whole-file I/O backing `pet.async_io.File` (§2.6).
        if (name == "read_at")
            return biFn({"path", "offset", "len"}, [](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                int64_t off = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 0;
                int64_t len = a.size() > 2 && a[2].k == VK::Int ? a[2].i : -1;
                std::ifstream f(path, std::ios::binary);
                if (!f) panicHere("cannot open '" + path + "'");
                f.seekg(off);
                std::string data;
                if (len < 0) {
                    data.assign((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
                } else {
                    data.resize((size_t)len);
                    f.read(data.data(), (std::streamsize)len);
                    data.resize((size_t)f.gcount());
                }
                Value v = Value::str(std::move(data));
                v.k = VK::Bytes;
                return v;
            });
        if (name == "write_at")
            return biFn({"path", "offset", "data"}, [](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                int64_t off = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 0;
                std::string data =
                    a.size() > 2 && isBuffer(a[2])
                        ? bytesData(a[2])
                        : toStr(a.size() > 2 ? a[2] : Value());
                std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
                if (!f) {
                    std::ofstream create(path, std::ios::binary);
                    create.close();
                    f.open(path, std::ios::in | std::ios::out | std::ios::binary);
                }
                if (!f) panicHere("cannot write '" + path + "'");
                f.seekp(off);
                f.write(data.data(), (std::streamsize)data.size());
                f.flush();
                if (!f) panicHere("write failed '" + path + "'");
                return Value::integer((int64_t)data.size());
            });
        if (name == "file_size")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                auto sz = std::filesystem::file_size(toStr(a[0]), ec);
                if (ec) panicHere("file_size '" + toStr(a[0]) + "': no such file");
                return Value::integer((int64_t)sz);
            });
        if (name == "copy_file")
            return biFn({"from", "to"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                std::filesystem::copy_file(toStr(a[0]), toStr(a[1]),
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) panicHere("copy '" + toStr(a[0]) + "' -> '" +
                                  toStr(a[1]) + "': " + ec.message());
                auto sz = std::filesystem::file_size(toStr(a[1]), ec);
                return Value::integer(ec ? 0 : (int64_t)sz);
            });
        if (name == "rename")
            return biFn({"from", "to"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                std::filesystem::rename(toStr(a[0]), toStr(a[1]), ec);
                if (ec) panicHere("rename '" + toStr(a[0]) + "' -> '" +
                                  toStr(a[1]) + "': " + ec.message());
                return Value::none();
            });
        if (name == "remove_file")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                bool removed = std::filesystem::remove(toStr(a[0]), ec);
                if (ec) panicHere("remove '" + toStr(a[0]) + "': " + ec.message());
                if (!removed) panicHere("remove '" + toStr(a[0]) + "': no such file");
                return Value::none();
            });
        if (name == "create_dir_all")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                std::filesystem::create_directories(toStr(a[0]), ec);
                if (ec) panicHere("create_dir_all '" + toStr(a[0]) + "': " +
                                  ec.message());
                return Value::none();
            });
        if (name == "exists")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::error_code ec;
                bool ok = std::filesystem::exists(toStr(a[0]), ec);
                return Value::boolean(ok && !ec);
            });
        if (name == "metadata")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                std::error_code ec;
                auto st = std::filesystem::status(path, ec);
                if (ec || st.type() == std::filesystem::file_type::not_found)
                    panicHere("metadata '" + path + "': no such file");
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("is_file"),
                               Value::boolean(std::filesystem::is_regular_file(st)));
                dictPutInPlace(d, Value::str("is_dir"),
                               Value::boolean(std::filesystem::is_directory(st)));
                auto sz = std::filesystem::file_size(path, ec);
                dictPutInPlace(d, Value::str("size"),
                               Value::integer(ec ? 0 : (int64_t)sz));
                auto mt = std::filesystem::last_write_time(path, ec);
                int64_t ms = 0;
                if (!ec) {
                    auto sys = std::chrono::time_point_cast<
                        std::chrono::milliseconds>(
                        mt - decltype(mt)::clock::now() +
                        std::chrono::system_clock::now());
                    ms = (int64_t)sys.time_since_epoch().count();
                }
                dictPutInPlace(d, Value::str("modified_ms"),
                               Value::integer(ms));
                return d;
            });
        if (name == "read_dir")
            return biFn({"path"}, [](std::vector<Value>& a) -> Value {
                std::string path = toStr(a[0]);
                std::error_code ec;
                std::filesystem::directory_iterator it(path, ec), end;
                if (ec) panicHere("read_dir '" + path + "': " + ec.message());
                Value out = Value::list();
                for (; it != end; it.increment(ec)) {
                    if (ec) break;
                    out.vec->push_back(Value::str(it->path().filename().string()));
                }
                return out;
            });
        // ---- process primitives (POWER_PLAN §2.9) --------------------------
        // `run` blocks to completion and captures output; `spawn` returns a
        // handle (pid + temp paths) for `pet.async.process.Child`.
        if (name == "run")
            return biFn({"argv"}, [](std::vector<Value>& a) -> Value {
                std::vector<std::string> argv;
                if (!a.empty() && a[0].k == VK::List && a[0].vec)
                    for (auto& e : *a[0].vec) argv.push_back(toStr(e));
                else if (!a.empty())
                    argv.push_back(toStr(a[0]));
                return runProcess(argv);
            });
        if (name == "spawn")
            return biFn({"argv"}, [](std::vector<Value>& a) -> Value {
                std::vector<std::string> argv;
                if (!a.empty() && a[0].k == VK::List && a[0].vec)
                    for (auto& e : *a[0].vec) argv.push_back(toStr(e));
                else if (!a.empty())
                    argv.push_back(toStr(a[0]));
                SpawnedProc sp;
                std::string err;
                if (!launchProcess(argv, sp, err)) panicHere(err);
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("pid"), Value::integer(sp.pid));
                dictPutInPlace(d, Value::str("out_path"), Value::str(sp.outPath));
                dictPutInPlace(d, Value::str("err_path"), Value::str(sp.errPath));
                dictPutInPlace(d, Value::str("in_path"), Value::str(sp.inPath));
                return d;
            });
        if (name == "wait")
            return biFn({"pid"}, [](std::vector<Value>& a) -> Value {
                int pid = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                return Value::integer(waitProcess(pid));
            });
        if (name == "kill")
            return biFn({"pid"}, [](std::vector<Value>& a) -> Value {
#ifndef _WIN32
                if (!a.empty() && a[0].k == VK::Int)
                    ::kill((pid_t)a[0].i, SIGKILL);
#endif
                return Value::none();
            });
        // ---- signal primitives (POWER_PLAN §2.9) ---------------------------
        // `install_ctrl_c` arms a SIGINT handler that only raises a flag;
        // `ctrl_c_pending` polls it, so `pet.async.signal.ctrl_c` can wait
        // cooperatively with a timer instead of blocking a worker.
        if (name == "install_ctrl_c")
            return biFn({}, [](std::vector<Value>&) -> Value {
#ifndef _WIN32
                std::signal(SIGINT, kirnCtrlCHandler);
#endif
                return Value::none();
            });
        if (name == "ctrl_c_pending")
            return biFn({}, [](std::vector<Value>&) -> Value {
                return Value::integer((int64_t)g_ctrlC);
            });
        if (name == "reset_ctrl_c")
            return biFn({}, [](std::vector<Value>&) -> Value {
                g_ctrlC = 0;
                return Value::none();
            });
        panicHere("os has no member '" + name + "'");
    }

    // POWER_PLAN §5.1/5.2: anyhow-style dynamic error. `error` is the import
    // root for `pet.error` (importRootOf gives "error"); the prebound module
    // value is Value::module("error").
    if (mod == "error" || mod == "pet.error") {
        if (name == "msg")
            return biFn({"msg"}, [](std::vector<Value>& a) -> Value {
                return mkError(toStr(a[0]), Value::none(), Value::none());
            });
        if (name == "new")
            return biFn({"e"}, [](std::vector<Value>& a) -> Value {
                return mkError(toStr(a[0]), Value::none(), a[0]);
            });
        if (name == "context")   // error.context(r, c) — same as r.context(c)
            return biFn({"r", "ctx"}, [](std::vector<Value>& a) -> Value {
                return errorWithContext(a[0], toStr(a[1]));
            });
        if (name == "chain")
            return biFn({"e"}, [](std::vector<Value>& a) -> Value {
                std::vector<Value> out;
                const Value* cur = a.empty() ? nullptr : &a[0];
                while (cur && isErrorStruct(cur->typeName)) {
                    out.push_back(Value::str(errorFieldStr(*cur, "msg")));
                    const Value* src = structField(*cur, "source");
                    cur = (src && isErrorStruct(src->typeName)) ? src : nullptr;
                }
                return Value::list(std::move(out));
            });
        if (name == "root_cause")
            return biFn({"e"}, [](std::vector<Value>& a) -> Value {
                if (a.empty()) return Value::none();
                const Value* cur = &a[0];
                while (cur && isErrorStruct(cur->typeName)) {
                    const Value* src = structField(*cur, "source");
                    if (src && isErrorStruct(src->typeName)) { cur = src; continue; }
                    break;
                }
                return (cur && isErrorStruct(cur->typeName)) ? *cur : a[0];
            });
        if (name == "is" || name == "downcast") {
            bool down = (name == "downcast");
            return biFn({"e", "name"}, [down](std::vector<Value>& a) -> Value {
                const Value* e = a.size() > 0 ? &a[0] : nullptr;
                std::string want = a.size() > 1 ? toStr(a[1]) : "";
                // origin's runtime type name must match `want` (dot or plain).
                const Value* origin = e ? structField(*e, "origin") : nullptr;
                if (!origin) return Value::boolean(!down ? false : false);
                std::string tn = typeName(*origin);
                bool match = (tn == want) ||
                             (tn.rfind(want + ".", 0) == 0) ||
                             (want.rfind(tn, 0) == 0);
                if (down) return match ? *origin : Value::none();
                return Value::boolean(match);
            });
        }
        if (name == "display")
            return biFn({"e"}, [](std::vector<Value>& a) -> Value {
                return Value::str(a.empty() ? "" : toStr(a[0]));
            });
        if (name == "bail")
            return biFn({"msg"}, [](std::vector<Value>& a) -> Value {
                throw SignalRaise{Value::resultErr(
                    mkError(toStr(a[0]), Value::none(), Value::none()))};
            });
        if (name == "ensure")
            return biFn({"cond", "msg"}, [](std::vector<Value>& a) -> Value {
                if (a.size() < 2 || !truthy(a[0]))
                    throw SignalRaise{Value::resultErr(
                        mkError(toStr(a.size() > 1 ? a[1] : a[0]),
                                Value::none(), Value::none()))};
                return Value::none();
            });
        panicHere("error has no member '" + name + "'");
    }

    // POWER_PLAN §2.7: blocking POSIX sockets. `pet.async_net` wraps each in
    // an `async fn`; fds are plain Ints, so the stdlib handles are cheap.
    if (mod == "net") {
#ifndef _WIN32
        if (name == "tcp_bind")
            return biFn({"addr"}, [](std::vector<Value>& a) -> Value {
                std::string bound, err;
                int fd = bindSocket(toStr(a[0]), SOCK_STREAM, bound, err);
                if (fd < 0) panicHere("tcp_bind: " + err);
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("fd"), Value::integer(fd));
                dictPutInPlace(d, Value::str("addr"), Value::str(bound));
                return d;
            });
        if (name == "tcp_accept")
            return biFn({"fd"}, [](std::vector<Value>& a) -> Value {
                int lfd = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                sockaddr_storage ss{};
                socklen_t sl = sizeof ss;
                int fd = ::accept(lfd, (sockaddr*)&ss, &sl);
                if (fd < 0) panicHere(std::string("tcp_accept: ") +
                                      std::strerror(errno));
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("fd"), Value::integer(fd));
                dictPutInPlace(d, Value::str("peer"),
                               Value::str(sockAddrStr((sockaddr*)&ss, sl)));
                return d;
            });
        if (name == "tcp_connect")
            return biFn({"addr"}, [](std::vector<Value>& a) -> Value {
                std::string peer, err;
                int fd = connectSocket(toStr(a[0]), SOCK_STREAM, peer, err);
                if (fd < 0) panicHere("tcp_connect: " + err);
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("fd"), Value::integer(fd));
                dictPutInPlace(d, Value::str("peer"), Value::str(peer));
                return d;
            });
        if (name == "tcp_read")
            return biFn({"fd", "n"}, [](std::vector<Value>& a) -> Value {
                int fd = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                size_t n = a.size() > 1 && a[1].k == VK::Int && a[1].i > 0
                               ? (size_t)a[1].i
                               : 4096;
                std::string buf(n, '\0');
                ssize_t k;
                do {
                    k = ::recv(fd, buf.data(), n, 0);
                } while (k < 0 && errno == EINTR);
                if (k < 0)
                    panicHere(std::string("tcp_read: ") + std::strerror(errno));
                buf.resize((size_t)k);
                Value v = Value::str(std::move(buf));
                v.k = VK::Bytes;
                return v;
            });
        if (name == "tcp_write")
            return biFn({"fd", "data"}, [](std::vector<Value>& a) -> Value {
                int fd = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                std::string data =
                    a.size() > 1 && isBuffer(a[1])
                        ? bytesData(a[1])
                        : toStr(a.size() > 1 ? a[1] : Value());
                size_t sent = 0;
                while (sent < data.size()) {
                    ssize_t k = ::send(fd, data.data() + sent,
                                       data.size() - sent, 0);
                    if (k < 0) {
                        if (errno == EINTR) continue;
                        panicHere(std::string("tcp_write: ") +
                                  std::strerror(errno));
                    }
                    if (k == 0) break;
                    sent += (size_t)k;
                }
                return Value::integer((int64_t)sent);
            });
        if (name == "close")
            return biFn({"fd"}, [](std::vector<Value>& a) -> Value {
                if (!a.empty() && a[0].k == VK::Int) ::close((int)a[0].i);
                return Value::none();
            });
        if (name == "udp_bind")
            return biFn({"addr"}, [](std::vector<Value>& a) -> Value {
                std::string bound, err;
                int fd = bindSocket(toStr(a[0]), SOCK_DGRAM, bound, err);
                if (fd < 0) panicHere("udp_bind: " + err);
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("fd"), Value::integer(fd));
                dictPutInPlace(d, Value::str("addr"), Value::str(bound));
                return d;
            });
        if (name == "udp_send_to")
            return biFn({"fd", "data", "addr"}, [](std::vector<Value>& a) -> Value {
                int fd = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                std::string data =
                    a.size() > 1 && isBuffer(a[1])
                        ? bytesData(a[1])
                        : toStr(a.size() > 1 ? a[1] : Value());
                std::string addr = a.size() > 2 ? toStr(a[2]) : "";
                addrinfo* res = nullptr;
                std::string err;
                if (!resolveAddr(addr, SOCK_DGRAM, false, res, err))
                    panicHere("udp_send_to: " + err);
                ssize_t k = -1;
                for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                    k = ::sendto(fd, data.data(), data.size(), 0,
                                 ai->ai_addr, ai->ai_addrlen);
                    if (k >= 0) break;
                }
                freeaddrinfo(res);
                if (k < 0)
                    panicHere(std::string("udp_send_to: ") +
                              std::strerror(errno));
                return Value::integer((int64_t)k);
            });
        if (name == "udp_recv_from")
            return biFn({"fd", "n"}, [](std::vector<Value>& a) -> Value {
                int fd = (!a.empty() && a[0].k == VK::Int) ? (int)a[0].i : -1;
                size_t n = a.size() > 1 && a[1].k == VK::Int && a[1].i > 0
                               ? (size_t)a[1].i
                               : 4096;
                std::string buf(n, '\0');
                sockaddr_storage ss{};
                socklen_t sl = sizeof ss;
                ssize_t k;
                do {
                    k = ::recvfrom(fd, buf.data(), n, 0, (sockaddr*)&ss, &sl);
                } while (k < 0 && errno == EINTR);
                if (k < 0)
                    panicHere(std::string("udp_recv_from: ") +
                              std::strerror(errno));
                buf.resize((size_t)k);
                Value v = Value::str(std::move(buf));
                v.k = VK::Bytes;
                Value d = Value::dict();
                dictPutInPlace(d, Value::str("data"), v);
                dictPutInPlace(d, Value::str("addr"),
                               Value::str(sockAddrStr((sockaddr*)&ss, sl)));
                return d;
            });
        if (name == "lookup_host")
            return biFn({"host"}, [](std::vector<Value>& a) -> Value {
                std::string host = a.empty() ? "" : toStr(a[0]);
                addrinfo hints{};
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_family = AF_UNSPEC;
                addrinfo* res = nullptr;
                int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
                if (rc != 0) panicHere(std::string("lookup_host: ") +
                                       gai_strerror(rc));
                Value out = Value::list();
                for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                    std::string s = sockAddrStr(ai->ai_addr, ai->ai_addrlen);
                    auto c = s.rfind(':');
                    if (c != std::string::npos) s = s.substr(0, c);
                    out.vec->push_back(Value::str(s));
                }
                freeaddrinfo(res);
                return out;
            });
#endif
        panicHere("net has no member '" + name + "'");
    }

    panicHere("unknown module '" + mod + "'");
}

// ---------------------------------------------------------------------------
// statements
// ---------------------------------------------------------------------------

static bool isResultRet(const Stmt& fn) {
    // `Result[T]` is the prelude spelling of `result[T]` (POWER_PLAN §6); both
    // must drive the raise-catch and ok-wrap paths identically.
    return fn.ret && fn.ret->kind == ast::TyKind::Name &&
           (fn.ret->name == "result" || fn.ret->name == "Result");
}

void Interpreter::registerUsing(const Value& v) {
    std::thread::id tid = std::this_thread::get_id();
    std::lock_guard<std::mutex> lk(usingsM_);
    auto it = usings_.find(tid);
    if (it == usings_.end() || it->second.empty()) return;
    it->second.back().push_back(v);
}

void Interpreter::runUsingFrame(std::thread::id tid) {
    std::vector<Value> mine;
    {
        std::lock_guard<std::mutex> lk(usingsM_);
        auto it = usings_.find(tid);
        if (it == usings_.end() || it->second.empty()) return;
        mine = std::move(it->second.back());
        it->second.pop_back();
    }
    // LIFO: the last binding declared is the first released.
    while (!mine.empty()) {
        Value v = std::move(mine.back());
        mine.pop_back();
        try {
            if (hasStructMethod(v, "drop"))
                invokeMethod(v, "drop", {}, {}, globals_, 0, 0, nullptr);
            else if (hasStructMethod(v, "__del__"))
                invokeMethod(v, "__del__", {}, {}, globals_, 0, 0, nullptr);
            else if (hasStructMethod(v, "exit"))
                invokeMethod(v, "exit", {}, {}, globals_, 0, 0, nullptr);
            else if (hasStructMethod(v, "__exit__"))
                invokeMethod(v, "__exit__", {}, {}, globals_, 0, 0, nullptr);
        } catch (...) {
        }
    }
}

void Interpreter::execBlock(const std::vector<ast::StmtP>& body, Env env) {
    // `using` bindings are released when the block exits, on normal completion
    // or on unwind. Only blocks that actually declare one pay for a frame.
    bool hasUsing = false;
    for (const auto& st : body)
        if (st && st->kind == ast::StKind::Using) { hasUsing = true; break; }
    const std::thread::id tid = std::this_thread::get_id();
    if (hasUsing) {
        std::lock_guard<std::mutex> lk(usingsM_);
        usings_[tid].emplace_back();
    }
    struct UsingGuard {
        Interpreter* self;
        std::thread::id tid;
        bool active;
        ~UsingGuard() { if (active) self->runUsingFrame(tid); }
    } usingGuard{this, tid, hasUsing};

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
            if (s.declType && s.declType->kind == ast::TyKind::Name)
                v.declTyName = s.declType->name;
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
            if (s.declType && s.declType->kind == ast::TyKind::Name)
                v.declTyName = s.declType->name;
            if (!s.target || s.target->kind != ast::ExKind::Ident) return;
            env->vars[s.target->text] = std::move(v);
            return;
        }

        case ast::StKind::GlobalDecl: {
            // write/read the module top-level binding.
            Value v = s.value ? eval(*s.value, env) : Value::none();
            if (s.declType && s.declType->kind == ast::TyKind::Name)
                v.declTyName = s.declType->name;
            if (!s.target || s.target->kind != ast::ExKind::Ident) return;
            EnvS* root = env->root();
            if (s.value) root->vars[s.target->text] = std::move(v);
            return;
        }

        case ast::StKind::TempDecl: {
            // like a normal var decl, but carries a compile-time use budget
            // (enforced in the checker); runtime treats it as an ordinary binding.
            Value v = s.value ? eval(*s.value, env) : Value::none();
            if (s.declType && s.declType->kind == ast::TyKind::Name)
                v.declTyName = s.declType->name;
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
            structEnv_[s.name] = env;
            env->vars[s.name] = Value::module("__struct__:" + s.name);
            return;
        }

        case ast::StKind::EnumDef: {
            enums_[s.name] = &s;
            registerEnumDerive(s);
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
            if (s.augOp == "?" "?=") {
                // `a ?? = b`: assign only when `a` is none (short-circuit).
                Value cur = eval(*s.exprs[0], env);
                if (cur.isNone())
                    assignTo(*s.exprs[0], eval(*s.exprs[1], env), env);
                return;
            }
            if (s.augOp == "@=") {
                // PEP 465: `a @= b` calls `a.__imatmul__(b)` when defined, and
                // otherwise falls back to `a = a @ b` (matmul/`__matmul__`).
                Value cur = eval(*s.exprs[0], env);
                Value rhs = eval(*s.exprs[1], env);
                Value res = hasStructMethod(cur, "__imatmul__")
                                ? invokeMethod(cur, "__imatmul__", {rhs}, {},
                                               globals_, s.span.line,
                                               s.span.col, nullptr)
                                : binop("@", cur, rhs, s.span.line, s.span.col);
                assignTo(*s.exprs[0], res, env);
                return;
            }
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

        // `with e1 as n1, e2 as n2 { body }`: enter each manager left-to-right
        // (binding its name to the enter result), run the body, then exit each in
        // reverse order -- on normal completion and on a raised error. An exit
        // that returns truthy suppresses the error (PEP 343).
        case ast::StKind::With: {
            Env child = makeChild(env);
            std::vector<std::pair<Value, std::string>> entered;
            auto enterOne = [&](Value cm, const std::string& name) {
                Value enteredVal = cm;
                if (hasStructMethod(cm, "enter"))
                    enteredVal = invokeMethod(cm, "enter", {}, {}, env, 0, 0, nullptr);
                else if (hasStructMethod(cm, "__enter__"))
                    enteredVal =
                        invokeMethod(cm, "__enter__", {}, {}, env, 0, 0, nullptr);
                if (!name.empty()) child->vars[name] = enteredVal;
                std::string exitName;
                if (hasStructMethod(cm, "exit")) exitName = "exit";
                else if (hasStructMethod(cm, "__exit__")) exitName = "__exit__";
                entered.emplace_back(cm, exitName);
            };
            auto runExits = [&](bool* suppress) {
                for (auto it = entered.rbegin(); it != entered.rend(); ++it) {
                    if (it->second.empty()) continue;
                    try {
                        Value r = invokeMethod(it->first, it->second, {}, {}, env,
                                               0, 0, nullptr);
                        if (suppress && truthy(r)) *suppress = true;
                    } catch (...) {
                    }
                }
            };
            try {
                for (size_t i = 0; i < s.exprs.size(); ++i) {
                    Value cm = eval(*s.exprs[i], env);
                    enterOne(cm, i < s.withNames.size() ? s.withNames[i]
                                                        : std::string());
                }
            } catch (...) {
                runExits(nullptr);
                throw;
            }
            try {
                execBlock(s.body, child);
            } catch (const SignalRaise&) {
                bool sup = false;
                runExits(&sup);
                if (!sup) throw;
                return;
            } catch (...) {
                runExits(nullptr);
                throw;
            }
            runExits(nullptr);
            return;
        }

        // `using name = expr;`: bind now, release via drop() when the enclosing
        // block exits (see execBlock's using frame).
        case ast::StKind::Using: {
            if (s.value) {
                Value v = eval(*s.value, env);
                if (s.target && s.target->kind == ast::ExKind::Ident)
                    env->vars[s.target->text] = v;
                registerUsing(v);
            }
            return;
        }

        case ast::StKind::ComptimeBlock:
            // The checker already evaluated the block; at runtime it is an
            // ordinary scoped block (any `const` bindings inside stay local).
            execBlock(s.body, makeChild(env));
            return;

            case ast::StKind::If:
                // block bodies get a fresh child frame so `var` shadowing matches
                // the checker's lexical scopes (previously the same env was used).
                //
                // The else branch used to iterate its statements and give each
                // one its own makeChild(env), which is not the same thing: a
                // declaration in one statement became invisible to the next, so
                //
                //     if false { print(0) } else { var y = 2; print(y) }
                //
                // panicked with "undefined variable 'y'". When the outer scope
                // happened to have a binding of the same name it was worse --
                // silently reading the wrong one. One child env for the whole
                // block, matching the then branch and the elif branch below, is
                // what the language means. (The bytecode fast path was correct
                // here by accident, which is why gate 4's tree-vs-VM comparison
                // is the thing that found it.)
                if (truthy(eval(*s.exprs[0], env)))
                    execBlock(s.body, makeChild(env));
                else if (!s.elseBody.empty())
                    execBlock(s.elseBody, makeChild(env));
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
            // Check for @[par] decorator on the for statement (POWER_PLAN §4.4)
            bool isParallel = false;
            for (const auto& d : s.decorators) {
                if (d.name == "par") { isParallel = true; break; }
            }
            if (isParallel) {
                // POWER_PLAN §4.4: `@[par] for x in xs { … }` runs the body
                // concurrently across the worker pool. Lower to
                // `pet.parallel.par_iter(xs).for_each(fn (it) { … })`; the
                // synthetic native closure re-binds the original loop pattern
                // against each element, exactly like a serial `for`.
                Value seq = eval(*s.exprs[0], env);
                const ast::Stmt* loop = &s;
                Env base = env;
                Value bodyFn = biFn({"it"}, [this, loop, base](std::vector<Value>& a) -> Value {
                    Value item = a.empty() ? Value::none() : a[0];
                    Env child = makeChild(base);
                    matchPat(*loop->pat, item, child);
                    execBlock(loop->body, child);
                    return Value::none();
                });
                Value mod = loadModuleFile("pet.parallel");
                if (mod.k != VK::Module)
                    panicHere("`@[par]` requires the pet.parallel module");
                Value parIterFn = moduleMember(mod.typeName, "par_iter");
                Value parIterRes =
                    callValue(parIterFn, {seq}, s.span.line, s.span.col);
                // A struct method resolves through invokeMethod (memberRead on
                // a struct only sees fields), so dispatch `for_each` with the
                // synthetic body closure as its argument.
                invokeMethod(parIterRes, "for_each", {bodyFn}, {}, env,
                             s.span.line, s.span.col, nullptr);
                return;
            }
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

// Type-name classification for `is`/`as`/casts and integral method width.
// Accepts the full spelling surface: classic lowercase micros (i4..i256,
// u4..u256, f4..f256, uf4..uf256, isize/usize, b8.., char8..) plus the
// capitalized primitives and caps width families (Int/UInt/Float/UFloat,
// Int4..UInt256, Float4..UFloat256, ISize/USize, Bool8.., Char8..).
static bool allDigits(const std::string& s, size_t from) {
    return s.size() > from &&
           s.find_first_not_of("0123456789", from) == std::string::npos;
}

// Width/signedness for an integer type NAME; returns false for unknown.
static bool intNameBits(const std::string& n, int& bits, bool& isUnsigned) {
    if (n.empty() || n == "Int" || n == "int" || n == "ISize" ||
        n == "isize") { bits = 64; isUnsigned = false; return true; }
    if (n == "UInt" || n == "uint" || n == "USize" || n == "usize") {
        bits = 64; isUnsigned = true; return true; }
    if (n.rfind("UInt", 0) == 0 && allDigits(n, 4)) {
        bits = atoi(n.c_str() + 4); isUnsigned = true; return true; }
    if (n.rfind("Int", 0) == 0 && allDigits(n, 3)) {
        bits = atoi(n.c_str() + 3); isUnsigned = false; return true; }
    if (n.size() >= 2 && n[0] == 'u' && isdigit((unsigned char)n[1]) &&
        allDigits(n, 1)) { bits = atoi(n.c_str() + 1); isUnsigned = true; return true; }
    if (n.size() >= 2 && n[0] == 'i' && isdigit((unsigned char)n[1]) &&
        allDigits(n, 1)) { bits = atoi(n.c_str() + 1); isUnsigned = false; return true; }
    return false;
}

static bool isFloatTypeName(const std::string& t) {
    if (t == "Float" || t == "float" || t == "UFloat") return true;
    if (t.rfind("UFloat", 0) == 0 && allDigits(t, 6)) return true;
    if (t.rfind("Float", 0) == 0 && allDigits(t, 5)) return true;
    if (t.rfind("uf", 0) == 0 && t.size() >= 3 &&
        isdigit((unsigned char)t[2]) && allDigits(t, 2)) return true;
    if (t.size() >= 2 && t[0] == 'f' && isdigit((unsigned char)t[1]) &&
        allDigits(t, 1)) return true;
    return false;
}

static bool isBoolTypeName(const std::string& t) {
    if (t == "Bool" || t == "bool") return true;
    if (t.rfind("Bool", 0) == 0 && allDigits(t, 4)) return true;
    if (t.size() >= 2 && t[0] == 'b' && isdigit((unsigned char)t[1]) &&
        allDigits(t, 1)) return true;
    return false;
}

static bool isCharTypeName(const std::string& t) {
    if (t == "Char" || t == "char") return true;
    if (t.rfind("Char", 0) == 0 && allDigits(t, 4)) return true;
    if (t.rfind("char", 0) == 0 && t.size() > 4 &&
        t.find_first_not_of("0123456789", 4) == std::string::npos) return true;
    return false;
}

static bool typeTestMatches(const Value& v, const std::string& t) {
    if (t == "none") return v.k == VK::None;
    if (t == "Str" || t == "str" || t == "String" || t == "string" ||
        t == "strings")
        return v.k == VK::Str;
    if (t == "bytes") return v.k == VK::Bytes;
    if (t == "bytearray") return v.k == VK::ByteArray;
    if (t == "memoryview") return v.k == VK::View;
    if (t == "Complex") return v.k == VK::Complex;
    if (t == "Decimal") return v.k == VK::Decimal;
    if (t == "BigInt" || t == "bigint" || t == "big_int")
        return v.k == VK::BigInt;
    if (t == "Rational") return v.k == VK::Rational;
    if (isCharTypeName(t)) return v.k == VK::Char;
    if (isBoolTypeName(t)) return v.k == VK::Bool;
    int bits; bool isUnsigned;
    if (intNameBits(t, bits, isUnsigned)) return v.k == VK::Int;
    if (isFloatTypeName(t)) return v.k == VK::Float;
    if (t == "list") return v.k == VK::List;
    if (t == "dict") return v.k == VK::Dict;
    if (t == "set")  return v.k == VK::Set;
    if (t == "tuple") return v.k == VK::Tuple;
    if (t == "result" || t == "Result") return v.k == VK::Result;
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
            if (e.op == "await") {
                // POWER_PLAN §2: awaiting a Task (ThreadH) joins it and yields
                // its return value; awaiting a Timer sleeps until its deadline;
                // awaiting any other value is the identity (a completed future).
                Value fut = eval(*e.rhs, env);
                if (fut.k == VK::ThreadH) {
                    Value r = awaitTask(fut);
                    if (r.k == VK::None) return Value::none();
                    return r;
                }
                if (fut.k == VK::Timer) {
                    int64_t now = nowMs();
                    if (fut.deadlineMs > now)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(fut.deadlineMs - now));
                    return Value::none();
                }
                return fut;
            }
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
                    case VK::Deque:
                        for (const auto& el : *r.vec)
                            if (valuesEqual(l, el)) return Value::boolean(true);
                        return Value::boolean(false);
                    case VK::Dict:
                        for (const auto& kv : *r.map)
                            if (valuesEqual(l, kv.first)) return Value::boolean(true);
                        return Value::boolean(false);
                    case VK::Counter:
                        for (const auto& kv : *r.map)
                            if (kv.second.i != 0 && valuesEqual(l, kv.first))
                                return Value::boolean(true);
                        return Value::boolean(false);
                    case VK::Str:
                        if (l.k != VK::Str)
                            panicHere("'in' on strings needs a string needle");
                        return Value::boolean(r.s.find(l.s) != std::string::npos);
                    case VK::Bytes:
                    case VK::ByteArray:
                    case VK::View: {
                        std::string hay = bytesData(r);
                        if (isBuffer(l))
                            return Value::boolean(hay.find(bytesData(l)) !=
                                                  std::string::npos);
                        if (l.k == VK::Int)
                            return Value::boolean(
                                hay.find((char)(l.i & 0xff)) !=
                                std::string::npos);
                        panicHere("'in' on bytes needs an int or bytes needle");
                    }
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

            // sync.Atomic[T](v) / Atomic[T](v): generic element type
            if (e.lhs->kind == ast::ExKind::Index && e.lhs->lhs) {
                const ast::Expr* b = e.lhs->lhs.get();
                bool isAtomic =
                    (b->kind == ast::ExKind::Ident && b->text == "Atomic") ||
                    (b->kind == ast::ExKind::Member && b->text == "Atomic");
                if (isAtomic) {
                    int elem = 0;
                    const ast::Expr* tyArg = e.lhs->rhs.get();
                    if (tyArg && tyArg->kind == ast::ExKind::Ident) {
                        if (tyArg->text == "Bool" || tyArg->text == "bool")
                            elem = 1;
                        else if (tyArg->text == "Float" || tyArg->text == "float" ||
                                 tyArg->text == "f64" || tyArg->text == "f32")
                            elem = 2;
                    }
                    Value v = makeSync(VK::Atomic);
                    v.sync->elem = elem;
                    if (!e.args.empty() && e.args[0].value) {
                        Value init = eval(*e.args[0].value, env);
                        if (elem == 1) v.sync->ab.store(init.b);
                        else if (elem == 2)
                            v.sync->ad.store(init.k == VK::Int ? (double)init.i
                                                               : init.d);
                        else
                            v.sync->ai.store(init.k == VK::Float
                                                 ? (int64_t)init.d
                                                 : init.i);
                    }
                    return v;
                }
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
                    // POWER_PLAN §2: calling an `async fn` schedules a task.
                    if (fi->second->isAsync && !fi->second->externDef) {
                        if (!named.empty()) {
                            std::vector<std::string> slots;
                            for (const auto& p : fi->second->params)
                                slots.push_back(p.name);
                            mapNamedIntoPos(slots, pos, named);
                        }
                        Value fv;
                        fv.k = VK::Fn;
                        fv.fn = fi->second;
                        fv.env = globals_;
                        return spawnTask(std::move(fv), std::move(pos));
                    }
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
                if (cv.fn->isAsync && !cv.fn->externDef) {
                    if (!named.empty()) {
                        std::vector<std::string> slots;
                        for (const auto& p : cv.fn->params) slots.push_back(p.name);
                        mapNamedIntoPos(slots, pos, named);
                    }
                    return spawnTask(cv, std::move(pos));
                }
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
                case VK::Deque:
                    return (*obj.vec)[normIndex((int64_t)obj.vec->size(), idx.i)];
                case VK::Counter:
                    for (const auto& kv : *obj.map)
                        if (valuesEqual(kv.first, idx)) return kv.second;
                    return Value::integer(0);
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
                case VK::ByteArray:
                case VK::View: {
                    std::string d = bytesData(obj);
                    size_t n = normIndex((int64_t)d.size(), idx.i);
                    return Value::integer((unsigned char)d[n]);
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

        case ast::ExKind::SafeIndex: {
            Value obj = eval(*e.lhs, env);
            if (obj.k == VK::Weak) obj = lockHeap(obj);
            if (obj.isNone()) return Value::none();
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
                case VK::Deque:
                    return (*obj.vec)[normIndex((int64_t)obj.vec->size(), idx.i)];
                case VK::Counter:
                    for (const auto& kv : *obj.map)
                        if (valuesEqual(kv.first, idx)) return kv.second;
                    return Value::integer(0);
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
                case VK::ByteArray:
                case VK::View: {
                    std::string d = bytesData(obj);
                    size_t n = normIndex((int64_t)d.size(), idx.i);
                    return Value::integer((unsigned char)d[n]);
                }
                default: {
                    if (obj.k == VK::Struct || obj.k == VK::Heap)
                        return invokeMethod(obj, "index", {idx}, {}, env,
                                            e.span.line, e.span.col, nullptr);
                    panicHere("value is not indexable");
                }
            }
        }

        case ast::ExKind::SafeCall: {
            Value cv = eval(*e.lhs, env);
            if (cv.isNone()) return Value::none();
            std::vector<Value> pos;
            NamedArgs named;
            for (const auto& a : e.args) {
                if (a.name.empty()) pos.push_back(eval(*a.value, env));
                else named.emplace_back(a.name, eval(*a.value, env));
            }
            if (!named.empty() && cv.k == VK::Builtin && !cv.biParams.empty()) {
                mapNamedIntoPos(cv.biParams, pos, named);
                named.clear();
            }
            if (cv.k == VK::Fn && cv.fn) {
                CallFrameGuard guard(cv.fn->name, e.span.line, e.span.col);
                return runFunc(cv.fn, std::move(pos), std::move(named),
                               cv.env ? cv.env : globals_);
            }
            return callValue(std::move(cv), std::move(pos), e.span.line,
                             e.span.col);
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

        case ast::ExKind::Comptime:
            // Validated as pure by the checker; evaluating it yields the same
            // compile-time constant.
            return e.lhs ? eval(*e.lhs, env) : Value::none();

        case ast::ExKind::ListComp:
        case ast::ExKind::SetComp:
        case ast::ExKind::Generator: {
            std::vector<Value> out;
            std::function<void(size_t, Env)> walk =
                [&](size_t ci, Env cenv) {
                    if (ci >= e.clauses.size()) {
                        const Expr& el = *e.elems[0];
                        if (el.kind == ast::ExKind::Spread) {
                            Value seq = eval(*el.lhs, cenv);
                            iterateSeq(
                                seq,
                                [&](Value& item, Env&) {
                                    out.push_back(item);
                                    return true;
                                },
                                cenv);
                        } else {
                            out.push_back(eval(el, cenv));
                        }
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
            if (e.kind == ast::ExKind::SetComp) return makeSet(std::move(out));
            return Value::list(std::move(out));
        }

        case ast::ExKind::DictComp: {
            Value d = Value::dict();
            std::function<void(size_t, Env)> walk =
                [&](size_t ci, Env cenv) {
                    if (ci >= e.clauses.size()) {
                        const Expr& el = *e.elems[0];
                        if (el.kind == ast::ExKind::Spread) {
                            Value src = eval(*el.lhs, cenv);
                            if (src.k == VK::Dict && src.map)
                                for (const auto& kv : *src.map)
                                    dictPutInPlace(d, kv.first, kv.second);
                        } else {
                            Value key = eval(el, cenv);
                            dictPutInPlace(d, std::move(key),
                                           e.value ? eval(*e.value, cenv)
                                                   : Value::none());
                        }
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
            return d;
        }

        case ast::ExKind::Spread:
            // A Spread is folded by its enclosing display; if one is evaluated
            // directly, yield the operand unchanged.
            return e.lhs ? eval(*e.lhs, env) : Value::none();

        case ast::ExKind::List:
        case ast::ExKind::Set:
        case ast::ExKind::Tuple: {
            std::vector<Value> items;
            for (const auto& el : e.elems) {
                if (el->kind == ast::ExKind::Spread) {
                    Value seq = eval(*el->lhs, env);
                    iterateSeq(seq, [&](Value& item, Env&) {
                        items.push_back(item);
                        return true;
                    }, env);
                } else {
                    items.push_back(eval(*el, env));
                }
            }
            if (e.kind == ast::ExKind::Set) return makeSet(std::move(items));
            if (e.kind == ast::ExKind::Tuple) return Value::tuple(std::move(items));
            return Value::list(std::move(items));
        }

        case ast::ExKind::Dict: {
            Value d = Value::dict();
            for (const auto& pr : e.pairs) {
                if (pr.first && pr.first->kind == ast::ExKind::Spread) {
                    Value src = eval(*pr.first->lhs, env);
                    if (src.k == VK::Dict && src.map)
                        for (const auto& kv : *src.map)
                            dictPutInPlace(d, kv.first, kv.second);
                    continue;
                }
                Value key = eval(*pr.first, env);
                dictPutInPlace(d, std::move(key), eval(*pr.second, env));
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
            if (t == "Str" || t == "str" || t == "String" || t == "string")
                return Value::str(toStr(v));
            int wb; bool wu;
            if (intNameBits(t, wb, wu)) {
                if (wb >= 64) {
                    if (v.k == VK::Float) return Value::integer((int64_t)v.d);
                    if (v.k == VK::Bool) return Value::integer(v.b ? 1 : 0);
                    if (v.k == VK::Char) return Value::integer((int64_t)v.ch);
                    if (v.k == VK::Decimal) return Value::integer(decToI64Trunc(v));
                    return v;
                }
                if (v.k == VK::Int) {
                    uint64_t mask = (1ull << wb) - 1;
                    return Value::integer((int64_t)((uint64_t)v.i & mask));
                }
            }
            if (isFloatTypeName(t))
                return Value::floating(v.k == VK::Int ? (double)v.i
                                                      : v.k == VK::Float ? v.d
                                                                         : 0.0);
            if (isBoolTypeName(t)) return Value::boolean(truthy(v));
            if (t == "Complex") {
                if (v.k == VK::Complex) return v;
                if (v.k == VK::Int || v.k == VK::Float || v.k == VK::Rational)
                    return fromCplx(toCplx(v));
                panicHere("invalid cast to 'Complex'");
            }
            if (t == "Decimal") {
                if (v.k == VK::Decimal) return v;
                if (v.k == VK::Int) return decFromSigned(std::to_string(v.i), 0);
                if (v.k == VK::Float) return decFromDouble(v.d);
                if (v.k == VK::Str) return decParse(v.s);
                if (v.k == VK::Rational)
                    return decDiv(decFromSigned(std::to_string(v.num), 0),
                                  decFromSigned(std::to_string(v.den), 0));
                panicHere("invalid cast to 'Decimal'");
            }
            if (t == "bytes") {
                if (v.k == VK::Bytes) return v;
                if (isBuffer(v)) {
                    Value b = Value::str(bytesData(v)); b.k = VK::Bytes; return b;
                }
                if (v.k == VK::Str) {
                    Value b = Value::str(v.s); b.k = VK::Bytes; return b;
                }
                if (v.k == VK::List) {
                    std::string s;
                    for (auto& e2 : *v.vec) s.push_back((char)(e2.i & 0xff));
                    Value b = Value::str(std::move(s)); b.k = VK::Bytes; return b;
                }
                panicHere("invalid cast to 'bytes'");
            }
            if (t == "bytearray") {
                if (v.k == VK::ByteArray) return v;
                if (isBuffer(v)) return Value::byteArray(bytesData(v));
                if (v.k == VK::Str) return Value::byteArray(v.s);
                if (v.k == VK::List) {
                    std::string s;
                    for (auto& e2 : *v.vec) s.push_back((char)(e2.i & 0xff));
                    return Value::byteArray(std::move(s));
                }
                panicHere("invalid cast to 'bytearray'");
            }
            if (t == "memoryview") {
                if (v.k == VK::View) return v;
                if (v.k == VK::ByteArray && v.bstr)
                    return Value::view(v.bstr, 0, v.bstr->size(), false);
                if (isBuffer(v)) {
                    auto buf = std::make_shared<std::string>(bytesData(v));
                    return Value::view(std::move(buf), 0, buf->size(), true);
                }
                panicHere("invalid cast to 'memoryview'");
            }
            if (v.k == VK::None && t == "none") return v;
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

        case ast::ExKind::AssignExpr: {
            // Walrus `(name := value)`: evaluate, store, and yield the value.
            Value v = e.rhs ? eval(*e.rhs, env) : Value::none();
            if (e.lhs) assignTo(*e.lhs, v, env);
            return v;
        }

        case ast::ExKind::Topic: {
            if (const Value* v = env->find(kPipeTopic)) return *v;
            panicHere("E-PIPE-TOPIC: '%' used outside a pipe");
        }

        case ast::ExKind::Pipe: {
            // Evaluate the input once and bind it as the topic for the body.
            // The topic lives in the current frame (not a fresh child), so a
            // pipe does not allocate an environment; the binding is saved and
            // restored to keep nested pipes correct. `%` is not a valid
            // identifier, so it cannot collide with a user binding.
            Value in = e.lhs ? eval(*e.lhs, env) : Value::none();
            auto& vars = env->vars;
            auto it = vars.find(kPipeTopic);
            const bool had = it != vars.end();
            Value saved = had ? it->second : Value();
            vars[kPipeTopic] = std::move(in);
            Value out = e.rhs ? eval(*e.rhs, env) : Value::none();
            if (had) vars[kPipeTopic] = std::move(saved);
            else vars.erase(kPipeTopic);
            return out;
        }

        case ast::ExKind::Coalesce:
            // `a ?? b`: short-circuit; only evaluate b when a is none.
            if (e.lhs) {
                Value l = eval(*e.lhs, env);
                if (!l.isNone()) return l;
            }
            return e.rhs ? eval(*e.rhs, env) : Value::none();
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
    std::string buf;
    bool bufMode = false;
    switch (obj.k) {
        case VK::List:
        case VK::Tuple:
        case VK::Set:
        case VK::Deque:
            len = (int64_t)obj.vec->size();
            break;
        case VK::Str:
            len = (int64_t)obj.s.size();
            break;
        case VK::Bytes:
        case VK::ByteArray:
        case VK::View:
            buf = bytesData(obj);
            len = (int64_t)buf.size();
            bufMode = true;
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
    bool strMode = bufMode || obj.k == VK::Str;
    for (int64_t i = lo; i < hi; i += step) {
        if (bufMode) chars += buf[(size_t)i];
        else if (obj.k == VK::Str) chars += obj.s[(size_t)i];
        else items.push_back((*obj.vec)[(size_t)i]);
    }
    if (strMode) {
        Value r = Value::str(std::move(chars));
        if (obj.k == VK::Bytes) r.k = VK::Bytes;
        else if (obj.k == VK::ByteArray) r.k = VK::ByteArray;
        else if (obj.k == VK::View) {
            r.k = VK::View;
            r.bstr = std::make_shared<std::string>(r.s);
            r.voff = 0;
            r.vlen = r.s.size();
            r.vreadonly = obj.vreadonly;
        }
        return r;
    }
    if (obj.k == VK::Tuple) return Value::tuple(std::move(items));
    if (obj.k == VK::Set)   return makeSet(std::move(items));
    if (obj.k == VK::Deque) return Value::deque(std::move(items));
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
            // POWER_PLAN §2: calling an `async fn` schedules a task and returns
            // its handle; `await` drives it to completion.
            if (callee.fn && callee.fn->isAsync && !callee.fn->externDef)
                return spawnTask(callee, std::move(args));
            if (callee.lam && callee.lam->isAsync)
                return spawnTask(callee, std::move(args));
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
        // `async fn` bodies may contain `await`, which only the tree-walker
        // understands; never lower them to the VM/native fast paths.
        if (fn->isAsync)
            execBlock(fn->body, fenv);
        else if (const NativeBody* nb = nativeFuncFor(fn))
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
        if (isResultRet(*fn)) {
            // POWER_PLAN §5.4: a single-arg `result[T]` defaults E to
            // error.Error, so a non-Error payload reached via `raise`/`?` is
            // wrapped into an Error (caller can .context()/.chain() it, and the
            // raw value stays reachable as origin for downcast). Explicit-E
            // `result[T, E]` functions propagate E2 unchanged.
            if (fn->ret->generics.size() == 1 && sraise.errResult.k == VK::Result &&
                !sraise.errResult.payload.empty() &&
                !isPetError(sraise.errResult.payload[0])) {
                Value& p = sraise.errResult.payload[0];
                p = mkError(toStr(p), Value::none(), p);
            }
            return sraise.errResult;
        }
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
    if (isResultRet(*fn))
        // A result fn yields either a plain value (wrap it in ok) or an
        // already-constructed result -- `return ok(v)` / `return err(e)` /
        // `return inner()` -- which must pass through untouched, else the wrap
        // would nest as ok(ok(v)) / ok(err(e)).
        return ret.k == VK::Result ? std::move(ret)
                                   : Value::resultOk(std::move(ret));
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
        // POWER_PLAN §5: pet.error's Error methods are native (no AST decl).
        if (isErrorStruct(tn)) {
            if (name == "chain") {
                std::vector<Value> out;
                const Value* cur = &obj;
                while (cur && isErrorStruct(cur->typeName)) {
                    out.push_back(Value::str(errorFieldStr(*cur, "msg")));
                    const Value* src = structField(*cur, "source");
                    cur = (src && isErrorStruct(src->typeName)) ? src : nullptr;
                }
                return Value::list(std::move(out));
            }
            if (name == "root_cause") {
                const Value* cur = &obj;
                while (cur && isErrorStruct(cur->typeName)) {
                    const Value* src = structField(*cur, "source");
                    if (src && isErrorStruct(src->typeName)) { cur = src; continue; }
                    break;
                }
                return (cur && isErrorStruct(cur->typeName)) ? *cur : obj;
            }
            if (name == "is" || name == "downcast") {
                bool down = (name == "downcast");
                std::string want = pos.empty() ? "" : toStr(pos[0]);
                const Value* origin = structField(obj, "origin");
                if (!origin) return down ? Value::none() : Value::boolean(false);
                std::string tn2 = typeName(*origin);
                bool match = tn2 == want || tn2.rfind(want + ".", 0) == 0 ||
                             want.rfind(tn2, 0) == 0;
                if (down) return match ? *origin : Value::none();
                return Value::boolean(match);
            }
            if (name == "backtrace") return Value::none();  // not captured yet
            panicHere("type '" + tn + "' has no method '" + name + "'");
        }
        const Stmt* method = nullptr;
        Env menv = globals_;
        auto si = structs_.find(tn);
        if (si != structs_.end()) {
            for (const auto& m : si->second->body)
                if (m->kind == ast::StKind::FuncDef && m->name == name) {
                    method = m.get();
                    break;
                }
            if (method) {
                auto se = structEnv_.find(tn);
                if (se != structEnv_.end() && se->second) menv = se->second;
            }
        }
        if (!method) {
            for (auto it = impls_.rbegin(); it != impls_.rend(); ++it) {
                if (it->typeName != tn) continue;
                auto mi = it->methods.find(name);
                if (mi != it->methods.end()) {
                    method = mi->second;
                    if (it->env) menv = it->env;
                    break;
                }
            }
        }
        if (!method)
            panicHere("type '" + tn + "' has no method '" + name + "'");
std::vector<Value> full;
        full.reserve(1 + pos.size());
        full.push_back(obj);
        for (auto& v : pos) full.push_back(std::move(v));
        // POWER_PLAN §2: an async method schedules a task (self is arg 0).
        if (method->isAsync && !method->externDef) {
            Value fv;
            fv.k = VK::Fn;
            fv.fn = method;
            fv.env = menv;
            return spawnTask(std::move(fv), std::move(full));
        }
        CallFrameGuard guard(method->name, line, col);
        return runFunc(method, std::move(full), std::move(named), menv,
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
    auto bigOf = [](const Value& v) -> bignum::Big {
        if (v.k == VK::BigInt) {
            bignum::Big b;
            b.neg = v.neg;
            if (v.limbs) b.limbs = *v.limbs;
            return b;
        }
        if (v.k == VK::Int) return bignum::fromI64(v.i);
        panicHere("expected an int or bigint");
        return {};
    };
    auto bigVal = [](bignum::Big b) -> Value {
        Value v;
        v.k = VK::BigInt;
        v.neg = b.neg;
        v.limbs = std::make_shared<std::vector<uint64_t>>(std::move(b.limbs));
        return v;
    };
    auto hashSelf = [&](void) -> uint64_t { (void)obj; return 0; };
    (void)hashSelf;

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
            // POWER_PLAN §5.1: `.context(c)` / `.with_context(f)` push a cause
            // layer onto an err payload; ok passes through (anyhow Context).
            if (name == "context")
                return bind({"ctx"}, [](std::vector<Value>& a) {
                    return errorWithContext(a[0], a.size() > 1 ? toStr(a[1]) : "");
                });
            if (name == "with_context")
                return bind({"f"}, [this](std::vector<Value>& a) {
                    Value c = Value::none();
                    if (a.size() > 1) c = callValue(a[1], {}, 0, 0);
                    return errorWithContext(a[0], toStr(c));
                });
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
                    // POWER_PLAN §4: a task handle's `join()` now yields the
                    // task's return value (previously `none`), which lets
                    // `pet.parallel` collect parallel results without `await`.
                    return awaitTask(a[0]);
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

        // ---- M9: `sync` primitives -----------------------------------------
        case VK::Mutex: {
            if (name == "lock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].sync->m.lock();
                    a[0].sync->locked = true;
                    a[0].sync->owner = std::this_thread::get_id();
                    return Value::none();
                });
            if (name == "unlock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].sync->locked = false;
                    a[0].sync->owner = std::thread::id{};
                    a[0].sync->m.unlock();
                    return Value::none();
                });
            if (name == "try_lock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (!a[0].sync->m.try_lock()) return Value::boolean(false);
                    a[0].sync->locked = true;
                    a[0].sync->owner = std::this_thread::get_id();
                    return Value::boolean(true);
                });
            if (name == "locked")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::boolean(a[0].sync->locked);
                });
            break;
        }

        case VK::RwMutex: {
            if (name == "lock" || name == "wlock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::unique_lock<std::mutex> lk(s->m);
                    s->cv.wait(lk, [&] { return !s->writer && s->readers == 0; });
                    s->writer = true;
                    return Value::none();
                });
            if (name == "unlock" || name == "wunlock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    { std::lock_guard<std::mutex> lk(s->m); s->writer = false; }
                    s->cv.notify_all();
                    return Value::none();
                });
            if (name == "rlock" || name == "rdlock" || name == "read_lock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::unique_lock<std::mutex> lk(s->m);
                    s->cv.wait(lk, [&] { return !s->writer; });
                    s->readers++;
                    return Value::none();
                });
            if (name == "runlock" || name == "r_unlock" || name == "read_unlock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    { std::lock_guard<std::mutex> lk(s->m); --s->readers; }
                    s->cv.notify_all();
                    return Value::none();
                });
            if (name == "try_lock")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::lock_guard<std::mutex> lk(s->m);
                    if (!s->writer && s->readers == 0) {
                        s->writer = true;
                        return Value::boolean(true);
                    }
                    return Value::boolean(false);
                });
            if (name == "try_rlock" || name == "try_read_lock") {
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::lock_guard<std::mutex> lk(s->m);
                    if (!s->writer) {
                        s->readers++;
                        return Value::boolean(true);
                    }
                    return Value::boolean(false);
                });
            }
            break;
        }

        case VK::WaitGroup: {
            if (name == "add")
                return bind({"n"}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    int64_t n = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 1;
                    {
                        std::lock_guard<std::mutex> lk(s->m);
                        s->count += n;
                    }
                    if (n <= 0) s->cv.notify_all();
                    return Value::none();
                });
            if (name == "done")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    {
                        std::lock_guard<std::mutex> lk(s->m);
                        --s->count;
                    }
                    s->cv.notify_all();
                    return Value::none();
                });
            if (name == "wait")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::unique_lock<std::mutex> lk(s->m);
                    s->cv.wait(lk, [&] { return s->count == 0; });
                    return Value::none();
                });
            if (name == "go")
                return bind({"f"}, [this](std::vector<Value>& a) -> Value {
                    auto sync = a[0].sync;
                    Value f = a.size() > 1 ? a[1] : Value::none();
                    {
                        std::lock_guard<std::mutex> lk(sync->m);
                        sync->count++;
                    }
                    auto t = std::make_shared<ThreadImpl>();
                    {
                        std::lock_guard<std::mutex> lk(threadsM_);
                        threads_.push_back(t);
                    }
                    t->th = std::thread([this, f, sync]() {
                        try {
                            callValue(f, {}, 0, 0);
                        } catch (...) {
                        }
                        {
                            std::lock_guard<std::mutex> lk(sync->m);
                            --sync->count;
                        }
                        sync->cv.notify_all();
                    });
                    return Value::none();
                });
            break;
        }

        case VK::Once: {
            if (name == "do" || name == "call")
                return bind({"f"}, [this](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    std::lock_guard<std::mutex> lk(s->m);
                    if (!s->done) {
                        s->done = true;
                        if (a.size() > 1) callValue(a[1], {}, 0, 0);
                    }
                    return Value::none();
                });
            if (name == "done")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::boolean(a[0].sync->done);
                });
            break;
        }

        case VK::Cond: {
            if (name == "wait")
                return bind({"mu"}, [](std::vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[1].k != VK::Mutex)
                        panicHere("Cond.wait requires a Mutex");
                    std::unique_lock<std::mutex> lk(a[1].sync->m, std::adopt_lock);
                    a[0].sync->cv.wait(lk);
                    lk.release();              // caller keeps holding the Mutex
                    return Value::none();
                });
            if (name == "signal")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].sync->cv.notify_one();
                    return Value::none();
                });
            if (name == "broadcast")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].sync->cv.notify_all();
                    return Value::none();
                });
            break;
        }

        case VK::Atomic: {
            auto loadVal = [](const Value& s) -> Value {
                if (s.sync->elem == 1) return Value::boolean(s.sync->ab.load());
                if (s.sync->elem == 2) return Value::floating(s.sync->ad.load());
                return Value::integer(s.sync->ai.load());
            };
            if (name == "load")
                return bind({}, [loadVal](std::vector<Value>& a) -> Value {
                    return loadVal(a[0]);
                });
            if (name == "store")
                return bind({"v"}, [](std::vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::none();
                    SyncImpl* s = a[0].sync.get();
                    if (a[1].k == VK::Bool) s->ab.store(a[1].b);
                    else if (a[1].k == VK::Float) s->ad.store(a[1].d);
                    else s->ai.store(a[1].i);
                    return Value::none();
                });
            if (name == "swap")
                return bind({"v"}, [loadVal](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    Value old = loadVal(a[0]);
                    if (a.size() > 1) {
                        if (a[1].k == VK::Bool) s->ab.store(a[1].b);
                        else if (a[1].k == VK::Float) s->ad.store(a[1].d);
                        else s->ai.store(a[1].i);
                    }
                    return old;
                });
            if (name == "add" || name == "sub" || name == "fetch_add" ||
                name == "fetch_sub" || name == "inc" || name == "dec") {
                bool sub = (name == "sub" || name == "fetch_sub" ||
                            name == "dec");
                return bind({"d"}, [sub](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    if (s->elem == 2) {
                        double d = a.size() > 1
                                       ? (a[1].k == VK::Int ? (double)a[1].i
                                                            : a[1].d)
                                       : 1.0;
                        if (sub) d = -d;
                        double now = s->ad.load() + d;
                        s->ad.store(now);
                        return Value::floating(now);
                    }
                    int64_t d = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 1;
                    if (sub) d = -d;
                    return Value::integer(s->ai.fetch_add(d) + d);
                });
            }
            if (name == "compare_exchange" || name == "cas")
                return bind({"old", "new"}, [](std::vector<Value>& a) -> Value {
                    SyncImpl* s = a[0].sync.get();
                    if (s->elem == 2) {
                        double old = a.size() > 1 ? a[1].d : 0.0;
                        double nw = a.size() > 2 ? a[2].d : 0.0;
                        if (s->ad.load() == old) { s->ad.store(nw); return Value::boolean(true); }
                        return Value::boolean(false);
                    }
                    if (s->elem == 1) {
                        bool old = a.size() > 1 ? a[1].b : false;
                        bool nw = a.size() > 2 ? a[2].b : false;
                        return Value::boolean(s->ab.compare_exchange_strong(old, nw));
                    }
                    int64_t old = a.size() > 1 && a[1].k == VK::Int ? a[1].i : 0;
                    int64_t nw = a.size() > 2 && a[2].k == VK::Int ? a[2].i : 0;
                    return Value::boolean(s->ai.compare_exchange_strong(old, nw));
                });
            break;
        }

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
                    return makeSet(std::move(out));
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].vec->size());
                });
            break;
        }

        case VK::Deque: {
            auto trim = [](Value& d) {
                if (d.i >= 0 && (int64_t)d.vec->size() > d.i)
                    d.vec->erase(d.vec->begin(),
                                 d.vec->begin() + ((int64_t)d.vec->size() - d.i));
            };
            if (name == "maxlen")
                return obj.i >= 0 ? Value::integer(obj.i) : Value::none();
            if (name == "append" || name == "push_back" || name == "append_right")
                return bind({}, [trim](std::vector<Value>& a) -> Value {
                    a[0].vec->push_back(a.size() > 1 ? a[1] : Value::none());
                    trim(a[0]);
                    return Value::none();
                });
            if (name == "append_left" || name == "appendleft" ||
                name == "push_front")
                return bind({}, [trim](std::vector<Value>& a) -> Value {
                    a[0].vec->insert(a[0].vec->begin(),
                                     a.size() > 1 ? a[1] : Value::none());
                    trim(a[0]);
                    return Value::none();
                });
            if (name == "pop" || name == "pop_back" || name == "pop_right")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].vec->empty()) panicHere("pop from empty deque");
                    Value v = a[0].vec->back();
                    a[0].vec->pop_back();
                    return v;
                });
            if (name == "pop_left" || name == "popleft" || name == "pop_front")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].vec->empty()) panicHere("pop from empty deque");
                    Value v = a[0].vec->front();
                    a[0].vec->erase(a[0].vec->begin());
                    return v;
                });
            if (name == "extend" || name == "extend_right")
                return bind({}, [this, trim](std::vector<Value>& a) -> Value {
                    if (a.size() > 1)
                        iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                            a[0].vec->push_back(it); trim(a[0]); return true;
                        }, globals_);
                    return Value::none();
                });
            if (name == "extend_left" || name == "extendleft")
                return bind({}, [this, trim](std::vector<Value>& a) -> Value {
                    std::vector<Value> xs;
                    if (a.size() > 1)
                        iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                            xs.push_back(it); return true;
                        }, globals_);
                    for (auto& it : xs)
                        a[0].vec->insert(a[0].vec->begin(), it);
                    trim(a[0]);
                    return Value::none();
                });
            if (name == "rotate")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    auto& v = *a[0].vec;
                    int64_t n = a.size() > 1 ? a[1].i : 1;
                    if (!v.empty()) {
                        int64_t k = ((n % (int64_t)v.size()) + (int64_t)v.size()) %
                                    (int64_t)v.size();
                        std::rotate(v.begin(), v.end() - k, v.end());
                    }
                    return Value::none();
                });
            if (name == "clear")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].vec->clear(); return Value::none();
                });
            if (name == "reverse")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::reverse(a[0].vec->begin(), a[0].vec->end());
                    return Value::none();
                });
            if (name == "count")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t n = 0;
                    for (auto& it : *a[0].vec)
                        if (a.size() > 1 && valEq(it, a[1])) ++n;
                    return Value::integer(n);
                });
            if (name == "remove")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a.size() > 1)
                        for (size_t i = 0; i < a[0].vec->size(); ++i)
                            if (valEq((*a[0].vec)[i], a[1])) {
                                a[0].vec->erase(a[0].vec->begin() + i);
                                return Value::none();
                            }
                    panicHere("deque.remove(x): x not found");
                });
            if (name == "index")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a.size() > 1)
                        for (size_t i = 0; i < a[0].vec->size(); ++i)
                            if (valEq((*a[0].vec)[i], a[1]))
                                return Value::integer((int64_t)i);
                    return Value::integer(-1);
                });
            if (name == "copy")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::deque(*a[0].vec, a[0].i);
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)a[0].vec->size());
                });
            break;
        }

        case VK::Counter: {
            auto bump = [](Value& c, const Value& key, int64_t delta) {
                for (auto& kv : *c.map)
                    if (valEq(kv.first, key)) {
                        kv.second = Value::integer(kv.second.i + delta);
                        return;
                    }
                c.map->push_back({key, Value::integer(delta)});
            };
            if (name == "total")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t t = 0;
                    for (auto& kv : *a[0].map) t += kv.second.i;
                    return Value::integer(t);
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t n = 0;
                    for (auto& kv : *a[0].map)
                        if (kv.second.i != 0) ++n;
                    return Value::integer(n);
                });
            if (name == "get")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a.size() > 1)
                        for (auto& kv : *a[0].map)
                            if (valEq(kv.first, a[1])) return kv.second;
                    return Value::integer(0);
                });
            if (name == "elements")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    for (auto& kv : *a[0].map)
                        for (int64_t n = 0; n < kv.second.i; ++n)
                            out.push_back(kv.first);
                    return Value::list(std::move(out));
                });
            if (name == "keys")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> out;
                    for (auto& kv : *a[0].map)
                        if (kv.second.i != 0) out.push_back(kv.first);
                    return Value::list(std::move(out));
                });
            if (name == "most_common")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<std::pair<Value, Value>> entries;
                    for (auto& kv : *a[0].map)
                        if (kv.second.i != 0) entries.push_back(kv);
                    std::stable_sort(entries.begin(), entries.end(),
                                     [](const std::pair<Value, Value>& x,
                                        const std::pair<Value, Value>& y) {
                                         return x.second.i > y.second.i;
                                     });
                    int64_t n = (a.size() > 1 && a[1].k == VK::Int)
                                    ? a[1].i : (int64_t)entries.size();
                    if (n >= 0 && (size_t)n < entries.size())
                        entries.erase(entries.begin() + (size_t)n, entries.end());
                    std::vector<Value> out;
                    for (auto& e : entries)
                        out.push_back(Value::tuple({e.first, e.second}));
                    return Value::list(std::move(out));
                });
            if (name == "update" || name == "subtract")
                return bind({}, [this, bump, name](std::vector<Value>& a) -> Value {
                    int64_t sign = (name == "update") ? 1 : -1;
                    if (a.size() > 1) {
                        if (a[1].k == VK::Counter)
                            for (auto& kv : *a[1].map)
                                bump(a[0], kv.first, sign * kv.second.i);
                        else if (a[1].k == VK::Dict)
                            for (auto& kv : *a[1].map)
                                bump(a[0], kv.first,
                                     sign * (kv.second.k == VK::Int
                                                 ? kv.second.i : 1));
                        else
                            iterateSeq(a[1], [&](Value& it, Env&) -> bool {
                                bump(a[0], it, sign); return true;
                            }, globals_);
                    }
                    return Value::none();
                });
            if (name == "copy")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    Value c = Value::counter();
                    *c.map = *a[0].map;
                    return c;
                });
            if (name == "clear")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    a[0].map->clear(); return Value::none();
                });
            break;
        }

        case VK::Bytes:
        case VK::ByteArray:
        case VK::View: {
            if (name == "hex")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    static const char* hx = "0123456789abcdef";
                    std::string s = bytesData(a[0]), o;
                    for (unsigned char c : s) {
                        o += hx[c >> 4];
                        o += hx[c & 0xf];
                    }
                    return Value::str(o);
                });
            if (name == "decode")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::str(bytesData(a[0]));
                });
            if (name == "len")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::integer((int64_t)bytesData(a[0]).size());
                });
            if (name == "to_string")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    return Value::str(bytesData(a[0]));
                });
            if (name == "to_list" || name == "tolist")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    std::vector<Value> xs;
                    for (unsigned char c : bytesData(a[0]))
                        xs.push_back(Value::integer((int64_t)c));
                    return Value::list(std::move(xs));
                });
            if (name == "to_bytes")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    Value v = Value::str(bytesData(a[0]));
                    v.k = VK::Bytes;
                    return v;
                });
            if (name == "contains")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t needle = a.size() > 1 ? a[1].i : -1;
                    for (unsigned char c : bytesData(a[0]))
                        if ((int64_t)c == needle) return Value::boolean(true);
                    return Value::boolean(false);
                });
            if (name == "count")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    int64_t needle = a.size() > 1 ? a[1].i : -1;
                    int64_t n = 0;
                    for (unsigned char c : bytesData(a[0]))
                        if ((int64_t)c == needle) ++n;
                    return Value::integer(n);
                });
            // mutating methods: only reachable through bytearray receivers
            if (name == "append" || name == "push_back")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].k != VK::ByteArray || !a[0].bstr)
                        panicHere("append(): expected a bytearray");
                    a[0].bstr->push_back((char)(a[1].i & 0xff));
                    return Value::none();
                });
            if (name == "extend" || name == "extend_from")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].k != VK::ByteArray || !a[0].bstr)
                        panicHere("extend(): expected a bytearray");
                    if (a.size() > 1 && isBuffer(a[1]))
                        a[0].bstr->append(bytesData(a[1]));
                    return Value::none();
                });
            if (name == "clear")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].k != VK::ByteArray || !a[0].bstr)
                        panicHere("clear(): expected a bytearray");
                    a[0].bstr->clear();
                    return Value::none();
                });
            if (name == "pop" || name == "pop_back")
                return bind({}, [](std::vector<Value>& a) -> Value {
                    if (a[0].k != VK::ByteArray || !a[0].bstr)
                        panicHere("pop(): expected a bytearray");
                    if (a[0].bstr->empty()) panicHere("pop() on empty bytearray");
                    unsigned char c = (unsigned char)a[0].bstr->back();
                    a[0].bstr->pop_back();
                    return Value::integer((int64_t)c);
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

        case VK::Int: {
            // A Phase-1 integral method takes its semantics from the declared
            // width a: e.g. `var a: i32 = ...` bounds `checked_*` to i32.
            // Bare `int`/`isize` use the full i64 (unsigned usize => u64).
            int wb = 64;  bool wu = false;
            const std::string& wn = obj.declTyName;
            if (!intNameBits(wn, wb, wu)) { wb = 64; wu = false; }
            if (wb < 1 || wb > 64) wb = 64;
            const uint64_t wmask = wb >= 64 ? ~0ull : (1ull << wb) - 1ull;

            // {{{ checked semantics (Option == Value::none() on overflow) }}}
            if (name == "checked_add" || name == "checked_sub" ||
                name == "checked_mul" || name == "checked_div" ||
                name == "checked_rem") {
                return bind({"rhs"}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    int64_t rl = a[0].i, rr = a[1].i;
                    if (!wu) {
                        i128 smin = wb >= 64 ? (i128)LLONG_MIN
                                                 : (i128)(-(1ll << (wb - 1)));
                        i128 smax = wb >= 64 ? (i128)LLONG_MAX
                                                 : (i128)((1ll << (wb - 1)) - 1);
                        i128 wide = 0;
                        bool division = name == "checked_div" || name == "checked_rem";
                        if (division) {
                            if (rr == 0) return Value::none();
                            if (rl == (wb >= 64 ? LLONG_MIN : -(int64_t)(1ll << (wb - 1))) && rr == -1)
                                return Value::none();
                            wide = name == "checked_div" ? (i128)(rl / rr)
                                                              : (i128)(rl % rr);
                        } else {
                            wide = name == "checked_add" ? (i128)(rl) + (i128)rr
                                 : name == "checked_sub" ? (i128)(rl) - (i128)rr
                                                         : (i128)(rl) * (i128)rr;
                        }
                        if (wide >= smin && wide <= smax) return Value::integer((int64_t)wide);
                        return Value::none();
                    }
                    u128 u0 = (u128)((uint64_t)rl & wmask),
                                      u1 = (u128)((uint64_t)rr & wmask);
                    u128 umax = wb >= 64 ? (u128)ULLONG_MAX
                                                     : (u128)((1ull << wb) - 1ull);
                    u128 r;
                    if (name == "checked_add") r = u0 + u1;
                    else if (name == "checked_sub") r = u0 >= u1 ? u0 - u1 : (u128)~0ull;
                    else if (name == "checked_mul") r = u0 * u1;
                    else {
                        if (u1 == 0) return Value::none();
                        r = name == "checked_div" ? u0 / u1 : u0 % u1;
                    }
                    return r <= umax ? Value::integer((int64_t)r) : Value::none();
                });
            }
            if (name == "checked_neg") {
                return bind({}, [wb, wu](std::vector<Value>& a) -> Value {
                    if (wu) return Value::none();
                    int64_t v = a[0].i;
                    if (wb < 64 && v == -((int64_t)(1ll << (wb - 1)))) return Value::none();
                    return Value::integer(-v);
                });
            }
            if (name == "checked_shl" || name == "checked_shr") {
                return bind({"rhs"}, [wb, wu, &name](std::vector<Value>& a) -> Value {
                    int64_t rl = a[0].i, rr = a[1].i;
                    if (rr < 0 || rr >= wb) return Value::none();
                    if (name == "checked_shr")
                        return Value::integer(rl >> rr);
                    i128 wide = (i128)rl << rr;
                    i128 smin = wu ? 0
                                       : wb >= 64 ? LLONG_MIN
                                                  : (i128)(-(1ll << (wb - 1)));
                    i128 smax = wu ? (wb >= 64 ? (i128)ULLONG_MAX
                                                   : (i128)((1ull << wb) - 1ull))
                                       : wb >= 64 ? LLONG_MAX
                                                  : (i128)((1ll << (wb - 1)) - 1);
                    return (wide >= smin && wide <= smax)
                               ? Value::integer((int64_t)wide)
                               : Value::none();
                });
            }
            if (name == "checked_pow") {
                return bind({"rhs"}, [wb, wu, &name](std::vector<Value>& a) -> Value {
                    int64_t base = a[0].i, exp = a[1].i;
                    if (exp < 0) return Value::none();
                    i128 wide = 1;
                    for (int64_t i = 0; i < exp && i < 256; ++i)
                        wide *= (i128)base;
                    i128 smin = wu ? 0
                                       : wb >= 64 ? LLONG_MIN
                                                  : (i128)(-(1ll << (wb - 1)));
                    i128 smax = wu ? (wb >= 64 ? (i128)ULLONG_MAX
                                                   : (i128)((1ull << wb) - 1ull))
                                       : wb >= 64 ? LLONG_MAX
                                                  : (i128)((1ll << (wb - 1)) - 1);
                    return (wide >= smin && wide <= smax)
                               ? Value::integer((int64_t)wide)
                               : Value::none();
                });
            }

            // {{{ wrapping_* }}}
            if (name.rfind("wrapping_", 0) == 0) {
                return bind({"rhs"}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    int64_t rl = a[0].i, rr = a[1].i;
                    if (wu) {
                        uint64_t r = 0;
                        if (name == "wrapping_add") r = (uint64_t)rl + (uint64_t)rr;
                        else if (name == "wrapping_sub") r = (uint64_t)rl - (uint64_t)rr;
                        else if (name == "wrapping_mul") r = (uint64_t)rl * (uint64_t)rr;
                        else if (name == "wrapping_div") { if (rr == 0) panicHere("division by zero"); r = (uint64_t)rl / (uint64_t)rr; }
                        else if (name == "wrapping_rem") { if (rr == 0) panicHere("division by zero"); r = (uint64_t)rl % (uint64_t)rr; }
                        else if (name == "wrapping_shl")
                            r = (uint64_t)rl << (uint64_t)(rr & (wb - 1));
                        else if (name == "wrapping_shr")
                            r = (uint64_t)rl >> (uint64_t)(rr & (wb - 1));
                        else if (name == "wrapping_pow") {
                            r = 1;
                            uint64_t b = (uint64_t)rl;
                            int64_t e = rr;
                            for (int64_t i = 0; i < e; ++i) r *= b;
                        }
                        else { panicHere("no such wrapping op '" + name + "'"); }
                        r &= wmask;
                        return Value::integer((int64_t)r);
                    }
                    int64_t r = 0;
                    if (name == "wrapping_add") r = (int64_t)((uint64_t)rl + (uint64_t)rr);
                    else if (name == "wrapping_sub") r = (int64_t)((uint64_t)rl - (uint64_t)rr);
                    else if (name == "wrapping_mul") r = (int64_t)((uint64_t)rl * (uint64_t)rr);
                    else if (name == "wrapping_div") { if (rr == 0) panicHere("division by zero"); r = rl / rr; }
                    else if (name == "wrapping_rem") { if (rr == 0) panicHere("division by zero"); r = rl % rr; }
                    else if (name == "wrapping_neg") r = -rl;
                    else if (name == "wrapping_abs") r = rl < 0 ? (rl == LLONG_MIN ? LLONG_MIN : -rl) : rl;
                    else if (name == "wrapping_shl")
                        r = (int64_t)((uint64_t)rl << (uint64_t)(rr & (wb - 1)));
                    else if (name == "wrapping_shr")
                        r = (int64_t)((uint64_t)rl >> (uint64_t)(rr & (wb - 1)));
                    else if (name == "wrapping_pow") {
                        r = 1;
                        uint64_t b = (uint64_t)rl;
                        for (int64_t i = 0; i < rr; ++i) r = (int64_t)((uint64_t)r * b);
                    }
                    else { panicHere("no such wrapping op"); }
                    if (wb >= 64) return Value::integer(r);
                    r = (int64_t)((uint64_t)r & wmask);
                    if (!wu && (r & (1ll << (wb - 1))))
                        r = (int64_t)((int64_t)r | (int64_t)(~wmask));
                    return Value::integer(r);
                });
            }

            // {{{ saturating_* }}}
            if (name.rfind("saturating_", 0) == 0) {
                return bind({"rhs"}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    int64_t rl = a[0].i, rr = a[1].i;
                    i128 smin = wu ? 0 : wb >= 64 ? LLONG_MIN : (i128)(-(1ll << (wb - 1)));
                    i128 smax = wu
                                        ? (wb >= 64 ? LLONG_MAX : (i128)((1ull << wb) - 1ull))
                                        : (wb >= 64 ? LLONG_MAX : (i128)((1ll << (wb - 1)) - 1));
                    i128 wide = 0;
                    if (name == "saturating_add") wide = (i128)rl + (i128)rr;
                    else if (name == "saturating_sub") wide = (i128)rl - (i128)rr;
                    else if (name == "saturating_mul") wide = (i128)rl * (i128)rr;
                    else if (name == "saturating_neg") wide = -(i128)rl;
                    else if (name == "saturating_abs") wide = rl < 0 ? -(i128)rl : rl;
                    else if (name == "saturating_div") { if (rr == 0) return Value::none(); wide = (i128)rl / rr; }
                    else if (name == "saturating_pow") {
                        if (rr < 0) return Value::none();
                        wide = 1;
                        for (int64_t i = 0; i < rr && i < 256; ++i)
                            wide *= (i128)rl;
                    }
                    else { panicHere("no such saturating op"); }
                    if (wide < smin) return Value::integer((int64_t)smin);
                    if (wide > smax) return Value::integer((int64_t)smax);
                    return Value::integer((int64_t)wide);
                });
            }

            // {{{ euclid & ceil }}}
            if (name == "div_euclid" || name == "rem_euclid" || name == "div_ceil") {
                return bind({"rhs"}, [wu, &name](std::vector<Value>& a) -> Value {
                    int64_t x = a[0].i, y = a[1].i;
                    if (y == 0) panicHere("division by zero");
                    if (wu) {
                        int64_t a0 = x < 0 ? 0 : x, b0 = y <= 0 ? 1 : y;
                        if (name == "div_ceil") return Value::integer((a0 + b0 - 1) / b0);
                        int64_t q = a0 / b0;
                        return name == "div_euclid" ? Value::integer(q)
                                                    : Value::integer(a0 - q * b0);
                    }
                    if (name == "div_ceil") {
                        int64_t q = x / y, r = x - q * y;
                        if (r != 0 && ((x > 0) == (y > 0))) q += 1;
                        return Value::integer(q);
                    }
                    // Rust euclidean division: quotient rounded toward -inf on
                    // negative remainder, remainder always in [0, |y|) for
                    // div_euclid; rem_euclid remainder never negative.
                    int64_t q = x / y;
                    int64_t r = x % y;
                    if (r < 0) {
                        if (name == "rem_euclid") {
                            return Value::integer(y < 0 ? r - y : r + y);
                        }
                        return Value::integer(y > 0 ? q - 1 : q + 1);
                    }
                    return Value::integer(name == "rem_euclid" ? r : q);
                });
            }

            // {{{ bit window: counts from the masked representation (Rust parity) }}}
            if (name == "count_ones" || name == "count_zeros" ||
                name == "leading_zeros" || name == "trailing_zeros" ||
                name == "leading_ones" || name == "trailing_ones" ||
                name == "is_power_of_two" || name == "next_power_of_two" ||
                name == "abs" || name == "signum" || name == "ilog2" ||
                name == "min" || name == "max" || name == "bits" ||
                name == "reverse_bits" || name == "swap_bytes" ||
                name == "pow" || name == "pow_glam") {
                return bind({}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    int64_t v = a[0].i;
                    uint64_t u = wu ? ((uint64_t)v & wmask)
                                    : (wb >= 64 ? (uint64_t)v : (uint64_t)v & wmask);
                    auto maskBits = [&](void) -> int { return wb; };
                    (void)maskBits;
                    if (name == "count_ones") {
                        int n = 0; unsigned long long x = (unsigned long long)u;
                        while (x) { n += (int)(x & 1ull); x >>= 1; }
                        return Value::integer(n);
                    }
                    if (name == "count_zeros") {
                        int ones = 0; unsigned long long x = (unsigned long long)u;
                        while (x) { ones += (int)(x & 1ull); x >>= 1; }
                        return Value::integer(wb - ones);
                    }
                    if (name == "leading_zeros" || name == "leading_ones") {
                        unsigned long long x = (unsigned long long)u;
                        if (name == "leading_ones") x = ~x & wmask;
                        int leading = 0;
                        if (x == 0) return Value::integer(wb);
                        // count from the MSB of the field:
                        for (int i = wb - 1; i >= 0; --i) {
                            if (x & (1ull << i)) break;
                            leading++;
                        }
                        return Value::integer(leading);
                    }
                    if (name == "trailing_zeros" || name == "trailing_ones") {
                        unsigned long long x = (unsigned long long)u;
                        if (name == "trailing_ones") x = ~x & wmask;
                        if (x == 0) return Value::integer(wb);
                        int t = 0;
                        while (t < wb && !(x & (1ull << t))) t++;
                        return Value::integer(t);
                    }
                    if (name == "is_power_of_two") {
                        if (wu) return Value::boolean(u != 0 && (u & (u - 1)) == 0);
                        return Value::boolean(v > 0 && (v & (v - 1)) == 0);
                    }
                    if (name == "next_power_of_two") {
                        unsigned long long x = wu ? (unsigned long long)u : (v <= 0 ? 0ULL : (unsigned long long)v);
                        if (x == 0) return Value::integer(1);
                        x--;
                        x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x |= x >> 32;
                        return Value::integer((int64_t)(x + 1));
                    }
                    if (name == "abs") {
                        if (wu) return Value::integer(wu && wb < 64 && (uint64_t)v > 0 && wb < 64 ? (int64_t)((uint64_t)v & wmask) : (int64_t)v);
                        if (v == LLONG_MIN) return Value::none();
                        return Value::integer(v < 0 ? -v : v);
                    }
                    if (name == "signum") return Value::integer(v == 0 ? 0 : (v > 0 ? 1 : -1));
                    if (name == "ilog2") {
                        if (wu) u = v < 0 ? (uint64_t)v & wmask : (uint64_t)v;
                        long long x = v <= 0 ? (long long)((uint64_t)v) : (long long)v;
                        if (x <= 0) return Value::none();
                        int n = 0; while ((1LL << (n + 1)) <= x) n++;
                        return Value::integer(n);
                    }
                    if (name == "bits") return Value::integer(wb);
                    if (name == "min") {
                        if (wu) return Value::integer(0);
                        if (wb >= 64) return Value::integer(LLONG_MIN);
                        return Value::integer(-(int64_t)(1ll << (wb - 1)));
                    }
                    if (name == "max") {
                        if (wu) return Value::integer((int64_t)wmask);
                        if (wb >= 64) return Value::integer(LLONG_MAX);
                        return Value::integer((int64_t)((1ll << (wb - 1)) - 1));
                    }
                    if (name == "reverse_bits") {
                        uint64_t r = 0;
                        for (int i = 0; i < wb; ++i)
                            if (u & (1ull << i)) r |= (1ull << (wb - 1 - i));
                        if (!wu && wb < 64 && (r & (1ull << (wb - 1))))
                            r |= ~wmask;
                        return Value::integer((int64_t)r);
                    }
                    if (name == "swap_bytes") {
                        uint64_t r = u;
                        if (wb == 16) r = ((u & 0xffull) << 8) | ((u >> 8) & 0xffull);
                        else if (wb == 32)
                            r = ((u & 0xffull) << 24) | ((u & 0xff00ull) << 8) |
                                ((u >> 8) & 0xff00ull) | ((u >> 24) & 0xffull);
                        else if (wb == 64)
                            r = kirn::bswap64(u);
                        if (!wu && wb < 64 && (r & (1ull << (wb - 1))))
                            r |= ~wmask;
                        return Value::integer((int64_t)r);
                    }
                    // noop fallback via pow: user can add
                    panicHere("internal method not available");
                    return Value::none();
                });
            }
            // {{{ rotates: masked count within the declared width }}}
            if (name == "rotate_left" || name == "rotate_right") {
                return bind({"rhs"}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    uint64_t u = a[0].i < 0 && !wu ? ((uint64_t)a[0].i & wmask)
                                                   : ((uint64_t)a[0].i & wmask);
                    int64_t n = a[1].i % wb;
                    if (n < 0) n += wb;
                    uint64_t r;
                    if (wb == 64) {
                        r = name == "rotate_left" ? ((u << n) | (u >> ((64 - n) % 64)))
                                                  : ((u >> n) | (u << ((64 - n) % 64)));
                    } else {
                        uint64_t mask = wmask;
                        r = name == "rotate_left"
                                ? (((u << n) | (u >> ((wb - n) % wb))) & mask)
                                : (((u >> n) | (u << ((wb - n) % wb))) & mask);
                    }
                    if (!wu && wb < 64 && (r & (1ull << (wb - 1)))) r |= ~wmask;
                    return Value::integer((int64_t)r);
                });
            }
            if (name == "pow") {
                return bind({"exp"}, [wb, wu, wmask, &name](std::vector<Value>& a) -> Value {
                    int64_t base = a[0].i, exp = a[1].i;
                    if (exp < 0) return Value::none();
                    i128 r = 1, b = base;
                    for (int64_t i = 0; i < exp; ++i) {
                        r *= (i128)b;
                        u128 lim = wmask;
                        if (wu) { r &= (i128)wmask; }
                        else { // signed saturate-ish overflow as saturating? use wrapping to keep total sound applying mask:
                            r &= (i128)wmask; }
                        (void)lim;
                    }
                    int64_t v = (int64_t)r;
                    if (!wu && wb < 64 && (v & (1ll << (wb - 1))))
                        v = (int64_t)((int64_t)r | (int64_t)(~wmask));
                    return Value::integer(v);
                });
            }
            break;
        }

        case VK::BigInt: {
            if (name == "pow") {
                return bind({"exp"}, [bigOf, bigVal](std::vector<Value>& a) -> Value {
                    bignum::Big base = bigOf(a[0]);
                    bignum::Big exp  = bigOf(a[1]);
                    if (exp.neg || exp.limbs.size() > 1) return Value::none();
                    uint64_t e = exp.limbs.empty() ? 0 : exp.limbs[0];
                    return bigVal(base.pow(e));
                });
            }
            if (name == "powmod") {
                return bind({"exp", "mod"}, [bigOf, bigVal](std::vector<Value>& a) -> Value {
                    bignum::Big base = bigOf(a[0]);
                    bignum::Big exp  = bigOf(a[1]);
                    bignum::Big mod  = bigOf(a[2]);
                    if (mod.zero()) return Value::none();
                    return bigVal(base.powmod(exp, mod));
                });
            }
            if (name == "bit_length") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    return Value::integer(bignum::bitLength(b.limbs));
                });
            }
            if (name == "bit_count") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    return Value::integer(bignum::bitCount(b.limbs));
                });
            }
            if (name == "to_int") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    if (b.limbs.empty()) return Value::integer(0);
                    uint64_t lo = b.limbs[0];
                    int64_t v = (int64_t)lo;
                    if (b.neg) v = -(int64_t)lo;
                    return Value::integer(v);
                });
            }
            if (name == "is_prime") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    return Value::boolean(bignum::isPrime(bigOf(a[0])));
                });
            }
            if (name == "gcd" || name == "lcm") {
                bool isLcm = name == "lcm";
                return bind({"other"}, [bigOf, bigVal, isLcm](std::vector<Value>& a) -> Value {
                    bignum::Big x = bigOf(a[0]), y = bigOf(a[1]);
                    bignum::Big g = bignum::gcd(x, y);
                    if (!isLcm) return bigVal(g);
                    if (g.zero()) return bigVal(g);
                    bignum::Big l = x.div(g).mul(y).abs();
                    return bigVal(l);
                });
            }
            if (name == "mod_inverse") {
                return bind({"mod"}, [bigOf, bigVal](std::vector<Value>& a) -> Value {
                    bignum::Big inv;
                    if (!bignum::modInverse(bigOf(a[0]), bigOf(a[1]), inv))
                        return Value::none();
                    return bigVal(inv);
                });
            }
            if (name == "mod_sqrt") {
                return bind({"mod"}, [bigOf, bigVal](std::vector<Value>& a) -> Value {
                    bignum::Big r;
                    if (!bignum::modSqrt(bigOf(a[0]), bigOf(a[1]), r))
                        return Value::none();
                    return bigVal(r);
                });
            }
            if (name == "to_rational") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    if (b.limbs.empty()) {
                        Value v; v.k = VK::Rational; v.num = 0; v.den = 1;
                        return v;
                    }
                    for (size_t i = 1; i < b.limbs.size(); ++i)
                        if (b.limbs[i] != 0)
                            panicHere("rational numerator out of range");
                    if (b.limbs[0] > (uint64_t)INT64_MAX) {
                        panicHere("rational numerator out of range");
                    }
                    Value v; v.k = VK::Rational;
                    v.num = (int64_t)b.limbs[0];
                    v.den = 1;
                    if (b.neg) v.num = -v.num;
                    return v;
                });
            }
            if (name == "to_string") {
                return bind({"base"}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    if (a.size() > 1 && a[1].k == VK::Int)
                        return Value::str(b.toStringBase((unsigned)a[1].i));
                    return Value::str(b.toString());
                });
            }
            if (name == "to_float") {
                return bind({}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    return Value::floating(std::stod(b.toString()));
                });
            }
            if (name == "to_bytes") {
                return bind({"len", "endian"}, [bigOf](std::vector<Value>& a) -> Value {
                    bignum::Big b = bigOf(a[0]);
                    int64_t n = (a.size() > 1 && a[1].k == VK::Int)
                                    ? a[1].i
                                    : (int64_t)((bignum::bitLength(b.limbs) + 7) / 8);
                    if (n < 0) panicHere("to_bytes: length must be non-negative");
                    std::vector<uint8_t> bytes((size_t)n, 0);
                    std::vector<uint8_t> mag;
                    for (uint64_t l : b.limbs)
                        for (int i = 0; i < 8; ++i)
                            mag.push_back((uint8_t)((l >> (8 * i)) & 0xff));
                    for (int64_t i = 0; i < n && i < (int64_t)mag.size(); ++i)
                        bytes[i] = mag[i];
                    if (b.neg) {
                        int carry = 1;
                        for (int64_t i = 0; i < n; ++i) {
                            int xx = (uint8_t)~bytes[i] + carry;
                            bytes[i] = (uint8_t)xx;
                            carry = xx >> 8;
                        }
                    }
                    if (a.size() > 2 && a[2].k == VK::Str && a[2].s == "big")
                        std::reverse(bytes.begin(), bytes.end());
                    Value bv = Value::str(std::string(bytes.begin(), bytes.end()));
                    bv.k = VK::Bytes;
                    return bv;
                });
            }
            break;
        }

        case VK::Float: {
            double d = obj.d;
            auto fn1 = [&](double (*f)(double)) {
                return bind({}, [d, f](std::vector<Value>&) -> Value {
                    return Value::floating(f(d));
                });
            };
            if (name == "floor") return bind({}, [d](std::vector<Value>&) { return Value::integer((int64_t)std::floor(d)); });
            if (name == "ceil")  return bind({}, [d](std::vector<Value>&) { return Value::integer((int64_t)std::ceil(d)); });
            if (name == "trunc") return bind({}, [d](std::vector<Value>&) { return Value::integer((int64_t)std::trunc(d)); });
            if (name == "round") {
                return bind({"n"}, [d](std::vector<Value>& a) -> Value {
                    if (a.size() > 1 && a[1].k == VK::Int) {
                        double p = std::pow(10.0, (double)a[1].i);
                        return Value::floating(std::nearbyint(d * p) / p);
                    }
                    return Value::integer((int64_t)std::nearbyint(d));
                });
            }
            if (name == "fract") return bind({}, [d](std::vector<Value>&) { return Value::floating(d - std::trunc(d)); });
            if (name == "abs")   return bind({}, [d](std::vector<Value>&) { return Value::floating(std::fabs(d)); });
            if (name == "signum") return bind({}, [d](std::vector<Value>&) { return Value::floating(d > 0 ? 1.0 : d < 0 ? -1.0 : d); });
            if (name == "sqrt")  return fn1(&std::sqrt);
            if (name == "exp")   return fn1(&std::exp);
            if (name == "ln")    return fn1(&std::log);
            if (name == "log2")  return fn1(&std::log2);
            if (name == "log10") return fn1(&std::log10);
            if (name == "log") {
                return bind({"base"}, [d](std::vector<Value>& a) -> Value {
                    double b = a.size() > 1 ? (a[1].k == VK::Float ? a[1].d : (double)a[1].i) : std::exp(1.0);
                    return Value::floating(std::log(d) / std::log(b));
                });
            }
            if (name == "sin") return fn1(&std::sin);
            if (name == "cos") return fn1(&std::cos);
            if (name == "tan") return fn1(&std::tan);
            if (name == "asin") return fn1(&std::asin);
            if (name == "acos") return fn1(&std::acos);
            if (name == "atan") return fn1(&std::atan);
            if (name == "sinh") return fn1(&std::sinh);
            if (name == "cosh") return fn1(&std::cosh);
            if (name == "tanh") return fn1(&std::tanh);
            if (name == "atan2") {
                return bind({"x"}, [d](std::vector<Value>& a) -> Value {
                    double x = a.size() > 1 ? (a[1].k == VK::Float ? a[1].d : (double)a[1].i) : 0.0;
                    return Value::floating(std::atan2(d, x));
                });
            }
            if (name == "pow") {
                return bind({"exp"}, [d](std::vector<Value>& a) -> Value {
                    double e = a.size() > 1 ? (a[1].k == VK::Float ? a[1].d : (double)a[1].i) : 1.0;
                    return Value::floating(std::pow(d, e));
                });
            }
            if (name == "copysign") {
                return bind({"sign"}, [d](std::vector<Value>& a) -> Value {
                    double s = a.size() > 1 ? (a[1].k == VK::Float ? a[1].d : (double)a[1].i) : d;
                    return Value::floating(std::copysign(d, s));
                });
            }
            if (name == "is_nan")      return bind({}, [d](std::vector<Value>&) { return Value::boolean(std::isnan(d)); });
            if (name == "is_infinite") return bind({}, [d](std::vector<Value>&) { return Value::boolean(std::isinf(d)); });
            if (name == "is_finite")   return bind({}, [d](std::vector<Value>&) { return Value::boolean(std::isfinite(d)); });
            if (name == "to_degrees")  return bind({}, [d](std::vector<Value>&) { return Value::floating(d * 180.0 / 3.14159265358979323846); });
            if (name == "to_radians")  return bind({}, [d](std::vector<Value>&) { return Value::floating(d * 3.14159265358979323846 / 180.0); });
            if (name == "total_cmp") {
                return bind({"other"}, [d](std::vector<Value>& a) -> Value {
                    double o = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
                    uint64_t x = 0, y = 0;
                    std::memcpy(&x, &d, 8);
                    std::memcpy(&y, &o, 8);
                    auto key = [](uint64_t b) -> int64_t {
                        return (b & 0x8000000000000000ull)
                                   ? (int64_t)~b
                                   : (int64_t)(b ^ 0x8000000000000000ull);
                    };
                    int64_t kx = key(x), ky = key(y);
                    return Value::integer(kx < ky ? -1 : kx > ky ? 1 : 0);
                });
            }
            if (name == "min") {
                return bind({"other"}, [d](std::vector<Value>& a) -> Value {
                    double o = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
                    return Value::floating(std::fmin(d, o));
                });
            }
            if (name == "max") {
                return bind({"other"}, [d](std::vector<Value>& a) -> Value {
                    double o = a[1].k == VK::Float ? a[1].d : (double)a[1].i;
                    return Value::floating(std::fmax(d, o));
                });
            }
            break;
        }

        case VK::Rational: {
            if (name == "numerator")   return bind({}, [](std::vector<Value>& a) { return Value::integer(a[0].num); });
            if (name == "denominator") return bind({}, [](std::vector<Value>& a) { return Value::integer(a[0].den); });
            if (name == "to_float")    return bind({}, [](std::vector<Value>& a) { return Value::floating((double)a[0].num / (double)a[0].den); });
            if (name == "to_int") {
                return bind({}, [](std::vector<Value>& a) {
                    int64_t n = a[0].num, d = a[0].den;
                    return Value::integer((n >= 0 ? n : -n) / d * (n >= 0 ? 1 : -1));
                });
            }
            if (name == "abs") {
                return bind({}, [](std::vector<Value>& a) {
                    Value v = a[0];
                    if (v.num < 0) v.num = -v.num;
                    return v;
                });
            }
            break;
        }

        case VK::Complex: {
            if (name == "real") return Value::floating(obj.re);
            if (name == "imag") return Value::floating(obj.im);
            if (name == "conjugate" || name == "conj")
                return bind({}, [](std::vector<Value>& a) { return Value::complexNum(a[0].re, -a[0].im); });
            if (name == "abs")
                return bind({}, [](std::vector<Value>& a) { return Value::floating(std::hypot(a[0].re, a[0].im)); });
            if (name == "arg")
                return bind({}, [](std::vector<Value>& a) { return Value::floating(std::atan2(a[0].im, a[0].re)); });
            if (name == "exp")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::exp(toCplx(a[0]))); });
            if (name == "log")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::log(toCplx(a[0]))); });
            if (name == "sqrt")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::sqrt(toCplx(a[0]))); });
            if (name == "sin")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::sin(toCplx(a[0]))); });
            if (name == "cos")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::cos(toCplx(a[0]))); });
            if (name == "tan")
                return bind({}, [](std::vector<Value>& a) { return fromCplx(std::tan(toCplx(a[0]))); });
            if (name == "pow") {
                return bind({"exp"}, [](std::vector<Value>& a) -> Value {
                    return fromCplx(std::pow(toCplx(a[0]), toCplx(a[1])));
                });
            }
            break;
        }

        case VK::Decimal: {
            if (name == "to_float")
                return bind({}, [](std::vector<Value>& a) { return Value::floating(decToDouble(a[0])); });
            if (name == "to_int")
                return bind({}, [](std::vector<Value>& a) { return Value::integer(decToI64Trunc(a[0])); });
            if (name == "abs") {
                return bind({}, [](std::vector<Value>& a) {
                    Value v = a[0];
                    v.dneg = false;
                    return v;
                });
            }
            if (name == "sqrt") {
                return bind({}, [](std::vector<Value>& a) {
                    if (a[0].dneg && a[0].mant != "0")
                        panicHere("decimal: sqrt of a negative value");
                    return decFromDouble(std::sqrt(decToDouble(a[0])));
                });
            }
            if (name == "ln" || name == "log")
                return bind({}, [](std::vector<Value>& a) { return decFromDouble(std::log(decToDouble(a[0]))); });
            if (name == "exp")
                return bind({}, [](std::vector<Value>& a) { return decFromDouble(std::exp(decToDouble(a[0]))); });
            if (name == "quantize") {
                return bind({"exp"}, [](std::vector<Value>& a) {
                    int64_t e = a.size() > 1
                                    ? (a[1].k == VK::Int ? a[1].i : (int64_t)a[1].d)
                                    : 0;
                    return decQuantize(a[0], e);
                });
            }
            if (name == "to_string")
                return bind({}, [](std::vector<Value>& a) { return Value::str(decToString(a[0])); });
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

static bignum::Big toBigNum(const Value& v) {
    if (v.k == VK::BigInt) {
        bignum::Big b;
        b.neg = v.neg;
        if (v.limbs) b.limbs = *v.limbs;
        else b.limbs.clear();
        return b;
    }
    if (v.k == VK::Int) return bignum::fromI64(v.i);
    panicHere("expected an int or bigint");
    return {};
}

static double numToDouble(const Value& v) {
    if (v.k == VK::BigInt) return std::stod(toBigNum(v).toString());
    if (v.k == VK::Int) return (double)v.i;
    return v.d;
}

static double numToDouble2(const Value& v) {
    if (v.k == VK::Rational) return (double)v.num / (double)v.den;
    return numToDouble(v);
}

// Decompose an Int/BigInt/Rational into an exact (num,den) pair. Returns false
// for Float (callers compare those through double instead).
static bool ratParts(const Value& v, i128& n, i128& d) {
    if (v.k == VK::Rational) { n = v.num; d = v.den; return true; }
    if (v.k == VK::Int)      { n = v.i;   d = 1;     return true; }
    if (v.k == VK::BigInt) {
        bignum::Big b = toBigNum(v);
        i128 x = b.limbs.empty() ? 0 : (i128)(int64_t)b.limbs[0];
        n = b.neg ? -x : x;
        d = 1;
        return true;
    }
    return false;
}

static bool valEq(const Value& a, const Value& b) {
    if (a.k == VK::Complex || b.k == VK::Complex) {
        auto isnum = [](VK k) {
            return k == VK::Complex || k == VK::Int || k == VK::Float ||
                   k == VK::Rational;
        };
        if (isnum(a.k) && isnum(b.k)) return toCplx(a) == toCplx(b);
        return false;
    }
    if (a.k == VK::Decimal || b.k == VK::Decimal) {
        auto isd = [](VK k) { return k == VK::Decimal || k == VK::Int; };
        if (isd(a.k) && isd(b.k)) return decCmp(a, b) == 0;
        return false;
    }
    if (isBuffer(a) || isBuffer(b)) {
        if (isBuffer(a) && isBuffer(b))
            return bytesData(a) == bytesData(b);
        return false;
    }
    if (a.k == VK::Rational || b.k == VK::Rational) {
        if (a.k == VK::Float || b.k == VK::Float)
            return numToDouble2(a) == numToDouble2(b);
        i128 an, ad, bn, bd;
        if (ratParts(a, an, ad) && ratParts(b, bn, bd))
            return an * bd == bn * ad;
    }
    if (a.k != b.k) {
        if ((a.k == VK::Int || a.k == VK::Float || a.k == VK::BigInt) &&
            (b.k == VK::Int || b.k == VK::Float || b.k == VK::BigInt)) {
            if (a.k == VK::BigInt || b.k == VK::BigInt) {
                if (a.k == VK::Float || b.k == VK::Float) {
                    double x = numToDouble(a);
                    double y = numToDouble(b);
                    return x == y;
                }
                return toBigNum(a).compare(toBigNum(b)) == 0;
            }
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
        case VK::Complex: return a.re == b.re && a.im == b.im;
        case VK::Decimal: return decCmp(a, b) == 0;
        case VK::BigInt:
            return a.neg == b.neg &&
                   (a.limbs == b.limbs ||
                    (a.limbs && b.limbs && *a.limbs == *b.limbs) ||
                    (!a.limbs && b.limbs && b.limbs->empty()) ||
                    (a.limbs && !b.limbs && a.limbs->empty()));
        case VK::Char:  return a.ch == b.ch;
        case VK::Str:
        case VK::Bytes: return a.s == b.s;
        case VK::List:
        case VK::Tuple:
        case VK::Set:
        case VK::Deque:
            if (a.vec->size() != b.vec->size()) return false;
            for (size_t i = 0; i < a.vec->size(); ++i)
                if (!valEq((*a.vec)[i], (*b.vec)[i])) return false;
            return true;
        case VK::Counter: {
            for (const auto& kv : *a.map) {
                if (kv.second.i == 0) continue;
                bool found = false;
                for (const auto& kv2 : *b.map)
                    if (valEq(kv.first, kv2.first)) {
                        found = (kv.second.i == kv2.second.i);
                        break;
                    }
                if (!found) return false;
            }
            for (const auto& kv : *b.map) {
                if (kv.second.i == 0) continue;
                bool found = false;
                for (const auto& kv2 : *a.map)
                    if (valEq(kv.first, kv2.first) &&
                        kv.second.i == kv2.second.i) { found = true; break; }
                if (!found) return false;
            }
            return true;
        }
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
    if (l.k == VK::Decimal || r.k == VK::Decimal) {
        auto isd = [](VK k) { return k == VK::Decimal || k == VK::Int; };
        if (isd(l.k) && isd(r.k)) return decCmp(l, r) < 0;
        panicHere("cannot order these values");
    }
    if (l.k == VK::Rational || r.k == VK::Rational) {
        if (l.k == VK::Float || r.k == VK::Float)
            return numToDouble2(l) < numToDouble2(r);
        i128 ln, ld, rn, rd;
        if (ratParts(l, ln, ld) && ratParts(r, rn, rd))
            return ln * rd < rn * ld;
    }
    if (l.k == VK::Int && r.k == VK::Int) return l.i < r.i;
    if ((l.k == VK::Int || l.k == VK::Float || l.k == VK::BigInt) &&
        (r.k == VK::Int || r.k == VK::Float || r.k == VK::BigInt)) {
        if (l.k == VK::BigInt || r.k == VK::BigInt) {
            if (l.k == VK::Float || r.k == VK::Float)
                return numToDouble(l) < numToDouble(r);
            return toBigNum(l).compare(toBigNum(r)) < 0;
        }
        double x = l.k == VK::Int ? (double)l.i : l.d;
        double y = r.k == VK::Int ? (double)r.i : r.d;
        return x < y;
    }
    if (l.k == VK::Str && r.k == VK::Str)   return l.s < r.s;
    if (isBuffer(l) && isBuffer(r))         return bytesData(l) < bytesData(r);
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

bool Interpreter::hasStructMethod(const Value& v, const std::string& name) {
    if (v.k != VK::Struct && v.k != VK::Heap) return false;
    const std::string& tn = v.k == VK::Heap ? v.heap->typeName : v.typeName;
    auto si = structs_.find(tn);
    if (si != structs_.end())
        for (const auto& m : si->second->body)
            if (m->kind == ast::StKind::FuncDef && m->name == name) return true;
    for (const auto& imp : impls_)
        if (imp.typeName == tn && imp.methods.count(name)) return true;
    return false;
}

Value Interpreter::binop(const std::string& op, const Value& l, const Value& r,
                         int line, int col) {
    // `@` is the matrix-product operator (PEP 465). Scalars are never valid
    // operands, so reject them before the numeric paths with a specific code.
    if (op == "@") {
        auto num = [](VK k) {
            return k == VK::Int || k == VK::Float || k == VK::BigInt ||
                   k == VK::Rational;
        };
        if (num(l.k) || num(r.k))
            panicHere("E-MATMUL-SCALAR: '@' requires matrix/vector operands "
                      "(use '*' for element-wise multiplication)");
    }
    if (l.k == VK::Struct || l.k == VK::Heap) {
        static const std::unordered_map<std::string, std::string> ARITH = {
            {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"},
            {"%", "mod"}, {"@", "matmul"}};
        auto nm = ARITH.find(op);
        if (nm != ARITH.end()) {
            // `@` also accepts the Python-style dunder spelling.
            std::string meth = nm->second;
            if (op == "@" && !hasStructMethod(l, meth) &&
                hasStructMethod(l, "__matmul__"))
                meth = "__matmul__";
            if (hasStructMethod(l, meth))
                return invokeMethod(l, meth, {r}, {}, globals_, line, col,
                                    nullptr);
        }
    }
    // PEP 465 reflected fallback: `a @ b` where `a` does not implement `@`
    // calls `b.__rmatmul__(a)`.
    if (op == "@" && hasStructMethod(r, "__rmatmul__"))
        return invokeMethod(r, "__rmatmul__", {l}, {}, globals_, line, col,
                            nullptr);

    auto numL = [&]() { return l.k == VK::Int ? (double)l.i : l.d; };
    auto numR = [&]() { return r.k == VK::Int ? (double)r.i : r.d; };
    auto isBig = [](VK k) { return k == VK::BigInt; };
    auto bigRes = [](bignum::Big b) -> Value {
        Value v;
        v.k = VK::BigInt;
        v.neg = b.neg;
        v.limbs = std::make_shared<std::vector<uint64_t>>(std::move(b.limbs));
        return v;
    };

    // M6 complex arithmetic: any numeric operand promotes to complex.
    if (l.k == VK::Complex || r.k == VK::Complex) {
        if (op == "@")
            panicHere("E-MATMUL-SCALAR: '@' requires matrix/vector operands "
                      "(use '*' for element-wise multiplication)");
        std::complex<double> x = toCplx(l), y = toCplx(r);
        if (op == "+") return fromCplx(x + y);
        if (op == "-") return fromCplx(x - y);
        if (op == "*") return fromCplx(x * y);
        if (op == "/") {
            if (y == std::complex<double>(0.0, 0.0))
                panicHere("complex division by zero");
            return fromCplx(x / y);
        }
        if (op == "**") return fromCplx(std::pow(x, y));
        panicHere("unsupported operator on complex");
    }

    // M6 decimal arithmetic: exact +,-,* and precision-bounded /. Mixing a
    // decimal with a float is rejected (explicit conversion only); Int operands
    // are promoted to Decimal.
    if (l.k == VK::Decimal || r.k == VK::Decimal) {
        auto asDec = [](const Value& v) -> Value {
            if (v.k == VK::Decimal) return v;
            if (v.k == VK::Int) return decFromSigned(std::to_string(v.i), 0);
            panicHere("decimal: implicit mixing with float/other is not allowed; "
                      "convert explicitly");
            return Value::none();
        };
        if (op == "==" || op == "!=")
            return Value::boolean((op == "==") == (decCmp(l, r) == 0));
        if (op == "<")  return Value::boolean(decCmp(l, r) < 0);
        if (op == ">")  return Value::boolean(decCmp(l, r) > 0);
        if (op == "<=") return Value::boolean(decCmp(l, r) <= 0);
        if (op == ">=") return Value::boolean(decCmp(l, r) >= 0);
        Value x = asDec(l), y = asDec(r);
        if (op == "+") return decAdd(x, y);
        if (op == "-") return decSub(x, y);
        if (op == "*") return decMul(x, y);
        if (op == "/") return decDiv(x, y);
        if (op == "**") {
            if (r.k != VK::Int) panicHere("decimal: exponent must be an int");
            return decPowInt(x, r.i);
        }
        panicHere("unsupported operator on decimal");
    }

    // Phase 2 exact rational arithmetic: Rational with Int/BigInt/Rational.
    // A Float operand demotes the whole expression to float.
    if ((l.k == VK::Rational || r.k == VK::Rational) &&
        !(l.k == VK::Str || r.k == VK::Str)) {
        auto ratNorm = [](i128 n, i128 d) -> Value {
            if (d == 0) panicHere("rational: zero denominator");
            if (d < 0) { n = -n; d = -d; }
            i128 x = n < 0 ? -n : n, y = d;
            while (y) { i128 t = y; y = x % y; x = t; }
            if (x) { n /= x; d /= x; }
            if (n > INT64_MAX || n < INT64_MIN || d > INT64_MAX)
                panicHere("rational: result out of range");
            Value v; v.k = VK::Rational;
            v.num = (int64_t)n;
            v.den = (int64_t)d;
            return v;
        };
        if (l.k == VK::Float || r.k == VK::Float) {
            double x = numToDouble2(l), y = numToDouble2(r);
            if (op == "+") return Value::floating(x + y);
            if (op == "-") return Value::floating(x - y);
            if (op == "*") return Value::floating(x * y);
            if (op == "/") {
                if (y == 0.0) panicHere("division by zero");
                return Value::floating(x / y);
            }
            if (op == "//") return Value::floating(std::floor(x / y));
            if (op == "%")
                return Value::floating(std::fmod(std::fmod(x, y) + y, y));
            if (op == "**") return Value::floating(std::pow(x, y));
        }
        i128 ln, ld, rn, rd;
        ratParts(l, ln, ld);
        ratParts(r, rn, rd);
        if (op == "+") return ratNorm(ln * rd + rn * ld, ld * rd);
        if (op == "-") return ratNorm(ln * rd - rn * ld, ld * rd);
        if (op == "*") return ratNorm(ln * rn, ld * rd);
        if (op == "/") {
            if (rn == 0) panicHere("division by zero");
            return ratNorm(ln * rd, ld * rn);
        }
        if (op == "//") {
            if (rn == 0) panicHere("division by zero");
            i128 a = ln * rd, b = ld * rn;
            i128 q = a / b;
            if (a % b != 0 && ((a < 0) != (b < 0))) --q;
            return Value::integer((int64_t)q);
        }
        if (op == "%") {
            if (rn == 0) panicHere("modulo by zero");
            i128 a = ln * rd, b = ld * rn;
            i128 q = a / b;
            if (a % b != 0 && ((a < 0) != (b < 0))) --q;
            return ratNorm(a - q * b, ld * rd);
        }
        if (op == "**") {
            if (r.k != VK::Int) {
                double x = numToDouble2(l), y = numToDouble2(r);
                return Value::floating(std::pow(x, y));
            }
            int64_t e = r.i;
            bool neg = e < 0;
            uint64_t ue = neg ? (uint64_t)(-(e + 1)) + 1 : (uint64_t)e;
            i128 n = 1, d = 1;
            for (uint64_t i = 0; i < ue; ++i) { n *= ln; d *= ld; }
            if (neg) { i128 t = n; n = d; d = t; }
            return ratNorm(n, d);
        }
    }

    if (op == "+") {
        if (isBig(l.k) || isBig(r.k)) {
            if (l.k == VK::Str || r.k == VK::Str)
                return Value::str(l.k == VK::Str ? l.s + toStr(r)
                                                 : toStr(l) + r.s);
            return bigRes(toBigNum(l).add(toBigNum(r)));
        }
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
        if (isBuffer(l) && isBuffer(r)) {
            std::string out = bytesData(l) + bytesData(r);
            if (l.k == VK::ByteArray || r.k == VK::ByteArray)
                return Value::byteArray(std::move(out));
            Value v = Value::str(std::move(out));
            v.k = VK::Bytes;
            return v;
        }
        if (l.k == VK::Counter && r.k == VK::Counter) {
            Value c = Value::counter();
            auto addKey = [&](const Value& key, int64_t delta) {
                for (auto& kv : *c.map)
                    if (valEq(kv.first, key)) {
                        kv.second = Value::integer(kv.second.i + delta);
                        return;
                    }
                c.map->push_back({key, Value::integer(delta)});
            };
            for (auto& kv : *l.map) addKey(kv.first, kv.second.i);
            for (auto& kv : *r.map) addKey(kv.first, kv.second.i);
            std::vector<std::pair<Value, Value>> keep;
            for (auto& kv : *c.map)
                if (kv.second.i > 0) keep.push_back(kv);
            *c.map = std::move(keep);
            return c;
        }
    }
    if (op == "-") {
        if (isBig(l.k) || isBig(r.k))
            return bigRes(toBigNum(l).sub(toBigNum(r)));
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i - r.i);
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float))
            return Value::floating(numL() - numR());
        if (l.k == VK::Counter && r.k == VK::Counter) {
            Value c = Value::counter();
            for (auto& kv : *l.map) c.map->push_back(kv);
            auto subKey = [&](const Value& key, int64_t delta) {
                for (auto& kv : *c.map)
                    if (valEq(kv.first, key)) {
                        kv.second = Value::integer(kv.second.i - delta);
                        return;
                    }
                c.map->push_back({key, Value::integer(-delta)});
            };
            for (auto& kv : *r.map) subKey(kv.first, kv.second.i);
            std::vector<std::pair<Value, Value>> keep;
            for (auto& kv : *c.map)
                if (kv.second.i > 0) keep.push_back(kv);
            *c.map = std::move(keep);
            return c;
        }
    }
    if (op == "*") {
        if (isBig(l.k) || isBig(r.k)) {
            return bigRes(toBigNum(l).mul(toBigNum(r)));
        }
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
        if (isBuffer(l) && r.k == VK::Int) {
            std::string base = bytesData(l), out;
            for (int64_t n = 0; n < r.i; ++n) out += base;
            if (l.k == VK::ByteArray) return Value::byteArray(std::move(out));
            Value v = Value::str(std::move(out));
            v.k = VK::Bytes;
            return v;
        }
    }
    if (op == "/") {
        if (isBig(l.k) || isBig(r.k)) {
            double x = numToDouble(l), y = numToDouble(r);
            if (y == 0.0) panicHere("division by zero");
            return Value::floating(x / y);
        }
        if ((l.k == VK::Int || l.k == VK::Float) &&
            (r.k == VK::Int || r.k == VK::Float)) {
            if (numR() == 0.0) panicHere("division by zero");
            return Value::floating(numL() / numR());
        }
    }
    if (op == "//") {
        if (isBig(l.k) || isBig(r.k)) {
            bignum::Big q = toBigNum(l).div(toBigNum(r));
            // div() already applies floor semantics
            return bigRes(q);
        }
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
        if (isBig(l.k) || isBig(r.k)) {
            bignum::Big m = toBigNum(r);
            if (m.zero()) panicHere("modulo by zero");
            return bigRes(toBigNum(l).mod(m));
        }
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
        if (isBig(l.k) || isBig(r.k)) {
            bignum::Big e = toBigNum(r);
            if (e.neg) return bigRes(toBigNum(l).pow(0));
            uint64_t ee = e.limbs.empty() ? 0 : e.limbs[0];
            return bigRes(toBigNum(l).pow(ee));
        }
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
        if (isBig(l.k) || isBig(r.k)) {
            if (l.k == VK::BigInt && r.k == VK::Int && r.i >= 0)
                return bigRes(toBigNum(l).shlBits((uint64_t)r.i));
            if (l.k == VK::Int && r.k == VK::Int && r.i >= 0)
                return bigRes(bignum::fromI64(l.i).shlBits((uint64_t)r.i));
            panicHere("'<<' requires integer operands and a non-negative shift");
        }
        if (l.k == VK::Int && r.k == VK::Int && r.i >= 0 && r.i < 63)
            return Value::integer((int64_t)((uint64_t)l.i << r.i));
    }
    if (op == ">>") {
        if (isBig(l.k) || isBig(r.k)) {
            if (l.k == VK::BigInt && r.k == VK::Int && r.i >= 0)
                return bigRes(toBigNum(l).shrBits((uint64_t)r.i));
            if (l.k == VK::Int && r.k == VK::Int && r.i >= 0)
                return bigRes(bignum::fromI64(l.i).shrBits((uint64_t)r.i));
            panicHere("'>>' requires integer operands and a non-negative shift");
        }
        if (l.k == VK::Int && r.k == VK::Int && r.i >= 0 && r.i < 63)
            return Value::integer(l.i >> r.i);
    }
    if (op == "&") {
        if (isBig(l.k) || isBig(r.k))
            return bigRes(bignum::bitwiseOp(toBigNum(l), toBigNum(r), '&'));
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i & r.i);
    }
    if (op == "|") {
        if (isBig(l.k) || isBig(r.k))
            return bigRes(bignum::bitwiseOp(toBigNum(l), toBigNum(r), '|'));
        if (l.k == VK::Int && r.k == VK::Int) return Value::integer(l.i | r.i);
        if (l.k == VK::Set && r.k == VK::Set) {
            std::vector<Value> out = *l.vec;
            for (auto& el : *r.vec) {
                bool found = false;
                for (auto& o : out)
                    if (valEq(o, el)) { found = true; break; }
                if (!found) out.push_back(el);
            }
            return makeSet(std::move(out));
        }
    }
    if (op == "^") {
        if (isBig(l.k) || isBig(r.k))
            return bigRes(bignum::bitwiseOp(toBigNum(l), toBigNum(r), '^'));
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
        case VK::ByteArray:
        case VK::View: {
            std::string data = bytesData(seq);
            for (char c : data) {
                Env child = makeChild(env);
                Value item = Value::integer((int64_t)(unsigned char)c);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::Deque: {
            std::vector<Value> snapshot = *seq.vec;
            for (auto& item : snapshot) {
                Env child = makeChild(env);
                if (!body(item, child)) return;
            }
            return;
        }
        case VK::Counter: {
            std::vector<Value> keys;
            for (auto& kv : *seq.map)
                if (kv.second.i != 0) keys.push_back(kv.first);
            for (auto& key : keys) {
                Env child = makeChild(env);
                if (!body(key, child)) return;
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
                    rp->k == VK::Tuple || rp->k == VK::Deque)
                    (*rp->vec)[normIndex((int64_t)rp->vec->size(), idx2.i)] =
                        std::move(v);
                else if (rp->k == VK::ByteArray || rp->k == VK::View) {
                    if (rp->k == VK::View && rp->vreadonly)
                        panicHere("cannot mutate a read-only memoryview");
                    size_t lim = rp->k == VK::View ? rp->vlen : rp->bstr->size();
                    size_t n = normIndex((int64_t)lim, idx2.i);
                    (*rp->bstr)[(rp->k == VK::View ? rp->voff : 0) + n] =
                        (char)(v.i & 0xff);
                }
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
                case VK::ByteArray:
                case VK::View: {
                    if (cont.k == VK::View && cont.vreadonly)
                        panicHere("cannot mutate a read-only memoryview");
                    size_t lim = cont.k == VK::View ? cont.vlen
                                                    : cont.bstr->size();
                    size_t n = normIndex((int64_t)lim, idx.i);
                    (*cont.bstr)[(cont.k == VK::View ? cont.voff : 0) + n] =
                        (char)(v.i & 0xff);
                    return;
                }
                case VK::Dict:
                    for (auto& kv : *cont.map)
                        if (valEq(kv.first, idx)) {
                            kv.second = std::move(v);
                            return;
                        }
                    cont.map->emplace_back(std::move(idx), std::move(v));
                    return;
                case VK::Counter:
                    for (auto& kv : *cont.map)
                        if (valEq(kv.first, idx)) {
                            kv.second = std::move(v);
                            return;
                        }
                    cont.map->push_back({std::move(idx), std::move(v)});
                    return;
                case VK::Deque:
                    (*cont.vec)[normIndex((int64_t)cont.vec->size(), idx.i)] =
                        std::move(v);
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
    t->th = std::thread([this, calleeCopy, args, t]() {
        threadEntry(calleeCopy, args, t);
    });

    Value h;
    h.k = VK::ThreadH;
    h.thread = t;
    return h;
}

Value Interpreter::spawnTask(Value callee, std::vector<Value> args) {
    auto t = std::make_shared<ThreadImpl>();
    {
        std::lock_guard<std::mutex> lk(threadsM_);
        threads_.push_back(t);
    }
    if (!callee.env) callee.env = globals_;
    t->th = std::thread([this, callee, args, t]() {
        threadEntry(callee, args, t);
    });
    Value h;
    h.k = VK::ThreadH;
    h.thread = t;
    return h;
}

Value Interpreter::awaitTask(const Value& task) {
    auto t = task.thread;
    if (!t) return Value::none();
    if (!t->done) {
        std::unique_lock<std::mutex> lk(t->m);
        t->cv.wait(lk, [&] { return t->done; });
    }
    // Join the OS thread unless we are awaiting our own task.
    if (!t->joined && t->th.joinable()) {
        if (std::this_thread::get_id() == t->th.get_id())
            t->th.detach();
        else
            t->th.join();
        t->joined = true;
    }
    // A raised error is catchable at the await site; a panic is not.
    if (t->raised) throw SignalRaise{t->raiseValue};
    if (t->panicked) panicHere(t->panicMsg);
    return t->result;
}

void Interpreter::threadEntry(Value callee, std::vector<Value> args,
                              std::shared_ptr<ThreadImpl> t) {
    Value ret = Value::none();
    bool panicked = false;
    bool raised = false;
    Value raiseValue;
    std::string msg;
    std::vector<std::string> frames;
    try {
        // Invoke the callee directly. Going through `callValue` would re-enter
        // the async dispatch and spawn a nested task forever for `async fn`.
        if (callee.k == VK::Fn && callee.lam)
            ret = runLambda(callee.lam, std::move(args),
                            callee.env ? callee.env : globals_);
        else if (callee.k == VK::Fn && callee.fn)
            ret = runFunc(callee.fn, std::move(args), {},
                          callee.env ? callee.env : globals_);
        else
            ret = callValue(std::move(callee), std::move(args), 0, 0);
    } catch (const PanicSignal& p) {
        panicked = true;
        msg = p.msg;
        frames = p.frames;
    } catch (const SignalRaise& r) {
        // `raise` in a task is a value to be propagated, not a panic: capture
        // it and let awaitTask re-raise it in the awaiting context.
        raised = true;
        raiseValue = r.errResult;
    } catch (...) {
        panicked = true;
        msg = "unknown error in task";
    }
    if (t) {
        {
            std::lock_guard<std::mutex> lk(t->m);
            t->result = std::move(ret);
            t->panicked = panicked;
            t->panicMsg = msg;
            t->panicFrames = frames;
            t->raised = raised;
            t->raiseValue = raiseValue;
            t->done = true;
        }
        t->cv.notify_all();
        if (panicked) {
            std::string full = "panic in spawned thread: " + msg;
            for (const auto& f : frames) full += "\n  " + f;
            fputs((full + "\n").c_str(), stderr);
        }
    } else if (panicked) {
        std::string full = "panic in spawned thread: " + msg;
        for (const auto& f : frames) full += "\n  " + f;
        fputs((full + "\n").c_str(), stderr);
    }
}

Value Interpreter::selectExec(const Stmt& s, Env env) {
    struct Prepared {
        enum Kind { Recv, Timer, Task, Gate } kind = Kind::Gate;
        Value cachedChan;                      // Recv
        Value cached;                          // Timer/Task/Gate
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
            } else if (cv.k == VK::ThreadH) {
                // POWER_PLAN §2.10: a task handle is a source. The arm is ready
                // once the task finished; the bound name receives its result.
                pr.kind = Prepared::Task;
                pr.cached = cv;
            } else {
                pr.kind = Prepared::Gate;
                pr.cached = cv;
            }
        }
        prepared.push_back(std::move(pr));
    }

    const size_t n = prepared.size();
    // Unbiased selects rotate their starting arm so a permanently-ready arm
    // cannot starve the rest; `select biased` pins the source order. (Tokio
    // randomizes instead, but a rotation is deterministic and testable.)
    const size_t start =
        (n > 0 && !s.selBiased) ? (selectTurn_++) % n : 0;

    for (;;) {
        for (size_t k = 0; k < n; ++k) {
            const auto& pr = prepared[(start + k) % n];
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
                case Prepared::Task: {
                    auto t = pr.cached.thread;
                    if (t) {
                        std::lock_guard<std::mutex> lk(t->m);
                        ready = t->done;
                    } else {
                        ready = true;
                    }
                    // Join and read the result (or re-raise the task's error)
                    // only once it is done, so the select does not block here.
                    if (ready) payload = awaitTask(pr.cached);
                    break;
                }
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
        // POWER_PLAN §2.10: `else` is the non-blocking tail - taken when a
        // full pass found nothing ready.
        if (!s.elseBody.empty()) {
            execBlock(s.elseBody, makeChild(env));
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
    // Join (never detach) outstanding tasks. A detached thread outlives this
    // Interpreter and keeps touching globals_/env while they are destroyed,
    // which segfaulted intermittently when main exited with a live task
    // (`var h = spawn f;` and not awaiting it). threadEntry catches every
    // panic and never rethrows, so joining cannot abort the process; tasks
    // that never complete simply block here rather than crash.
    for (auto& t : local)
        if (t && t->th.joinable()) t->th.join();
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
        "print","len","range","panic","catch_panic","printf","strlen",
        "assert","assert_eq","type","sum","typeof",
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
        "sizeof","alignof",
    };
    std::unordered_set<std::string> mod = {
        "math","time","io","mem","json","text","os",
    };
    vmProg_ = std::make_unique<vm::CompileResult>(
        vm::compileProgram(program_->body, mainFn, uf, bi, mod));
    if (envGet("KIRN_VM_DUMP")) {
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
    if (isResultRet(*fn))
        return r.k == VK::Result ? std::move(r) : Value::resultOk(std::move(r));
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
                    else if (k == B_MOD)
                        res = std::fmod(std::fmod(a, b) + b, b);
                    else res = std::pow(a, b);
                    out.k = VK::Float; out.u.d = res; return true;
                }
                int64_t a = l.u.i, b = r.u.i, res;
                if (k == B_ADD) res = a + b;
                else if (k == B_SUB) res = a - b;
                else if (k == B_MUL) res = a * b;
                else if (k == B_DIV) {
                    if (b == 0) return false;
                    out.k = VK::Float; out.u.d = (double)a / (double)b; return true;
                }
                else if (k == B_MOD) {
                    if (b == 0) return false;
                    res = a % b;
                    if (res != 0 && ((res < 0) != (b < 0))) res += b;
                }
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
                else if (ins.op == OP_MAKE_SET) push(fromVal(makeSet(std::move(vs))));
                else push(fromVal(Value::tuple(std::move(vs))));
                break;
            }

            case OP_MAKE_DICT: {
                Value d = Value::dict();
                // Stack holds k1 v1 k2 v2 ... so pop reverses; replay in
                // original order so last-wins matches the tree-walker.
                std::vector<std::pair<Value, Value>> items;
                items.reserve((size_t)ins.a);
                for (int32_t i = 0; i < ins.a; ++i) {
                    Value val = pop().take();
                    Value key = pop().take();
                    items.emplace_back(std::move(key), std::move(val));
                }
                for (auto it = items.rbegin(); it != items.rend(); ++it)
                    dictPutInPlace(d, std::move(it->first), std::move(it->second));
                push(fromVal(std::move(d)));
                break;
            }

            case OP_APPEND: {
                // Comprehensions grow a collection inside a loop. a selects the
                // target kind: 0=list, 1=set, 2=dict. Stack is [cont, item] for
                // list/set or [cont, key, val] for dict; pop the item(s) and the
                // container, mutate in place, push the container back.
                if (ins.a == 2) {
                    Value val = pop().take();
                    Value key = pop().take();
                    Value cont = pop().take();
                    if (cont.k == VK::Dict) dictPutInPlace(cont, std::move(key), std::move(val));
                    push(fromVal(std::move(cont)));
                } else {
                    Value item = pop().take();
                    Value cont = pop().take();
                    if (ins.a == 1) {
                        if (cont.k == VK::Set) {
                            bool dup = false;
                            for (auto& e : *cont.vec)
                                if (valEq(e, item)) { dup = true; break; }
                            if (!dup) cont.vec->push_back(std::move(item));
                        }
                    } else if (cont.k == VK::List) {
                        cont.vec->push_back(std::move(item));
                    }
                    push(fromVal(std::move(cont)));
                }
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
                    case VK::ByteArray: case VK::View: {
                        std::string d = bytesData(obj);
                        size_t n = normIndex((int64_t)d.size(), idx.i);
                        push(fromVal(Value::integer((unsigned char)d[n])));
                        break;
                    }
                    case VK::Deque: {
                        push(fromVal(Value((*obj.vec)[normIndex((int64_t)obj.vec->size(), idx.i)])));
                        break;
                    }
                    case VK::Counter: {
                        Value rv = Value::integer(0);
                        for (const auto& kv : *obj.map)
                            if (valuesEqual(kv.first, idx)) { rv = kv.second; break; }
                        push(fromVal(std::move(rv)));
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

            case OP_DEFINE: {
                // A declaration always creates a NEW binding in the current
                // frame. It must not use OP_STORE's findRef, which walks to the
                // nearest enclosing binding: that turns `var x = 2` inside a
                // block into an assignment to the outer x, so the block-local
                // declaration escapes the block. The tree-walker draws the same
                // line in bindDecl ("a declaration ALWAYS creates a new binding
                // in the current frame (shadowing) ... mirrors the checker").
                const std::string& n = SCC[(size_t)ins.a];
                env->vars[n] = pop().take();
                break;
            }

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
                    case VK::Deque:
                        items = *seq.vec;
                        break;
                    case VK::Counter:
                        for (const auto& kv : *seq.map)
                            if (kv.second.i != 0) items.push_back(kv.first);
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
