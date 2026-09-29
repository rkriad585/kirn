# Kirn Tooling (`tools/`)

Command-line tools and the project driver, built in C++ and linked against
[`src/`](../src/).

## CLI tools

| Tool | Source | Purpose |
|------|--------|---------|
| `kirn` | `kirn.cpp` | Primary driver: `run`, `test`, `build`, `doc`, `install/add/update/remove/clone`, project/entry and module resolution |
| `kirncheck` | `kirncheck.cpp` | Type-check / static analysis only |
| `kirnlex` | `kirnlex.cpp` | Dump tokens (lexer oracle) |
| `kirnparse` | `kirnparse.cpp` | Dump `--ast` (parser oracle) |
| `kirnrun` | `kirnrun.cpp` | Run a Kirn program directly |

`tools/kirn.cpp` also hosts the reusable project model used elsewhere in the
repo: `frontEnd` (lex → parse → check), `resolveEntry`, `libDirsFor`,
`resolveSource`, `collectImports`, and the `kirn.toml` manifest handling.

## Scratch / tests

The remaining `*.co` / `scratch_*` / `t?.co` / `u?.co` files and `out.txt` /
`err.txt` are ad-hoc developer scratch; they are not part of the public
interface.
