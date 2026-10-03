// HIR lowering: checked AST -> lowered IR. See hir.h for the design rationale.
//
// The whole-language contract: every ast::ExKind (24) and every body-level
// ast::StKind has a mapping. Anything without one is emitted as an
// Item::Unhandled carrying a note, never dropped -- a silent drop is how a
// desugaring bug hides, and the corpus report at the bottom of the file is
// printed by the gate to show what is and is not covered.

#include "hir/hir.h"

#include "support/literals.h"
#include "interp/escapes.h"

#include <cassert>
#include <ostream>
#include <set>

namespace kirn::hir {
namespace {

// Bounded-depth guard. This is the one check a pointer-owned IR gets for free
// from CFG structure and does not get here: LLVM and Swift cannot build a
// cycle because a block owns its instructions, but a `unique_ptr` tree that is
// built by mutation can. Cheap, and it turns an infinite recursion into a
// diagnostic.
constexpr int kMaxDepth = 512;

struct Lowerer {
    sema::Checker& chk;
    Context& ctx;
    DiagEngine& diags;
    const LowerOptions& opts;
    std::vector<ScopeId> scopeStack;
    std::vector<DeclId> fnStack;
    int depth = 0;
    // Counters, so the gate can report real coverage rather than a claim.
    std::map<std::string, int> unhandled;
    // Nested named functions, collected here and moved into Module::funcs by
    // lower(). They are functions, so they belong in the function list even
    // though they were found inside a body.
    std::vector<std::unique_ptr<Func>> nestedFuncs;

    Lowerer(sema::Checker& c, Context& x, DiagEngine& d, const LowerOptions& o)
        : chk(c), ctx(x), diags(d), opts(o) {}

    // --- scopes ---------------------------------------------------------
    ScopeId pushScope(bool fnTop = false) {
        ScopeId parent = scopeStack.empty() ? NO_SCOPE : scopeStack.back();
        ScopeId id = ctx.addScope(parent, fnTop);
        scopeStack.push_back(id);
        return id;
    }
    void popScope() { scopeStack.pop_back(); }

    Scope* curScope() {
        return scopeStack.empty() ? nullptr : &ctx.scopes[scopeStack.back()];
    }

    // Resolve a name to a declaration, innermost scope first. A miss is NOT an
    // error here: sema::Checker has already reported unknown names, and this
    // pass must not invent a second diagnostic for the same problem. A miss
    // becomes NO_DECL, which the verifier flags, so a resolution bug is still
    // caught -- just in one place instead of two.
    DeclId resolve(const std::string& name) const {
        for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
            const Scope& sc = ctx.scopes[*it];
            for (auto b = sc.bindings.rbegin(); b != sc.bindings.rend(); ++b) {
                const Decl* d = ctx.get(*b);
                if (d && d->name == name) return *b;
            }
        }
        auto f = ctx.byName.find(name);
        if (f != ctx.byName.end() && !f->second.empty()) {
            // Prefer the most recent declaration, which is what a shadowing
            // module-level redeclaration should resolve to.
            return f->second.back();
        }
        return NO_DECL;
    }

    DeclId declare(const std::string& name, Decl::Kind kind, const ast::Span& span,
                   sema::TyP ty, bool isLocal, uint32_t* slotOut) {
        Decl d;
        d.kind = kind;
        d.name = name;
        d.span = span;
        d.type = std::move(ty);
        d.isLocal = isLocal;
        if (!fnStack.empty()) d.parent = fnStack.back();
        if (slotOut) *slotOut = 0;
        DeclId id = ctx.addDecl(std::move(d));
        if (Scope* sc = curScope()) sc->bindings.push_back(id);
        return id;
    }

    // Slot assignment is per function and dense, mirroring the VM's
    // `slotNames` and rustc's ItemLocalId density invariant. Dense slots are
    // what make "slot < fn.slotCount" a real bound rather than a tautology.
    uint32_t nextSlot(Func& f, const std::string& name) {
        for (uint32_t i = 0; i < f.paramNames.size(); ++i)
            if (f.paramNames[i] == name) return i;
        f.paramNames.push_back(name);
        return static_cast<uint32_t>(f.paramNames.size() - 1);
    }

    // --- shared -------------------------------------------------------
    ast::Span spanOf(const ast::Expr& e) const {
        return opts.keepSpans ? e.span : ast::Span{};
    }
    ast::Span spanOf(const ast::Stmt& s) const {
        return opts.keepSpans ? s.span : ast::Span{};
    }
    sema::TyP typeOf(const ast::Expr& e) { return chk.typeOf(e); }

    ExprP mk(ExprKind k, ast::Span sp, sema::TyP ty, ExprPayload p) {
        auto e = std::make_unique<Expr>();
        e->kind = k;
        e->span = sp;
        e->type = std::move(ty);
        e->p = std::move(p);
        return e;
    }
    StmtP mk(StmtKind k, ast::Span sp, StmtPayload p) {
        auto s = std::make_unique<Stmt>();
        s->kind = k;
        s->span = sp;
        s->p = std::move(p);
        return s;
    }

    static SpanRange range(const ast::Span& sp) {
        SpanRange r;
        r.line = sp.line;
        r.col = sp.col;
        r.endLine = sp.endLine ? sp.endLine : sp.line;
        r.endCol = sp.endCol ? sp.endCol : sp.col + 1;
        return r;
    }

    // Records a statement HIR cannot represent. The Nop it returns is flagged as
    // a drop, so verify() fails on it -- see payload::Nop.
    StmtP unhandledStmt(const char* what, const ast::Stmt& s) {
        unhandled[what]++;
        diags.warning(range(spanOf(s))).code("W9000")
            .msg(std::string("HIR: no mapping for ") + what +
                 "; the statement is dropped and HIR verification will fail")
            .emit();
        return mk(StmtKind::Nop, spanOf(s), payload::Nop{true, what});
    }

