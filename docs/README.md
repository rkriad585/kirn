# Kirn Docs (`docs/`)

<p align="center">
  <img src="https://raw.githubusercontent.com/rkriad585/kirn/main/kirn-logos/kirn-icon-on-light.png" alt="Kirn logo" width="180">
</p>

## Install Kirn

```sh
curl -fsSL https://raw.githubusercontent.com/rkriad585/kirn/main/installer.sh | sh -s -- --version v0.0.1-b3
npm install -g @kirn-lang/kirn
python -m pip install kirn
```

```powershell
choco install kirn --version 0.0.1-b3 --yes
```

Design and engineering documentation for the Kirn language.

| Document                       | Description                                                                               |
| ------------------------------ | ----------------------------------------------------------------------------------------- |
| `docs/KIRN_PLAN.md`            | The master design + roadmap document: phases, grammar history/change record, feature list |
| `docs/FEATURE_GAP_ANALYSIS.md` | Tracks implemented vs planned language features                                           |

Related planning lives in [`KIRN_PLANS/`](../KIRN_PLANS/) (per-topic,
phased implementation plans such as the docs website, standard library,
highlighting, LSP, and cross-compilation).

Normative references:

- [Grammar (EBNF)](../grammar/kirn.ebnf)
- [Examples](../examples/)
- [Repository README](../README.md)
