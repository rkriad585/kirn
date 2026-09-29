<p align="center">
  <!-- Theme-matched logo (Kirn brand accent) -->
  <img src="kirn-logos/kirn-logo.png" alt="Kirn logo" width="180" />
</p>

<h1 align="center">Kirn Programming Language</h1>

<p align="center">
  <em>A modern, expressive, and fast general-purpose programming language</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-WIP-informational" alt="Status: WIP" />
  <img src="https://img.shields.io/badge/version-0.0.1--beta-orange" alt="Version: 0.0.1-beta" />
  <img src="https://img.shields.io/badge/license-MIT-green" alt="License: MIT" />
</p>

<p align="center">
  <a href="https://kirn-lang.github.io">Website</a> ·
  <a href="https://pets-registry.github.io">Library Registry</a> ·
  <a href="https://github.com/rkriad585/kirn">Source</a>
</p>

> **⚠️ Work in progress.** Kirn is under active development and **not yet
> complete**. The language, standard library, tooling, and this documentation
> may change at any time. Expect breaking changes and unfinished features.

---

## Overview

**Kirn** is a general-purpose programming language being designed and built by
**RK Riad Khan** ([rkriad585](https://github.com/rkriad585)).

It aims to combine the _simplicity and expressiveness_ of a high-level language
with the _performance and control_ of a compiled language. Kirn is written as a
modern **AOT (ahead-of-time) compiler in C++20** with the following high-level
goals:

- **Performance** — target near-native speed through a compiled pipeline.
- **Simplicity** — clean, readable, Python-inspired syntax.
- **Safety** — a strong, expressive type system and explicit error handling.
- **Concurrency** — lightweight built-in concurrency primitives.

### A taste of Kirn

```kirn
struct Greeter {
    name: string;

    def greet(self) -> string {          # methods live inside the struct
        return "Hello, " + self.name + "!";
    }
}

def fib(n: int) -> int {                 # simple recursion
    if n <= 1 { return n; }
    return fib(n - 1) + fib(n - 2);
}

def main() {
    for i in 0..=5 {                     # inclusive range loop
        print(fib(i));
    }
    g = Greeter(name: "world");          # named-field construction
    print(g.greet());                    # value semantics by default
}
```

Kirn reads like Python but compiles to fast code: block-scoped `{}` + `;`
syntax with no significant indentation, `#` line comments, optional type
annotations with inference, pattern matching with guards, protocols/traits,
generics, optionals/`none`, `try`-based error handling, generators (`yield`),
OOP (`class`/`interface`/`record`/`extends`), and built-in concurrency
(`spawn`/`chan`/`select`).

> **Note on feature claims:** Kirn is incomplete. Features and capabilities
> described in this README may be planned rather than fully implemented. See
> the [docs](docs/) and [Implementations](#status) below for what currently
> exists.

---

## Status

Kirn is **under development and incomplete**. Key points:

- The compiler is **AOT** (ahead-of-time) — a JIT/back-end native pipeline is
  still being worked on.
- Core language features exist (functions, control flow, data types, structs,
  enums, traits, generics, options/results, concurrency, OOP, generators) and
  are covered by runnable [examples](examples/).
- The standard library and tooling are **in progress**.
- Versioning, packaging, and ecosystem are **not yet finalized** (currently
  pinned to `0.0.1-beta`).

---

## Getting Started

### Prerequisites

- A C++20-capable compiler
- [CMake](https://cmake.org/) ≥ 3.x

### Building from source

```bash
cmake -S . -B build
cmake --build build --config Debug
```

### Running a program

```kirn
print("Hello, World!");
```

```bash
kirn run main.kn
# Hello, World!
```

See the [examples](examples/) directory for a growing set of runnable programs.

---

## Features

> The following reflects the current implementation. Items marked _planned_ are
> goals, not yet complete.

- [x] Compiled interpreter pipeline (lexer → parser → semantic analysis)
- [x] Functions, closures, and higher-order functions
- [x] Ranges, loops, and `match`/guards
- [x] Structs, enums, traits/dispatch
- [x] Generics and collections
- [x] Optionals / `none` and Results / `try`
- [x] `defer` / `panic` error handling
- [x] Concurrency: `spawn`, `chan`, `select`
- [x] OOP: `class`, `interface`, `record`, `extends`
- [x] Generators / `yield`
- [ ] Full AOT native code generation (_planned_)
- [ ] JIT compilation (_planned_)
- [ ] Networking standard library (_planned_)
- [ ] IDE / debugger integration (_planned_)

---

## Command-Line Interface

The `kirn` driver provides multiple subcommands:

| Command                                                | Description                                      |
| ------------------------------------------------------ | ------------------------------------------------ |
| `kirn run`                                             | Run a Kirn program                               |
| `kirn new`                                             | Scaffold a new project / library                 |
| `kirn test`                                            | Run tests                                        |
| `kirn build`                                           | Build a program (release/debug, targets, native) |
| `kirn install` / `add` / `update` / `remove` / `clone` | Package management                               |
| `kirn doc`                                             | Generate documentation                           |
| `kirn list` / `list online`                            | List packages                                    |

Companion tools: `kirnrun`, `kirncheck`, `kirnlex`, `kirnparse`.

Run `kirn --help` for full usage.

---

## Standard Library

Kirn ships a growing standard library. Current modules (each with a matching
`_test.kn`):

- `core`, `io`, `math`, `strings`, `collections`
- `json`, `os`, `path`, `regexp`, `time`, `text/slug`

---

## Packages, Libraries & Trust

Kirn has a lightweight library ecosystem. In addition to the bundled
[standard library](#standard-library), you can install third-party packages from:

- **The official library registry:** [`github.com/pets-registry/pets`](https://pets-registry.github.io)
- Any Git repository (`github.com/user/repo` or `user/repo` shorthand), or a
  local path / `.pet` bundle.

### Using libraries

```bash
kirn install math-ext                    # resolve a bare name through the registry
kirn install github.com/user/mylib@v1   # pin a git tag/commit
kirn add mylib                          # add + record a dependency
kirn update                             # refresh installed dependencies
kirn list / kirn list online            # show installed / registry packages
```

Installed packages go into `./pets/` (project-local) or `~/.kirn/pets`
(global). Each resolved dependency is recorded in **`kirn.lock`**, which pins
the exact source and commit SHA so builds are reproducible — **commit
`kirn.lock`** with your project.

### Verification & security

> ⚠️ **Kirn currently has no sandbox or code-signing.** Libraries are fetched
> from Git and run with the same privileges as your user.

- Every dependency records its **commit SHA** in `kirn.lock`, so what you audit
  today is what you build tomorrow. Check `kirn.lock` diffs when you run
  `kirn update`.
- Installed packages are plain Kirn source — **review the code before using a
  third-party library**, especially anything handling untrusted input, network,
  files, or `extern`/FFI.
- Prefer registry packages you can inspect and pin exact versions/tags over
  floating references.

---

## Project Structure

| Path          | Description                                                               |
| ------------- | ------------------------------------------------------------------------- |
| `src/`        | Compiler source (lexer → parser → sema → VM/interp → native backend)      |
| `stdlib/pet/` | Standard library modules                                                  |
| `examples/`   | Runnable example programs (01–44)                                         |
| `grammar/`    | Normative EBNF grammar                                                    |
| `tools/`      | CLI tool sources (`kirn`, `kirnrun`, `kirncheck`, `kirnlex`, `kirnparse`) |
| `scripts/`    | Build/test/verification harness scripts                                   |
| `docs/`       | Design and roadmap documents                                              |
| `kirn-logos/` | Brand assets                                                              |

---

## Documentation & Community

- **Website:** [kirn-lang.github.io](https://kirn-lang.github.io)
- [Design docs & roadmap](docs/)
- [Grammar (EBNF)](grammar/kirn.ebnf)
- [Examples](examples/)
- [Plans](COCO_PLANS/)
- **Library registry:** [pets-registry.github.io](https://pets-registry.github.io)

---

## Roadmap

Detailed, phased implementation plans live in [COCO_PLANS/](COCO_PLANS/),
covering the documentation website, language features, the standard library,
and cross-compilation. The Coco → Kirn rebrand is recorded in
[COCO_TO_KIRN_PLAN.md](COCO_PLANS/COCO_TO_KIRN_PLAN.md).

---

## License

Kirn is open-source software released under the [MIT License](LICENSE.txt).

---

<p align="center">
  Made with ❤️ by <a href="https://github.com/rkriad585">rkriad585</a>
</p>
