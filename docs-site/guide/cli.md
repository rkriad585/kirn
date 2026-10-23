---
title: CLI workflow
description: Use the Kirn command-line driver to run, test, build, and manage projects.
---

# CLI workflow

The `kirn` driver is the main entry point.

| Command | Purpose |
| --- | --- |
| `kirn run file.kn` | Run a program |
| `kirn new name` | Scaffold a project |
| `kirn test` | Run tests |
| `kirn build` | Build a program |
| `kirn install` | Install a package |
| `kirn add` | Add a dependency |
| `kirn update` | Update dependencies |
| `kirn remove` | Remove a dependency |
| `kirn doc` | Serve Markdown and API documentation |

## A normal loop

```bash
kirn new hello
cd hello
kirn run code/main.kn
kirn test
kirn build --release
```

Build options include `--debug`, `--release`, `--target`, `--native`, `--asan`, `-S`, `-O`, and `-o`. Run `kirn` without arguments for the current help output.

