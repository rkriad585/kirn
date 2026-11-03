# Kirn Tooling (`tools/`)

<p align="center">
  <img src="https://raw.githubusercontent.com/rkriad585/kirn/main/kirn-logos/kirn-icon-on-light.png" alt="Kirn logo" width="180">
</p>

## Install Kirn

npm:

```sh
npm install -g @kirn-lang/kirn
```

npx:

```sh
npx @kirn-lang/kirn run examples/01_hello.kn
```

PyPI:

```sh
python -m pip install kirn
```

Chocolatey:

```powershell
choco install kirn --version 0.0.1-b3 --yes
```

Linux and macOS:

```sh
curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/rkriad585/kirn/main/installer.ps1 | iex
```

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

The remaining `*.kn` / `scratch_*` / `t?.kn` / `u?.kn` files and `out.txt` /
`err.txt` are ad-hoc developer scratch; they are not part of the public
interface.
