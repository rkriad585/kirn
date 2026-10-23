---
title: Lexical structure
description: Learn Kirn identifiers, comments, keywords, and literals.
---

# Lexical structure

Kirn source files use the `.kn` extension. Whitespace is not significant; blocks use braces and simple statements normally end with semicolons.

## Identifiers

Identifiers begin with a letter or underscore and may contain numbers after the first character.

```kirn
user_name = "kirn";
_count = 1;
```

## Comments

A comment begins with `#` and continues to the end of the line.

```kirn
# This is a comment.
value = 42;
```

## Keywords

The lexer currently recognizes keywords including `fn`, `var`, `if`, `elif`, `else`, `while`, `for`, `in`, `return`, `match`, `struct`, `enum`, `trait`, `impl`, `import`, `export`, `pub`, `spawn`, `chan`, `select`, `try`, `catch`, `unsafe`, `extern`, `class`, `interface`, `record`, and `extends`.

The authoritative list is `src/lex/lexer.cpp`; syntax is specified in `grammar/kirn.ebnf`.

