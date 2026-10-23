---
title: Quick start
description: Write and run your first Kirn program.
---

# Quick start

Create a file named `hello.kn`:

```kirn
fn main() {
    name = "Kirn";
    print(f"Hello, {name}!");
}
```

Run it with the Kirn driver:

```bash
kirn run hello.kn
```

## A small function

Kirn programs can describe a value with a function and return it to the caller:

```kirn
fn greet(name: Str) -> Str {
    f"Hello, {name}!"
}

fn main() {
    message = greet("builder");
    print(message);
}
```

The exact set of built-ins and accepted syntax follows the compiler in the source repository. When an example differs from the current compiler, trust a runnable example and report the mismatch.

## Where to go next

- [Variables and values](/learn/basics/variables)
- [Functions and control flow](/learn/basics/functions)
- [Project structure](/guide/project)

