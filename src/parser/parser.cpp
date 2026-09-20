#include "parser/parser.h"

namespace kirn {

Parser::Parser(const std::vector<Token>& toks, DiagEngine& diags)
    : diags_(diags), toks_(toks) {}

namespace {

bool isAugOp(std::string_view s) {
    return s == "+=" || s == "-=" || s == "*=" || s == "/=" || s == "%=" ||
           s == "**=" || s == "//=" || s == "&=" || s == "|=" || s == "^=" ||
           s == "<<=" || s == ">>=";
}
bool isCmpTok(std::string_view s) {
    return s == "<" || s == ">" || s == "<=" || s == ">=" || s == "==" ||
           s == "!=" || s == "is" || s == "in";
}

// ---- helpers for class/interface/record desugaring ------------------------

// Deep-clone a type AST (Type has unique_ptr members; no default copy).
ast::TypeP cloneType(const ast::Type* t) {
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

// Deep-clone a parameter (Param has TypeP/ExprP members, so no default copy).
ast::Param cloneParam(const ast::Param& p) {
    ast::Param c;
    c.name = p.name;
    c.mutable_ = p.mutable_;
    c.variadic = p.variadic;
    c.selfParam = p.selfParam;
    c.span = p.span;
    c.type = p.type ? cloneType(p.type.get()) : nullptr;
    // defaultValue is only used for call defaults on user funcs; the impl
    // delegate carries no defaults, so dropping it is safe.
    c.defaultValue = nullptr;
    return c;
}

// Build `ExprStmt { self.<name>(p1, p2, ...) }` used as the body of a generated
// `impl Interface for Class` method that delegates to the inherent method.
ast::StmtP makeDelegateBody(const std::string& name,
                            const std::vector<ast::Param>& params) {
    auto call = std::make_unique<ast::Expr>();
    call->kind = ast::ExKind::Call;
    auto member = std::make_unique<ast::Expr>();
    member->kind = ast::ExKind::Member;
    auto recv = std::make_unique<ast::Expr>();
    recv->kind = ast::ExKind::Ident;
    recv->text = "self";
    member->lhs = std::move(recv);
    member->text = name;
    call->lhs = std::move(member);
    for (const auto& p : params) {
        if (p.selfParam || p.name == "self") continue;
        auto argExpr = std::make_unique<ast::Expr>();
        argExpr->kind = ast::ExKind::Ident;
        argExpr->text = p.name;
        ast::CallArg arg;
        arg.value = std::move(argExpr);
        call->args.push_back(std::move(arg));
    }
    auto st = std::make_unique<ast::Stmt>();
    st->kind = ast::StKind::ExprStmt;
    st->exprs.push_back(std::move(call));
    return st;
}

} // namespace

// ---------------------------------------------------------------------------
// token utilities
// ---------------------------------------------------------------------------

const Token& Parser::cur(size_t off) const {
    size_t i = idx_ + off;
    if (i >= toks_.size()) i = toks_.size() - 1;      // Eof sentinel
    return toks_[i];
}
bool Parser::at(Tok k, size_t off) const { return cur(off).kind == k; }
bool Parser::atOp(std::string_view op, size_t off) const {
    return cur(off).kind == Tok::Op && cur(off).text == op;
}
bool Parser::atPunct(std::string_view p, size_t off) const {
    return cur(off).kind == Tok::Punct && cur(off).text == p;
}
bool Parser::atIdent(std::string_view word, size_t off) const {
    return cur(off).kind == Tok::Ident && cur(off).text == word;
}
Token Parser::advance() {
    const Token& t = cur();
    if (idx_ + 1 < toks_.size()) ++idx_;
    return t;
}

ast::Span Parser::spanHere() const { return spanOf(cur()); }

ast::Span Parser::spanOf(const Token& t) const {
    ast::Span s{t.line, t.col};
    if (t.endLine || t.endCol) {
        s.endLine = t.endLine;
        s.endCol = t.endCol;
    }
    return s;
}

void Parser::skipNewlines() { while (at(Tok::Newline)) advance(); }

void Parser::syncToStatementEnd() {
    // C-style recovery: skip to the ';' that ends the broken statement,
    // or bail out of the current block at its closing '}'.
    int depth = 0;
    while (!at(Tok::Eof)) {
        if (cur().kind == Tok::Punct) {
            const std::string& x = cur().text;
            if (x == "{" || x == "(" || x == "[") {
                ++depth;
            } else if (x == "}" ) {
                if (depth == 0) { advance(); return; }
                --depth;
            } else if (x == ")" || x == "]") {
                if (depth > 0) --depth;
            } else if (x == ";" && depth == 0) {
                advance();
                return;
            }
        }
        advance();
    }
}

Token Parser::expect(Tok k, const char* what) {
    if (!at(k)) {
        std::string found =
            cur().kind == Tok::Newline ? "newline" : "'" + cur().text + "'";
        diags_.report(cur().line, cur().col,
                      std::string("expected ") + what + ", found " + found);
        throw Abort("expect");
    }
    return advance();
}
Token Parser::expectOp(std::string_view op) {
    if (!atOp(op)) {
        diags_.report(cur().line, cur().col,
                      std::string("expected '") + std::string(op) + "', found '" +
                          cur().text + "'");
        throw Abort("expectop");
    }
    return advance();
}
Token Parser::expectPunct(std::string_view p) {
    if (!atPunct(p)) {
        diags_.report(cur().line, cur().col,
                      std::string("expected '") + std::string(p) + "', found '" +
                          cur().text + "'");
        throw Abort("expectpunct");
    }
    return advance();
}

// ---------------------------------------------------------------------------
// program / dispatch
// ---------------------------------------------------------------------------

std::vector<ast::StmtP> Parser::parseProgram() {
    std::vector<ast::StmtP> out;
    skipNewlines();
    while (!at(Tok::Eof)) {
        if (atPunct("}")) {                             // stray closer: drop
            advance();
            continue;
        }
        try {
            auto s = parseTopOrStmt();
            if (s) out.push_back(std::move(s));
            for (auto& ex : extraTopStmts_) out.push_back(std::move(ex));
            extraTopStmts_.clear();
        } catch (Abort&) {
            extraTopStmts_.clear();
            syncToStatementEnd();
        }
        skipNewlines();
    }
    return out;
}

// Disambiguate `name : Type = value` (annotated immutable binding) from
// `name : <stmt>` (jump label). At bracket depth 0, scanning from the token
// after the colon, we look for a top-level `=` reached via type-ish tokens
// before any statement terminator (`;`, `{`, `}`, or an out-of-block token).
bool Parser::looksLikeTypedBinding() const {
    size_t depth = 0;
    for (size_t off = 2;; ++off) {
        const Token& t = cur(off);
        if (t.kind == Tok::Eof || t.kind == Tok::Newline ||
            t.kind == Tok::Dedent)
            return false;                      // no `=` reached → a label
        if (t.kind == Tok::Punct) {
            if (t.text == ";" || t.text == "{" || t.text == "}")
                return false;                  // statement/block boundary
            if (t.text == "[") { ++depth; continue; }
            if (t.text == "]") { if (depth) --depth; continue; }
            continue;
        }
        if (t.kind == Tok::Op && depth == 0 && t.text == "=") return true;
        if (t.kind == Tok::Op && depth == 0)
            return false;                      // e.g. `name : x += 1` → statement
    }
}

ast::StmtP Parser::parseTopOrStmt() {
    // labeled loop:  label: while ...  |  label: for ...
    if (cur().kind == Tok::Ident && atPunct(":", 1) &&
        (atIdent("while", 2) || atIdent("for", 2))) {
        std::string lbl = advance().text;
        advance();                                      // ':'
        auto s = parseCompound();
        s->label = lbl;
        return s;
    }
    // jump label:  label: <stmt>   (goto target; a label is a marker whose
    // body is the labeled statement)
    //
    // Disambiguation: `name : Type = value` is an annotated immutable binding
    // (see parseSimpleStmt). A jump label is `name : <stmt>`. Both start with
    // an identifier and a colon. Rule: if after the colon we find a type-ish
    // sequence that is terminated by `=` at the top level (before any statement
    // terminator), it is a typed declaration, not a label.
    if (cur().kind == Tok::Ident && atPunct(":", 1) &&
        !looksLikeTypedBinding()) {
        auto s = std::make_unique<ast::Stmt>();
        s->kind = ast::StKind::Label;
        s->label = advance().text;
        s->span = spanHere();
        advance();                                      // ':'
        s->body.push_back(parseTopOrStmt());
        return s;
    }
    if (atIdent("def")) return parseFuncDef(false);
    if (atIdent("fn")) return parseFuncDef(false);          // `fn` == `def`

    if (atIdent("pr")) {
        advance();
        if (atIdent("def") || atIdent("fn")) {
            auto s = parseFuncDef(false);
            s->privateDecl = true;
            return s;
        }
        if (atIdent("struct")) { auto s = parseStructDef(); s->privateDecl = true; return s; }
        if (atIdent("class"))  { auto s = parseClassDef(false); s->privateDecl = true; return s; }
        if (atIdent("interface")) { auto s = parseInterfaceDef(false); s->privateDecl = true; return s; }
        if (atIdent("record")) { auto s = parseRecordDef(false); s->privateDecl = true; return s; }
        if (atIdent("enum"))   { auto s = parseEnumDef(); s->privateDecl = true; return s; }
        if (atIdent("trait"))  { auto s = parseTraitDef(); s->privateDecl = true; return s; }
        if (atIdent("const"))  { advance(); auto s = finishConstDecl(); s->privateDecl = true; return s; }
        diags_.report(cur().line, cur().col,
                      "'pr' must precede def/fn/struct/class/interface/record/enum/trait/const");
        throw Abort("pr");
    }
    if (atIdent("pub")) {
        advance();
        if (atIdent("def")) return parseFuncDef(true);
        if (atIdent("fn")) return parseFuncDef(true);       // `pub fn` == `pub def`
        if (atIdent("struct")) { auto s = parseStructDef(); s->pub = true; return s; }
        if (atIdent("class"))  return parseClassDef(true);
        if (atIdent("interface")) return parseInterfaceDef(true);
        if (atIdent("record")) return parseRecordDef(true);
        if (atIdent("enum"))   { auto s = parseEnumDef();   s->pub = true; return s; }
        if (atIdent("trait"))  { auto s = parseTraitDef();  s->pub = true; return s; }
        if (atIdent("const"))  { advance(); auto s = finishConstDecl(); s->pub = true; return s; }
        diags_.report(cur().line, cur().col,
                      "'pub' must precede def/fn/struct/class/interface/record/enum/trait/const");
        throw Abort("pub");
    }
    if (atIdent("extern") && atIdent("def", 1)) {
        advance();
        auto s = parseFuncDef(false, false);
        s->externDef = true;
        expectPunct(";");                               // extern decls end with ';'
        return s;
    }
    if (atIdent("struct")) return parseStructDef();
    if (atIdent("class")) return parseClassDef(false);
    if (atIdent("interface")) return parseInterfaceDef(false);
    if (atIdent("record")) return parseRecordDef(false);
    if (atIdent("enum")) return parseEnumDef();
    if (atIdent("trait")) return parseTraitDef();
    if (atIdent("impl")) return parseImplDef();
    if (atIdent("import") || atIdent("from")) return parseImport(atIdent("from"), false);
    if (atIdent("export")) return parseImport(false, true);
    if (atIdent("const")) { advance(); return finishConstDecl(); }
    if (atIdent("try") && atPunct("{", 1)) return parseCompound();
    if (atIdent("if") || atIdent("while") || atIdent("for") ||
        atIdent("match") || atIdent("select") || atIdent("unsafe") ||
        (atIdent("do") && atPunct("{", 1)) ||
        (atIdent("gather") && atPunct("{", 1)))
        return parseCompound();
    if (atIdent("gather")) {
        // `gather <expr>,` — append an item to the enclosing worklist.
        // Terminated by ',' (not ';'), so handled here (parseSimple adds ';').
        advance();
        auto s = std::make_unique<ast::Stmt>();
        s->kind = ast::StKind::GatherStmt;
        s->span = spanHere();
        s->exprs.push_back(parseExpr());
        expectPunct(",");
        return s;
    }
    return parseSimple();
}

// ---------------------------------------------------------------------------
// declarations
// ---------------------------------------------------------------------------

std::vector<ast::TypeParam> Parser::parseTypeParams() {
    std::vector<ast::TypeParam> out;
    expectPunct("[");
    while (!atPunct("]")) {
        auto nameTok = expect(Tok::Ident, "type parameter name");
        ast::TypeParam tp;
        tp.name = nameTok.text;
        if (atIdent("is")) { advance(); tp.bound = parseType(); }
        if (atOp("=")) { advance(); tp.def = parseType(); }
        out.push_back(std::move(tp));
        if (atPunct(",")) advance();
        else break;
    }
    expectPunct("]");
    return out;
}

std::vector<ast::Param> Parser::parseParamList() {
    std::vector<ast::Param> out;
    expectPunct("(");
    while (!atPunct(")")) {
        if (atPunct(",")) { advance(); continue; }
        if ((cur().kind == Tok::Op && cur().text == "..") ||
            (cur().kind == Tok::Punct && cur().text == ".")) {
            while ((cur().kind == Tok::Op || cur().kind == Tok::Punct) &&
                   (cur().text == ".." || cur().text == "."))
                advance();                              // C '...' ellipsis
            ast::Param p;
            p.span = spanHere();
            p.variadic = true;
            out.push_back(std::move(p));
            break;
        }
        ast::Param p;
        p.span = spanHere();
        if (atIdent("self")) {
            advance();
            p.selfParam = true;
            p.name = "self";
            if (atPunct(":")) { advance(); p.type = parseType(); }
            out.push_back(std::move(p));
        } else if (atOp("*")) {
            advance();
            p.variadic = true;
            p.name = expect(Tok::Ident, "parameter name").text;
            if (atPunct(":")) { advance(); p.type = parseType(); }
            out.push_back(std::move(p));
        } else {
            if (atIdent("var") && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
                advance();
                p.mutable_ = true;
            }
            p.name = expect(Tok::Ident, "parameter name").text;
            if (atPunct(":")) { advance(); p.type = parseType(); }
            if (atOp("=")) { advance(); p.defaultValue = parseExpr(); }
            out.push_back(std::move(p));
        }
        if (atPunct(",")) advance();
        else break;
    }
    expectPunct(")");
    return out;
}

ast::StmtP Parser::parseFuncDef(bool pub, bool allowBody) {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::FuncDef;
    s->pub = pub;
    advance();                                          // def
    auto nameTok = expect(Tok::Ident, "function name");
    s->name = nameTok.text;
    s->span = spanOf(nameTok);
    if (atPunct("[")) s->typeParams = parseTypeParams();
    s->params = parseParamList();
    if (atOp("->")) { advance(); s->ret = parseType(); }
    if (allowBody) {
        s->body = parseBlock();
        endBlock();
    }
    return s;
}

ast::StmtP Parser::parseStructDef() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::StructDef;
    s->span = spanHere();
    advance();
    s->name = expect(Tok::Ident, "struct name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();
    expectPunct("{");

    for (;;) {                                          // fields then methods
        if (atPunct("}")) { advance(); break; }
        if (at(Tok::Eof)) {
            diags_.report(cur().line, cur().col,
                          "unexpected end of file inside struct body");
            throw Abort("structeof");
        }
        if (atIdent("def")) {
            s->body.push_back(parseFuncDef(false));
            continue;
        }
        ast::FieldDecl f;
        f.span = spanHere();
        if (atIdent("weak")) { advance(); f.weak = true; }
        if (atIdent("var") && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            f.mutable_ = true;
        }
        if (atIdent("pub")) { advance(); f.pub = true; }
        f.name = expect(Tok::Ident, "field name").text;
        expectPunct(":");
        f.type = parseType();
        if (atOp("=")) { advance(); f.defaultValue = parseExpr(); }
        expectPunct(";");                               // fields end with ';'
        s->fields.push_back(std::move(f));
    }
    return s;
}

// `class Name { fields; methods }`  (== Rust-style struct + inherent impl)
//   desugars to a StructDef whose fields stay in `struct` and whose methods are
//   relocated into `impl` nodes. The runtime's method resolution falls back to
//   trait impls (runtime.cpp invokeMethod), so both `x.method()` and
//   interface-object dispatch resolve the same methods. This is semantics-
//   preserving and needs no new VM/runtime support.
//
//   class Name implements IfaceA, IfaceB { ... }   -> struct Name + impl IfaceA +
//                                                     impl IfaceB (methods shared).
//   `extends` (single inheritance / virtual dispatch) is reserved (Phase-2 tail):
//   reported here so the syntax is rejected loudly rather than silently ignored.
ast::StmtP Parser::parseClassDef(bool pub) {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::StructDef;                  // facade over a struct
    s->pub = pub;
    s->span = spanHere();
    advance();                                          // class | pub class
    s->name = expect(Tok::Ident, "class name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();

    // Base class (single inheritance). Only one base allowed; additional bases
    // are reported as a clear error. Chainable: `class A extends B extends C`.
    if (atIdent("extends")) {
        advance();
        s->baseName = expect(Tok::Ident, "base class name").text;
        if (atIdent("extends")) {
            diags_.report(cur().line, cur().col,
                          s->baseName +
                              " already extends a base; Kirn supports single "
                              "inheritance (one base) plus structural "
                              "'implements' interfaces");
            throw Abort("extendsmulti");
        }
    }

    std::vector<std::string> ifaces;
    if (atIdent("implements")) {
        advance();
        for (;;) {
            auto nm = expect(Tok::Ident, "interface name").text;
            ifaces.push_back(nm);
            if (atPunct(",")) { advance(); continue; }
            break;
        }
    }

    expectPunct("{");
    std::vector<ast::StmtP> methods;
    for (;;) {
        if (atPunct("}")) { advance(); break; }
        if (at(Tok::Eof)) {
            diags_.report(cur().line, cur().col,
                          "unexpected end of file inside class body");
            throw Abort("classeof");
        }
        if (atIdent("def") || atIdent("fn")) {
            methods.push_back(parseFuncDef(false));
            continue;
        }
        ast::FieldDecl f;
        f.span = spanHere();
        if (atIdent("weak")) { advance(); f.weak = true; }
        if (atIdent("var") && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            f.mutable_ = true;
        }
        if (atIdent("pub")) { advance(); f.pub = true; }
        f.name = expect(Tok::Ident, "field name").text;
        expectPunct(":");
        f.type = parseType();
        if (atOp("=")) { advance(); f.defaultValue = parseExpr(); }
        expectPunct(";");                               // fields end with ';'
        s->fields.push_back(std::move(f));
    }
    // Methods stay inherent (direct `x.method()` finds them first at runtime).
    s->body = std::move(methods);
    // `class X implements I` also publishes each interface conformance as an
    // `impl I for X` whose methods delegate to the inherent method via `self`
    // (the runtime resolves `self.method` to the inherent one, so no recursion).
    for (const auto& iface : ifaces) {
        auto im = std::make_unique<ast::Stmt>();
        im->kind = ast::StKind::ImplDef;
        im->span = s->span;
        im->implTrait = ast::Type::makeName(iface, s->span);
        im->implType = ast::Type::makeName(s->name, s->span);
        for (const auto& m : s->body) {
            if (m->kind != ast::StKind::FuncDef) continue;
            auto del = std::make_unique<ast::Stmt>();
            del->kind = ast::StKind::FuncDef;
            del->name = m->name;
            for (const auto& p : m->params) del->params.push_back(cloneParam(p));
            del->ret = m->ret ? cloneType(m->ret.get()) : nullptr;
            del->body.push_back(makeDelegateBody(m->name, del->params));
            im->body.push_back(std::move(del));
        }
        extraTopStmts_.push_back(std::move(im));
    }
    return s;
}

// `interface Name { method(s): sig; }`  (Go-style structural bound)
//   desugars to a TraitDef with signature-only methods.
ast::StmtP Parser::parseInterfaceDef(bool pub) {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::TraitDef;
    s->pub = pub;
    s->span = spanHere();
    advance();                                          // interface
    s->name = expect(Tok::Ident, "interface name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();
    expectPunct("{");
    for (;;) {
        if (atPunct("}")) { advance(); break; }
        if (at(Tok::Eof)) {
            diags_.report(cur().line, cur().col,
                          "unexpected end of file inside interface body");
            throw Abort("ifaceeof");
        }
        if (atIdent("def") || atIdent("fn")) {
            // signature-only (no body): consume `def`, name, params, ret, ';'
            advance();
            auto nameTok = expect(Tok::Ident, "method name");
            auto params = parseParamList();
            ast::TypeP ret;
            if (atOp("->")) { advance(); ret = parseType(); }
            ast::TraitMethodSig sig;
            sig.name = nameTok.text;
            sig.params = std::move(params);
            sig.ret = std::move(ret);
            sig.span = spanOf(nameTok);
            s->sigs.push_back(std::move(sig));
            expectPunct(";");
            continue;
        }
        diags_.report(cur().line, cur().col,
                      "expected a method signature in interface body");
        throw Abort("ifacesig");
    }
    return s;
}

// `record Name(f: T, g: U = d) { }`  (immutable value type)
//   Desugars to a StructDef of fields; equality/repr use the value semantics that
//   the runtime already provides for structs (Phase-2 tail adds a full structural
//   `==`/sort order, mirroring `record` in C#/Java).
ast::StmtP Parser::parseRecordDef(bool pub) {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::StructDef;
    s->pub = pub;
    s->span = spanHere();
    advance();                                          // record'
    s->name = expect(Tok::Ident, "record name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();
    expectPunct("(");
    while (!atPunct(")")) {
        ast::FieldDecl f;
        f.span = spanHere();
        if (atIdent("var") && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            f.mutable_ = true;
        }
        f.name = expect(Tok::Ident, "field name").text;
        expectPunct(":");
        f.type = parseType();
        if (atOp("=")) { advance(); f.defaultValue = parseExpr(); }
        s->fields.push_back(std::move(f));
        if (atPunct(",")) { advance(); continue; }
        break;
    }
    expectPunct(")");
    if (atPunct("{")) { parseBlock(); endBlock(); }     // optional empty body
    return s;
}

std::vector<ast::Variant> Parser::parseVariants() {
    std::vector<ast::Variant> out;
    for (;;) {
        if (atPunct("}") || at(Tok::Eof)) break;
        if (atPunct(",")) { advance(); continue; }      // trailing comma
        if (atPunct(";")) { advance(); continue; }      // semicolon separator
        ast::Variant v;
        v.span = spanHere();
        v.name = expect(Tok::Ident, "variant name").text;
        if (atPunct("(")) {
            advance();
            while (!atPunct(")")) {
                if (atPunct(",")) { advance(); continue; }
                ast::VariantPayload pl;
                pl.name = expect(Tok::Ident, "payload name").text;
                expectPunct(":");
                pl.type = parseType();
                v.payload.push_back(std::move(pl));
                if (atPunct(",")) advance();
                else break;
            }
            expectPunct(")");
        }
        out.push_back(std::move(v));
    }
    return out;
}

ast::StmtP Parser::parseEnumDef() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::EnumDef;
    s->span = spanHere();
    advance();
    s->name = expect(Tok::Ident, "enum name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();
    expectPunct("{");
    s->variants = parseVariants();
    expectPunct("}");
    return s;
}

ast::StmtP Parser::parseTraitDef() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::TraitDef;
    s->span = spanHere();
    advance();
    s->name = expect(Tok::Ident, "trait name").text;
    if (atPunct("[")) s->typeParams = parseTypeParams();
    expectPunct("{");

    for (;;) {
        if (atPunct("}")) { advance(); break; }
        if (at(Tok::Eof)) {
            diags_.report(cur().line, cur().col,
                          "unexpected end of file inside trait body");
            throw Abort("traiteof");
        }
        size_t startIdx = idx_;
        expect(Tok::Ident, "'def'");                    // consumes 'def'
        auto nameTok = expect(Tok::Ident, "method name");
        bool hasBody = false;
        {
            // scan past optional generics/params/ret: '{' = default body,
            // ';' = signature-only
            size_t j = idx_;
            int depth = 0;
            for (; j < toks_.size(); ++j) {
                const Token& t = toks_[j];
                if (t.kind == Tok::Eof) break;
                if (t.kind == Tok::Punct) {
                    if (t.text == "(" || t.text == "[") ++depth;
                    else if (t.text == ")" || t.text == "]") --depth;
                    else if (depth == 0 && t.text == "{") { hasBody = true; break; }
                    else if (depth == 0 && t.text == ";") break;
                }
            }
        }
        if (hasBody) {                                  // default body
            idx_ = startIdx;
            s->body.push_back(parseFuncDef(false));
        } else {                                        // signature-only
            if (atPunct("[")) parseTypeParams();
            auto params = parseParamList();
            ast::TypeP ret;
            if (atOp("->")) { advance(); ret = parseType(); }
            ast::TraitMethodSig sig;
            sig.name = nameTok.text;
            sig.params = std::move(params);
            sig.ret = std::move(ret);
            sig.span = spanOf(nameTok);
            s->sigs.push_back(std::move(sig));
            expectPunct(";");                           // sigs end with ';'
        }
    }
    return s;
}

ast::StmtP Parser::parseImplDef() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::ImplDef;
    s->span = spanHere();
    advance();                                          // impl
    if (atPunct("[")) parseTypeParams();
    s->implTrait = parseType();
    if (!atIdent("for")) {
        diags_.report(cur().line, cur().col, "expected 'for' in impl header");
        throw Abort("implfor");
    }
    advance();
    s->implType = parseType();
    s->body = parseBlock();
    endBlock();
    return s;
}

ast::StmtP Parser::finishConstDecl() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::ConstDecl;
    s->span = spanHere();
    auto idTok = expect(Tok::Ident, "constant name");
    s->target = std::make_unique<ast::Expr>();
    s->target->kind = ast::ExKind::Ident;
    s->target->text = idTok.text;
    s->target->span = spanOf(idTok);
    if (atPunct(":")) { advance(); s->declType = parseType(); }
    expectOp("=");
    s->value = parseExpr();
    expectPunct(";");
    return s;
}

ast::StmtP Parser::parseImport(bool from, bool exportKw) {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::Import;
    s->span = spanHere();
    if (exportKw) advance();

    auto dottedName = [&]() {
        // quoted module path (Go style): import "github.com/user/repo"
        if (cur().kind == Tok::StrNormal || cur().kind == Tok::StrRaw) {
            s->moduleName = advance().text;
            return;
        }
        for (;;) {
            if (cur().kind == Tok::Ident && !atIdent("as") && !atIdent("import")) {
                s->moduleName += advance().text;
            } else if (atPunct(".")) {
                s->moduleName += advance().text;
            } else if (atPunct("/")) {
                s->moduleName += advance().text;
            } else {
                break;
            }
        }
    };

    if (from || (s->moduleName.empty() && exportKw && atIdent("from"))) {
        // handled below uniformly
    }
    if (atIdent("from")) {
        advance();
        s->fromImport = true;
        dottedName();
        if (!atIdent("import")) {
            diags_.report(cur().line, cur().col, "expected 'import' in from-import");
            throw Abort("fromimp");
        }
        advance();
        if (atOp("*")) { advance(); s->starImport = true; expectPunct(";"); return s; }
        for (;;) {
            auto n = expect(Tok::Ident, "imported name");
            ast::Stmt::ImportItem it{n.text, {}};
            if (atIdent("as")) { advance(); it.alias = expect(Tok::Ident, "alias").text; }
            s->importItems.push_back(std::move(it));
            if (atPunct(",")) { advance(); continue; }
            break;
        }
        expectPunct(";");
        return s;
    }

    expect(Tok::Ident, "'import'");                     // consume 'import'
    dottedName();
    if (atIdent("as")) { advance(); s->importAlias = expect(Tok::Ident, "alias").text; }
    expectPunct(";");
    return s;
}

// ---------------------------------------------------------------------------
// blocks & compound statements
// ---------------------------------------------------------------------------

std::vector<ast::StmtP> Parser::parseBlock() {
    expectPunct("{");
    std::vector<ast::StmtP> body;
    for (;;) {
        if (atPunct("}")) break;                        // caller consumes via endBlock
        if (at(Tok::Eof)) {
            diags_.report(cur().line, cur().col,
                          "unexpected end of file inside block (missing '}')");
            throw Abort("blockeof");
        }
        try {
            body.push_back(parseTopOrStmt());
            for (auto& ex : extraTopStmts_) body.push_back(std::move(ex));
            extraTopStmts_.clear();
        } catch (Abort&) {
            extraTopStmts_.clear();
            syncToStatementEnd();
        }
    }
    return body;
}

void Parser::endBlock() {
    if (atPunct("}")) advance();
}

ast::StmtP Parser::parseIfChain() {
    auto s = std::make_unique<ast::Stmt>();
    s->kind = ast::StKind::If;
    s->span = spanHere();
    advance();                                          // if | elif
    s->exprs.push_back(parseExpr());                    // condition in exprs[0]
    s->body = parseBlock();
    endBlock();

    if (atIdent("elif") || atIdent("else")) {
        if (atIdent("elif")) {
            s->elseBody.push_back(parseIfChain());      // legacy alias
            return s;
        }
        advance();                                      // else
        if (atIdent("if")) {                            // else-if chain
            s->elseBody.push_back(parseIfChain());
            return s;
        }
        s->elseBody = parseBlock();
        endBlock();
    }
    return s;
}

ast::StmtP Parser::parseCompound() {
    auto s = std::make_unique<ast::Stmt>();
    s->span = spanHere();

    if (atIdent("if")) return parseIfChain();

    if (atIdent("while")) {
        advance();
        s->kind = ast::StKind::While;
        s->exprs.push_back(parseExpr());
        s->body = parseBlock();
        endBlock();
        return s;
    }

    if (atIdent("do")) {
        // do { body } while cond ;        -> DoWhile (runs body once, then
        // do { body } while pat = expr ;  -> tests cond; with a binding
        //    pattern, cond's value binds each round)
        advance();
        s->kind = ast::StKind::DoWhile;
        s->body = parseBlock();            // parseBlock consumes '{' itself
        endBlock();
        if (!atIdent("while")) {
            diags_.report(s->span.line, s->span.col,
                          "expected 'while' after 'do { ... }'");
            throw Abort("dowhile");
        }
        advance();                          // 'while'
        if (cur().kind == Tok::Ident && atOp("=", 1) &&
            !isKeyword(cur().text)) {
            // search-loop form:  while pat = expr
            auto p = parsePattern();
            advance();                      // '='
            s->pat = std::move(p);
        }
        s->cond = parseExpr();
        if (atPunct(";")) advance();        // optional trailing ';' (C-style)
        return s;
    }

    if (atIdent("gather")) {
        advance();
        // gather { seeds }  body   — worklist loop
        if (atPunct("{")) {
            advance();                                  // seed block '{'
            s->kind = ast::StKind::Gather;
            while (!atPunct("}")) {
                if (atPunct(",")) { advance(); continue; }
                s->seedExprs.push_back(parseExpr());
                if (atPunct(",")) advance();
            }
            advance();                                  // seed block '}'
            s->body = parseBlock();
            endBlock();
            return s;
        }
    }

    if (atIdent("for")) {
        advance();
        s->kind = ast::StKind::For;
        s->pat = parsePattern();
        if (!atIdent("in")) {
            diags_.report(cur().line, cur().col, "expected 'in' in for loop");
            throw Abort("forin");
        }
        advance();
        s->exprs.push_back(parseExpr());
        s->body = parseBlock();
        endBlock();
        return s;
    }

    if (atIdent("match")) {
        advance();
        s->kind = ast::StKind::Match;
        s->exprs.push_back(parseExpr());
        expectPunct("{");
        for (;;) {
            if (atPunct("}") || at(Tok::Eof)) break;    // '}' via endBlock below
            expect(Tok::Ident, "'case'");               // consumes 'case'
            ast::MatchArm arm;
            arm.pat = parsePattern();
            if (atIdent("if")) { advance(); arm.guard = parseExpr(); }
            arm.body = parseBlock();
            endBlock();
            s->arms.push_back(std::move(arm));
        }
        endBlock();
        return s;
    }

    if (atIdent("select")) {
        advance();
        s->kind = ast::StKind::Select;
        expectPunct("{");
        for (;;) {
            if (atPunct("}") || at(Tok::Eof)) break;
            expect(Tok::Ident, "'case'");               // consumes 'case'
            ast::Stmt::SelArm arm;
            if (cur().kind == Tok::Ident && atOp("=", 1)) {
                arm.bind = advance().text;
                advance();                              // '='
            }
            if (atOp("<-")) {
                advance();                              // receive-discard prefix
                arm.chanOp = parseExpr();
            } else {
                arm.chanOp = parseExpr();
            }
            arm.body = parseBlock();
            endBlock();
            s->selArms.push_back(std::move(arm));
        }
        endBlock();
        return s;
    }

    if (atIdent("unsafe")) {
        advance();
        s->kind = ast::StKind::Unsafe;
        s->body = parseBlock();
        endBlock();
        return s;
    }

    if (atIdent("try") && atPunct("{", 1)) {
        advance();                                        // 'try'
        s->kind = ast::StKind::Try;
        s->body = parseBlock();
        endBlock();
        if (atIdent("catch")) {
            advance();                                    // 'catch'
            if (cur().kind == Tok::Ident && !isKeyword(cur().text) &&
                atPunct("{", 1)) {
                s->catchParam = advance().text;
            }
            s->elseBody = parseBlock();
            endBlock();
        } else {
            s->elseBody.clear();   // a `try` with no `catch` re-throws (no-op)
        }
        return s;
    }

    diags_.report(s->span.line, s->span.col, "internal: unknown compound statement");
    throw Abort("compound");
}

// ---------------------------------------------------------------------------
// simple statements
// ---------------------------------------------------------------------------

ast::StmtP Parser::parseSimple() {
    auto s = parseSimpleStmt();
    expectPunct(";");                                   // statements end with ';'
    return s;
}

ast::StmtP Parser::parseSimpleStmt() {
    auto s = std::make_unique<ast::Stmt>();
    s->span = spanHere();

    if (cur().kind == Tok::Ident) {
        const std::string& w = cur().text;
        if (w == "pass")    { advance(); s->kind = ast::StKind::Pass; return s; }
        if (w == "del") {
            advance();
            s->kind = ast::StKind::Del;
            s->exprs.push_back(parseExpr());
            return s;
        }
        if (w == "local" && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            s->kind = ast::StKind::LocalDecl;
            s->target = parseExpr();
            if (atPunct(":")) { advance(); s->declType = parseType(); }
            if (atOp("=")) { advance(); s->value = parseExpr(); }
            return s;
        }
        if (w == "global" && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            s->kind = ast::StKind::GlobalDecl;
            s->target = parseExpr();
            if (atPunct(":")) { advance(); s->declType = parseType(); }
            if (atOp("=")) { advance(); s->value = parseExpr(); }
            return s;
        }
        if (w == "temp" && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            s->kind = ast::StKind::TempDecl;
            s->tempVarName = advance().text;            // name
            if (cur().kind != Tok::Int)
                diags_.report(cur().line, cur().col,
                              "expected usage limit after 'temp <name>'");
            else
                s->tempBudget = std::stoi(advance().text);
            if (atPunct(":")) { advance(); s->declType = parseType(); }
            if (atOp("=")) { advance(); s->value = parseExpr(); }
            else diags_.report(cur().line, cur().col, "expected '=' in 'temp' declaration");
            return s;
        }
        if (w == "bucket") {
            advance();
            s->kind = ast::StKind::BucketDecl;
            if (atIdent("release")) {
                advance();
                s->bucketRelease = true;
            }
            if (cur().kind == Tok::Ident && !isKeyword(cur().text))
                s->tempVarName = advance().text;
            else
                diags_.report(cur().line, cur().col,
                              "expected variable name after 'bucket'");
            if (atOp("=")) { advance(); s->value = parseExpr(); }
            return s;
        }
        if (w == "yield") {
            advance();
            s->kind = ast::StKind::Yield;
            if (!atPunct(";")) s->exprs.push_back(parseExpr());
            return s;
        }
        if (w == "goto") {
            advance();
            s->kind = ast::StKind::Goto;
            if (cur().kind != Tok::Ident)
                diags_.report(cur().line, cur().col,
                              "expected label name after 'goto'");
            else
                s->label = advance().text;
            return s;
        }
        if (w == "return") {
            advance();
            s->kind = ast::StKind::Return;
            if (!atPunct(";") && !at(Tok::Eof)) {
                s->exprs.push_back(parseExpr());
                while (atPunct(",")) { advance(); s->exprs.push_back(parseExpr()); }
            }
            return s;
        }
        if (w == "raise") {
            advance();
            s->kind = ast::StKind::Raise;
            s->exprs.push_back(parseExpr());
            return s;
        }
        if (w == "break") {
            advance();
            s->kind = ast::StKind::Break;
            if (cur().kind == Tok::Ident) s->label = advance().text;
            return s;
        }
        if (w == "continue") {
            advance();
            s->kind = ast::StKind::Continue;
            if (cur().kind == Tok::Ident) s->label = advance().text;
            return s;
        }
        if (w == "defer") {
            advance();
            s->kind = ast::StKind::Defer;
            s->exprs.push_back(parseExpr());
            return s;
        }
        if (w == "spawn") {
            advance();
            s->kind = ast::StKind::Spawn;
            s->exprs.push_back(parseExpr());
            return s;
        }
        if (w == "var" && cur(1).kind == Tok::Ident && !isKeyword(cur(1).text)) {
            advance();
            s->kind = ast::StKind::VarDecl;
            s->varKw = true;
            s->target = parseExpr();
            if (atPunct(":")) { advance(); s->declType = parseType(); }
            if (atOp("=")) { advance(); s->value = parseExpr(); }
            return s;
        }
    }

    ast::ExprP first = parseExpr();

    // Annotated immutable binding:  name : Type = value
    if (first && first->kind == ast::ExKind::Ident && atPunct(":")) {
        s->kind = ast::StKind::VarDecl;
        s->target = std::move(first);
        advance();
        s->declType = parseType();
        expectOp("=");
        s->value = parseExpr();
        return s;
    }

    std::vector<ast::ExprP> targets;
    targets.push_back(std::move(first));
    while (atPunct(",")) {
        advance();
        targets.push_back(parseExpr());
    }

    if (atOp("=")) {
        s->kind = ast::StKind::Assign;
        for (auto& tgt : targets) s->exprs.push_back(std::move(tgt));
        advance();
        s->exprs.push_back(parseExpr());                // values
        while (atPunct(",")) { advance(); s->exprs.push_back(parseExpr()); }
        return s;
    }
    if (targets.size() == 1) {
        if (cur().kind == Tok::Op && isAugOp(cur().text)) {
            s->kind = ast::StKind::AugAssign;
            s->augOp = advance().text;
            s->exprs.push_back(std::move(targets[0]));
            s->exprs.push_back(parseExpr());
            return s;
        }
        s->kind = ast::StKind::ExprStmt;
        s->exprs.push_back(std::move(targets[0]));
        return s;
    }
    diags_.report(cur().line, cur().col,
                  "expected '=' after comma-separated targets");
    throw Abort("assignlist");
}

// ---------------------------------------------------------------------------
// patterns
// ---------------------------------------------------------------------------

static ast::PatP mkPat(ast::PatKind k, ast::Span s);

ast::PatP Parser::parsePattern() {
    // or-pattern: p1 | p2 | ...   (lowest precedence in patterns)
    auto first = parseCtorOrBind();
    if (!atOp("|")) return first;
    auto orp = mkPat(ast::PatKind::Or, first->span);
    orp->alts.push_back(std::move(first));
    while (atOp("|")) {
        advance();
        orp->alts.push_back(parseCtorOrBind());
    }
    return orp;
}

static ast::PatP mkPat(ast::PatKind k, ast::Span s) {
    auto p = std::make_unique<ast::Pat>();
    p->kind = k;
    p->span = s;
    return p;
}

static ast::ExprP litFromToken(const Token& t) {
    auto e = std::make_unique<ast::Expr>();
    e->span = ast::Span{t.line, t.col, t.endLine, t.endCol};
    switch (t.kind) {
        case Tok::Int:   e->kind = ast::ExKind::Int;   e->text = t.text; break;
        case Tok::Float: e->kind = ast::ExKind::Float; e->text = t.text; break;
        case Tok::Char:  e->kind = ast::ExKind::CharLit; e->text = t.text; break;
        case Tok::StrNormal: case Tok::StrRaw:
        case Tok::StrByte:   case Tok::StrC:
            e->kind = ast::ExKind::Str; e->text = t.text;
            e->flavor = t.kind == Tok::StrRaw   ? ast::StrFlavor::Raw
                      : t.kind == Tok::StrByte   ? ast::StrFlavor::Byte
                      : t.kind == Tok::StrC      ? ast::StrFlavor::C
                                                 : ast::StrFlavor::Normal;
            break;
        default:
            e->kind = ast::ExKind::Ident;               // true/false/none words
            e->text = t.text;
            break;
    }
    return e;
}

ast::PatP Parser::parseCtorOrBind() {
    ast::Span sp = spanHere();

    // wildcard
    if (cur().kind == Tok::Ident && cur().text == "_") {
        advance();
        return mkPat(ast::PatKind::Wild, sp);
    }

    // ref pattern: `&pat` dereferences the subject before matching `pat`.
    // Transparent in the v1 value model (subject is matched in place).
    if (atOp("&")) {
        advance();
        auto r = mkPat(ast::PatKind::Ref, sp);
        r->inner = parsePattern();
        return r;
    }

    // slice / list pattern:  [p1, p2, ..rest]  |  [p1, p2]
    if (atPunct("[")) {
        advance();
        auto sl = mkPat(ast::PatKind::Slice, sp);
        while (!atPunct("]")) {
            if (atPunct(",")) { advance(); continue; }
// rest: `..` [name]
            if (atOp("..")) {
                advance();
                sl->hasRest = true;                       // even unnamed `..`
                if (cur().kind == Tok::Ident && cur().text != "_")
                    sl->restName = advance().text;
                break;                                  // rest captures tail
            }
            sl->elems.push_back(parsePattern());
            if (atPunct(",")) advance();
            else break;
        }
        expectPunct("]");
        return sl;
    }

    // group / tuple pattern
    if (atPunct("(")) {
        advance();
        if (atPunct(")")) {                             // () unit
            advance();
            return mkPat(ast::PatKind::Tuple, sp);
        }
        // rest-only or leading rest: (..) | (..tail, ...)
        if (atOp("..")) {
            advance();
            auto tup = mkPat(ast::PatKind::Tuple, sp);
            tup->hasRest = true;
            if (cur().kind == Tok::Ident && cur().text != "_")
                tup->restName = advance().text;
            if (!atPunct(")")) {
                while (atPunct(",")) {
                    advance();
                    if (atPunct(")")) break;
                    tup->elems.push_back(parsePattern());
                    if (atPunct(",")) advance();
                    else break;
                }
            }
            expectPunct(")");
            return tup;
        }
        auto first = parsePattern();
        if (atPunct(",")) {
            auto tup = mkPat(ast::PatKind::Tuple, sp);
            tup->elems.push_back(std::move(first));
            while (atPunct(",")) {
                advance();
                if (atPunct(")")) break;
                if (atOp("..")) {                       // rest in tuple
                    advance();
                    tup->hasRest = true;
                    if (cur().kind == Tok::Ident && cur().text != "_")
                        tup->restName = advance().text;
                    break;
                }
                tup->elems.push_back(parsePattern());
            }
            expectPunct(")");
            return tup;
        }
        expectPunct(")");
        return first;                                   // grouping
    }

    // literal / range patterns
    bool minus = atOp("-");
    if (minus || cur().kind == Tok::Int || cur().kind == Tok::Float ||
        cur().kind == Tok::Char || cur().kind == Tok::StrNormal ||
        cur().kind == Tok::StrRaw || ((cur().kind == Tok::Ident) &&
            (cur().text == "true" || cur().text == "false" ||
             cur().text == "none" || cur().text == "nil"))) {
        auto lit = std::make_unique<ast::Expr>();
        if (minus) {
            advance();
            lit->kind = ast::ExKind::Unary;
            lit->op = "-";
            lit->rhs = litFromToken(expect(cur().kind == Tok::Int ? Tok::Int : Tok::Float,
                                           "number"));
        } else {
            lit = litFromToken(advance());
        }
        if (atOp("..") || atOp("..=")) {                // range pattern
            bool incl = cur().text == "..=";
            advance();
            auto r = mkPat(ast::PatKind::Range, sp);
            r->lo = mkPat(ast::PatKind::Literal, sp);
            r->lo->literal = std::move(lit);
            bool neg2 = atOp("-");
            if (neg2) advance();
            auto num = expect(cur().kind == Tok::Float ? Tok::Float : Tok::Int, "number");
            auto hiLit = litFromToken(num);
            if (neg2) {
                auto u = std::make_unique<ast::Expr>();
                u->kind = ast::ExKind::Unary;
                u->op = "-";
                u->rhs = std::move(hiLit);
                hiLit = std::move(u);
            }
            r->hi = mkPat(ast::PatKind::Literal, sp);
            r->hi->literal = std::move(hiLit);
            r->inclusive = incl;
            return r;
        }
        auto p = mkPat(ast::PatKind::Literal, sp);
        p->literal = std::move(lit);
        return p;
    }

    // identifier-led: ctor or binding
    if (cur().kind != Tok::Ident) {
        diags_.report(cur().line, cur().col,
                      "expected pattern, found '" + cur().text + "'");
        throw Abort("pattern");
    }

    if (atPunct("(", 1)) {                              // ctor pattern
        auto p = mkPat(ast::PatKind::Ctor, sp);
        p->ctorName = advance().text;
        advance();                                      // (
        while (!atPunct(")")) {
            if (atPunct(",")) { advance(); continue; }
            ast::PatField fld;
            if (cur().kind == Tok::Ident && atPunct(":", 1)) {
                fld.name = advance().text;
                advance();                              // ':'
                fld.pat = parsePattern();
            } else {
                fld.pat = parsePattern();
            }
            p->fields.push_back(std::move(fld));
            if (atPunct(",")) advance();
            else break;
        }
        expectPunct(")");
        return p;
    }

    // binding [var] name [is type]  |  sub-pat @ name (Rust-style alias)
    auto p = mkPat(ast::PatKind::Bind, sp);
    if (atIdent("var") && cur(1).kind == Tok::Ident) advance();
    p->bindName = expect(Tok::Ident, "binding name").text;
    if (atIdent("is")) { advance(); p->bindType = parseType(); }
    if (atOp("@")) {                                   // name @ sub-pattern
        advance();
        auto alias = mkPat(ast::PatKind::BindAlias, sp);
        alias->bindName = p->bindName;
        alias->bindType = std::move(p->bindType);
        alias->aliasSub = parsePattern();
        return alias;
    }
    return p;
}

// ---------------------------------------------------------------------------
// types
// ---------------------------------------------------------------------------

ast::TypeP Parser::parseBaseType() {
    ast::Span sp = spanHere();
    if (atIdent("fn")) {
        advance();
        std::vector<ast::TypeP> ps;
        expectPunct("(");
        while (!atPunct(")")) {
            if (atPunct(",")) { advance(); continue; }
            ps.push_back(parseType());
            if (atPunct(",")) advance();
            else break;
        }
        expectPunct(")");
        ast::TypeP ret;
        if (atOp("->")) { advance(); ret = parseType(); }
        return ast::Type::makeFn(std::move(ps), std::move(ret), sp);
    }
    if (cur().kind == Tok::Ident) {
        auto nameTok = advance();
        std::vector<ast::TypeP> gens;
        if (atPunct("[")) {
            advance();
            while (!atPunct("]")) {
                if (atPunct(",")) { advance(); continue; }
                gens.push_back(parseType());
                if (atPunct(",")) advance();
                else break;
            }
            expectPunct("]");
        }
        return ast::Type::makeName(nameTok.text, spanOf(nameTok),
                                   std::move(gens));
    }
    if (atPunct("(")) {
        advance();
        if (atPunct(")")) { advance(); return ast::Type::makeName("none", sp); }
        std::vector<ast::TypeP> ts;
        ts.push_back(parseType());
        bool tuple = false;
        while (atPunct(",")) {
            advance();
            tuple = true;
            if (atPunct(")")) break;
            ts.push_back(parseType());
        }
        expectPunct(")");
        if (!tuple) return std::move(ts[0]);            // grouping
        return ast::Type::makeTuple(std::move(ts), sp);
    }
    diags_.report(sp.line, sp.col, "expected type, found '" + cur().text + "'");
    throw Abort("type");
}

ast::TypeP Parser::parseType() {
    ast::Span sp = spanHere();
    ast::TypeP t;
    if (atOp("*")) {
        advance();
        t = ast::Type::makePointer(parseType(), sp);
    } else if (atOp("&")) {
        advance();
        bool mut = atIdent("mut") && cur(1).kind == Tok::Ident;
        if (mut) advance();
        t = ast::Type::makeRef(parseType(), mut, sp);
    } else {
        t = parseBaseType();
    }
    while (atOp("?")) {
        advance();
        t = ast::Type::makeOptional(std::move(t), spanHere());
    }
    return t;
}

// ---------------------------------------------------------------------------
// expressions — precedence ladder (grammar §2)
// ---------------------------------------------------------------------------

bool Parser::tryParseLambda(ast::ExprP& out) {
    if (!atPunct("(")) return false;
    size_t j = idx_;
    int depth = 0;
    bool okShape = true;
    for (;; ++j) {
        if (j >= toks_.size()) return false;
        const Token& t = toks_[j];
        if (t.kind == Tok::Eof || t.kind == Tok::Newline) return false;
        if (t.kind == Tok::Punct) {
            if (t.text == "(") { ++depth; continue; }
            if (t.text == ")") {
                if (--depth == 0) { ++j; break; }
                continue;
            }
            if (depth == 1 && t.text != ",") okShape = false;
            continue;
        }
        if (depth >= 1 && t.kind != Tok::Ident) okShape = false;
    }
    if (!okShape) return false;
    if (j >= toks_.size() || toks_[j].kind != Tok::Op || toks_[j].text != "=>")
        return false;

    advance();                                          // (
    auto lam = std::make_unique<ast::Expr>();
    lam->kind = ast::ExKind::Lambda;
    lam->span = spanHere();
    while (!atPunct(")")) {
        lam->lambdaParams.push_back(expect(Tok::Ident, "lambda parameter").text);
        if (atPunct(",")) advance();
        else break;
    }
    expectPunct(")");    expectOp("=>");
    lam->rhs = parseExpr();
    out = std::move(lam);
    return true;
}

// `fn (params) [-> ret] { body }` — a block-bodied closure. Distinct from a
// function declaration (`fn name(...)`) by the `(` immediately after `fn`.
ast::ExprP Parser::parseFnClosure() {
    auto lam = std::make_unique<ast::Expr>();
    lam->kind = ast::ExKind::Lambda;
    lam->span = spanHere();
    advance();                                      // 'fn'
    lam->closureParams = parseParamList();
    if (atOp("->")) {
        advance();
        lam->retType = parseType();
    }
    if (!atPunct("{")) {
        diags_.report(cur().line, cur().col,
                      "expected '{' to start the fn closure body");
        throw Abort("fnclosure");
    }
    lam->lambdaBody = parseBlock();
    endBlock();
    return lam;
}

ast::ExprP Parser::parseExpr() {
    ast::Span sp = spanHere();
    if (atIdent("spawn")) {                             // spawn as expression
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Unary;
        e->op = "spawn";
        e->span = sp;
        advance();
        e->rhs = parseExpr();
        return e;
    }
    if (atIdent("for")) {                               // generator expression
        auto g = std::make_unique<ast::Expr>();
        g->kind = ast::ExKind::Generator;
        g->span = sp;
        g->clauses.emplace_back();
        g->clauses.back().isFor = true;
        advance();
        g->clauses.back().pat = parsePattern();
        if (!atIdent("in")) {
            diags_.report(cur().line, cur().col, "expected 'in' in generator");
            throw Abort("genin");
        }
        advance();
        g->clauses.back().iter = parseExpr();
        for (;;) {
            if (atIdent("for")) {
                g->clauses.emplace_back();
                g->clauses.back().isFor = true;
                advance();
                g->clauses.back().pat = parsePattern();
                if (!atIdent("in")) {
                    diags_.report(cur().line, cur().col, "expected 'in'");
                    throw Abort("genin");
                }
                advance();
                g->clauses.back().iter = parseExpr();
            } else if (atIdent("if")) {
                g->clauses.emplace_back();
                g->clauses.back().isFor = false;
                advance();
                g->clauses.back().cond = parseExpr();
            } else {
                break;
            }
        }
        if (!atIdent("yield")) {
            diags_.report(cur().line, cur().col, "expected 'yield' in generator");
            throw Abort("genyield");
        }
        advance();
        g->elems.push_back(parseExpr());                // yielded value
        return g;
    }
    if (atIdent("if")) return parseCondExpr();

    if (atIdent("fn") && atPunct("(", 1)) return parseFnClosure();

    ast::ExprP lam;
    if (tryParseLambda(lam)) return lam;
    return parseOr();
}

ast::ExprP Parser::parseCondExpr() {
    // Rust-style conditional expression:  if c { a } else { b }
    auto e = std::make_unique<ast::Expr>();
    e->kind = ast::ExKind::Cond;
    e->span = spanHere();
    advance();                                          // if
    e->cond = parseExpr();                              // condition
    expectPunct("{");
    e->lhs = parseExpr();                               // then-value
    expectPunct("}");
    if (!atIdent("else")) {
        diags_.report(cur().line, cur().col,
                      "expected 'else { ... }' in conditional expression");
        throw Abort("condelse");
    }
    advance();
    expectPunct("{");
    e->rhs = parseExpr();                               // else-value
    expectPunct("}");
    return e;
}

ast::ExprP Parser::parseCompExpr(ast::ExprP first) {
    auto e = std::make_unique<ast::Expr>();
    e->kind = ast::ExKind::ListComp;
    e->span = first->span;
    e->elems.push_back(std::move(first));               // element expr
    for (;;) {
        if (atIdent("for")) {
            e->clauses.emplace_back();
            e->clauses.back().isFor = true;
            advance();
            e->clauses.back().pat = parsePattern();
            if (!atIdent("in")) {
                diags_.report(cur().line, cur().col, "expected 'in' in comprehension");
                throw Abort("compin");
            }
            advance();
            e->clauses.back().iter = parseExpr();
        } else if (atIdent("if")) {
            e->clauses.emplace_back();
            e->clauses.back().isFor = false;
            advance();
            e->clauses.back().cond = parseExpr();
        } else {
            break;
        }
    }
    return e;
}

static ast::ExprP bin(const char* op, ast::ExprP l, ast::ExprP r, ast::Span s) {
    auto e = std::make_unique<ast::Expr>();
    e->kind = ast::ExKind::Binary;
    e->op = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    e->span = s;
    return e;
}

ast::ExprP Parser::parseOr() {
    ast::ExprP l = parseAnd();
    while (atIdent("or")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("or", std::move(l), parseAnd(), sp);
    }
    return l;
}
ast::ExprP Parser::parseAnd() {
    ast::ExprP l = parseNot();
    while (atIdent("and")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("and", std::move(l), parseNot(), sp);
    }
    return l;
}
ast::ExprP Parser::parseNot() {
    if (atIdent("not")) {
        ast::Span sp = spanHere();
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Unary;
        e->op = "not";
        e->span = sp;
        e->rhs = parseNot();
        return e;
    }
    return parseComparison();
}
ast::ExprP Parser::parseComparison() {
    ast::ExprP l = parseRange();
    while ((cur().kind == Tok::Op && isCmpTok(cur().text)) ||
           atIdent("is") || atIdent("in")) {
        ast::Span sp = spanHere();
        std::string op = advance().text;
        l = bin(op.c_str(), std::move(l), parseRange(), sp);
    }
    return l;
}
ast::ExprP Parser::parseRange() {
    ast::ExprP l = parseBitOr();
    if (atOp("..") || atOp("..=")) {
        std::string op = cur().text;
        ast::Span sp = spanHere();
        const Token& n = cur(1);
        bool closed =
            n.kind == Tok::Eof || n.kind == Tok::Newline || n.kind == Tok::Dedent ||
            (n.kind == Tok::Punct &&
             (n.text == "]" || n.text == ")" || n.text == "}" ||
              n.text == "," || n.text == ":"));
        advance();
        // Open-ended range (e.g. xs[8..]): closer follows; rhs stays null.
        ast::ExprP r = closed ? nullptr : parseBitOr();
        return bin(op.c_str(), std::move(l), std::move(r), sp);
    }
    return l;
}
ast::ExprP Parser::parseBitOr() {
    ast::ExprP l = parseBitXor();
    while (atOp("|")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("|", std::move(l), parseBitXor(), sp);
    }
    return l;
}
ast::ExprP Parser::parseBitXor() {
    ast::ExprP l = parseBitAnd();
    while (atOp("^")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("^", std::move(l), parseBitAnd(), sp);
    }
    return l;
}
ast::ExprP Parser::parseBitAnd() {
    ast::ExprP l = parseShift();
    while (atOp("&")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("&", std::move(l), parseShift(), sp);
    }
    return l;
}
ast::ExprP Parser::parseShift() {
    ast::ExprP l = parseArith();
    while (atOp("<<") || atOp(">>")) {
        std::string op = cur().text;
        ast::Span sp = spanHere();
        advance();
        l = bin(op.c_str(), std::move(l), parseArith(), sp);
    }
    return l;
}
ast::ExprP Parser::parseArith() {
    ast::ExprP l = parseTerm();
    while (atOp("+") || atOp("-")) {
        std::string op = cur().text;
        ast::Span sp = spanHere();
        advance();
        l = bin(op.c_str(), std::move(l), parseTerm(), sp);
    }
    return l;
}
ast::ExprP Parser::parseTerm() {
    ast::ExprP l = parseUnary();
    while (atOp("*") || atOp("/") || atOp("%") || atOp("//")) {
        std::string op = cur().text;
        ast::Span sp = spanHere();
        advance();
        l = bin(op.c_str(), std::move(l), parseUnary(), sp);
    }
    return l;
}
ast::ExprP Parser::parseUnary() {
    if (atIdent("try")) {                               // try expr (propagate)
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Unary;
        e->op = "try";
        e->span = spanHere();
        advance();
        e->rhs = parseUnary();
        return e;
    }
    if (atOp("-") || atOp("+") || atOp("~") || atOp("&") || atOp("*") ||
        atIdent("not")) {
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Unary;
        e->span = spanHere();
        e->op = advance().text;
        e->rhs = parseUnary();
        return e;
    }
    return parsePower();
}
ast::ExprP Parser::parsePower() {
    ast::ExprP l = parsePostfix();
    if (atOp("**")) {
        ast::Span sp = spanHere();
        advance();
        l = bin("**", std::move(l), parseUnary(), sp);      // right-assoc
    }
    while (atIdent("as")) {                             // checked cast
        advance();
        auto c = std::make_unique<ast::Expr>();
        c->kind = ast::ExKind::Cast;
        c->lhs = std::move(l);
        c->newType = parseType();
        if (c->lhs) c->span = c->lhs->span;
        l = std::move(c);
    }
    return l;
}

ast::CallArg Parser::parseCallArg() {
    ast::CallArg a;
    if (cur().kind == Tok::Ident && atPunct(":", 1) && !atIdent("if")) {
        a.name = advance().text;
        advance();                                      // ':'
    }
    a.value = parseExpr();
    return a;
}

ast::ExprP Parser::parseIndexBody(ast::ExprP obj) {
    expectPunct("[");
    auto idx = std::make_unique<ast::Expr>();
    idx->lhs = std::move(obj);
    idx->span = spanHere();

    if (atPunct(":")) {                                 // [:hi[:step]] / [:]
        advance();
        idx->kind = ast::ExKind::Slice;
        idx->op = ":";
        idx->elems.push_back(nullptr);
        idx->elems.push_back(atPunct(":") || atPunct("]") ? nullptr : parseExpr());
        if (atPunct(":")) {
            advance();
            idx->elems.push_back(atPunct("]") ? nullptr : parseExpr());
        }
        expectPunct("]");
        return idx;
    }

    ast::ExprP first = parseExpr();                     // may be a range expr
    if (atPunct(":")) {                                 // [lo:hi[:step]]
        advance();
        idx->kind = ast::ExKind::Slice;
        idx->op = ":";
        idx->elems.push_back(std::move(first));
        idx->elems.push_back(atPunct(":") || atPunct("]") ? nullptr : parseExpr());
        if (atPunct(":")) {
            advance();
            idx->elems.push_back(atPunct("]") ? nullptr : parseExpr());
        }
        expectPunct("]");
        return idx;
    }

    idx->kind = ast::ExKind::Index;                     // subscript (incl. ranges)
    idx->rhs = std::move(first);
    expectPunct("]");
    return idx;
}

ast::ExprP Parser::parsePostfix() {
    ast::ExprP e = parsePrimary();
    for (;;) {
        if (atPunct("(")) {                             // call
            advance();
            auto c = std::make_unique<ast::Expr>();
            c->kind = ast::ExKind::Call;
            c->span = spanHere();
            c->lhs = std::move(e);
            while (!atPunct(")")) {
                if (atPunct(",")) { advance(); continue; }
                c->args.push_back(parseCallArg());
                if (atPunct(",")) advance();
                else break;
            }
            expectPunct(")");
            e = std::move(c);
            continue;
        }
        if (atPunct("[")) {
            e = parseIndexBody(std::move(e));
            continue;
        }
        if (atPunct(".") || atOp(".?.")) {
            bool nilSafe = cur().kind == Tok::Op && cur().text == ".?.";
            advance();
            auto m = std::make_unique<ast::Expr>();
            m->kind = ast::ExKind::Member;
            m->span = spanHere();
            m->lhs = std::move(e);
            m->nilSafe = nilSafe;
            m->text = expect(Tok::Ident, "member name").text;
            e = std::move(m);
            continue;
        }
        if (atOp("?")) {
            advance();
            auto t = std::make_unique<ast::Expr>();
            t->kind = ast::ExKind::Try;
            t->span = spanHere();
            t->lhs = std::move(e);
            e = std::move(t);
            continue;
        }
        break;
    }
    return e;
}

ast::ExprP Parser::parsePrimary() {
    ast::Span sp = spanHere();
    const Token& t = cur();

    // match-expression: `match <subj> { case pat [if g] { body } ... }` yields
    // the trailing expression of the chosen arm's body (else none).
    if (t.kind == Tok::Ident && t.text == "match") {
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Match;
        e->span = sp;
        e->lhs = parseExpr();                           // subject
        expectPunct("{");
        for (;;) {
            if (atPunct("}") || at(Tok::Eof)) break;
            expect(Tok::Ident, "'case'");
            ast::MatchArm arm;
            arm.pat = parsePattern();
            if (atIdent("if")) { advance(); arm.guard = parseExpr(); }
            arm.body = parseBlock();
            endBlock();
            e->matchArms.push_back(std::move(arm));
        }
        expectPunct("}");
        return e;
    }

    switch (t.kind) {
        case Tok::Int:
        case Tok::Float: {
            advance();
            auto e = std::make_unique<ast::Expr>();
            e->kind = t.kind == Tok::Int ? ast::ExKind::Int : ast::ExKind::Float;
            e->text = t.text;
            e->span = spanOf(t);
            return e;
        }
        case Tok::Char: {
            advance();
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::CharLit;
            e->text = t.text;
            e->span = spanOf(t);
            return e;
        }
        case Tok::StrNormal:
        case Tok::StrRaw:
        case Tok::StrByte:
        case Tok::StrC: {
            advance();
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::Str;
            e->text = t.text;
            e->span = spanOf(t);
            e->flavor = t.kind == Tok::StrRaw     ? ast::StrFlavor::Raw
                      : t.kind == Tok::StrByte     ? ast::StrFlavor::Byte
                      : t.kind == Tok::StrC        ? ast::StrFlavor::C
                                                   : ast::StrFlavor::Normal;
            return e;
        }
        case Tok::FStringStart:
            break;                                      // handled below
        default:
            break;
    }

    if (t.kind == Tok::FStringStart) {
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::FString;
        e->span = sp;
        while (!at(Tok::FStringEnd) && !at(Tok::Eof)) {
            if (at(Tok::FStringText)) {
                const Token& tt = cur();
                ast::FStrPart part;
                part.isExpr = false;
                part.text = tt.text;
                e->parts.push_back(std::move(part));
                advance();
                continue;
            }
            if (at(Tok::FStringLBrace)) {
                advance();
                // gather tokens of the interpolation expression
                std::vector<Token> sub;
                int depth = 0;
                std::string spec;
                bool sawSpec = false;
                while (!at(Tok::Eof)) {
                    if (at(Tok::FStringRBrace) && depth == 0) break;
                    if (at(Tok::FStringColon) && depth == 0) { sawSpec = true; break; }
                    if (atPunct("{")) ++depth;
                    if (atPunct("}")) --depth;
                    sub.push_back(cur());
                    advance();
                }
                if (sawSpec) {
                    advance();                          // FStringColon
                    if (at(Tok::FStringSpec)) { spec = cur().text; advance(); }
                }
                expect(Tok::FStringRBrace, "'}' closing interpolation");
                sub.push_back(Token{});                 // Eof sentinel
                sub.back().kind = Tok::Eof;
                DiagEngine ignore;
                Parser subParser(sub, ignore);
                ast::FStrPart part;
                part.isExpr = true;
                part.expr = subParser.parseExpr();
                part.spec = spec;
                e->parts.push_back(std::move(part));
                continue;
            }
            diags_.report(cur().line, cur().col, "malformed f-string");
            advance();
        }
        if (at(Tok::FStringEnd)) advance();
        return e;
    }

    if (t.kind == Tok::Ident) {
        if (t.text == "chan" && (atPunct("[", 1) || atPunct("(", 1))) {
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::New;
            e->span = sp;
            e->newType = parseBaseType();               // consumes chan[T]; args via postfix
            return e;
        }
        if (t.text == "new") {
            advance();
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::New;
            e->span = sp;
            e->newType = parseBaseType();
            return e;                                   // args attach via postfix call
        }
        if (isKeyword(t.text)) {
            // keywords valid as expressions here
            if (t.text == "none" || t.text == "true" || t.text == "false" ||
                t.text == "self") {
                advance();
                auto e = std::make_unique<ast::Expr>();
                e->kind = ast::ExKind::Ident;
                e->text = t.text;
                e->span = spanOf(t);
                return e;
            }
            diags_.report(t.line, t.col,
                          "unexpected keyword '" + t.text + "' in expression");
            throw Abort("kwexpr");
        }
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::Ident;
        e->text = t.text;
        e->span = spanOf(t);
        return e;
    }

    if (atPunct("(")) {
        ast::ExprP lam;
        if (tryParseLambda(lam)) return lam;
        advance();
        if (atPunct(")")) {                             // () empty tuple
            advance();
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::Tuple;
            e->span = sp;
            return e;
        }
        ast::ExprP first = parseExpr();
        if (atPunct(",")) {
            auto e = std::make_unique<ast::Expr>();
            e->kind = ast::ExKind::Tuple;
            e->span = sp;
            e->elems.push_back(std::move(first));
            while (atPunct(",")) {
                advance();
                if (atPunct(")")) break;
                e->elems.push_back(parseExpr());
            }
            expectPunct(")");
            return e;
        }
        expectPunct(")");
        return first;                                   // grouping
    }

    if (atPunct("[")) {
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->kind = ast::ExKind::List;
        e->span = sp;
        if (!atPunct("]")) {
            ast::ExprP first = parseExpr();
            if (atIdent("for")) {
                e = parseCompExpr(std::move(first));
                e->kind = ast::ExKind::ListComp;
            } else {
                e->elems.push_back(std::move(first));
                while (atPunct(",")) {
                    advance();
                    if (atPunct("]")) break;
                    e->elems.push_back(parseExpr());
                }
            }
        }
        expectPunct("]");
        return e;
    }

    if (atPunct("{")) {
        advance();
        auto e = std::make_unique<ast::Expr>();
        e->span = sp;
        if (atPunct("}")) {                             // {} empty dict
            advance();
            e->kind = ast::ExKind::Dict;
            return e;
        }
        ast::ExprP firstKey = parseExpr();
        if (atPunct(":")) {
            advance();
            e->kind = ast::ExKind::Dict;
            e->pairs.emplace_back(std::move(firstKey), parseExpr());
            while (atPunct(",")) {
                advance();
                if (atPunct("}")) break;
                auto k = parseExpr();
                expectPunct(":");
                e->pairs.emplace_back(std::move(k), parseExpr());
            }
        } else {
            e->kind = ast::ExKind::Set;
            e->elems.push_back(std::move(firstKey));
            while (atPunct(",")) {
                advance();
                if (atPunct("}")) break;
                e->elems.push_back(parseExpr());
            }
        }
        expectPunct("}");
        return e;
    }

    diags_.report(t.line, t.col, "expected expression, found '" + t.text + "'");
    throw Abort("primary");
}

} // namespace kirn
