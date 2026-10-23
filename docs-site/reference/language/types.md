---
title: Types
description: Reference Kirn primitive, container, optional, and result types.
---

# Types

Canonical primitive names are capitalized: `Int`, `UInt`, `Float`, `UFloat`, `Bool`, `Str`, `Char`, `BigInt`, and `Rational`.

Width families include forms such as `Int32`, `UInt64`, `Float32`, `i32`, `u64`, and `f64`. The grammar also includes `usize`, `isize`, character widths, and byte forms.

## Compound types

Kirn supports type forms for lists, dictionaries, sets, functions, optionals, and results:

```kirn
names: list<Str>;
lookup: dict<Str, Int>;
maybe: Int?;
work: fn(Int) -> Str;
```

Type syntax and accepted aliases can change before the grammar is frozen. The EBNF and checker are the source of truth.

