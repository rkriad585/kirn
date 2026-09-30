// A direct test of kirn::hir::verify, compiled and run by gates_linux.sh.
//
// Why this exists rather than a .kn fixture: no valid source can produce a
// verifier violation. sema::Checker rejects every source-level defect before the
// lowering runs, and HIR now maps the whole language, so there is no program that
// type-checks and lowers to broken HIR. A fixture-based rejection test was
// written first and was worse than useless: it passed by finding a construct HIR
// had not mapped yet, and the moment HIR learned that construct the test went
// quiet -- it would have stopped testing anything without failing.
//
// So the malformed IR is built here, in memory, and handed straight to the
// verifier. Each case below is a violation the verifier is supposed to catch;
// the test fails if any of them is accepted.
//
// This lives in the test tree and is not linked into any shipped binary, so a
// test-only backdoor never reaches the product.

#include <cstdio>
#include <string>
#include <vector>

#include "hir/hir.h"

using namespace kirn;
using namespace kirn::hir;

namespace {

int failures = 0;

void expectRejected(const char* what, Module& m, Context& ctx) {
    auto v = verify(m, ctx);
    if (v.empty()) {
        std::printf("  FAIL  %s: verifier ACCEPTED a broken module\n", what);
        ++failures;
    } else {
        std::printf("  ok    %s: %s\n", what, v.front().message.c_str());
    }
}

void expectAccepted(const char* what, Module& m, Context& ctx) {
    auto v = verify(m, ctx);
    if (!v.empty()) {
        std::printf("  FAIL  %s: verifier rejected a well-formed module (%s)\n",
                    what, v.front().message.c_str());
        ++failures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

// A module with one function whose body is a single return of one name.
Module baseModule(Context& ctx) {
    Module m;
    m.name = "selftest";
    Item it;
    it.kind = Item::Kind::Func;
    it.name = "f";
    it.span = ast::Span{1, 1, 1, 2};
    auto f = std::make_unique<Func>();
    f->name = "f";
    f->span = it.span;
    f->bodyScope = ctx.addScope(NO_SCOPE, true);
    Decl d;
    d.kind = Decl::Kind::Func;
    d.name = "f";
    d.span = it.span;
    f->decl = ctx.addDecl(std::move(d));
    f->body.push_back(std::make_unique<Stmt>());
    (*f->body[0]).kind = StmtKind::Return;
    (*f->body[0]).span = it.span;
    (*f->body[0]).p = payload::Return{};
    m.funcs.push_back(f.get());
    it.func = std::move(f);
    m.items.push_back(std::move(it));
    return m;
}

}  // namespace

int main() {
    // 1. A well-formed module must be ACCEPTED. Without this, a verifier that
    //    rejects everything would pass every other case here.
    {
        Context ctx;
        Module m = baseModule(ctx);
        expectAccepted("well-formed module is accepted", m, ctx);
    }

    // 2. A DROPPED statement must be rejected. This is the check that turns "no
    //    HIR mapping for X" into a hard error instead of a silent no-op, and it
    //    is the one that found three real drops after HIR learned the language.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Nop;
        s->span = ast::Span{2, 1, 2, 2};
        s->p = payload::Nop{true, "selftest construct"};
        f->body.insert(f->body.begin() + 1, std::move(s));
        expectRejected("dropped statement", m, ctx);
    }

    // 3. A genuine `pass` is NOT a drop and must still be accepted -- otherwise
    //    (2) is passing for the wrong reason.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Nop;
        s->span = ast::Span{2, 1, 2, 2};
        s->p = payload::Nop{false, {}};
        f->body.insert(f->body.begin() + 1, std::move(s));
        expectAccepted("a real pass is not a drop", m, ctx);
    }

    // 4. A name that resolves to nothing must be rejected, unless it is an
    //    explicit error node.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Name;
        e->span = ast::Span{3, 1, 3, 2};
        e->p = payload::Name{NO_DECL, "nowhere"};
        auto r = std::make_unique<Stmt>();
        r->kind = StmtKind::ExprStmt;
        r->span = e->span;
        payload::ExprStmt es;
        es.value = std::move(e);
        r->p = std::move(es);
        f->body.insert(f->body.begin() + 1, std::move(r));
        expectRejected("unresolved name", m, ctx);
    }

    // 5. A payload that disagrees with its kind tag must be rejected. This is
    //    the fat-node check in its new form, and the one the hand-written
    //    variant-index table got wrong nine times.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Name;          // says Name...
        e->span = ast::Span{3, 1, 3, 2};
        e->p = payload::Lit{1, 0, "", ast::StrFlavor::Normal, false, false, false};
                                                     // ...carries a Lit
        auto r = std::make_unique<Stmt>();
        r->kind = StmtKind::ExprStmt;
        r->span = e->span;
        payload::ExprStmt es;
        es.value = std::move(e);
        r->p = std::move(es);
        f->body.insert(f->body.begin() + 1, std::move(r));
        expectRejected("payload disagrees with its kind", m, ctx);
    }

    // 6. A node with no payload that is not an error node must be rejected: that
    //    is a node that verifies and means nothing.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Int;           // not an error node...
        e->span = ast::Span{3, 1, 3, 2};
        e->p = std::monostate{};           // ...but carries nothing
        auto r = std::make_unique<Stmt>();
        r->kind = StmtKind::ExprStmt;
        r->span = e->span;
        payload::ExprStmt es;
        es.value = std::move(e);
        r->p = std::move(es);
        f->body.insert(f->body.begin() + 1, std::move(r));
        expectRejected("empty payload on a real node", m, ctx);
    }

    // 7. An out-of-range decl id must be rejected rather than dereferenced.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Name;
        e->span = ast::Span{3, 1, 3, 2};
        e->p = payload::Name{9999, "bogus"};
        auto r = std::make_unique<Stmt>();
        r->kind = StmtKind::ExprStmt;
        r->span = e->span;
        payload::ExprStmt es;
        es.value = std::move(e);
        r->p = std::move(es);
        f->body.insert(f->body.begin() + 1, std::move(r));
        expectRejected("out-of-range decl id", m, ctx);
    }

    // 8. A block with no scope must be rejected. Scopes are explicit nodes in
    //    HIR, so an unnamed frame is a frame nobody can reason about.
    {
        Context ctx;
        Module m = baseModule(ctx);
        Func* f = m.funcs[0];
        auto s = std::make_unique<Stmt>();
        s->kind = StmtKind::Block;
        s->span = ast::Span{2, 1, 2, 2};
        s->p = payload::Block{};          // scope == NO_SCOPE
        f->body.insert(f->body.begin() + 1, std::move(s));
        expectRejected("block with no scope", m, ctx);
    }

    if (failures) {
        std::printf("%d verifier self-test case(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all verifier self-test cases passed\n");
    return 0;
}
