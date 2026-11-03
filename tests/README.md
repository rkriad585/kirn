# Kirn Tests (`tests/`)

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

Linux/MacOS/Termux:

```sh
curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh
```

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/rkriad585/kirn/main/installer.ps1 | iex
```

The acceptance suites that validate the compiler against the normative grammar
and expected semantics.

## Layout

| Directory | Contents |
|-----------|----------|
| `tests/negative/` | `n*.kn` — programs that **must fail** to compile, each pinned to one specific error (golden diagnostics); `run.ps1` drives checks |
| `tests/types/` | `n*.kn` / `p*.kn` — type-level negative (borrow, exhaustiveness, default-type) and positive cases |
| `tests/conventions/` | Convention-focused cases |

The stdlib acceptance tests live with each module under
[`stdlib/pat/*_test.kn`](../stdlib/README.md), and `examples/*.kn` form the
positive parse/run corpus.

## Running

```powershell
powershell -File scripts/negative.ps1   # negative suite
powershell -File scripts/types.ps1      # type suite
powershell -File scripts/runall.ps1     # full corpus
```

Rule: any grammar or semantic change must keep every `n*.kn` failing (with the
pinned diagnostic) and every positive case passing.