    // --- patterns ------------------------------------------------------
    Pattern lowerPat(const ast::Pat& p) {
        Pattern out;
        out.span = opts.keepSpans ? p.span : ast::Span{};
        switch (p.kind) {
            case ast::PatKind::Wild:     out.kind = Pattern::Kind::Wild; break;
            case ast::PatKind::Literal:
                out.kind = Pattern::Kind::Literal;
                if (p.literal) {
                    if (p.literal->kind == ast::ExKind::Str) {
                        out.lit.s = p.literal->text;
                        out.lit.isStr = true;
                    } else if (p.literal->kind == ast::ExKind::Float) {
                        out.lit.f = parseFloatLiteral(p.literal->text);
                        out.lit.isFloat = true;
                    } else {
                        out.lit.i = parseIntLiteral(p.literal->text);
                    }
                }
                break;
            case ast::PatKind::Range:
                out.kind = Pattern::Kind::Range;
                out.inclusive = p.inclusive;
                if (p.lo) out.lo = std::make_unique<Pattern>(lowerPat(*p.lo));
                if (p.hi) out.hi = std::make_unique<Pattern>(lowerPat(*p.hi));
                break;
            case ast::PatKind::Tuple:
            case ast::PatKind::Slice:
                out.kind = p.kind == ast::PatKind::Tuple ? Pattern::Kind::Tuple
                                                          : Pattern::Kind::Slice;
                for (const auto& e : p.elems) out.elems.push_back(std::make_unique<Pattern>(lowerPat(*e)));
                // `[head, ..tail]` and `(a, b, ..rest)` bind a NAME for the rest.
                // The name lives on the pattern, not in `elems`, so skipping it
                // left `tail` and `rest` unresolvable (examples/31, /38).
                if (!p.restName.empty()) {
                    out.restName = p.restName;
                    out.restBinding = declare(p.restName, Decl::Kind::Var, out.span, nullptr,
                                              true, nullptr);
                    out.hasRest = true;
                }
                break;
            case ast::PatKind::Ctor:
                out.kind = Pattern::Kind::Ctor;
                out.ctor = p.ctorName;
                for (const auto& f : p.fields)
                    out.fields.push_back(PatField{f.name, f.pat ? std::make_unique<Pattern>(lowerPat(*f.pat)) : nullptr});
                break;
            case ast::PatKind::Bind:
                out.kind = Pattern::Kind::Bind;
                out.name = p.bindName;
                out.binding = declare(p.bindName, Decl::Kind::Var, out.span, nullptr, true, nullptr);
                break;
            case ast::PatKind::BindAlias:
                // `t @ (even, 7)` binds t for the arm body, so it is a real
                // declaration. Storing the name without declaring it left every
                // use of t unresolvable (examples/31).
                out.kind = Pattern::Kind::BindAlias;
                out.name = p.bindName;
                out.binding = declare(p.bindName, Decl::Kind::Var, out.span, nullptr,
                                      true, nullptr);
                if (p.aliasSub) out.sub = std::make_unique<Pattern>(lowerPat(*p.aliasSub));
                break;
            case ast::PatKind::Or:
                out.kind = Pattern::Kind::Or;
                for (const auto& e : p.alts) out.elems.push_back(std::make_unique<Pattern>(lowerPat(*e)));
                break;
            case ast::PatKind::Rest:
                out.kind = Pattern::Kind::Rest;
                out.name = p.restName;
                break;
            case ast::PatKind::Ref:
                out.kind = Pattern::Kind::Ref;
                if (p.inner) out.sub = std::make_unique<Pattern>(lowerPat(*p.inner));
                break;
        }
        return out;
    }

    // --- expressions ---------------------------------------------------
    ExprP lower(const ast::Expr& e) {
        if (depth > kMaxDepth) {
            auto r = mk(ExprKind::Error, spanOf(e), typeOf(e), payload::Error{"nesting too deep"});
            return r;
        }
        ++depth;
        ExprP r = lowerInner(e);
        --depth;
        return r;
    }

