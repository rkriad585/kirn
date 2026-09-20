#pragma once
// Recursive-descent parser for Kirn (grammar/coco.ebnf §3).
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

    // Desugaring (class/record/interface can expand to extra top-level nodes,
    // e.g. `class X implements I {}` -> struct X + impl I for X). Drained by
    // the statement collectors after each parseTopOrStmt().
    std::vector<ast::StmtP> extraTopStmts_;
};

} // namespace kirn
