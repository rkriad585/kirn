---
title: Functions and control flow
description: Define functions and compose expressions in Kirn.
---

# Functions and control flow

Functions use `fn`, a name, parameters, and a body. A final expression can be returned from a block.

```kirn
fn classify(value: Int) -> Str {
    if value < 0 {
        "negative"
    } elif value == 0 {
        "zero"
    } else {
        "positive"
    }
}
```

Kirn includes `if`, `elif`, and `else`, plus `while` and `for` loops. Ranges use `..` and `..=` forms.

```kirn
for number in 1..=5 {
    print(number);
}
```

Closures can capture values from the surrounding scope. See the verified examples in `examples/06_functions_params.kn` and `examples/07_closures_captures.kn`.