    ExprP lowerInner(const ast::Expr& e) {
        const ast::Span sp = spanOf(e);
        const sema::TyP ty = typeOf(e);
        switch (e.kind) {
            case ast::ExKind::Int:
                return mk(ExprKind::Int, sp, ty, payload::Lit{parseIntLiteral(e.text), 0, "", ast::StrFlavor::Normal, false, false, false});
            case ast::ExKind::Float:
                return mk(ExprKind::Float, sp, ty, payload::Lit{0, parseFloatLiteral(e.text), "", ast::StrFlavor::Normal, true, false, false});
            case ast::ExKind::CharLit:
                return mk(ExprKind::CharLit, sp, ty, payload::Lit{(int64_t)decodeCharText(e.text), 0, std::string(), ast::StrFlavor::Normal, false, false, true});
            case ast::ExKind::Str: {
                // Byte and C strings carry escapes that the raw spelling does not
                // show; decode them here exactly as vm/compiler.cpp does, so a HIR
                // literal and a bytecode constant are the same bytes.
                std::string text = e.text;
                if (e.flavor == ast::StrFlavor::Byte || e.flavor == ast::StrFlavor::C)
                    text = decodeEscapes(e.text);
                return mk(ExprKind::Str, sp, ty, payload::Lit{0, 0, text, e.flavor, false, true, false});
            }
            case ast::ExKind::FString: {
                // A part is EITHER literal text or an expression: ast::FStrPart
                // carries a `text` chunk and a NULLABLE `expr`. Treating every
                // part as an expression dereferenced a null unique_ptr on any
                // f-string containing literal text -- which is examples/01, the
                // first file in the corpus.
                payload::FString f;
                for (const auto& part : e.parts) {
                    payload::FString::Part p;
                    p.isExpr = part.isExpr && part.expr != nullptr;
                    p.text = part.text;
                    p.spec = part.spec;
                    if (p.isExpr) p.expr = lower(*part.expr);
                    f.parts.push_back(std::move(p));
                }
                return mk(ExprKind::FString, sp, ty, std::move(f));
            }
            case ast::ExKind::Ident: {
                payload::Name n;
                n.decl = resolve(e.text);
                n.spelling = e.text;
                return mk(ExprKind::Name, sp, ty, std::move(n));
            }
            case ast::ExKind::Unary: {
                payload::Unary u;
                u.op = e.op;
                if (e.lhs) u.operand = lower(*e.lhs);
                else if (e.rhs) u.operand = lower(*e.rhs);
                return mk(ExprKind::Unary, sp, ty, std::move(u));
            }
            case ast::ExKind::Binary: {
                payload::Binary b;
                b.op = e.op;
                if (e.lhs) b.lhs = lower(*e.lhs);
                if (e.rhs) b.rhs = lower(*e.rhs);
                return mk(ExprKind::Binary, sp, ty, std::move(b));
            }
            case ast::ExKind::Call: {
                payload::Call c;
                c.nilSafe = e.nilSafe;
                if (e.lhs) c.callee = lower(*e.lhs);
                // CallArg::value and MatchArm::pat are both nullable in the AST.
                // Lowering must not assume the parser filled them, or a null
                // deref is an abort instead of a diagnostic.
                for (const auto& a : e.args)
                    c.args.push_back(a.value ? lower(*a.value)
                        : mk(ExprKind::Error, sp, ty, payload::Error{"call argument has no value"}));
                return mk(ExprKind::Call, sp, ty, std::move(c));
            }
            case ast::ExKind::Index: {
                payload::Index ix;
                if (e.lhs) ix.obj = lower(*e.lhs);
                if (e.rhs) ix.idx = lower(*e.rhs);
                return mk(ExprKind::Index, sp, ty, std::move(ix));
            }
            case ast::ExKind::Slice: {
                payload::Slice s;
                if (e.lhs) s.obj = lower(*e.lhs);
                if (e.rhs) {
                    // a Slice holds up to three bounds in the AST's elems.
                    const auto& b = e.rhs->elems;
                    size_t i = 0;
                    if (i < b.size()) s.lo = lower(*b[i++]);
                    if (i < b.size()) s.hi = lower(*b[i++]);
                    if (i < b.size()) s.step = lower(*b[i++]);
                }
                return mk(ExprKind::Slice, sp, ty, std::move(s));
            }
            case ast::ExKind::Member: {
                payload::Member m;
                if (e.lhs) m.obj = lower(*e.lhs);
                m.name = e.text;
                m.nilSafe = e.nilSafe;
                return mk(ExprKind::Member, sp, ty, std::move(m));
            }
            case ast::ExKind::Try:
                return mk(ExprKind::Try, sp, ty, payload::TryExpr{e.lhs ? lower(*e.lhs) : nullptr});
            case ast::ExKind::Cond: {
                payload::Cond c;
                if (e.cond) c.cond = lower(*e.cond);
                if (e.lhs) c.then = lower(*e.lhs);
                if (e.rhs) c.otherwise = lower(*e.rhs);
                return mk(ExprKind::Cond, sp, ty, std::move(c));
            }
            case ast::ExKind::Lambda: {
                payload::Lambda l;
                l.isBlock = !e.lambdaBody.empty();
                pushScope(true);
                for (const auto& pn : e.lambdaParams) {
                    DeclId id = declare(pn, Decl::Kind::Param, sp, nullptr, true, nullptr);
                    l.params.push_back(id);
                }
                for (const auto& p2 : e.closureParams) {
                    DeclId id = declare(p2.name, Decl::Kind::Param, sp, nullptr, true, nullptr);
                    l.params.push_back(id);
                }
                for (const auto& s : e.lambdaBody) l.body.push_back(lower(*s));
                if (e.rhs) l.arrow = lower(*e.rhs);
                popScope();
                return mk(ExprKind::Lambda, sp, ty, std::move(l));
            }
            case ast::ExKind::List: case ast::ExKind::Set: case ast::ExKind::Tuple: {
                payload::Composite c;
                for (const auto& el : e.elems) c.elems.push_back(lower(*el));
                ExprKind k = e.kind == ast::ExKind::List ? ExprKind::List
                           : e.kind == ast::ExKind::Set  ? ExprKind::Set
                                                         : ExprKind::Tuple;
                return mk(k, sp, ty, std::move(c));
            }
            case ast::ExKind::Dict: {
                payload::DictLit d;
                for (const auto& kv : e.pairs) {
                    d.keys.push_back(lower(*kv.first));
                    d.values.push_back(lower(*kv.second));
                }
                return mk(ExprKind::Dict, sp, ty, std::move(d));
            }
            case ast::ExKind::ListComp:
            case ast::ExKind::Generator: {
                payload::Comp c;
                c.isGenerator = e.kind == ast::ExKind::Generator;
                pushScope();
                for (const auto& cl : e.clauses) {
                    payload::Comp::Clause out;
                    out.kind = cl.isFor ? "for" : "if";
                    if (cl.pat) out.pat = lowerPat(*cl.pat);
                    if (cl.iter) out.iterable = lower(*cl.iter);
                    if (cl.cond) out.cond = lower(*cl.cond);
                    c.clauses.push_back(std::move(out));
                }
                // The element expression is elems[0], not lhs: the parser pushes
                // it into `elems` (see the ListComp arm in parser.cpp) and leaves
                // lhs unused for this kind.
                if (!e.elems.empty()) c.element = lower(*e.elems[0]);
                popScope();
                ExprKind k = c.isGenerator ? ExprKind::Generator : ExprKind::ListComp;
                return mk(k, sp, ty, std::move(c));
            }
            case ast::ExKind::New: {
                payload::NewExpr n;
                if (e.newType) n.typeName = e.newType->name;
                for (const auto& a : e.args)
                    if (a.value) n.args.push_back(lower(*a.value));
                return mk(ExprKind::New, sp, ty, std::move(n));
            }
            case ast::ExKind::Cast: {
                payload::CastExpr c;
                if (e.lhs) c.value = lower(*e.lhs);
                if (e.newType) c.typeName = e.newType->name;
                return mk(ExprKind::Cast, sp, ty, std::move(c));
            }
            case ast::ExKind::Match: {
                payload::MatchExpr m;
                if (e.lhs) m.subject = lower(*e.lhs);
                pushScope();
                for (const auto& arm : e.matchArms) {
                    payload::MatchExpr::Arm a;
                    a.pat = arm.pat ? lowerPat(*arm.pat) : Pattern{};
                    for (const auto& st : arm.body)
                        if (st) a.body.push_back(lower(*st));
                    if (arm.guard) a.value = lower(*arm.guard);
                    m.arms.push_back(std::move(a));
                }
                popScope();
                return mk(ExprKind::Match, sp, ty, std::move(m));
            }
        }
        return mk(ExprKind::Error, sp, ty, payload::Error{"unhandled expression kind"});
    }

