---
title: Build targets
description: Understand Kirn bytecode, native builds, and supported target shapes.
---

# Build targets

Kirn's toolchain can run source directly, produce bytecode bundles, and build native artifacts. The exact backend and target support are under active development.

```bash
kirn build --debug
kirn build --release
kirn build --native
kirn targets
```

The project tracks platform families across Windows, Linux, and Darwin, with amd64 and arm64 target shapes. Treat a target as supported only when the current CI or release workflow proves it.

## Output forms

- `.bkn` — bytecode bundle.
- `.kirnlib` — library package.
- Native executable output — platform-specific executable.

For compiler architecture, read the source pipeline from lexer and parser through semantic analysis, VM, interpreter, and native backend.

