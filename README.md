<p align="center">
  <img src="kirn-logos/kirn-logo.png" alt="Kirn logo" width="180">
</p>

<h1 align="center">Kirn Programming Language</h1>

<p align="center"><em>A modern, expressive general-purpose programming language built in the open.</em></p>

<p align="center">
  <img src="https://img.shields.io/badge/status-WIP-informational" alt="Status: work in progress">
  <img src="https://img.shields.io/badge/version-0.0.1--beta-orange" alt="Version: 0.0.1-beta">
  <img src="https://img.shields.io/badge/license-MIT%20%2F%20Apache--2.0-blue" alt="License: MIT or Apache-2.0">
  <img src="https://img.shields.io/badge/made%20by-rkriad585-111827" alt="Made by rkriad585">
</p>

<p align="center">
  <a href="https://kirn-lang.github.io/">Website</a> ·
  <a href="https://rkriad585.github.io/kirn/">Documentation</a> ·
  <a href="https://pets-registry.github.io/">Pets registry</a> ·
  <a href="https://github.com/rkriad585/kirn">Source</a>
</p>

> **Work in progress:** Kirn is actively developed and incomplete. The language,
> compiler, standard library, package workflow, and documentation may change.
> Treat features marked planned as goals, not guarantees.

## Overview

Kirn is a general-purpose programming language created by **RK Riad Khan**
([rkriad585](https://github.com/rkriad585)). It keeps readable, high-level source
close to explicit types, structured data, concurrency, and a compiled toolchain.

The compiler is written in modern C++ and includes a lexer, parser, semantic
analysis, interpreter/VM work, and an evolving native backend. Kirn is a public
laboratory for language design, compiler engineering, and developer tools.

## A taste of Kirn

Kirn uses braces for blocks, semicolons for simple statements, `#` comments,
and optional type annotations with inference:

~~~kirn
struct Greeter {
    name: Str;
}

fn greet(person: Greeter) -> Str {
    f"Hello, {person.name}!"
}

fn main() {
    person = Greeter(name: "world");
    print(greet(person));
}
~~~

The `examples/` directory is the best executable tour of the language.
Examples cover expressions, functions, closures, patterns, structs, enums,
traits, generics, collections, optionals, results, concurrency, FFI, value
semantics, generators, and object-oriented syntax.

## Current status

Kirn is under active development:

- **Implemented and exercised:** lexer/parser pipeline, functions, control flow,
  closures, ranges, pattern matching, structs, enums, traits, generics,
  collections, optionals, results, `defer__, `panic__, and concurrency primitives.
- **Available in examples:** `spawn__, `chan__, `select__, `join__, `yield__,
  `class__, `interface__, `record__, `extends__, dynamic values,
  FFI, and value-semantics experiments. Verify behavior against the compiler.
- **Still evolving:** native compilation, packaging, release targets, IDE/LSP
  support, debugging, networking libraries, and stable public APIs.

Performance comparisons, safety claims, and compatibility promises should be
backed by a reproducible example or benchmark. See the [official
documentation](https://rkriad585.github.io/kirn/) for verified behavior.

## Getting started

### Prerequisites

- A C++20-capable compiler.
- [CMake](https://cmake.org/) 3.x or newer.
- Git and a suitable shell.

### Build the compiler

~~~bash
cmake -S . -B build
cmake --build build --config Debug
~~~

### Run a program

Create `main.kn`:

~~~kirn
fn main() {
    print("Hello, world!");
}
~~~

Run it and try the repository examples:

~~~bash
kirn run main.kn
kirn run examples/01_hello.kn
kirn test
~~~

## Language and compiler pipeline

The implementation is organized as:

~~~text
source (.kn)
   │
   ▼
lexer → parser → semantic analysis → interpreter / VM
                                      └──────────────→ native backend (evolving)
~~~

The normative syntax is maintained in [`grammar/kirn.ebnf`]. The canonical
keyword set is maintained by `src/lex/lexer.cpp`. When planning documents and
compiler behavior disagree, prefer the compiler, grammar, tests, and examples.

## Features

The project includes or explores:

- Functions, closures, captures, and higher-order programming.
- `if` expressions, loops, ranges, `match`, guards, and destructuring.
- Structs, enums, methods, traits, implementations, and generic bounds.
- Lists, dictionaries, sets, slices, iterators, and collection operations.
- Optional values with `T?` and nil-safe access with `.?.`.
- Result-style error propagation with `try`, plus `defer__, `panic__, and `catch`.
- Concurrency primitives including `spawn__, channels, `select__, and `join__.
- FFI and `unsafe` boundaries.
- Experimental classes, interfaces, records, inheritance, dynamic values,
  generators, compile-time forms, and decorators.

Experimental entries should be validated against their corresponding example
before use in a production project.

## Command-line interface

The `kirn` driver is the main project tool:

| Command | Purpose |
| --- | --- |
| `kirn run <file.kn>` | Run a program |
| `kirn new <name>` | Scaffold a project |
| `kirn new pets <name>` | Scaffold a Pet and release workflow |
| `kirn test` | Run project tests |
| `kirn build` | Build a program or package |
| `kirn targets` | List target shapes |
| `kirn install` | Install a package or Pet |
| `kirn add` | Add a dependency |
| `kirn update` | Update dependencies |
| `kirn remove` | Remove a dependency |
| `kirn clone` | Clone package source |
| `kirn doc` | Serve Markdown and API documentation |
| `kirn list` | List installed packages |
| `kirn list online` | Query registry packages |

Build options include `--debug__, `--release__, `--target__, `--native__,
`--asan__, `-S__, `-O__, and `-o__. Run `kirn` without arguments
for the current help output. Companion tools may include `kirnrun__, `kirncheck__,
`kirnlex__, and `kirnparse`.

## Standard library

The current source tree includes:

- `core` — primitive values and built-ins.
- `io` — file and input/output helpers.
- `math` — numeric functions such as square root, clamp, and powers.
- `strings` — search, split, join, replacement, padding, and stripping.
- `collections` — collection helpers and randomization utilities.
- `json` — JSON encoding and decoding.
- `os` and `path` — environment, process, and path operations.
- `regexp`, `time`, and `text/slug`.

Each module’s source and `_test.kn` files are the authority for signatures
and behavior.

## Packages, Pets, and dependency trust

Kirn calls reusable ecosystem packages **Pets**. Each Pet’s source and release
workflow live in its own repository, while the registry provides discovery and
installation metadata.

~~~bash
kirn install math-ext
kirn install github.com/user/my-pet@v1
kirn add my-pet
kirn update
kirn list
~~~

Packages may be installed into `./pets/` or `~/.kirn/pets`. A
`kirn.lock` file records resolved sources and commit information for
reproducible builds; commit it with your project.

Kirn currently has no sandbox or code-signing layer for third-party source.
Dependencies run with the privileges of the calling process. Review packages
before installing them, especially code using files, external processes,
networking, or `extern__/FFI. Prefer pinned tags or commit SHAs and inspect
lockfile changes during updates.

Read the [Pets publishing guide](https://pets-registry.github.io/pets/) to
create a Pet, validate it, publish a release, fork the registry, and open a PR.

## Repository structure

| Path | Purpose |
| --- | --- |
| `src/` | Compiler source: lexer, parser, sema, VM, interpreter, backend |
| `stdlib/pet/` | Standard-library modules and tests |
| `examples/` | Runnable language examples |
| `grammar/` | Normative EBNF grammar |
| `tools/` | CLI and companion tools |
| `scripts/` | Build and verification helpers |
| `docs/` | Design notes and feature analysis |
| `docs-site/` | VitePress documentation source |
| `KIRN_PLANS/` | Phased implementation plans |
| `kirn-logos/` | Kirn brand assets |

## Documentation and ecosystem

- [Kirn website](https://kirn-lang.github.io/)
- [Kirn documentation](https://rkriad585.github.io/kirn/)
- [Kirn language source](https://github.com/rkriad585/kirn)
- [Pets registry website](https://pets-registry.github.io/)
- [Pets contribution docs](https://pets-registry.github.io/pets)
- [Kirnly editor](https://kirnly.github.io/)
- [KirnOS](https://kirnos-kernel.github.io/)
- [RK Riad Khan](https://rkriad585.github.io/)
- [Grammar](grammar/kirn.ebnf)
- [Examples](examples/)
- [Design docs and roadmap](docs/)

## Roadmap

Detailed plans live in `KIRN_PLANS/`. Roadmap themes include a more
complete native backend, stable cross-platform targets, package and release
improvements, standard-library growth, editor integration, documentation
generation, and clearer versioning.

Roadmap items are proposals until implemented, tested, and documented as
supported behavior.

## Contributing

Read [`CONTRIBUTING.md`](`CONTRIBUTING.md`) before opening a pull request.
Good contributions are focused, reproducible, and accompanied by tests or a
runnable example when behavior changes. Documentation examples should be
checked with the current compiler, and claims should identify their source or
benchmark.

Also review:

- [`CODE_OF_CONDUCT.md`](`CODE_OF_CONDUCT.md`)
- [`SECURITY.md`](`SECURITY.md`)
- [`ACCESSIBILITY.md`](`ACCESSIBILITY.md`)
- [`CONTRIBUTORS.md`](`CONTRIBUTORS.md`)

## License

Kirn is dual-licensed under either:

- [Apache License 2.0](LICENSE-APACHE)
- [MIT License](LICENSE-MIT)

You may choose either license subject to the terms in its license file.

<p align="center">Made with care by <a href="https://github.com/rkriad585">rkriad585</a>.</p>
