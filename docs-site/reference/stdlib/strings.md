---
title: strings
description: String operations in the Kirn standard library.
---

# `strings`

String helpers include searching, replacing, splitting, joining, repetition, reversing, padding, and stripping.

```kirn
clean = strip("  kirn  ");
parts = split(clean, " ");
joined = join(parts, "-");
```

The exact signatures are maintained in the source-backed module and its tests.

