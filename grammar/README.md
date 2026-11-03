# Kirn Grammar (`grammar/`)

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

The normative, machine-checkable grammar of the Kirn language.

| File | Description |
|------|-------------|
| `grammar/kirn.ebnf` | The authoritative EBNF grammar (ISO EBNF: `=` definition, `;` terminator, `[ ]` optional, `{ }` repatition, `(* *)` comments) |

## Usage

`grammar/kirn.ebnf` is the single source of truth for the language syntax. The
C++ parser in [`src/parser/`](../src/parser/) implements it, and the example
corpus ([`examples/*.kn`](../examples/)) is the **ground truth** the grammar must
accept.

**Rule:** any language change must update the grammar first, then be validated
against the entire example corpus before merge.

See [`docs/KIRN_PLAN.md`](../docs/KIRN_PLAN.md) for the grammar phase history
and change record.