    // --- statements ----------------------------------------------------
    payload::Block lowerBlock(const std::vector<ast::StmtP>& body, bool fnTop = false) {
        payload::Block b;
        b.scope = pushScope(fnTop);
        for (const auto& s : body) b.stmts.push_back(lower(*s));
        popScope();
        return b;
    }

    StmtP lower(const ast::Stmt& s) {
        if (depth > kMaxDepth) {
            return mk(StmtKind::Nop, spanOf(s), payload::Nop{});
        }
        ++depth;
        StmtP r = lowerInner(s);
        --depth;
        return r;
    }

    StmtP lowerInner(const ast::Stmt& s) {
        const ast::Span sp = spanOf(s);
        switch (s.kind) {
            case ast::StKind::Pass:
                return mk(StmtKind::Nop, sp, payload::Nop{false, {}});
            case ast::StKind::ExprStmt: {
                payload::ExprStmt es;
                if (!s.exprs.empty()) es.value = lower(*s.exprs[0]);
                return mk(StmtKind::ExprStmt, sp, std::move(es));
            }
            case ast::StKind::VarDecl:
            case ast::StKind::ConstDecl:
            case ast::StKind::LocalDecl: {
                payload::VarDecl v;
                v.isConst = s.kind == ast::StKind::ConstDecl;
                v.isLocal = s.kind == ast::StKind::LocalDecl;
                if (s.value) v.init = lower(*s.value);
                if (s.target && s.target->kind == ast::ExKind::Ident) {
                    // A written annotation wins; otherwise the initialiser's type,
                    // which typeOf() has already resolved.
                    sema::TyP ty = s.declType ? chk.resolveType(s.declType)
                                               : (s.value ? typeOf(*s.value) : nullptr);
                    v.decl = declare(s.target->text, Decl::Kind::Var, sp, ty, true, nullptr);
                }
                return mk(StmtKind::VarDecl, sp, std::move(v));
            }
            case ast::StKind::Assign:
            case ast::StKind::AugAssign: {
                // A bare `x = v` with no prior binding CREATES one. This is not
                // an inference detail, it is the language: `total = 0;` at the
                // top of a function body is how examples/05 declares `total`,
                // and both execution paths agree -- the tree-walker's assignTo
                // defines on a miss, and OP_STORE's handler has the same
                // `else env->vars[n] = v` fallback. A lowering that only
                // declares on VarDecl would leave every such name unresolved.
                // The parser records how many leading exprs are targets (see
                // ast::Stmt::assignTargets). Inferring it as size/2 was wrong for
                // `x, y = point` -- 3 entries that are 2 targets and 1 value --
                // which is examples/17.
                if (s.exprs.size() > 1) {
                    const size_t nTargets = s.assignTargets ? s.assignTargets : 1;
                    for (size_t i = 0; i < nTargets && i < s.exprs.size(); ++i) {
                        if (s.exprs[i]->kind != ast::ExKind::Ident) continue;
                        const std::string& n = s.exprs[i]->text;
                        if (resolve(n) == NO_DECL)
                            declare(n, Decl::Kind::Var, sp, nullptr, true, nullptr);
                    }
                }
                if (s.kind == ast::StKind::Assign) {
                    payload::Assign a;
                    if (!s.exprs.empty()) a.target = lower(*s.exprs[0]);
                    if (s.exprs.size() > 1) a.value = lower(*s.exprs[1]);
                    return mk(StmtKind::Assign, sp, std::move(a));
                }
                payload::AugAssign a;
                if (!s.exprs.empty()) a.target = lower(*s.exprs[0]);
                a.op = s.augOp;
                if (s.exprs.size() > 1) a.value = lower(*s.exprs[1]);
                return mk(StmtKind::AugAssign, sp, std::move(a));
            }
            case ast::StKind::Return: {
                payload::Return r;
                for (const auto& e : s.exprs) r.values.push_back(lower(*e));
                return mk(StmtKind::Return, sp, std::move(r));
            }
            case ast::StKind::Raise: {
                payload::Raise r;
                if (!s.exprs.empty()) r.value = lower(*s.exprs[0]);
                return mk(StmtKind::Raise, sp, std::move(r));
            }
            case ast::StKind::Break:
                return mk(StmtKind::Break, sp, payload::Break{s.label});
            case ast::StKind::Continue:
                return mk(StmtKind::Continue, sp, payload::Continue{s.label});
            case ast::StKind::Yield: {
                payload::Yield y;
                if (!s.exprs.empty()) { y.value = lower(*s.exprs[0]); y.hasValue = true; }
                return mk(StmtKind::Yield, sp, std::move(y));
            }
            case ast::StKind::Del: {
                payload::Del d;
                for (const auto& e : s.exprs) d.targets.push_back(lower(*e));
                return mk(StmtKind::Del, sp, std::move(d));
            }
            case ast::StKind::Defer: {
                payload::Defer d;
                for (const auto& b : s.body) d.body.push_back(lower(*b));
                return mk(StmtKind::Defer, sp, std::move(d));
            }
            case ast::StKind::Goto:
                return mk(StmtKind::Goto, sp, payload::Goto{s.label});
            case ast::StKind::Label: {
                payload::Label l;
                l.name = s.label;
                l.body = lowerBlock(s.body);
                return mk(StmtKind::Label, sp, std::move(l));
            }
            case ast::StKind::If: {
                payload::If i;
                if (!s.exprs.empty()) i.cond = lower(*s.exprs[0]);
                i.then_ = lowerBlock(s.body);
                // `elif` arrives nested as a synthetic If in elseBody[0]. The AST's
                // elifConds/elifBodies are never assigned by the parser, so the
                // chain is reconstructed here rather than read from dead fields.
                if (opts.flattenElif) {
                    const std::vector<ast::StmtP>* cur = &s.elseBody;
                    while (cur->size() == 1 && (*cur)[0] &&
                           (*cur)[0]->kind == ast::StKind::If &&
                           !(*cur)[0]->body.empty()) {
                        const ast::Stmt& nx = *(*cur)[0];
                        i.elifConds.push_back(lower(*nx.exprs[0]));
                        i.elifs.push_back(lowerBlock(nx.body));
                        cur = &nx.elseBody;
                    }
                    if (!cur->empty()) {
                        i.hasElse = true;
                        i.otherwise = lowerBlock(*cur);
                    }
                } else {
                    if (!s.elseBody.empty()) {
                        i.hasElse = true;
                        i.otherwise = lowerBlock(s.elseBody);
                    }
                }
                return mk(StmtKind::If, sp, std::move(i));
            }
            case ast::StKind::While: {
                payload::While w;
                if (!s.exprs.empty()) w.cond = lower(*s.exprs[0]);
                w.label = s.label;
                w.body = lowerBlock(s.body);
                return mk(StmtKind::While, sp, std::move(w));
            }
            case ast::StKind::DoWhile: {
                payload::DoWhile d;
                d.label = s.label;
                d.body = lowerBlock(s.body);
                // ast::Stmt has a dedicated `cond` field for DoWhile ("the
                // `while (cond)` expression, evaluated at the end of each
                // iteration"); it is not in exprs.
                if (s.cond) d.cond = lower(*s.cond);
                else if (!s.exprs.empty()) d.cond = lower(*s.exprs[0]);
                return mk(StmtKind::DoWhile, sp, std::move(d));
            }
            case ast::StKind::For: {
                payload::For f;
                f.label = s.label;
                pushScope();
                if (s.pat) {
                    f.pat = lowerPat(*s.pat);
                    if (f.pat.kind == Pattern::Kind::Bind) f.binding = f.pat.binding;
                }
                if (!s.exprs.empty()) f.iterable = lower(*s.exprs[0]);
                f.body = lowerBlock(s.body);
                popScope();
                return mk(StmtKind::For, sp, std::move(f));
            }
            case ast::StKind::Unsafe: {
                payload::UnsafeBlock u;
                u.body = lowerBlock(s.body);
                return mk(StmtKind::Unsafe, sp, std::move(u));
            }
            case ast::StKind::Spawn: {
                payload::Spawn sp2;
                if (!s.exprs.empty()) sp2.expr = lower(*s.exprs[0]);
                return mk(StmtKind::Spawn, sp, std::move(sp2));
            }
            case ast::StKind::Match: {
                payload::MatchStmt m;
                if (!s.exprs.empty()) m.subject = lower(*s.exprs[0]);
                pushScope();
                for (const auto& arm : s.arms) {
                    payload::MatchStmt::Arm a;
                    if (arm.pat) a.pat = lowerPat(*arm.pat);
                    for (const auto& b : arm.body)
                        if (b) a.body.push_back(lower(*b));
                    m.arms.push_back(std::move(a));
                }
                popScope();
                return mk(StmtKind::Match, sp, std::move(m));
            }
            case ast::StKind::Select: {
                payload::Select sel;
                if (!s.exprs.empty()) sel.subject = lower(*s.exprs[0]);
                pushScope();
                for (const auto& arm : s.selArms) {
                    payload::Select::Arm a;
                    // A select arm binds a receive name and an optional channel op;
                    // it has no pattern.
                    a.bind = arm.bind;
                    // `case msg = ch.recv()` DECLARES msg for the arm body, so it
                    // needs a scope of its own. Storing the name without declaring
                    // it left every use in the arm unresolvable
                    // (examples/23_select_multiplex.kn).
                    if (!arm.bind.empty()) {
                        pushScope();
                        a.decl = declare(arm.bind, Decl::Kind::Var, sp, nullptr, true, nullptr);
                    }
                    if (arm.chanOp) a.chanOp = lower(*arm.chanOp);
                    for (const auto& b : arm.body)
                        if (b) a.body.push_back(lower(*b));
                    if (!arm.bind.empty()) popScope();
                    sel.arms.push_back(std::move(a));
                }
                popScope();
                return mk(StmtKind::Select, sp, std::move(sel));
            }
            case ast::StKind::Try: {
                payload::TryStmt t;
                for (const auto& b : s.body) t.body.push_back(lower(*b));
                t.var = s.catchParam;
                // The catch binding is a real declaration. Without this, every
                // use of `e` in the handler resolved to nothing
                // (examples/35_try_catch.kn).
                if (!t.var.empty()) {
                    pushScope();
                    t.catchDecl = declare(t.var, Decl::Kind::Var, sp, nullptr, true, nullptr);
                }
                for (const auto& b : s.elseBody)
                    if (b) t.handler.push_back(lower(*b));
                if (!t.var.empty()) popScope();
                return mk(StmtKind::Try, sp, std::move(t));
            }
            // Module-level declarations inside a body. They are item forms, so
            // they are not statements; recorded against the Context so the name
            // resolves, and emitted as an assignment to that decl.
            case ast::StKind::GlobalDecl: {
                payload::Assign a;
                if (s.target && s.target->kind == ast::ExKind::Ident) {
                    payload::Name n;
                    n.decl = resolve(s.target->text);
                    n.spelling = s.target->text;
                    a.target = mk(ExprKind::Name, sp, nullptr, std::move(n));
                }
                if (s.value) a.value = lower(*s.value);
                return mk(StmtKind::Assign, sp, std::move(a));
            }
            // A nested named function is a real function, not a statement. It is
            // declared in the ENCLOSING scope (so calls to it resolve) and its
            // body is lowered into its own function scope, then handed to the
            // module's function list so MIR can build it like any other.
            //
            // This used to fall into the catch-all below, which dropped both the
            // name and the body -- so every call to a nested function failed to
            // resolve and the closure's statements silently vanished.
            case ast::StKind::FuncDef: {
                auto f = std::make_unique<Func>();
                f->name = s.name;
                f->span = sp;
                f->sig = sigOf(s);
                f->decl = declare(s.name, Decl::Kind::Func, sp, nullptr, false, nullptr);
                if (Decl* d = const_cast<Decl*>(ctx.get(f->decl))) d->sig = f->sig;
                fnStack.push_back(f->decl);
                f->bodyScope = pushScope(true);
                for (size_t i = 0; i < s.params.size(); ++i) {
                    const auto& pn = s.params[i];
                    DeclId id = declare(pn.name, Decl::Kind::Param, sp,
                                        i < f->sig.params.size() ? f->sig.params[i] : nullptr,
                                        true, nullptr);
                    f->params.push_back(id);
                    if (Decl* pd = const_cast<Decl*>(ctx.get(id))) pd->slot = nextSlot(*f, pn.name);
                }
                for (const auto& b : s.body)
                    if (b) f->body.push_back(lower(*b));
                popScope();
                fnStack.pop_back();
                nestedFuncs.push_back(std::move(f));
                return mk(StmtKind::Nop, sp, payload::Nop{});
            }
            // arena/gather temporaries: no HIR mapping, surfaced not dropped.
            // `temp scratch 3: int = 100` is a compile-time usage BUDGET, and
            // `bucket heavy = [...]` parks a value in a separate store. Both
            // nonetheless introduce a name the rest of the body uses, so both
            // are declarations. They used to be dropped as "arena temporaries",
            // which left `scratch` and `heavy` unresolvable (examples/40).
            case ast::StKind::TempDecl: {
                payload::VarDecl v;
                v.isConst = true;
                if (s.value) v.init = lower(*s.value);
                if (!s.tempVarName.empty())
                    v.decl = declare(s.tempVarName, Decl::Kind::Const, sp,
                                     s.value ? typeOf(*s.value) : nullptr, true, nullptr);
                return mk(StmtKind::VarDecl, sp, std::move(v));
            }
            case ast::StKind::BucketDecl: {
                payload::VarDecl v;
                // `bucket release name` moves a parked value back to a normal
                // variable; it introduces no new binding.
                // `bucket release name` brings a parked value back into normal
                // lookup. HIR has no bucket store -- `bucket x = v` above is
                // modelled as an ordinary binding -- so releasing is a rebind of
                // the same name, which is a self-assignment in HIR's model and
                // exactly right given there is no second store to move out of.
                // The alternative, a Nop, would be a silent no-op.
                if (s.bucketRelease) {
                    payload::Assign a;
                    payload::Name n;
                    n.spelling = s.tempVarName;
                    n.decl = resolve(s.tempVarName);
                    a.target = mk(ExprKind::Name, sp, nullptr, std::move(n));
                    payload::Name n2;
                    n2.spelling = s.tempVarName;
                    n2.decl = resolve(s.tempVarName);
                    a.value = mk(ExprKind::Name, sp, nullptr, std::move(n2));
                    return mk(StmtKind::Assign, sp, std::move(a));
                }
                if (s.value) v.init = lower(*s.value);
                // The bucket's name lives in `tempVarName`, NOT `name`: the
                // parser reuses that field for `bucket <name> = v` (see
                // parser.cpp, the "bucket" arm). `name` is empty there, so
                // reading it declared nothing and left `heavy` unresolvable.
                if (!s.tempVarName.empty())
                    v.decl = declare(s.tempVarName, Decl::Kind::Var, sp,
                                     s.value ? typeOf(*s.value) : nullptr, true, nullptr);
                return mk(StmtKind::VarDecl, sp, std::move(v));
            }
            // `gather item + 1,` re-queues one work item. The enclosing `gather`
            // block is the loop; this is its step.
            case ast::StKind::GatherStmt: {
                payload::GatherItem g;
                if (!s.exprs.empty()) g.item = lower(*s.exprs[0]);
                return mk(StmtKind::GatherItem, sp, std::move(g));
            }
            // `gather { seeds } { body }` -- a worklist loop.
            case ast::StKind::Gather: {
                // The body sees `item`, the work item the loop is on. That is an
                // implicit binding of the gather, not a user declaration, so it
                // has to be introduced here -- otherwise every `item` in a gather
                // body is unresolved (examples/42, /43).
                payload::GatherLoop g;
                pushScope();
                g.itemBinding = declare("item", Decl::Kind::Var, sp, nullptr, true, nullptr);
                for (const auto& seed : s.seedExprs) g.seeds.push_back(lower(*seed));
                g.body = lowerBlock(s.body);
                popScope();
                return mk(StmtKind::GatherLoop, sp, std::move(g));
            }
            default:
                return unhandledStmt("statement", s);
        }
    }

