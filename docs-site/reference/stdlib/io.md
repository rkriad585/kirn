---
title: io
description: Input and output helpers in the Kirn standard library.
---

# `io`

The `io` module provides file and stream operations used by programs that interact with the local environment.

Common source-backed operations include reading and writing files. Check the module tests for accepted arguments, error behavior, and platform details.

```kirn
text = read_file("notes.txt");
write_file("copy.txt", text);
```

