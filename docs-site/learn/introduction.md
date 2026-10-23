---
title: Introduction
description: What Kirn is, who it is for, and how to read the documentation.
---

# Introduction

Kirn is a general-purpose programming language created by [RK Riad Khan](https://rkriad585.github.io/). Its design aims to keep readable source, explicit structure, and a compiled toolchain close together.

The language is open source and actively evolving. The most useful way to understand it is to run the examples in the [official repository](https://github.com/rkriad585/kirn), then use these docs as a map.

## What Kirn gives you

- Familiar, expression-oriented syntax for small programs and larger systems.
- Strong types, structs, enums, traits, generics, and optionals.
- Functions, closures, modules, pattern matching, and error propagation.
- Concurrency primitives including `spawn`, channels, `select`, and `join`.
- A CLI for running, testing, building, packaging, and documenting projects.

Kirn is not finished. Claims about performance, ownership, networking, IDE support, or future tooling should be treated as roadmap material unless they are backed by the source and a runnable example.

## Choose your path

- **New to Kirn?** Continue to [Installation](/learn/installation), then [Quick start](/learn/quick-start).
- **Know the basics?** Read the [CLI workflow](/guide/cli) and [Project structure](/guide/project).
- **Looking for exact syntax?** Open the [Language reference](/reference/language/lexical).
- **Looking for packages?** Visit the [Pets registry](https://pets-registry.github.io/).

## Source of truth

The normative grammar lives in `grammar/kirn.ebnf`. The canonical keyword set is maintained by the lexer. Examples in `examples/` are the working corpus used to validate language behavior.

