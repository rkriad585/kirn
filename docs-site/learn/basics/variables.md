---
title: Variables and values
description: Bind values and use type annotations in Kirn.
---

# Variables and values

Kirn supports inferred values and explicit type annotations. Bindings are immutable by default; use the language's mutable-binding forms only when the program needs them.

```kirn
count: Int = 3;
ratio: Float = 0.5;
label: Str = "ready";
enabled: Bool = true;
```

Literal values include numbers, strings, booleans, characters, lists, dictionaries, and sets. The canonical primitive names are capitalized:

```kirn
age = 25;
title = "A small program";
scores = [10, 20, 30];
```

Use the [Types reference](/reference/language/types) for the current primitive and container families.

