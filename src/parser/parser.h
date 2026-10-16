#pragma once
// Recursive-descent parser for Kirn (grammar/kirn.ebnf §3).
#include "ast/ast.h"
#include "support/diag.h"
#include "lex/token.h"

#include <stdexcept>
#include <vector>

namespace kirn {

class Parser {
public:
    Parser(const std::vector<Token>& toks, DiagEngine& diags);

    // Parses a whole translation unit. On statement-level errors, reports a
    // diagnostic, synchronizes to the next Newline/Dedent and keeps going.
    std::vector<ast::StmtP> parseProgram();

private:
    struct Abort : std::runtime_error {
        explicit Abort(const char* w) : std::runtime_error(w) {}
    };

    // ---- token utilities ----
    const Token& cur(size_t off = 0) const;
    bool at(Tok k, size_t off = 0) const;
    bool atOp(std::string_view op, size_t off = 0) const;
    bool atPunct(std::string_view p, size_t off = 0) const;
    bool atIdent(std::string_view word, size_t off = 0) const;
    Token advance();
    Token expect(Tok k, const char* what);
    Token expectOp(std::string_view op);
    Token expectPunct(std::string_view p);
    void skipNewlines();
    void syncToStatementEnd();        // after an error: eat until Newline/Dedent
    bool looksLikeTypedBinding() const;  // `name : Type = value` vs `name : stmt`
    bool looksLikeWithStmt() const;      // contextual `with … { }` vs expr stmt

    ast::Span spanHere() const;
    ast::Span spanOf(const Token& t) const;

    // ---- declarations & statements ----
    ast::StmtP parseTopOrStmt();
    ast::StmtP parseFuncDef(bool pub, bool allowBody = true);
    ast::StmtP parseStructDef();
    ast::StmtP parseClassDef(bool pub);
    ast::StmtP parseInterfaceDef(bool pub);
    ast::StmtP parseRecordDef(bool pub);
    ast::StmtP parseEnumDef();
    ast::StmtP parseTraitDef();
    ast::StmtP parseImplDef();
    ast::StmtP parseImport(bool from, bool exportKw);
    ast::StmtP parseSimple();                       // parseSimpleStmt + ';'
    ast::StmtP parseSimpleStmt();
    ast::StmtP parseCompound();

    std::vector<ast::Param> parseParamList();     // inside ()
    std::vector<ast::TypeParam> parseTypeParams();
    std::vector<ast::StmtP> parseBlock();          // '{' .. '}'
    void endBlock();                               // consume the block's '}'
    ast::StmtP finishConstDecl();
    ast::StmtP parseIfChain();                     // if / else if / elif / else
    ast::StmtP parseWithStmt();                    // with e [as n], … { }
    std::vector<ast::Variant> parseVariants();

    // ---- patterns ----
    ast::PatP parsePattern();
    ast::PatP parseCtorOrBind();

    // ---- types ----
    ast::TypeP parseType();
    ast::TypeP parseBaseType();

    // ---- expressions (grammar §2 precedence ladder) ----
    ast::ExprP parseExpr();                       // lambda | cond | comp | or
    ast::ExprP parseCondExpr();
    ast::ExprP parseCompExpr(ast::ExprP first);   // generator / listcomp body
    ast::ExprP parseTernary();                    // cond ? a : b   (C-style)
    ast::ExprP parsePipe();                       // a |> body   (M1, loosest)
    ast::ExprP parseWalrus();                     // (name := expr) (M1)
    ast::ExprP parseElvis();                      // a ?? b      (M1, right-assoc)
    ast::ExprP parseOr();
    ast::ExprP parseAnd();
    ast::ExprP parseNot();
    ast::ExprP parseComparison();
    ast::ExprP parseRange();
    ast::ExprP parseBitOr();
    ast::ExprP parseBitXor();
    ast::ExprP parseBitAnd();
    ast::ExprP parseShift();
    ast::ExprP parseArith();
    // M12: W-nosugar nudge for `x++`/`x--` (rejected; use `+= 1`/`-= 1`).
    // Called at a binary `+`/`-` position; emits a warning only when the two
    // operator tokens are adjacent and the next token terminates the
    // expression, which can only be an attempted increment/decrement.
    void nudgeIncDec();
    // ++x / x++ / --x / x-- desugar to `x := x +/- 1` (AssignExpr), yielding the
    // updated value. `op` is "++" or "--"; `postfix` records the spelling, and
    // `opSpan` locates the operator for the W-nosugar nudge (`prefer x += 1`).
    ast::ExprP makeIncrDecr(ast::ExprP target, const std::string& op, bool postfix,
                            ast::Span opSpan);
    // True when the `?` at the cursor opens a C-style ternary `c ? a : b`
    // (a depth-0 ':' follows before the expression terminates) rather than the
    // postfix `?` propagation operator.
    bool ternaryQuestion() const;
    ast::ExprP parseTerm();
    ast::ExprP parseUnary();
    ast::ExprP parsePower();
    ast::ExprP parsePostfix();
    ast::ExprP parsePrimary();
    bool tryParseLambda(ast::ExprP& out);
    ast::ExprP parseFnClosure();      // block-bodied `fn (params) [-> T] { … }`
    ast::CallArg parseCallArg();
    ast::ExprP parseIndexBody(ast::ExprP obj);    // indexing vs slicing

    DiagEngine& diags_;
    const std::vector<Token>& toks_;
    size_t idx_ = 0;

    // M1 pipe/topic state. One entry per pipe body currently being parsed:
    // `topicUse_.back()` counts the `%` topic tokens seen in the innermost
    // body, so a body with none can be rewritten as an explicit call with an
    // implicit topic argument (unary sugar `x |> f` == `x |> f(%)`). `%` is
    // only a legal primary while this stack is non-empty (else E-PIPE-TOPIC).
    std::vector<unsigned> topicUse_;

    // Desugaring (class/record/interface can expand to extra top-level nodes,
    // e.g. `class X implements I {}` -> struct X + impl I for X). Drained by
    // the statement collectors after each parseTopOrStmt().
    std::vector<ast::StmtP> extraTopStmts_;
};

} // namespace kirn
