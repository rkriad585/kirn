#pragma once
// Token model for the Kirn lexer (grammar/coco.ebnf §1).
#include <cstdint>
#include <string>
#include <string_view>

namespace kirn {

enum class Tok : uint8_t {
    Eof,
    Newline,
    Indent,
    Dedent,
    Ident,
    Int,
    Float,
    Char,
    StrNormal,
    StrRaw,
    StrByte,
    StrC,
    FStringStart,
    FStringText,
    FStringLBrace,
    FStringRBrace,
    FStringColon,
    FStringSpec,
    FStringEnd,
    Op,
    Punct,
};

const char* tokName(Tok t);

struct Token {
    Tok kind = Tok::Eof;
    uint32_t line = 1;
    uint32_t col = 1;
    // Inclusive end extent of the token (start line/col .. end line/col).
    // Filled by the lexer's production loop; 0 = not yet stamped (legacy).
    uint32_t endLine = 0;
    uint32_t endCol = 0;
    std::string text;

    bool is(Tok k) const { return kind == k; }
};

// Frozen v1 keyword set (grammar §1 KEYWORD). Identifiers are looked up
// against this by the parser; the lexer deliberately emits plain Ident.
bool isKeyword(std::string_view ident);

} // namespace kirn
