# AGENTS.md — Coco repository guide

Coco is a WIP compiled language. **This is a Windows + PowerShell + C++20
codebase.** Do not assume POSIX tooling or Bash semantics.

## Build (CMake, C++20)

```powershell
cmake -S . -B build
cmake --build build --config Debug          # binaries in build\Debug\*.exe
```

- **Release** (needed for meaningful benchmarks): `cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release`
- **ASan**: `cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOCO_ASAN=ON`
  (or just run `scripts/asanall.ps1`, which configures/builds/runs it).
- New binaries must be added as `add_executable` targets in `CMakeLists.txt`,
  linked against the layered static libs (`coco_support → coco_lex → coco_ast →
  coco_parser → coco_sema → coco_vm → coco_interp`, plus `coco_backend`).
  Only the `coco` driver links `ws2_32`.

## The CLI tools

| Binary | File | Purpose |
|--------|------|---------|
| `coco` | `tools/coco.cpp` | Driver: `run/test/build/doc/install/new`; contains `frontEnd`, `resolveEntry`, `libDirsFor`, `resolveSource`, `collectImports` |
| `cococheck` | `tools/cococheck.cpp` | Type-check only (no run) |
| `cocolex` | `tools/cocolex.cpp` | Token dump (lexer oracle); `--dump` |
| `cocoparse` | `tools/cocoparse.cpp` | AST dump (parser oracle); `--ast` |
| `cocorun` | `tools/cocorun.cpp` | Run a program |

The bytecode VM is the **default runner**; `--no-vm` forces the tree-walker.

## Verification commands (the ones to actually run)

All scripts are PowerShell at the repo root. The runner paths are the Windows
build-layout `.exe`s. A fresh `build` (Debug) produces `build\Debug\x.exe`.

```powershell
# Example corpus — every examples/*.co must run. THE ground-truth gate.
scripts/runall.ps1 -Runner build\Debug\cocorun.exe

# Negative suite — each tests/negative/n*.co must FAIL cococheck and its
# `# expect:` comment substring must appear in the diagnostic output.
scripts/negative.ps1 -Runner build\Debug\cococheck.exe

# Type suite — tests/types/p*.co pass check+run; n*.co fail with `# expect:`.
scripts/types.ps1

# VM/tree-walker differential — the VM runner must match the tree-walker over the corpus.
scripts/vm_diff.ps1       # vm diff

# Conventions — main.co/pin.co entry resolution + package init run-once.
tests/conventions/run.ps1

# ASan corpus + native smoke test.
scripts/asanall.ps1

# Benchmarks (needs build-rel, honest perf claims live in this file's header).
scripts/bench.ps1
```

Stdlib and project tests run via `coco test` and target `*_test.co` files.

## Testing conventions

- **Negative tests** (`tests/negative/`, `tests/types/n*`) encode the expected
  diagnostic as an `# expect: <substring>` comment line; the harness asserts the
  command **exits nonzero** and the output contains that substring. Don't write
  a negative test without the `# expect:` line.
- **Examples** (`examples/*.co`) are the normative parse/run corpus. A few
  intentionally return nonzero via `# expect-exit: N` (e.g. a native
  `main() -> int`). The grammar (`grammar/coco.ebnf`) must always accept the
  whole corpus — the README rule is "any grammar change must be validated
  against examples before merge."
- Diagnostics use stable codes `E####`/`W####` (see `src/support/diag.h`).

## Architecture notes that aren't obvious from filenames

- `tools/coco.cpp` is more than a CLI: it hosts the reusable project/module
  model. Entry resolution order is `coco.toml [package] main` → `code/main.co`
  → `main.co` → `code/pin.co` → `pin.co`. A package's `pin.co` runs **once** as
  its initializer (see `tests/conventions/run.ps1`).
- `src/sema/symbols.h` `Symbol` carries declarations/signatures; the checker's
  `typeOf(expr)` returns resolved types; `src/support/diag.h` `Diag`/`FixIt` are
  already LSP-shaped (`SpanRange{line,col,endLine,endCol}`, 1-based inclusive).
- The COCO `PLAN` files in `COCO_PLANS/` are living architecture docs; some
  plans (e.g. `COCO_LSP_PLAN.md`, `COCO_HIGHLIGHT_PLAN.md`) are grounded in the
  exact compiler components above. Treat declared-but-unimplemented syntax in
  the plans as authoritative — **don't "fix" code to remove plan-declared
  features** (a user override has reverted such edits before).

## Repo conventions / gotchas

- Line endings are enforced by `.gitattributes`: `.co/.h/.cpp/.md/.ebnf` are LF,
  `.ps1/.bat` are CRLF. Don't fight the normalization or touch endianness of
  `.cocolib` (binary, never opened as source).
- Do not commit `tools/*.co` scratch files without reason — many (`scratch_*.co`,
  `t?.co`, `u?.co`, `out.txt`) are ad-hoc developer debris.
- No external dependencies beyond CMake + a C++20 compiler (matches the
  zero-dependency tooling philosophy). Don't introduce a framework where the
  C++ standard library suffices.
- MSVC is `/W4 /permissive-`; GCC/Clang are `-Wall -Wextra -Wpedantic`.
