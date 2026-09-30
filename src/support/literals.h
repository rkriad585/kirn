#pragma once
// Literal decoding shared by everything that reads a value out of an
// ast::Expr/ast::Pat.
//
// Why this exists: `ast::Expr` stores a numeric literal only as `text`, because
// the lexer consumed the suffix/base syntax and the AST does not model a typed
// literal payload. Every consumer therefore has to re-parse the spelling. That
// had already produced two divergent copies -- `parseInt` in src/vm/compiler.cpp
// and `parseIntText` in src/interp/runtime.cpp -- and HIR would have been the
// third. One implementation, in the layer that exists for shared helpers.
//
// This is deliberately in support/ rather than lex/ or interp/: the value
// belongs to no one stage, and putting it under interp/ would make the front end
// depend on the runtime, which is the dependency direction this codebase has
// been careful to keep one-way.

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace kirn {

// Decimal unless the text carries a 0x / 0b / 0o radix prefix. `_` separators
// are stripped, matching the lexer's acceptance of `1_000_000`.
inline int64_t parseIntLiteral(const std::string& raw) {
    std::string t;
    t.reserve(raw.size());
    for (char c : raw)
        if (c != '_') t += c;
    int base = 10;
    if (t.size() > 2 && t[0] == '0') {
        char p = (char)tolower((unsigned char)t[1]);
        if (p == 'x') { base = 16; t = t.substr(2); }
        else if (p == 'b') { base = 2; t = t.substr(2); }
        else if (p == 'o') { base = 8; t = t.substr(2); }
    }
    return (int64_t)strtoll(t.c_str(), nullptr, base);
}

inline double parseFloatLiteral(const std::string& raw) {
    std::string t;
    t.reserve(raw.size());
    for (char c : raw)
        if (c != '_') t += c;
    return strtod(t.c_str(), nullptr);
}

}  // namespace kirn
