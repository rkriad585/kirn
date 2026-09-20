#include "lex/lexer.h"

#include <cctype>

namespace kirn {

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

const char* tokName(Tok t) {
    switch (t) {
        case Tok::Eof: return "Eof";
        case Tok::Newline: return "Newline";
        case Tok::Indent: return "Indent";
        case Tok::Dedent: return "Dedent";
        case Tok::Ident: return "Ident";
        case Tok::Int: return "Int";
        case Tok::Float: return "Float";
        case Tok::Char: return "Char";
        case Tok::StrNormal: return "Str";
        case Tok::StrRaw: return "StrRaw";
        case Tok::StrByte: return "StrByte";
        case Tok::StrC: return "StrC";
        case Tok::FStringStart: return "FStr#";
        case Tok::FStringText: return "FStrTxt";
        case Tok::FStringLBrace: return "FStr{";
        case Tok::FStringRBrace: return "FStr}";
        case Tok::FStringColon: return "FStr:";
        case Tok::FStringSpec: return "FStrSpec";
        case Tok::FStringEnd: return "FStrEnd";
        case Tok::Op: return "Op";
        case Tok::Punct: return "Punct";
    }
    return "?";
}

bool isKeyword(std::string_view ident) {
    static const std::string_view kKeywords[] = {
        "def", "fn", "var", "let", "if", "elif", "else", "while", "for", "in",
        "return", "break", "continue", "match", "case", "struct", "enum",
        "trait", "impl", "import", "export", "pub", "defer", "spawn", "chan",
        "select", "try", "raise", "catch", "unsafe", "extern", "new", "box", "self",
        "Self", "and", "or", "not", "is", "as", "true", "false", "none",
        "pass", "class", "interface", "record", "implements", "extends",
        "dynamic",
        "None",
        "del", "pr", "local", "global", "temp",
        "bucket",
    };
    for (std::string_view k : kKeywords)
        if (k == ident) return true;
    return false;
}

namespace {

constexpr std::string_view kOps3[] = {"<<=", ">>=", "..=", ".?."};
constexpr std::string_view kOps2[] = {
    "**", "//", "==", "!=", "<=", ">=", "<<", ">>", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "->", "<-", "..", "=>",
};
constexpr std::string_view kPunct1 = "()[]{},:.;";
constexpr std::string_view kOp1 = "+-*/%&|^~<>=?@";

bool isIdentStart(char c) {
    unsigned u = static_cast<unsigned char>(c);
    return c == '_' || std::isalpha(u);
}
bool isIdentCont(char c) {
    unsigned u = static_cast<unsigned char>(c);
    return c == '_' || std::isalnum(u);
}
bool isDec(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) {
    unsigned u = static_cast<unsigned char>(c);
    int lo = std::tolower(u);
    return isDec(c) || (lo >= 'a' && lo <= 'f');
}
bool isOct(char c) { return c >= '0' && c <= '7'; }
bool isBin(char c) { return c == '0' || c == '1'; }

struct Pos {
    size_t p;
    uint32_t l, c;
};

} // namespace

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

Lexer::Lexer(std::string_view source, std::string filename, DiagEngine& diags)
    : src_(source), filename_(std::move(filename)), diags_(diags) {}

char Lexer::peek(size_t off) const {
    return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
}

void Lexer::diagHere(std::string msg) { diags_.report(line_, col_, std::move(msg)); }

void Lexer::stampExtent(Token& t) {
    // At call time line_/col_ sit just past the last consumed char, so the
    // extent end is (line_, col_-1) for a same-line token. Newline separators
    // between multi-line strings/f-strings make the end a whole line; we use
    // col_-1 clamped to >= t.col for the common single-line case.
    t.endLine = line_;
    t.endCol = (col_ > 1) ? (col_ - 1) : 1;
    if (t.endLine == t.line && t.endCol < t.col) t.endCol = t.col;
}

void Lexer::skipInlineWs() {
    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t'))
        advanceWs();
}

void Lexer::advanceWs() {
    ++pos_;
    ++col_;
}

bool Lexer::atEol(size_t p) const {
    return p < src_.size() && (src_[p] == '\n' || src_[p] == '\r');
}

void Lexer::consumeEol() {
    if (peek() == '\r') {
        ++pos_;
        if (peek() == '\n') ++pos_;
    } else {
        ++pos_;
    }
    ++line_;
    col_ = 1;
    atLineStart_ = true;    // C-style grammar: newlines are plain whitespace
}

void Lexer::skipToEol() {
    while (pos_ < src_.size() && !atEol(pos_)) {
        ++pos_;
        ++col_;
    }
}

void Lexer::processIndentation() {
    // C-style grammar: {...} blocks + ';' terminators replaced the
    // INDENT/DEDENT engine - leading whitespace is insignificant.
    while (pos_ < src_.size() && (src_[pos_] == ' ' || src_[pos_] == '\t'))
        advanceWs();
    atLineStart_ = false;
}

void Lexer::finishFile() {
    if (parenDepth_ > 0)
        diags_.report(line_, col_, "unclosed '(' / '[' at end of file");

    Token t;
    t.kind = Tok::Eof;
    t.line = line_;
    t.col = col_;
    queue_.push_back(std::move(t));
    eofDone_ = true;
}

void Lexer::fill() {
    while (queue_.empty() && !eofDone_) {
        if (pos_ >= src_.size()) {
            finishFile();
            continue;
        }
        if (atLineStart_ && parenDepth_ == 0) {
            processIndentation();
            continue;
        }
        skipInlineWs();
        if (pos_ >= src_.size()) {
            finishFile();
            continue;
        }
        char c = peek();
        if (atEol(pos_)) {
            consumeEol();
            continue;
        }
        if (c == '#') {
            skipToEol();
            continue;
        }
        if (c == '\\' && atEol(pos_ + 1)) {           // explicit continuation
            ++pos_;
            ++col_;
            consumeEolNoNewline();
            continue;
        }
        if (fsMode_ == FsMode::Expr && specActive_ && inner_ == 0 && fbrace_ == 0) {
            // Format spec: raw chars until '}' / quote / EOL.
            while (pos_ < src_.size() && peek() != '}' &&
                   peek() != '"' && !atEol(pos_)) {
                specBuf_.push_back(src_[pos_]);
                ++pos_;
                ++col_;
            }
            Token sp;
            sp.kind = Tok::FStringSpec;
            sp.text = specBuf_;
            sp.line = line_;
            sp.col = col_;
            stampExtent(sp);
            specBuf_.clear();
            specActive_ = false;
            queue_.push_back(std::move(sp));
            if (peek() == '}') {
                Token rb;
                rb.kind = Tok::FStringRBrace;
                rb.text = "}";
                rb.line = line_;
                rb.col = col_;
                stampExtent(rb);
                queue_.push_back(std::move(rb));
                fsMode_ = FsMode::Text;
                ++pos_;
                ++col_;
            } else {
                if (atEol(pos_) || pos_ >= src_.size())
                    diags_.report(line_, col_, "unterminated f-string literal");
                Token et;
                et.kind = Tok::FStringEnd;
                et.text = "\"";
                et.line = line_;
                et.col = col_;
                queue_.push_back(std::move(et));
                fsMode_ = FsMode::None;
                if (peek() == '"') { ++pos_; ++col_; }
            }
            lineHasToken_ = true;
            continue;
        }
        Token t = lexToken();
        stampExtent(t);
        queue_.push_back(std::move(t));
        lineHasToken_ = true;
    }
}

void Lexer::consumeEolNoNewline() {                   // continuation: no NEWLINE token
    if (peek() == '\r') {
        ++pos_;
        if (peek() == '\n') ++pos_;
    } else {
        ++pos_;
    }
    ++line_;
    col_ = 1;
}

Token Lexer::take() {
    Token t = std::move(queue_.front());
    queue_.pop_front();
    return t;
}

std::vector<Token> Lexer::lexAll() {
    std::vector<Token> out;
    // skip a UTF-8 byte-order mark if present
    if (pos_ + 2 < src_.size() && static_cast<unsigned char>(src_[pos_]) == 0xEF &&
        static_cast<unsigned char>(src_[pos_ + 1]) == 0xBB &&
        static_cast<unsigned char>(src_[pos_ + 2]) == 0xBF)
        pos_ += 3;
    for (;;) {
        fill();
        out.push_back(take());
        if (out.back().kind == Tok::Eof) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Token scanners
// ---------------------------------------------------------------------------

Token Lexer::lexToken() {
    if (fsMode_ == FsMode::Text) return scanFText();

    char c = peek();
    if (isIdentStart(c)) return scanIdentOrPrefixedString();
    if (isDec(c)) return scanNumber();
    if (c == '\'') return scanChar();
    if (c == '"') {
        ++pos_;
        ++col_;
        return scanString(0, line_, col_ - 1);
    }

    Token t = scanOperator();
    if (fsMode_ == FsMode::Expr) t = adjustFExpr(std::move(t));
    return t;
}

Token Lexer::scanIdentOrPrefixedString() {
    Token t;
    t.line = line_;
    t.col = col_;
    std::string id;
    id.push_back(src_[pos_]);
    ++pos_;
    ++col_;
    while (pos_ < src_.size() && isIdentCont(src_[pos_])) {
        id.push_back(src_[pos_]);
        ++pos_;
        ++col_;
    }

    if (peek() == '"' && (id == "r" || id == "b" || id == "c")) {
        char prefix = id[0];
        ++pos_;
        ++col_;
        Token s = scanString(prefix, t.line, t.col);
        return s;
    }
    if (peek() == '"' && id == "f") {
        ++pos_;
        ++col_;
        t.kind = Tok::FStringStart;
        t.text = "f\"";
        fsMode_ = FsMode::Text;
        return t;
    }

    t.kind = Tok::Ident;
    t.text = std::move(id);
    return t;
}

Token Lexer::scanNumber() {
    Token t;
    t.line = line_;
    t.col = col_;

    auto eat = [&](bool (*pred)(char)) {
        bool any = false;
        while (pos_ < src_.size()) {
            char c = peek();
            if (pred(c)) {
                t.text.push_back(src_[pos_]);
                ++pos_;
                ++col_;
                any = true;
            } else if (c == '_') {
                t.text.push_back(src_[pos_]);
                ++pos_;
                ++col_;
            } else {
                break;
            }
        }
        return any;
    };

    if (peek() == '0') {
        char n = static_cast<char>(std::tolower(static_cast<unsigned char>(peek(1))));
        if (n == 'x' || n == 'o' || n == 'b') {
            t.text.push_back(src_[pos_]);               // 0
            ++pos_;
            ++col_;
            t.text.push_back(src_[pos_]);               // x | o | b
            ++pos_;
            ++col_;
            bool (*pred)(char) = n == 'x' ? isHex : (n == 'o' ? isOct : isBin);
            if (!eat(pred)) diagHere("numeric literal needs at least one digit");
            if (t.text.back() == '_')
                diags_.report(line_, col_, "trailing '_' in numeric literal");
            if (pos_ < src_.size() && (isIdentStart(peek()) || isDec(peek())))
                diags_.report(line_, col_, "invalid character in numeric literal");
            t.kind = Tok::Int;
            return t;
        }
    }

    eat(isDec);
    bool isFloat = false;
    if (peek() == '.' && isDec(peek(1))) {
        isFloat = true;
        t.text.push_back(src_[pos_]);
        ++pos_;
        ++col_;
        eat(isDec);
    }
    if (peek() == 'e' || peek() == 'E') {
        Pos save{pos_, line_, col_};
        std::string exp;
        exp.push_back(src_[pos_]);
        ++pos_;
        ++col_;
        if (peek() == '+' || peek() == '-') {
            exp.push_back(src_[pos_]);
            ++pos_;
            ++col_;
        }
        if (pos_ < src_.size() && isDec(peek())) {
            isFloat = true;
            t.text += exp;
            eat(isDec);
        } else {
            pos_ = save.p;
            line_ = save.l;
            col_ = save.c;
        }
    }

    if (t.text.back() == '_')
        diags_.report(line_, col_, "trailing '_' in numeric literal");
    if (!isFloat && pos_ < src_.size() &&
        (isIdentStart(peek()) || (peek() == '.' && isDec(peek(1)))))
        diags_.report(line_, col_, "invalid suffix on numeric literal");

    t.kind = isFloat ? Tok::Float : Tok::Int;
    return t;
}

Token Lexer::scanChar() {
    Token t;
    t.kind = Tok::Char;
    t.line = line_;
    t.col = col_;
    ++pos_;                                             // opening '
    ++col_;
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == '\'') {
            ++pos_;
            ++col_;
            return t;
        }
        if (atEol(pos_)) break;
        if (c == '\\') {
            t.text.push_back(src_[pos_]);
            ++pos_;
            ++col_;
            if (pos_ < src_.size()) {
                char e = src_[pos_];
                t.text.push_back(e);
                ++pos_;
                ++col_;
                if (e == 'u' && peek() == '{') {
                    t.text.push_back(src_[pos_]);
                    ++pos_;
                    ++col_;
                    while (pos_ < src_.size() && peek() != '}') {
                        t.text.push_back(src_[pos_]);
                        ++pos_;
                        ++col_;
                    }
                    if (peek() == '}') {
                        t.text.push_back('}');
                        ++pos_;
                        ++col_;
                    } else {
                        diagHere("unterminated \\u{...} escape");
                    }
                }
            }
        } else {
            t.text.push_back(src_[pos_]);
            ++pos_;
            ++col_;
        }
    }
    diagHere("unterminated character literal");
    return t;
}

Token Lexer::scanString(char prefix, uint32_t sl, uint32_t sc) {
    Token t;
    t.kind = prefix == 'r' ? Tok::StrRaw
           : prefix == 'b' ? Tok::StrByte
           : prefix == 'c' ? Tok::StrC
                           : Tok::StrNormal;
    t.line = sl;
    t.col = sc;
    bool raw = prefix == 'r';

    while (pos_ < src_.size()) {
        char c = peek();
        if (c == '"') {
            ++pos_;
            ++col_;
            return t;
        }
        if (atEol(pos_)) break;
        if (c == '\\' && !raw) {
            t.text.push_back(src_[pos_]);
            ++pos_;
            ++col_;
            if (pos_ >= src_.size()) break;
            char e = src_[pos_];
            t.text.push_back(e);
            ++pos_;
            ++col_;
            if (e == 'u') {
                if (peek() != '{') {
                    diags_.report(line_, col_, "\\u escape requires '{hex}' form");
                    continue;
                }
                t.text.push_back('{');
                ++pos_;
                ++col_;
                while (pos_ < src_.size() && peek() != '}' && peek() != '"') {
                    t.text.push_back(src_[pos_]);
                    ++pos_;
                    ++col_;
                }
                if (peek() == '}') {
                    t.text.push_back('}');
                    ++pos_;
                    ++col_;
                } else {
                    diags_.report(line_, col_, "unterminated \\u{...} escape");
                }
            }
        } else {
            t.text.push_back(src_[pos_]);
            ++pos_;
            ++col_;
        }
    }
    diags_.report(sl, sc, "unterminated string literal");
    return t;
}

Token Lexer::scanFText() {
    auto textTok = [&](std::string s) {
        Token t;
        t.kind = Tok::FStringText;
        t.text = std::move(s);
        t.line = line_;
        t.col = col_;
        return t;
    };
    auto endTok = [&]() {
        Token t;
        t.kind = Tok::FStringEnd;
        t.text = "\"";
        t.line = line_;
        t.col = col_;
        return t;
    };

    std::string run;
    while (pos_ < src_.size()) {
        char c = peek();
        if (c == '"') {
            ++pos_;
            ++col_;
            fsMode_ = FsMode::None;
            if (!run.empty())                           // text precedes End
                queue_.push_back(textTok(std::move(run)));
            return endTok();
        }
        if (atEol(pos_)) {
            diags_.report(line_, col_, "unterminated f-string literal");
            fsMode_ = FsMode::None;
            if (!run.empty())
                queue_.push_back(textTok(std::move(run)));
            return endTok();
        }
        if (c == '\\') {                                // keep escapes verbatim
            run.push_back(src_[pos_]);
            ++pos_;
            ++col_;
            if (pos_ < src_.size()) {
                run.push_back(src_[pos_]);
                ++pos_;
                ++col_;
            }
            continue;
        }
        if (c == '{') {
            if (peek(1) == '{') {                       // {{ -> literal {
                run.push_back('{');
                pos_ += 2;
                col_ += 2;
                continue;
            }
            Token lb;
            lb.kind = Tok::FStringLBrace;
            lb.text = "{";
            lb.line = line_;
            lb.col = col_;
            ++pos_;
            ++col_;
            fsMode_ = FsMode::Expr;
            fbrace_ = 0;
            inner_ = 0;
            specActive_ = false;
            specBuf_.clear();
            if (run.empty()) return lb;
            queue_.push_back(textTok(std::move(run)));
            return lb;
        }
        if (c == '}') {
            if (peek(1) == '}') {                       // }} -> literal }
                run.push_back('}');
                pos_ += 2;
                col_ += 2;
                continue;
            }
            diags_.report(line_, col_, "stray '}' in f-string literal");
            ++pos_;
            ++col_;
            continue;
        }
        run.push_back(src_[pos_]);
        ++pos_;
        ++col_;
    }

    diags_.report(line_, col_, "unterminated f-string literal");
    fsMode_ = FsMode::None;
    return endTok();
}

Token Lexer::adjustFExpr(Token t) {
    if (t.kind != Tok::Punct && t.kind != Tok::Op) return t;
    const std::string& x = t.text;

    if (x == "{") {
        ++fbrace_;
        return t;
    }
    if (x == "}") {
        if (fbrace_ > 0) {
            --fbrace_;
            return t;
        }
        fsMode_ = FsMode::Text;
        if (specActive_) {                              // close spec + interp
            Token spec;
            spec.kind = Tok::FStringSpec;
            spec.text = specBuf_;
            spec.line = t.line;
            spec.col = t.col;
            specBuf_.clear();
            specActive_ = false;
            Token rb;
            rb.kind = Tok::FStringRBrace;
            rb.text = "}";
            rb.line = t.line;
            rb.col = t.col;
            queue_.push_back(std::move(rb));
            return spec;
        }
        t.kind = Tok::FStringRBrace;
        return t;
    }
    if (x == "(" || x == "[") {
        ++inner_;
        return t;
    }
    if (x == ")" || x == "]") {
        --inner_;
        return t;
    }
    if (x == ":" && fbrace_ == 0 && inner_ == 0 && !specActive_) {
        t.kind = Tok::FStringColon;                     // starts format spec
        specActive_ = true;
        return t;
    }
    return t;
}

Token Lexer::scanOperator() {
    Token t;
    t.line = line_;
    t.col = col_;

    for (std::string_view op : kOps3) {
        if (src_.compare(pos_, op.size(), op) == 0) {
            t.kind = Tok::Op;
            t.text.assign(op);
            pos_ += op.size();
            col_ += static_cast<uint32_t>(op.size());
            return t;
        }
    }
    for (std::string_view op : kOps2) {
        if (src_.compare(pos_, op.size(), op) == 0) {
            t.kind = Tok::Op;
            t.text.assign(op);
            pos_ += op.size();
            col_ += static_cast<uint32_t>(op.size());
            return t;
        }
    }
    char c = peek();
    if (kPunct1.find(c) != std::string_view::npos) {
        if (c == '(' || c == '[') ++parenDepth_;
        if ((c == ')' || c == ']') && parenDepth_ > 0) --parenDepth_;
        t.kind = Tok::Punct;
    } else if (kOp1.find(c) != std::string_view::npos) {
        t.kind = Tok::Op;
    } else {
        diags_.report(line_, col_, std::string("unexpected character '") + c + "'");
        ++pos_;
        ++col_;
        t.kind = Tok::Punct;
        t.text = "?";
        return t;
    }
    t.text.push_back(c);
    ++pos_;
    ++col_;
    return t;
}

} // namespace kirn
