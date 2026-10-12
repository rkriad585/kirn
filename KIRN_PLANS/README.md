# Kirn Plans (`KIRN_PLANS/`)

## Goal

This is the single index that maps every plan file in `KIRN_PLANS/` to the one
concern it owns, so the Kirn roadmap stays non-contradictory for both humans and
agents. Read this index first; then open the owning plan for its phases, code,
testing, and limitations.

## Must Follow

- The toolchain build ships exactly two executables: `kirn` (driver) and `kirnc`
  (checker/compiler). No other binaries.
- Kirn style is minimal: no over-engineering, and exactly one owner per topic.
- AI-first: this index is machine-readable and stable; filenames and plan roles
  must not drift.
- Kirn-style spellings everywhere: functions are `fn`, variables are `var`, base
  types are `Str`/`Int`/`Float`/`Bool`/`Char`/`BigInt`/`Rational`, and free
  builtins/types are CapCase (e.g. `Str(...)`, `BigInt(...)`, `Min`, `Max`,
  `__Sqrt`).
- Every plan file listed here stays gate-aligned with `scripts/gates_linux.sh`.

## Important Note

Keep the existing table and its ordering; only the file list is refreshed to the
exact current set below. Do not rename, add, or drop a plan file without updating
this index and the owning plan.

Phased implementation plans for the Kirn language and tooling. Each plan opens
with a research/source-review note before offering step-by-step phases with
goals, problems solved, files affected, code, testing, and limitations.

| Plan                                                   | Owned concern                                                                            |
| ------------------------------------------------------ | ---------------------------------------------------------------------------------------- |
| `PLAN.md` / `DO_FIRST_PLAN.md`                         | Overall roadmap and immediate-first priorities                                           |
| `REBRAND_PLAN.md`                                      | Historical rename decisions (legacy extension → `.kn`)                                   |
| `KIRN_DOCS_PLAN.md`                                    | Documentation website                                                                    |
| `SYNTAX_PLAN.md` / `EXP_PLAN.md` / `DATA_TYPE_PLAN.md` | Syntax, expressions, data types                                                          |
| `MISSING_PLAN.md`                                      | Missing features                                                                         |
| `WHY_PLAN.md` / `WHY_USE_KIRN_PLAN.md`                 | Rationale and adoption messaging                                                         |
| `STD_LIBS_PLAN.md`                                     | Standard library                                                                         |
| `KIRN_HIGHLIGHT_PLAN.md`                               | Syntax highlighting: TextMate + tree-sitter                                              |
| `KIRN_LSP_PLAN.md`                                     | Language Server Protocol server (`kirn-lsp`)                                             |
| `KIRN_CROSS_PLAN.md`                                   | Cross-compilation / targets                                                              |
| `POWER_PLAN.md`                                        | Default-on power layer: async runtime, serialization, data parallelism, ergonomic errors |

> Reference: rust — exhaustive `match` with guards and inclusive range patterns.

```kn
fn classify(n: Int) -> Str {
    match n {
        case 0 { return "zero"; }
        case 1..=9 { return "digit"; }
        case v if v < 0 { return "negative"; }
        case _ { return "many"; }
    }
}
```

These plans are living documents; keep them in sync with the code and the
normative grammar ([`grammar/kirn.ebnf`](../grammar/kirn.ebnf)) and the example
corpus ([`examples/`](../examples/)).
