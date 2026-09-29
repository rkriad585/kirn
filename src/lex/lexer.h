#pragma once
// Indentation-aware lexer implementing grammar/kirn.ebnf §1:
// INDENT/DEDENT synthesis (CPython algorithm), frozen keyword/operator sets,
// int/float/char/string literals (raw r"" byte b"" C c""), and f-strings
// lexed as Start / Text / {L,R}Brace / Colon / Spec / End token runs.
#include "support/diag.h"
#include "lex/token.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace kirn {

enum class FsMode : uint8_t { None, Text, Expr };

class Lexer {
public:
    Lexer(std::string_view source, std::string filename, DiagEngine& diags);

    std::vector<Token> lexAll();

private:
    void fill();                 // ensure queue_ has >= 1 token or EOF reached
    Token take();                // pop queue_ front (assumes fill())
    void processIndentation();   // at-line-start width handling
    void consumeEol();           // eat \r\n | \n | \r (may emit Newline)
    void consumeEolNoNewline();  // continuation lines: no Newline token
    void advanceWs();            // one whitespace char, col tracking only
    void skipInlineWs();
    void skipToEol();
    bool atEol(size_t p) const;
    void finishFile();           // flush pending Newline/DEDENT*/EOF

    Token lexToken();            // dispatch per current mode
    Token scanIdentOrPrefixedString();
    Token scanNumber();
    Token scanChar();
    Token scanString(char prefix, uint32_t sl, uint32_t sc); // 0 | 'r' | 'b' | 'c'
    Token scanFText();
    Token adjustFExpr(Token t);          // post-process a token inside {...}
    Token scanOperator();

    char peek(size_t off = 0) const;
    bool match(char c);
    void diagHere(std::string msg);
    // Stamps endLine/endCol on t from the current lexer position (the
    // position just past the token's last character). Exact for
    // single-token scans; an inclusive upper bound for multi-token runs.
    void stampExtent(Token& t);

    std::string_view src_;
    std::string filename_;
    DiagEngine& diags_;

    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;

    std::vector<uint32_t> indents_{0};
    bool atLineStart_ = true;
    bool lineHasToken_ = false;
    int parenDepth_ = 0;                 // (), [] suppress NEWLINE/indent logic

    FsMode fsMode_ = FsMode::None;
    int fbrace_ = 0;                     // nested {} depth inside interpolation
    int inner_ = 0;                      // ([ nesting inside interpolation
    bool specActive_ = false;
    std::string specBuf_;

    std::deque<Token> queue_;
    bool eofDone_ = false;
};

} // namespace kirn