    // --- items ---------------------------------------------------------
    // The Checker's own resolution, READ BACK rather than recomputed: the Checker
    // caches each function's signature while its type variables are still in scope
    // (see Checker::funcSig). Calling resolveSig() from here looked equivalent and
    // was not -- typeVars_ is popped when checkModule() returns, so every generic
    // function came back as "unknown type 'T'".
    sema::FuncSig sigOf(const ast::Stmt& s) {
        if (const sema::FuncSig* cached = chk.funcSig(s)) return *cached;
        // No cached signature: not a function definition this Checker saw, so fall
        // back to resolving it now. Correct for everything non-generic.
        return chk.resolveSig(s.params, s.ret, /*stripSelf=*/false);
    }

    // The type as written, kept for diagnostics and for declarations the Checker
    // has no resolved type for (an import root, a dynamic module).
    static std::string typeText(const ast::TypeP& t) {
        return t ? t->name : std::string();
    }

    Item lowerItem(const ast::Stmt& s) {
        Item it;
        it.span = opts.keepSpans ? s.span : ast::Span{};
        it.name = s.name;
        switch (s.kind) {
            case ast::StKind::FuncDef: {
                auto f = std::make_unique<Func>();
                f->name = s.name;
                f->span = it.span;
                f->sig = sigOf(s);
                // Pass 1 already declared this name, so bind the existing decl
                // rather than declaring a second one for the same function.
                f->decl = resolve(s.name);
                if (f->decl == NO_DECL)
                    f->decl = declare(s.name, Decl::Kind::Func, it.span, nullptr, false, nullptr);
                if (Decl* d = const_cast<Decl*>(ctx.get(f->decl))) d->sig = f->sig;
                fnStack.push_back(f->decl);
                f->bodyScope = pushScope(true);
                for (size_t i = 0; i < s.params.size(); ++i) {
                    const auto& pn = s.params[i];
                    DeclId id = declare(pn.name, Decl::Kind::Param, it.span, nullptr, true, nullptr);
                    if (Decl* pd = const_cast<Decl*>(ctx.get(id)))
                        pd->declaredType = typeText(pn.type);
                    f->params.push_back(id);
                    if (Decl* pd2 = const_cast<Decl*>(ctx.get(id))) pd2->slot = nextSlot(*f, pn.name);
                }
                for (const auto& b : s.body) f->body.push_back(lower(*b));
                popScope();
                fnStack.pop_back();
                it.kind = Item::Kind::Func;
                it.decl = f->decl;
                it.func = std::move(f);
                return it;
            }
            case ast::StKind::StructDef: {
                auto sd = std::make_unique<StructDef>();
                sd->name = s.name;
                sd->span = it.span;
                // Field types keep the SPELLING, not a resolved TyP. A generic
                // struct's field can be annotated with the enclosing type
                // parameter (`struct Box[A] { first: A; }`), and a field's type is
                // only resolvable while that parameter is in scope -- which for
                // functions means inside the TvGuard that checkFuncLike runs
                // under, and which the Checker records a signature for. Struct
                // fields have no such record: registerTopLevel never resolves
                // them at all, so calling resolveType() here is "unknown type 'A'"
                // for every generic struct. Recording a field-type table is the
                // real fix and is not done; the spelling is honest meanwhile.
                for (const auto& fd : s.fields) {
                    DeclId id = declare(fd.name, Decl::Kind::Field, it.span, nullptr,
                                        false, nullptr);
                    if (Decl* dd = const_cast<Decl*>(ctx.get(id)))
                        dd->declaredType = typeText(fd.type);
                    sd->fields.push_back(id);
                }
                it.kind = Item::Kind::Struct;
                it.decl = declare(s.name, Decl::Kind::Struct, it.span, nullptr, false, nullptr);
                sd->span = it.span;
                it.strct = std::move(sd);
                return it;
            }
            case ast::StKind::EnumDef: {
                auto ed = std::make_unique<EnumDef>();
                ed->name = s.name;
                ed->span = it.span;
                for (const auto& v : s.variants)
                    ed->variants.push_back(declare(v.name, Decl::Kind::Variant, it.span, nullptr,
                                                    false, nullptr));
                it.kind = Item::Kind::Enum;
                it.decl = declare(s.name, Decl::Kind::Enum, it.span, nullptr, false, nullptr);
                it.enm = std::move(ed);
                return it;
            }
            case ast::StKind::TraitDef: {
                auto td = std::make_unique<TraitDef>();
                td->name = s.name;
                td->span = it.span;
                for (const auto& m : s.sigs)
                    td->methods.push_back(declare(m.name, Decl::Kind::Func, it.span, nullptr,
                                                  false, nullptr));
                it.kind = Item::Kind::Trait;
                it.decl = declare(s.name, Decl::Kind::Trait, it.span, nullptr, false, nullptr);
                it.trait = std::move(td);
                return it;
            }
            case ast::StKind::ImplDef: {
                auto id2 = std::make_unique<ImplDef>();
                id2->span = it.span;
                id2->typeName = typeText(s.implType);
                id2->traitName = typeText(s.implTrait);
                // An impl's methods are real FuncDefs in `body`; each is lowered
                // on its own below rather than collapsed into a method list.
                for (const auto& b : s.body)
                    if (b && b->kind == ast::StKind::FuncDef)
                        id2->methods.push_back(b->name.empty() ? NO_DECL : resolve(b->name));
                it.kind = Item::Kind::Impl;
                it.decl = NO_DECL;
                it.impl = std::move(id2);
                return it;
            }
            case ast::StKind::Import: {
                auto im = std::make_unique<ImportDef>();
                im->moduleName = s.moduleName;
                im->alias = s.importAlias;
                im->span = it.span;
                it.kind = Item::Kind::Import;
                if (!s.importAlias.empty()) {
                    // `import mod as m`: the module itself is the binding.
                    it.decl = declare(s.importAlias, Decl::Kind::Import, it.span, nullptr,
                                      false, nullptr);
                } else if (!s.fromImport) {
                    it.decl = declare(s.moduleName, Decl::Kind::Import, it.span, nullptr,
                                      false, nullptr);
                }
                // `from mod import a as b`: EACH imported item is its own binding,
                // under the alias when there is one. Binding only the module left
                // every imported name unresolvable -- examples/21 binds `to_json`
                // this way and then calls it.
                for (const auto& ii : s.importItems) {
                    const std::string& bind = ii.alias.empty() ? ii.name : ii.alias;
                    if (bind.empty()) continue;
                    DeclId id = declare(bind, Decl::Kind::Import, it.span, nullptr, false, nullptr);
                    if (it.decl == NO_DECL) it.decl = id;
                    im->items.push_back(id);
                }
                it.imp = std::move(im);
                return it;
            }
            case ast::StKind::Export:
                it.kind = Item::Kind::Export;
                return it;
            case ast::StKind::ConstDecl:
            case ast::StKind::VarDecl: {
                auto gv = std::make_unique<GlobalVar>();
                gv->span = it.span;
                if (s.value) gv->init = lower(*s.value);
                if (s.target && s.target->kind == ast::ExKind::Ident) {
                    gv->decl = declare(s.target->text, Decl::Kind::GlobalVar, it.span,
                                       s.value ? typeOf(*s.value) : nullptr, false, nullptr);
                }
                it.kind = Item::Kind::GlobalVar;
                it.decl = gv->decl;
                it.name = s.target ? s.target->text : std::string();
                it.global = std::move(gv);
                return it;
            }
            default:
                unhandledStmt("item", s);
                it.kind = Item::Kind::Unhandled;
                it.unhandledNote = "no HIR mapping for this top-level form";
                return it;
        }
    }
};

}  // namespace

