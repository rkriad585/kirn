---
title: Syntax and operators
description: Explore Kirn expressions, statements, patterns, and operators.
---

# Syntax and operators

Kirn uses braces for blocks and semicolons for simple statements. Expressions can appear where a value is expected, including conditional and match expressions.

## Operators

The grammar includes arithmetic, comparison, boolean, range, assignment, bitwise, and propagation operators.

```text
+  -  *  /  %       ==  !=  <  <=  >  >=
and  or  not        ..  ..=      ?.  .?.
->  <-              +=  -=  *=  /=  %= 
```

Operator precedence is defined by `grammar/kirn.ebnf`. When in doubt, use parentheses and consult the grammar rather than relying on a language from memory.

## Pattern matching

```kirn
match value {
    case 0 => "zero";
    case n if n > 0 => "positive";
    case _ => "negative";
}
```

Patterns, guards, destructuring, and ranges are covered by the example corpus.

