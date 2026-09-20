#include "sema/borrow.h"
#include <set>
#include <string>

namespace kirn {
namespace sema {

// ---- helpers ---------------------------------------------------------------

static void collectPatLocals(const ast::Pat& p,
                             std::set<std::string>& locals) {
    switch (p.kind) {
        case ast::PatKind::Bind:
            if (!p.bindName.empty()) locals.insert(p.bindName);
            break;
        case ast::PatKind::BindAlias:
            if (!p.bindName.empty()) locals.insert(p.bindName);
            if (p.aliasSub) collectPatLocals(*p.aliasSub, locals);
            break;
        default:
            break;
    }
    for (const auto& f : p.fields)
        if (f.pat) collectPatLocals(*f.pat, locals);
    for (const auto& e : p.elems)
        collectPatLocals(*e, locals);
    for (const auto& a : p.alts)
        collectPatLocals(*a, locals);
    if (p.inner) collectPatLocals(*p.inner, locals);
}

static void collectStmtLocals(const ast::Stmt& s,
                              std::set<std::string>& locals) {
    switch (s.kind) {
        case ast::StKind::VarDecl:
            if (s.target && s.target->kind == ast::ExKind::Ident)
                locals.insert(s.target->text);
            break;
        case ast::StKind::Assign:
        case ast::StKind::AugAssign:
            for (const auto& e : s.exprs)
                if (e && e->kind == ast::ExKind::Ident) locals.insert(e->text);
            break;
        case ast::StKind::Match:
            for (const auto& arm : s.arms)
                if (arm.pat) collectPatLocals(*arm.pat, locals);
            break;
        case ast::StKind::For:
            if (s.pat) collectPatLocals(*s.pat, locals);
            break;
        case ast::StKind::FuncDef:
            for (const auto& p : s.params)
                if (!p.name.empty() && !p.selfParam) locals.insert(p.name);
            break;
        default:
            break;
    }
    for (const auto& b : s.body) collectStmtLocals(*b, locals);
    for (const auto& b : s.elseBody) collectStmtLocals(*b, locals);
    for (const auto& b : s.elifBodies) collectStmtLocals(*b, locals);
}

// ---- escaping-borrow detection ---------------------------------------------

static void flagBorrow(DiagEngine& diags, const ast::Span& span,
                        const std::string& name) {
    diags.error(SpanRange::point(span.line, span.col))
        .code("E-BORROW-ESCAPE")
        .msg("escape: '&" + name +
             "' must not be returned or stored — it borrows a local "
             "whose lifetime ends at scope exit")
        .emit();
}

static void checkExprEscapes(DiagEngine& diags, const ast::Expr& e,
                             const std::set<std::string>& locals,
                             bool escapable) {
    if (e.kind == ast::ExKind::Unary && e.op == "&" && e.rhs &&
        e.rhs->kind == ast::ExKind::Ident && escapable &&
        locals.count(e.rhs->text))
        flagBorrow(diags, e.span, e.rhs->text);
    if (e.lhs) checkExprEscapes(diags, *e.lhs, locals, false);
    if (e.rhs) checkExprEscapes(diags, *e.rhs, locals, false);
    if (e.cond) checkExprEscapes(diags, *e.cond, locals, false);
    for (const auto& a : e.args)
        if (a.value) checkExprEscapes(diags, *a.value, locals, true);
    for (const auto& f : e.pairs)
        if (f.first) checkExprEscapes(diags, *f.first, locals, false);
    for (const auto& f : e.pairs)
        if (f.second) checkExprEscapes(diags, *f.second, locals, true);
    for (const auto& el : e.elems)
        if (el) checkExprEscapes(diags, *el, locals, true);
    for (const auto& cl : e.clauses) {
        if (cl.iter) checkExprEscapes(diags, *cl.iter, locals, false);
        if (cl.cond) checkExprEscapes(diags, *cl.cond, locals, false);
    }
    for (const auto& ep : e.parts)
        if (ep.expr) checkExprEscapes(diags, *ep.expr, locals, false);
}

static void checkStmtEscapes(DiagEngine& diags, const ast::Stmt& s,
                              const std::set<std::string>& locals) {
    switch (s.kind) {
        case ast::StKind::Return:
            for (const auto& e : s.exprs)
                checkExprEscapes(diags, *e, locals, true);
            break;
        case ast::StKind::VarDecl:
            if (s.value) checkExprEscapes(diags, *s.value, locals, true);
            if (s.target) checkExprEscapes(diags, *s.target, locals, false);
            break;
        case ast::StKind::Assign: {
            // s.exprs = [targets..., values...] when paired (n>=2, even).
            // A borrow stored into a plain local variable (single Ident target)
            // stays inside the function and is NOT an escape; a borrow stored
            // into anything else (member, index, tuple element) escapes.
            size_t n = s.exprs.size();
            bool paired = n >= 2 && n % 2 == 0;
            size_t half = paired ? n / 2 : 0;
            for (size_t i = 0; i < n; ++i) {
                if (!s.exprs[i]) continue;
                if (!paired) {
                    checkExprEscapes(diags, *s.exprs[i], locals, true);
                    continue;
                }
                bool valueSide = i >= half;
                bool escapable;
                if (valueSide) {
                    size_t tgt = i - half;
                    const ast::Expr* target =
                        (tgt < half && s.exprs[tgt]) ? s.exprs[tgt].get() : nullptr;
                    escapable = !(target && target->kind == ast::ExKind::Ident);
                } else {
                    escapable = false;
                }
                checkExprEscapes(diags, *s.exprs[i], locals, escapable);
            }
            break;
        }
        case ast::StKind::AugAssign:
            for (size_t i = 1; i < s.exprs.size(); ++i)
                if (s.exprs[i])
                    checkExprEscapes(diags, *s.exprs[i], locals, true);
            break;
        case ast::StKind::ExprStmt:
            for (const auto& e : s.exprs)
                checkExprEscapes(diags, *e, locals, true);
            break;
        case ast::StKind::Match:
            for (const auto& arm : s.arms) {
                if (arm.guard) checkExprEscapes(diags, *arm.guard, locals, false);
                for (const auto& bs : arm.body) checkStmtEscapes(diags, *bs, locals);
            }
            break;
        case ast::StKind::If:
            if (s.cond) checkExprEscapes(diags, *s.cond, locals, false);
            for (const auto& b : s.body) checkStmtEscapes(diags, *b, locals);
            for (const auto& e : s.elifConds)
                checkExprEscapes(diags, *e, locals, false);
            for (const auto& b : s.elifBodies) checkStmtEscapes(diags, *b, locals);
            for (const auto& b : s.elseBody) checkStmtEscapes(diags, *b, locals);
            break;
        case ast::StKind::While:
        case ast::StKind::DoWhile:
            if (s.cond) checkExprEscapes(diags, *s.cond, locals, false);
            for (const auto& b : s.body) checkStmtEscapes(diags, *b, locals);
            for (const auto& b : s.elseBody) checkStmtEscapes(diags, *b, locals);
            break;
        case ast::StKind::For:
            if (!s.exprs.empty())
                checkExprEscapes(diags, *s.exprs[0], locals, false);
            for (const auto& b : s.body) checkStmtEscapes(diags, *b, locals);
            break;
        case ast::StKind::Gather:
            for (const auto& e : s.seedExprs)
                checkExprEscapes(diags, *e, locals, false);
            for (const auto& b : s.body) checkStmtEscapes(diags, *b, locals);
            break;
        case ast::StKind::Try:
            for (const auto& b : s.body) checkStmtEscapes(diags, *b, locals);
            for (const auto& b : s.elseBody) checkStmtEscapes(diags, *b, locals);
            break;
        case ast::StKind::Select:
            for (const auto& arm : s.selArms) {
                if (arm.chanOp) checkExprEscapes(diags, *arm.chanOp, locals, false);
                for (const auto& b : arm.body) checkStmtEscapes(diags, *b, locals);
            }
            break;
        default:
            break;
    }
}

// ---- per-function entry ----------------------------------------------------

static void checkFunc(DiagEngine& diags, const ast::Stmt& func) {
    std::set<std::string> locals;
    for (const auto& p : func.params)
        if (!p.name.empty() && !p.selfParam) locals.insert(p.name);
    for (const auto& s : func.body) collectStmtLocals(*s, locals);
    for (const auto& s : func.body) checkStmtEscapes(diags, *s, locals);
}

// ---- module entry ----------------------------------------------------------

void BorrowChecker::checkModule(const std::vector<ast::StmtP>& prog) {
    for (const auto& s : prog) {
        if (s->kind == ast::StKind::FuncDef && !s->externDef)
            checkFunc(diags_, *s);
        else if (s->kind == ast::StKind::StructDef ||
                 s->kind == ast::StKind::TraitDef ||
                 s->kind == ast::StKind::ImplDef) {
            for (const auto& m : s->body)
                if (m->kind == ast::StKind::FuncDef) checkFunc(diags_, *m);
        }
    }
}

} // namespace sema
} // namespace kirn