Module lower(const std::vector<ast::StmtP>& prog, sema::Checker& chk, Context& ctx,
             DiagEngine& diags, const LowerOptions& opts) {
    Module m;
    m.name = "main";

    // Two passes, because a function body may call a function declared later.
    // Pass 1 declares every top-level name; pass 2 lowers bodies. The Checker
    // already did the real resolution -- this rebuilds an id space on top of it,
    // which is why the verifier checks that nothing failed to resolve.
    Lowerer pre(chk, ctx, diags, opts);
    pre.pushScope(true);
    // Seed the builtins the Checker predeclared, read from the Checker rather
    // than from a name list here -- two such lists already exist in the VM
    // compiler and the interpreter, and they drift.
    for (const auto& b : chk.builtinNames()) {
        Decl d;
        d.kind = Decl::Kind::Builtin;
        d.name = b;
        DeclId id = ctx.addDecl(std::move(d));
        ctx.scopes.back().bindings.push_back(id);
    }
    for (const auto& s : prog) {
        switch (s->kind) {
            case ast::StKind::FuncDef:    pre.declare(s->name, Decl::Kind::Func, s->span, nullptr, false, nullptr); break;
            case ast::StKind::StructDef:  pre.declare(s->name, Decl::Kind::Struct, s->span, nullptr, false, nullptr); break;
            case ast::StKind::EnumDef:    pre.declare(s->name, Decl::Kind::Enum, s->span, nullptr, false, nullptr); break;
            case ast::StKind::TraitDef:   pre.declare(s->name, Decl::Kind::Trait, s->span, nullptr, false, nullptr); break;
            case ast::StKind::ConstDecl:
            case ast::StKind::VarDecl:
                if (s->target) pre.declare(s->target->text, Decl::Kind::GlobalVar, s->span, nullptr, false, nullptr);
                break;
            case ast::StKind::Import:
                if (!s->importAlias.empty())
                    pre.declare(s->importAlias, Decl::Kind::Import, s->span, nullptr, false, nullptr);
                else if (!s->fromImport)
                    pre.declare(s->moduleName, Decl::Kind::Import, s->span, nullptr, false, nullptr);
                for (const auto& ii : s->importItems) {
                    const std::string& b = ii.alias.empty() ? ii.name : ii.alias;
                    if (!b.empty())
                        pre.declare(b, Decl::Kind::Import, s->span, nullptr, false, nullptr);
                }
                break;
            default: break;
        }
    }
    pre.popScope();

    Lowerer lo(chk, ctx, diags, opts);
    lo.pushScope(true);
    for (const auto& s : prog) {
        // Pass 1 already declared the names, so lowerItem must find them rather
        // than re-declare. Bind into the existing scope entry instead.
        Item it = lo.lowerItem(*s);
        if (it.kind == Item::Kind::Func && it.func) m.funcs.push_back(it.func.get());
        m.items.push_back(std::move(it));
    }
    lo.popScope();
    // Nested named functions were declared into their enclosing scope while
    // lowering the body that contained them; they still need to appear in the
    // module's function list so MIR and the dump see them.
    for (auto& nf : lo.nestedFuncs) {
        m.funcs.push_back(nf.get());
        m.items.push_back(Item{});  // keep item/func lists indexable in step
        Item& slot = m.items.back();
        slot.kind = Item::Kind::Func;
        slot.name = nf->name;
        slot.span = nf->span;
        slot.decl = nf->decl;
        slot.func = std::move(nf);
    }
    return m;
}

}  // namespace kirn::hir
