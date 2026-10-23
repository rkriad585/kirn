---
title: Project structure
description: Understand a Kirn project scaffold and kirn.toml.
---

# Project structure

`kirn new demo` creates a project with a manifest, source tree, tests, and documentation:

```text
demo/
├── kirn.toml
├── code/
│   └── main.kn
├── tests/
├── docs/
└── .gitignore
```

The manifest describes package metadata and dependencies:

```toml
[package]
name = "demo"
version = "0.1.0"
type = "bin"
main = "code/main.kn"
license = "MIT"

[dependencies]
```

Keep examples and tests close to the project. A package is easier to understand when its source, docs, and executable checks evolve together.

