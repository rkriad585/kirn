# Kirn Tests (`tests/`)

The acceptance suites that validate the compiler against the normative grammar
and expected semantics.

## Layout

| Directory | Contents |
|-----------|----------|
| `tests/negative/` | `n*.kn` — programs that **must fail** to compile, each pinned to one specific error (golden diagnostics); `run.ps1` drives checks |
| `tests/types/` | `n*.kn` / `p*.kn` — type-level negative (borrow, exhaustiveness, default-type) and positive cases |
| `tests/conventions/` | Convention-focused cases |

The stdlib acceptance tests live with each module under
[`stdlib/pet/*_test.kn`](../stdlib/README.md), and `examples/*.kn` form the
positive parse/run corpus.

## Running

```powershell
powershell -File scripts/negative.ps1   # negative suite
powershell -File scripts/types.ps1      # type suite
powershell -File scripts/runall.ps1     # full corpus
```

Rule: any grammar or semantic change must keep every `n*.kn` failing (with the
pinned diagnostic) and every positive case passing.
