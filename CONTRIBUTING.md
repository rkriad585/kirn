# Contributing to Coco (Kirn)

Coco is a Windows-first, C++20 compiled language that is being renamed to
**Kirn** per [COCO_PLANS/COCO_TO_KIRN_PLAN.md](COCO_PLANS/COCO_TO_KIRN_PLAN.md).
Anything you commit must keep the harnesses green at every phase.

## Environment

- Windows + PowerShell 5.1 + Visual Studio (MSVC) + CMake + NMake.
- **No POSIX/Bash assumptions**; the harnesses are PowerShell (`runall.ps1`,
  `types.ps1`, `negative.ps1`, `vm_diff.ps1`, `conventions`) plus the parser
  tools built into `build\` (single-config NMake: exes live at `build\*.exe`,
  **not** `build\Debug\`).

## Build

```powershell
cmake -S . -B build
cmake --build build                 # NMake single-config; exes in build\
```

Release build for benchmarks:

```powershell
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
```

ASan tree (must be configured separately):

```powershell
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOCO_ASAN=ON
cmake --build build-asan
```

## Verify (the gate — must be green before you commit)

```powershell
powershell -NoProfile -File scripts\runall.ps1          # examples corpus
powershell -NoProfile -File scripts\types.ps1           # type-checks (13)
powershell -NoProfile -File scripts\negative.ps1        # must-reject cases (18)
powershell -NoProfile -File scripts\vm_diff.ps1         # Kirn VM parity (vm_*)
# conventions (including line-ending + final-newline checks):
powershell -NoProfile -File tests\conventions\run.ps1
```

Or run the whole 5-gate snapshot (Phase-1 harness) at once:

```powershell
powershell -NoProfile -File scripts\rename_baseline.ps1
# -> "baseline OK: 5/5 gates passed" and transcripts under _rename/baseline/
```

> Note: `scripts\types.ps1 -Check/-Run`, `scripts\negative.ps1 -Runner`, and
> `rename_baseline.ps1` accept the exe path explicitly; the defaults resolve
> to the `build\` (then `build\Debug\`) layout above.

## Line endings (repo policy — non-negotiable)

- **LF everywhere**, UTF-8 no BOM, final newline, no trailing whitespace. See
  `.gitattributes` (`* text=auto eol=lf`), `.editorconfig`, `.prettierrc`.
- `core.autocrlf` is **off** for this repo — git performs no auto-conversion and
  emits no LF/CRLF warnings. Editors (VSCode, nvim, prettier) are configured to
  write LF.
- Repo-scoped conventions files map to the same rule:
  - `.co`/C++ = 4-space indent; `.yml/.yaml/.json/.toml` = 2-space; LF.
- Do **not** add CRLF anywhere; do **not** re-enable autocrlf.

## Commit conventions

- Conventional Commits, enforced optionally via `commitlint.config.cjs`
  (type list: `meta, scripts, rename, tools, examples, docs, tests, stdlib,
gitignore, build, chore, fix, refactor, style, perf, CI`... scope optional,
  e.g. `rename(phase1): baseline snapshot`).
- One commit per cohesive phase/change; reference the plan phase in the body
  when the change implements a plan row (e.g. `rename(phase1): dry-run
inventory`).
- Keep the tree green at every commit (run the 5 gates above).

## Testing & the migration plan

- `COCO_PLANS/COCO_TO_KIRN_PLAN.md` is the authoritative migration plan
  (Phases 1..13, decision log D1..D8). **Phase 1 (baseline snapshot) is done.**
- New language/compiler behavior starts from `docs/COCO_PLAN.md` and
  `grammar/coco.ebnf`; corpus examples live in `examples/`, negative cases in
  `tests/negative/`, type cases in `tests/types/`, vm-parity cases in
  `tests/vm_diff/`, and convention checks in `tests/conventions/`.
- Each change that touches the corpus or grammar must be verified by the
  matching harness (see "Verify" above) and left green before commit.

## Code of Conduct

By participating you agree to abide by the
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Security

Vulnerabilities: see [SECURITY.md](SECURITY.md). For run-of-the-mill bugs, open
an issue rather than a security report.
