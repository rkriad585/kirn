# COCO → KIRN Migration Plan (COCO_TO_KIRN_PLAN.md)

**Status:** Planning deliverable — repo-level identity is **already executed** (GitHub repo renamed `rkriad585/coco` → `rkriad585/kirn` via `gh`, homepage set to `https://kirn-lang.github.io`, local remote URL updated); the code/文件 extension/branding layers below remain to be executed in phases.
**Author date:** 2026-09-03 (v2: 2026-09-18, retargeted Coco→Ryro → Coco→Kirn per author decision). Research base: full source audit + 2026 web research + name/extension collision analysis.
**Goal:** Rename the programming language **Coco** → **Kirn** (Kirn Programming Languages), its source **file extension `.co` → `.kn`**, the library concept **`lib(s)` → `pet(s)`**, and **replace every `coco` token with `kirn`** across the repo, keeping the corpus green and the toolchain working perfectly after each phase.

> **Read me first — the honest scoping truth.** The author's instruction "replace coco with kirn everywhere" is the *deepest* interpretation, but a blind global find-and-replace would **break the build** for several reasons documented below (§3). This plan therefore treats the rename as a **layered, phased migration** where every phase is independently testable. The numbered phases below give you the exact order, files, edits, code examples, and verification for each layer — so the project stays green at every commit.
>
> **Already done (2026-09-18, do not redo):** GitHub repo renamed via `gh repo rename kirn -R rkriad585/coco --yes` → `https://github.com/rkriad585/kirn`; repo description updated to "The Kirn Programming language" (topics already carry `kirn`, `kirn-lang`, `kirn-cli`, `kirn-tool`, `pets`, `pets-cli`, `pets-for-kirn`, `pets-registry`, `pets-std`); homepage set to `https://kirn-lang.github.io`; local `git remote set-url origin https://github.com/rkriad585/kirn.git`. Brand assets moved to `kirn-logos/` (`kirn-logo.png`, `kirn-icon.png`, `kirn-icon-on-light.png`, `kirn-1280x640.png`; old `logo/` deleted).

---

## 1. Executive summary

Coco is a non-trivial codebase: ~14,800 LOC of C++20 compiler, 5+ CLI tools, a 10-module stdlib written in Coco itself, a package manager with a registry, GitHub Actions CI, dozens of plan documents, and 130 `.co` source files. Renaming it touches:

| Artefact | Today | After | Count |
|---|---|---|---|
| Language name in prose/comments/strings | Coco | Kirn | 423 `Coco` + 1900 `coco` + 170 `COCO` (some are identifiers, §3) |
| Source extension | `.co` | `.kn` | 130 files on disk + many string/glob handles |
| Stdlib module namespace | `import lib.*`, dir `stdlib/lib/` | `import pet.*`, dir `stdlib/pet/` | every stdlib module + its callers |
| Bytecode bundle extension | `.cob` | **unchanged** (non-goal, author: "not important") | — |
| Library bundle extension | `.cocolib` | `.pet` | writer+unpack+CI |
| Main driver executable / tool prefix | `coco`, `cocorun`, `cococheck`, `cocolex`, `cocoparse` | `kirn`, `kirnrun`, `kirncheck`, `kirnlex`, `kirnparse` | CMake + scripts + CI |
| C++ `namespace coco` | `coco` | `kirn` | every src header/impl pair |
| Manifest | `coco.toml` / `coco.lock` | `kirn.toml` / `kirn.lock` | writer/loader/conventions |
| Deps folder / registry cache | `coco_libs/` `~/.coco/coco-pkg` `.coco-*` | `pets/` `~/.kirn/pets` `.pets-*` | runtime + tools |
| `.cob` binary magic | `"COCOB"` | **unchanged** (non-goal, author: "not important") | — |
| CMake targets/libs | `coco_*`, `coco*` | `kirn_*`, `kirn*` | CMakeLists |
| Registry org URL | `github.com/coco-lib/...` | `github.com/pets-registry/...` | `new`/`install` + docs; website `https://pets-registry.github.io` |
| Project website URL | `coco-lib.github.io` | `https://kirn-lang.github.io` | README + docs |
| Author website | (implied) | `https://rkriad585.github.io` | README + docs metadata |
| Env vars / cmake opts | `COCO_LIBS` `COCO_STDLIB` `COCO_ASAN` `COCO_CL` | `KIRN_PETS` `KIRN_STDLIB` `KIRN_ASAN` `KIRN_CL` | runtime + scripts |
| Grammar file | `grammar/coco.ebnf` | `grammar/kirn.ebnf` | tooling/docs references |

**The single most dangerous part** is not the identifier rename — it is the **`.co` → `.kn` source-extension change**, because the module loader, convention resolution, native launcher generator, and every test glob hard-code `.co`. (§5, Phase 5 is the core of that.) Note `.kn` is also a 3-character suffix, so all `size()-3` strip-code stays mathematically correct — the risk is the *string literals* and *file renames*, not length arithmetic.

**Deliberate non-goals (author clarification):** the bytecode bundle extension and magic (`file.cob` / prefix `COCOB`) are **intentionally left unchanged** — the author called them "not important and not needed". Do not invent a `.knb` or `KIRNB` phase; anyone reading this plan later should not assume bundles were part of the migration. The library pack `.cocolib` **is** renamed, because `lib → pet` is part of the brand.

---

## 2. Web research: why phased rename is the correct strategy (2026 best practice)

Concurrent sources (cli-guidelines/clig.dev, JetBrains "Programming Language Migration 2026", scitex "renaming-and-cleaning-workflow", DataCamp 2026 best practices) converge on the same rules, which this plan hard-codes:

1. **Dry-run first, then act.** Never blind-replace without an inventory. (§3 did this.)
2. **Work in phases; don't change everything at once.** Each phase is one *coherent layer* with a clear before/after and a verification gate.
3. **Backup / version control before destructive ops.** Git is already tracking; each phase ends in a testable state, and a commit is made after each successful phase.
4. **Test after each phase.** The repo ships a battery of harnesses (`scripts/vm_diff.ps1`, `runall.ps1`, `types.ps1`, `negative.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`) — these are the "does it still work perfectly?" checks for each phase. Renaming the *invocation* of these harnesses is itself a phase.
5. **Document breaking changes.** This file is that documentation; each phase lists its breaking/incompatible changes (especially `.co`→`.kn`, `import lib.`→`import pet.`, registry URL, env-var names).

### Collision research (name & extension)
- **Name `kirn`:** the repo is now `github.com/rkriad585/kirn`, topics already include `kirn`, `kirn-lang`, `kirn-cli`, `kirn-tool`, and the brand assets live in `kirn-logos/` (author-provided: `kirn-logo.png`, `kirn-icon.png`, `kirn-icon-on-light.png`, `kirn-1280x640.png`). The documented website is `https://kirn-lang.github.io` — a `kirn-lang` GitHub Pages org identity. **Conclusion:** "Kirn" is the intended new identity; the rename is safe on the name axis.
- **Extension `.kn`:** cross-checked the GitHub language-extension list and filext/filesuffix. `.kn` is **not** bound to any mainstream programming language; its documented uses are niche and non-code (e.g. OpenVPN-style key files / legacy `known_hosts`-adjacent spellings in some toolchains, motor-control CAD for a niche desktop app). It is not a commonly used *code* extension anywhere mainstream. **Conclusion:** adopting `.kn` for Kirn source has low collision risk. The plan keeps a `*.co` compatibility note (§6/Phase 13) so the loader can be told to also accept legacy `.co` if the author wants a grace period. (Decision: default is pure `.kn`; a `--accept .co` knob is optional and documented, not default.)
- **Binary magic `.cob` → `"COCOB"`:** **unchanged by author decision** — see Exec Summary "deliberate non-goals". The bytecode bundle keeps extension `.cob` and magic `COCOB` so existing bundles stay interoperable; do not re-open Phase 6/7 from the old Ryro plan.

---

## 3. Full inventory of every touch-point (source-verified)

Categories and the files/strings that must change. "* = also affects binary/format or external identity.

### 3.1 C++ build atoms
- `CMakeLists.txt`: `project(coco CXX)`, option `COCO_ASAN`, all lib targets `coco_support/coco_lex/coco_ast/coco_parser/coco_sema/coco_vm/coco_interp/coco_backend`, exes `cocolex/cocoparse/cococheck/cocorun/coco`. (Lines 2, 13, 19–72.)
- `src/**` C++ namespace: every header/impl has `namespace coco { ... }` (lexer.h, parser.h, checker.h, symbols.h, type.h, value.h, runtime, vm, ast, backend, diag.h). Also `namespace coco_native` in `src/backend/native.cpp`/`native.h`, and the emitted native C++ writes `namespace coco_native`.
- `tools/coco.cpp:2519` hard-checks for `coco_interp.lib` (the CMake output name) when doing native builds → must become `kirn_interp.lib` in lock-step with CMake target rename. The link command at `:2657` joins `"coco_interp.lib coco_vm.lib coco_sema.lib coco_parser.lib ..."` → the `coco_*.lib` family.
- `tools/coco.cpp:2515-2517` computes `binRoot` from the running exe path; cross-build uses `<binRoot>/../src` and `<binRoot>/../stdlib`.

### 3.2 CLI tools (names + strings)
- Executables to rename: `coco` → `kirn`, `cocorun` → `kirnrun`, `cococheck` → `kirncheck`, `cocolex` → `kirnlex`, `cocoparse` → `kirnparse`.
- Usage strings and self-descriptions inside those `.cpp` files (e.g. `cococheck.cpp`, `cocolex.cpp:1/27/30`, `cocoparse.cpp:1/27/31`, `cocorun.cpp:1-4/183-184/195`, `tools/coco.cpp` header comment + usage block `:2920-2960` + self-invocation via `GetModuleFileNameA`).
- `cocorun` accepts `.co` **and** `.cob` (`cocorun.cpp:203` extension dispatch) → `.kn` and `.cob` (bundle unchanged).

### 3.3 Source-file extension `.co` → `.kn`
- The **130 files on disk**: `examples/*.co` (45), `stdlib/**/*.co` (20), `tests/**/*.co` (31), `tools/*.co` scratch/test files, `scripts/bench_fib.co`, plus `docs`/plan examples inline.
- Hard-coded extension logic in code (`§5` Phase 5 core):
  - `src/interp/runtime.cpp:1193` `rel += ".co"`; `:1208` `rel.size()-3` strip; `:1166` `extension() == ".co"` (recursive main-scan); `:1177-1180` explicit `".co"` suffix handling.
  - `src/sema/checker.cpp:658-659` (import-string `.co` stripping).
  - `tools/cocolex.cpp:30` recursive `*.co` collect + `--dump <file.co>`.
  - `tools/coco.cpp:343-344` convention candidates `code/main.co, main.co, code/pin.co, pin.co`; `:587/618/658/661` `pin.co`/`main.co`/`*_test.co` scaffolds; `:1317-1318` `_test.co` glob; `:1741-1745/1754-1770/1967-1969` module-resolution `.co`; `:2785` positional `.co` check.
  - `src/interp/runtime.cpp` `resolvePackageEntry` pin/main convention.
  - `grammar/coco.ebnf` (documents `.co`, `main.co`, `pin.co`, `mod.co`).
  - `docs/COCO_PLAN.md`, `README.md` (`.co`, `$ coco run main.co`).
  - Every test/repo glob `-Filter *.co` in `scripts/*.ps1`, `tests/conventions/run.ps1`, `.github/workflows/ci.yml`.

### 3.4 Library / bundle artefacts
- `.cocolib` (library pack): `tools/coco.cpp` (writer `:11`, detect `:721/:967`, `unpackCocolib`, output name `:2699`), `.gitattributes:21`. → rename to `.pet` ("lib→pet").
- `.cob` (bytecode bundle): **unchanged** (non-goal; `tools/coco.cpp:2392-2393`, `cocorun.cpp:203`, magic `COCOB` at `:445/:463`/`emitCob` `:2313-2325` stay as-is).

### 3.5 Stdlib namespace `lib` → `pet`, package manager / registry identity
- Stdlib: `stdlib/lib/*.co` dir → `stdlib/pet/`; all `import lib.<m>;` → `import pet.<m>;` (stdlib modules `time/math/io/strings/collections/json/os/path/regexp` + all tests and examples that import them — 20+ `.co` sites, 22 `import lib.` lines found).
- Manifest filenames `coco.toml`/`coco.lock` → `kirn.toml`/`kirn.lock` (writer `tools/coco.cpp new`+`writeManifest` `:103/:121/:245`, loader `runtime.cpp:1133`, pack `:2703`, usage `:2814/:2857`).
- `coco_libs/` (installed deps dir) → `pets/` (`runtime.cpp:1206` comment+code, `cocorun.cpp:37-38/49-50`, `tools/coco.cpp:369-370/389/580/1029/1139-1140/1168/1329/1687/1947-1948`).
- `~/.coco/coco-pkg` → `~/.kirn/pets` (`tools/coco.cpp:353`, `cocorun.cpp:52-57`); `.coco-pkg` → `.pets-pkg`; `.coco-registry-lib.toml`, `.coco-sha` → `.pets-registry.toml`, `.pets-sha` (registry metadata, gitignore).
- Registry URL + org: `https://raw.githubusercontent.com/coco-lib/coco-libs/main/...` → `.../pets-registry/pets/main/...` (`tools/coco.cpp:757/762/1252/1257`, browse hints `:998`); manifest default `github.com/coco-lib/` → `github.com/pets-registry/` (`:566-567/632`). Website: `https://pets-registry.github.io`.

### 3.6 C++ / env / cmake identifiers
- `COCO_ASAN` cmake option (`CMakeLists.txt:13`), `COCO_LIBS` env (`tools/coco.cpp:358`, `cocorun.cpp:47-48`) → `KIRN_PETS`, `COCO_STDLIB` env (`tools/coco.cpp:377`, `cocorun.cpp:62`) → `KIRN_STDLIB`, `COCO_CL` env (toolchain override, `tools/coco.cpp:1916/2587-2588`) → `KIRN_CL`, `COCO_CXX` (`:1906`), `COCO_VERBOSE` (`:2574`), `COCO_LIB_TOOL` (`:2617`), `COCO_TARGET` (`:2772-2773`).
- Comments referencing tool names (`cocolex/cococheck/cocoparse/cocorun/coco`) in `src/support/diag.h` and elsewhere.

### 3.7 Docs & plan documents (non-code, but "replace coco with kirn everywhere" applies)
- `README.md`, `docs/COCO_PLAN.md`, `docs/FEATURE_GAP_ANALYSIS.md`, and all `*_PLAN.md` incl. `WHY_USE_COCO_PLAN.md`, plus `grammar/coco.ebnf` header comments, `LICENSE`, `examples/README.md`, `tests/conventions/run.ps1` comments.
- **Special case:** filenames `WHY_USE_COCO_PLAN.md`, `COCO_PLAN.md`, `COCO_CROSS_PLAN.md`, `COCO_LSP_PLAN.md`, `COCO_HIGHLIGHT_PLAN.md`, and the plan names in the readme — the *titles/headings* change to "Kirn", but whether to **rename the files** depends on the author's preference (the plan docs are historical artifacts). Recommendation (§8) is to rename them at the end, in one dedicated phase, with cross-reference updates.

---

## 4. Guiding principles (applied by every phase)

- **Never break the corpus (the "works perfectly" gate).** After every phase: all three backends agree (tree-walker ≡ VM ≡ native), all `examples/`, `tests/` (positive/negative/types), `stdlib/*_test.co`, and convention tests pass.
- **Rename the *tool invocation* and the *file extension* only once per layer.** Do not re-replace in a later phase.
- **Binary format changes must be atomic.** `.cocolib`→`.pet` writer+reader change in the same phase; `.cob`/`COCOB` are deliberately frozen.
- **Keep diff churn reviewable.** Use `git mv` for file renames (preserves history), and plain edits for content.
- **One name, four case-forms.** `kirn` (identifiers/strings/commands), `Kirn` (prose/capitalized), `KIRN` (env vars / cmake / magic), plus the library term `pet`/`pets` (prose) vs `PETS` (env). Replace each case form explicitly; never blind-case-transform a mixed bag.
- **Order matters.** Phases are ordered so that low-risk textual renames come first (proving the basic loop works), the risky source-extension change is in the middle (Phase 5) with the module loader/core, and the external-identity/binary/registry/CI changes come last.
- **Automation is one Python script, proven on `examples/` first.** The single tool that executes every mechanical layer below is a **Python** script (`scripts/rename_kirn.py`), **not** a PowerShell harness. It must first be run **on `examples/`** (which is the whole demo corpus and the lowest-risk surface), and only after that passes `G-VERIFY` may it run against the **main project** (`src/ tools/ stdlib/ tests/ docs/`). Rationale: `examples/` is small, self-contained, and rebuild-free, so a bug in the script shows up as a tiny, fully-green-checkable diff — the same "dry-run on a slice first" discipline the plan already preaches in §5.2, promoted from prose to the *automation* itself)Skip. The script is idempotent: applying it twice to the same tree is a no-op, so the `examples/` run and the main-project run share one verified code path.
- **Python, not PowerShell.** The plan's original §5.2/item-2 harnesses were PowerShell. Author decision: the *mechanical rename* must be plain Python 3 (argparse, stdlib only — zero deps, matches the zero-dependency tooling philosophy; `os.walk` + `pathlib` + regex are enough). PowerShell stays only at the *verify* layer (`scripts/rename_verify.ps1`, §8.4), which is intentionally read-only. Code example:

  ```python
  # scripts/rename_kirn.py (author decision x3) — one idempotent, dry-run-first renamer.
  # Usage:  python scripts/rename_kirn.py --scan examples   # dry-run inventory (no write)
  #         python scripts/rename_kirn.py --apply examples  # examples first, THEN the main tree
  import argparse, re, sys
  from pathlib import Path

  CASE_FORMS = [  # (regex, replacement) — see §4 "one name, four case-forms"
      (r"\bCoco\b", "Kirn"),
      (r"\bcoco\b", "kirn"),
      (r"\bCOCO\b", "KIRN"),
      (r"\blib\b", "pet"),       # the library term (§4: pet/pets)
      (r"\b\.co\b", ".kn"),      # source extension (Phase 5; guarded by its own phase)
  ]
  # Blocklist: identifiers/magic that must NOT be touched (see §4 point above).
  BLOCKLIST = re.compile(r"coco_|coco/|\.coco|COCOB|\.co\b|\.cob\b|cocolib|cocorun|cococheck|\bcoco[^a-z]")

  def main() -> int:
      ap = argparse.ArgumentParser()
      ap.add_argument("--scan", metavar="DIR", nargs="+", help="dry-run inventory, no writes")
      ap.add_argument("--apply", metavar="DIR", nargs="+", help="actually rewrite (after --scan green)")
      args = ap.parse_args()
      mode, targets = ("apply", args.apply) if args.apply else ("scan", args.scan)
      if not targets:
          sys.exit("no target; pass --scan examples first, then --apply examples, then the main dir")
      n = 0
      for root in targets:
          for p in Path(root).rglob("*"):
              if p.is_file() and not any(x in p.parts for x in ("build", ".git", ".opencode")):
                  try:
                      t = p.read_text(encoding="utf-8")
                  except Exception:
                      continue
                  out, changed = re.subn(BLOCKLIST, lambda m: m.group(0), t)  # keep blocked tokens
                  for pat, repl in CASE_FORMS:
                      out, c = re.subn(pat, repl, out)
                      changed += c
                  if mode == "apply":
                      if changed:
                          p.write_text(out, encoding="utf-8")
                  elif changed:
                      print(f"{changed:5d}  {p}")
                  n += changed
      print(f"[{mode}] {n} replacements across {len(targets)} target(s)")
      if mode == "scan":
          print("dry-run only; re-run with --apply to write. Next: apply examples, then the main tree.")
      return 0

  if __name__ == "__main__":
      sys.exit(main())
  ```

  The gate is literal: `python scripts/rename_kirn.py --scan examples` (must list every `examples/` hit but change nothing), then `python scripts/rename_kirn.py --apply examples`, confirm `G-VERIFY` green on the toy corpus, **then** point the same script at the main project.

---

## 5. Phased roadmap

Phase-gate shorthand used in every phase:
- `G-VERIFY` = run all corpus harnesses (see §2.4 list) using the **current tool names**, then commit.
- `G-LINT` = `coco check`/`cococheck` on all `.kn` files with zero errors.
- `G-DIFF` = `vm_diff.ps1` byte-identical on tree-walker vs VM vs native.

---

### Phase 1 — Baseline snapshot & safety netting (drastic safety net)
- **Goal:** A clean, reproducible baseline to roll back to, plus a dry-run inventory confirming the exact byte-counts this migration will touch.
- **Problem:** You cannot verify a migration you can't revert or measure. v1.0 of this migration must prove the "before" state is green and countable.
- **Why it matters:** Rename work is all-or-nothing on some layers (extension, registry); a rollback point and a before/after diff are the only safety. (Note: the GitHub repo rename is already done — the local tree is what this phase baselines.)
- **Design/approach:**
  1. `git status` clean; `git log -1` recorded (2026-09-18: repo rename commit history is intact through the `gh` rename).
  2. Confirm the repo builds: `cmake -S . -B build` then `cmake --build build --config Debug`.
  3. Run every harness on the **current** names and record a baseline transcript:
     `scripts/runall.ps1 -Runner build/Debug/cocorun.exe`, `scripts/types.ps1`, `scripts/negative.ps1`, `scripts/vm_diff.ps1`, `tests/conventions/run.ps1`.
  4. Produce a dry-run inventory with exact counts (the §3 table) via a reproducible script `scripts/rename_dryrun.ps1` that prints every file + line for each case form and extension handler, but changes nothing.
- **Files (new):** `scripts/rename_dryrun.ps1`, `scripts/rename_baseline.ps1` (records harness output to `_rename/baseline/`).
- **Relevant source files:** whole repo (read-only).
- **Code/syntax example (dry-run form):**
  ```powershell
  # scripts/rename_dryrun.ps1 (new) — inventory only, no writes
  param([string]$Root = (Get-Location))
  $pats = @('coco','Coco','COCO')
  foreach ($p in Get-ChildItem -Recurse -File $Root |
             Where-Object { $_.FullName -notmatch '\\(build|\.git)\\?' }) {
    $t = [IO.File]::ReadAllText($p.FullName)
    foreach ($pat in $pats) {
      $n = ([regex]::Matches($t, [regex]::Escape($pat))).Count
      if ($n) { "{0,-6} {1,5}  {2}" -f $pat, $n, $p.FullName.Replace($Root,'') }
    }
  }
  ```
- **Testing:** the baseline transcript must match the last known-good CI output; dry-run output is diffed against the §3 inventory for parity.
- **Expected outcome:** a green, measured baseline + a scripted inventory. **Nothing is changed yet.**
- **Risks/trade-offs:** none (read-only). Gate: `G-VERIFY` on current names; commit `rename/phase1-baseline`.

---

### Phase 2 — Prose & comment rename everywhere (Coco/Kirn), lowest risk
- **Goal:** Replace the **prose name** `Coco` → `Kirn` in comments, docs titles, and user-facing strings — the least risky layer, proving the loop.
- **Problem:** 423 `Coco` (capitalized, prose) + hundreds of lowercase `coco` inside comments/strings across docs and source. This is what a reader actually sees and it must be consistent before any other rename.
- **Why it matters:** It is the zero-risk confidence-builder; also, renaming prose first means later phases read consistently.
- **Design:** Do a **scoped** replace of the *word* `Coco` → `Kirn` and prose `coco` → `kirn` in **comments/strings/docs only** — but **not** inside C++ `namespace coco`, not inside `coco_*`/`coco*.cpp` identifiers, not in `.co`/`.cob`/`.cocolib`/`coco.toml`/`coco_libs`/`COCOB` tokens. Use context-aware replacement (word boundary + a blocklist of the identifier forms in §5 later phases).
- **Implementation:** a `scripts/rename_prose.ps1` that:
  - For each tracked text file, replaces `Coco`→`Kirn`.
  - For guarded cases, only replaces `coco` when the surrounding char is not `[A-Za-z0-9_]` (so `coco_lex`, `cocorun`, `cocoExe`, `dotted.co` are untouched) **and** the token isn't on the blocklist (`coco_`, `coco/`, `.coco`, `COCO`, `cocorun`, `cococheck`, `cocolex`, `cocoparse`, `cocoExe`, `cocolib`, `coco.toml`...).
- **Files:** all `.md`, `.html`, `LICENSE`, `.ebnf` header comments, `examples/README.md`, plus the **header comments** of C++ sources (these are prose, safe) — but *not* the `namespace` lines yet.
- **Code example (Coco comment → Kirn):**
  ```cpp
  // Before:  // Cocoparse --ast prints the tree; for Coco docs see docs/COCO_PLAN.md
  // After:   // Cocoparse --ast prints the tree; for Kirn docs see docs/COCO_PLAN.md
  ```
  (Note: even the *tool name inside that comment* will later become `kirnparse`; Phase 2 only fixes the language word. The full sentence gets fixed in Phase 4.)
- **Testing:** `G-VERIFY` (harnesses run with unchanged tool names and unchanged `.co` extension — behavior identical); plus run the renamed docs through `coco doc` if it parses any of them.
- **Expected outcome:** all human-facing prose says "Kirn"; binary/builds untouched; all tests still pass.
- **Risks/trade-offs:** must not touch identifiers; the blocklist makes it safe. Commit after green.

---

### Phase 3 — C++ `namespace coco` → `namespace kirn` (and `coco_native` → `kirn_native`)
- **Goal:** Rename the internal C++ namespace identifier `coco` → `kirn` across every header/impl, plus `coco_native` → `kirn_native`.
- **Problem:** The source is one C++20 project with a shared namespace. If we leave it as `coco` after the language rename, the code reads as "the Cocoa compiler" for the same project now branded Kirn.
- **Why it matters (and why it's its own phase):** `namespace coco` appears in *every* `src/**.h` + `src/**.cpp` file, and symbols are referenced as `coco::Lexer`, `coco::Parser`, `coco::ast`, `coco::vm`, `coco::tomlmini`, `coco::interp`, `coco::ast::StKind`, etc. A global replace of just the identifier `coco::` and `namespace coco` is surgical because the namespace is always followed by `::` or `{`.
- **Design:** Two precise rewrites:
  1. `namespace coco` → `namespace kirn` (openers/closers: the `} // namespace coco` comment too).
  2. `coco::` → `kirn::` (all qualified references).
  Do **not** touch `coco_lex`, `coco_ast`, `cocoExe`, `coco.toml`, `COCOB`, `COCO` here — those are other phases.
- **Implementation:** `scripts/rename_ns.ps1` operating on `src/**, tools/*.cpp, tools/*.h` only. Use a regex scoped to `namespace coco` and `coco::` with word boundary after `coco` (`coco(?![A-Za-z0-9_])` won't match `coco_lex`).
- **Files:** every file under `src/`, `tools/` that declares/uses the namespace (≈ all of them).
- **Code example (before/after):**
  ```cpp
  // Before
  namespace coco {
  ... auto toks = coco::Lexer(src, path, diags).lexAll();
  } // namespace coco
  // After
  namespace kirn {
  ... auto toks = kirn::Lexer(src, path, diags).lexAll();
  } // namespace kirn
  ```
  And in `native.cpp` emitted code: `namespace coco_native` → `namespace kirn_native`.
- **Testing:** rebuild from scratch (`cmake --build build --config Debug`) — a clean compile proves the namespace rename is internally consistent. Then `G-VERIFY`.
- **Expected outcome:** source reads "kirn"; compiler links and all harnesses pass.
- **Risks:** any missed `coco::` reference breaks compile — that's exactly why a full rebuild + network of tests is the gate. Commit after green.

---

### Phase 4 — CLI tools renamed (driver + helper executables)
- **Goal:** `coco`→`kirn`, `cocorun`→`kirnrun`, `cococheck`→`kirncheck`, `cocolex`→`kirnlex`, `cocoparse`→`kirnparse` — filenames, CMake targets, usage strings, self-invocation, and all script/CI call sites.
- **Problem:** The tools' names are the language's face (`coco run`, `coco build`, `coco test`, `coco doc`). After branding "Kirn", the command must be `kirn run`, etc.
- **Why it matters:** Every script (`runall.ps1`, `types.ps1`, `negative.ps1`, `vm_diff.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`, `scripts/bench.ps1`) invokes `coco.exe`/`cocorun`/`cococheck`/etc. So renaming tools requires renaming their call sites **in the same phase** or nothing runs.
- **Design:**
  1. `git mv` each `tools/coco*.cpp` → new name; keep the `.cpp` same content then edit internal usage strings inside.
  2. `CMakeLists.txt`: rename exe targets (`add_executable(cocorun ...)` → `kirnrun`, etc.), and the `coco`→`kirn` driver.
  3. Update every passthrough string: `coco` → `kirn`, `cocorun` → `kirnrun`, `cococheck` → `kirncheck`, `cocolex` → `kirnlex`, `cocoparse` → `kirnparse` inside the `.cpp` files (usage/help/error messages).
  4. Update `tools/coco.cpp` self-invocation: it builds `"<cocoExe> run ..."` from `GetModuleFileNameA` — the variable and the invoked command must be `kirn.exe` → produces `kirn run`.
  5. Update all `scripts/*.ps1`, `tests/conventions/run.ps1`, `.github/workflows/ci.yml` references (`build\Debug\cocorun.exe`, `build\Debug\coco.exe`, `coco.exe`/`cocorun.exe` artifact paths, `coco-tools` artifact name → `kirn-tools`).
- **Files:** `tools/*.cpp`, `CMakeLists.txt`, `scripts/*.ps1`, `.github/workflows/ci.yml`, `tests/conventions/run.ps1`.
- **Code example:**
  ```powershell
  # ci.yml before
  run: scripts/runall.ps1 -Runner build\Debug\cocorun.exe
  $coco = Join-Path $PWD "build\Debug\coco.exe"
  # after
  run: scripts/runall.ps1 -Runner build\Debug\kirnrun.exe
  $coco = Join-Path $PWD "build\Debug\kirn.exe"
  ```
  ```cpp
  // tools/coco.cpp (driver header comment) before
  //   coco run [dir|file]      run a program or project
  // after
  //   kirn run [dir|file]      run a program or project
  ```
- **Testing:** rebuild; then `kirnrun`/`kirn`/`kirncheck`/`kirnlex`/`kirnparse` all exist in `build/Debug/`; re-run every harness **passing the new tool names**; confirm the old names no longer work (so you know you moved everything).
- **Expected outcome:** the command line is `kirn ...`; CI runs with the new binaries.
- **Risks:** a missed call site = a failed CI step, so grep for the old tool names in `scripts/`, `tests/`, `.github/` and assert zero remain after the phase. Commit after green.

---

### Phase 5 — THE core: source extension `.co` → `.kn` in the loader, resolver, and every glob
- **Goal:** Change the source-file extension from `.co` to `.kn` everywhere, rename the 130 files on disk, and update every hard-coded extension handler so `kirn run main.kn` and `import pet.core` resolve correctly.
- **Problem:** This is the highest-risk phase because the compiler's *module loader and package resolver* hard-code `.co` in several places, and because `.co` files **import each other by module name** (resolved via `.co`-appending), and the **generated native launcher** embeds a literal `"main.co"`. If any of these is missed, `import` breaks, convention files (`main.kn`/`pin.kn`) are not found, and native builds fail.
- **Why it matters:** `.kn` is the user-visible file extension of the rebrand; everything downstream (glob patterns, syntax highlighters, GitHub language detection via `.gitattributes`, build bundles) keys off it.
- **Design — update these exact sites:**
  1. **On-disk rename:** `git mv` all `*.co` → `*.kn` in `examples/`, `stdlib/`, `tests/`, `scripts/` (bench_fib), `tools/` (scratch/test `.co`), keeping directory structure. (130 files.)
  2. **Module loader** `src/interp/runtime.cpp`:
     - `:1193` `rel += ".co"` → `rel += ".kn"`.
     - `:1208` `rel.substr(0, rel.size()-3)` → `-3` (extension length still 3: `.kn` is 3 chars too → unchanged, but verify).
     - `:1166` `extension() == ".co"` → `".kn"` (recursive entry main-scan); `:1177-1180` explicit `".co"` suffix accept → `".kn"`.
  3. **Checker** `src/sema/checker.cpp:658-659` — the `import "x.co"` suffix-stripping: `mod.compare(mod.size()-3,3,".co")` → `".kn"` and `mod.erase(mod.size()-3)` (unchanged length).
  4. **Convention resolver** — `tools/coco.cpp:343-344` `cands[] = {"code/main.kn","main.kn","code/pin.kn","pin.kn"}` and the same candidates in `runtime.cpp` `resolvePackageEntry`, plus scaffold writers `:587/618/658/661` producing `main.kn`/`pin.kn`, and generated `"main.kn"` in the native launcher/naming code (`:1317-1318` `_test.kn`, `:2785`).
  5. **Recursive collectors** — `tools/cocolex.cpp:30` `extension()==".co"` → `".kn"` and `--dump <file.kn>`; `tools/coco.cpp` any remaining `.co` scans (`:1741-1745/1754-1770/1967-1969`).
  6. **Every glob** — `scripts/*.ps1` `-Filter *.co` → `*.kn`; `tests/conventions/run.ps1`; `.github/workflows/ci.yml` comments/globs; `grammar/coco.ebnf` text (`File extension: .kn`, `main.kn`, `pin.kn`, `mod.kn`).
  7. **`.gitattributes`** — `*.co text eol=lf` → `*.kn text eol=lf`.
- **Files:** everything above.
- **Code example — loader (before/after):**
  ```cpp
  // src/interp/runtime.cpp  (loadModuleFile)
  std::string key = rel;
  rel += ".co";                              // BEFORE
  ...
  //                    AFTER
  rel += ".kn";
  ```
  ```cpp
  // tools/coco.cpp convention candidates BEFORE
  const char* cands[] = {"code/main.co","main.co","code/pin.co","pin.co"};
  // AFTER
  const char* cands[] = {"code/main.kn","main.kn","code/pin.kn","pin.kn"};
  ```
  ```cpp
  // generated native launcher BEFORE
  << "    auto toks = coco::Lexer(kMainSrc, \"main.co\", diags).lexAll();\n"
  // AFTER
  << "    auto toks = kirn::Lexer(kMainSrc, \"main.kn\", diags).lexAll();\n"
  ```
- **Testing:** full rebuild; `G-LINT` on all `.kn`; `G-DIFF` (vm_diff must match byte-for-byte through the newly `.kn`-resolved modules — **critical**: `import` must now resolve to `.kn`); `runall.ps1 -Runner build/Debug/kirnrun.exe` over `examples/*.kn`; `types.ps1` (p/n `.kn`); `negative.ps1`; `asanall.ps1`; `tests/conventions/run.ps1` (must resolve `code/main.kn`/`pin.kn`). Also run `kirn new demo` then `kirn run demo/main.kn` from a fresh dir.
- **Expected outcome:** everything that said `.co` says `.kn`; all modules/imports/conventions resolve; the whole corpus is green on all backends.
- **Risks/trade-offs:** **This is the phase most likely to break things.** Mitigation: do the on-disk rename and the loader/glob edits in a single commit (they are mutually dependent); run `G-DIFF` immediately (the module loader resolving `.kn`). Optional compat: a `--accept-co` loader knob is documented (not default) if the author wants legacy `.co` imported. Commit after green.

---

### Phase 6 — Stdlib + library rename: `lib` → `pet` (module namespace, `stdlib/lib/`, deps layout)
- **Goal:** Rebrand the *library* concept from `lib(s)` to `pet(s)`: stdlib module namespace `import lib.` → `import pet.`, dir `stdlib/lib/` → `stdlib/pet/`, package-install layout `coco_libs/` → `pets/`, library bundles `.cocolib` → `.pet`, global cache `~/.coco/coco-pkg` → `~/.kirn/pets`.
- **Problem:** "the library" is user-facing: users write `import lib.time;`, packages install into `coco_libs/`, library packs are `<n>-<v>.cocolib`. After the Kirn rename, leaving "lib/coco_libs/cocolib" would mix brands.
- **Why it matters:** this is the ecosystem's *library* identity — the registry is the "pets" registry (`https://pets-registry.github.io`), so the local names must match.
- **Design:**
  1. **Stdlib namespace:** `git mv stdlib/lib stdlib/pet`; edit every `import lib.<m>;` → `import pet.<m>;` in `stdlib/pet/*.co`, `tools/*.co`, `examples/`, tests (22 `import lib.` sites found). The loader maps the first import segment to a directory under each stdlib/search dir (see §3.5), so renaming the directory + the import segment is a paired edit — do both in one commit or imports break.
  2. **Deps layout:** `coco_libs/` → `pets/` at every site: `cocorun.cpp:37-38/49-50`, `tools/coco.cpp:369-370/389` (`fs::path("coco_libs")`), gitignore template `:580`, test-scan skip `:1329`, install/remove legacy paths `:1029/1139-1140/1168/1687`, module-search dirs `:1947-1948`. Keep the `libs/`-vs-root split semantics (`pets/libs` + `pets`) as today.
  3. **Global cache:** `home + "/.coco/coco-pkg"` (`tools/coco.cpp:353`, `cocorun.cpp:52-57`) → `home + "/.kirn/pets"`; `.coco-pkg` → `.pets-pkg`.
  4. **Library pack:** `.cocolib` → `.pet` — writer (`tools/coco.cpp:11`, output `:2699`), detectors (`:721/:967`), unpack (`:2838/:2862`), usage strings (`:2929/:2956`), `.gitattributes:21`.
- **Files:** `src/interp/runtime.cpp`, `tools/coco.cpp`, `tools/cocorun.cpp`, `stdlib/lib/*` → `stdlib/pet/*`, all importers, `.gitattributes`.
- **Code example:**
  ```cpp
  // tools/coco.cpp deps dir BEFORE
  return fs::path("coco_libs");
  // AFTER
  return fs::path("pets");
  ```
  ```cpp
  // stdlib module BEFORE
  import lib.json;
  // AFTER
  import pet.json;
  ```
- **Testing:** full rebuild; `G-VERIFY` (stdlib tests via `kirn test` targeting `*_test.kn` must pass — proving `import pet.*` resolves through `stdlib/pet/`); `kirn new demo`, `kirn add mylib` installs into `pets/`; `kirn build lib` produces `n-v.pet` and `kirn install file.pet` unpacks.
- **Expected outcome:** stdlib imports and the package layout say "pet"; bundle packs are `.pet`; the old `coco_libs/`/`lib.` strings are gone.
- **Risks:** forgetting the paired dir-rename+import-rename breaks every stdlib import (loud, catches itself in `G-VERIFY`); legacy `coco_libs/` on disk won't be found → run `git clean`/migrate or keep a compat note. Commit after green.

---

### Phase 7 — Package manager & registry identity (`kirn.toml`/`kirn.lock`, `pets`, `~/.kirn`, registry URL)
- **Goal:** Rebrand all package-manager filenames, folders, caches, and the registry org URL to Kirn/pets.
- **Problem:** currently `coco.toml`, `coco.lock`, `.coco-registry-lib.toml`, `.coco-sha`, and registry URL `github.com/coco-lib/coco-libs`.
- **Why it matters:** these are user-visible and ecosystem-identity. A `kirn.toml` says Kirn; `coco.toml` would say Coco.
- **Design:**
  1. Manifest: writer (`tools/coco.cpp` `new` + `writeManifest` `:103/:121/:245`), loader (`runtime.cpp:1133` `readFileIfExists(dir+"/coco.toml")`), pack (`:2703`), usage (`:2814/:2857`) → all `coco.toml`→`kirn.toml`, `coco.lock`→`kirn.lock`.
  2. Registry metadata: `.coco-registry-lib.toml` → `.pets-registry.toml`, `.coco-sha` → `.pets-sha` (register/verify sites in `tools/coco.cpp`, and the `.gitignore`/scaffold template).
  3. Registry URL + org: `m.repo`/`m.homepage` defaults `github.com/coco-lib/` → `github.com/pets-registry/` (`:566-567`), registry fetch `https://raw.githubusercontent.com/coco-lib/coco-libs/main/` → `.../pets-registry/pets/main/` (`:757/762`), `list online` (`:1252/1257`), browse hints (`:998`), install hint `coco install github.com/coco-lib/...` → `kirn install github.com/pets-registry/...` (`:632`).
- **Files:** `src/interp/runtime.cpp`, `tools/coco.cpp`, `tools/cocorun.cpp`, docs.
- **Code example:**
  ```cpp
  // runtime.cpp BEFORE
  if (readFileIfExists(dir + "/coco.toml", manifest)) {...}
  // AFTER
  if (readFileIfExists(dir + "/kirn.toml", manifest)) {...}
  ```
  ```cpp
  // tools/coco.cpp registry BEFORE
  "https://raw.githubusercontent.com/coco-lib/coco-libs/main/"
  // AFTER
  "https://raw.githubusercontent.com/pets-registry/pets/main/"
  ```
- **Testing:** fresh `kirn new demo` writes `kirn.toml`/`kirn.lock` and `pets/` ignored; `kirn run .`/`kirn test` resolve from the new manifest; `kirn install` writes `.pets-registry.toml`; a `code/main.kn`+`pin.kn` convention app runs from a fresh dir; CI `kirn new demo` → `kirn build` round-trip green.
- **Expected outcome:** every package-manager artefact and URL says `kirn`/`pets`; the ecosystem identity is consistent.
- **Risks:** any leftover `coco.toml` detection breaks `kirn run .`; grep for `coco\.toml|coco_libs|\.coco-registry|/\.coco/|coco-lib` and assert zero after the phase. Commit after green.

---

### Phase 8 — Env vars & CMake options (`COCO_*` → `KIRN_*`, `COCO_LIBS` → `KIRN_PETS`)
- **Goal:** `COCO_LIBS`→`KIRN_PETS`, `COCO_STDLIB`→`KIRN_STDLIB`, `COCO_ASAN`→`KIRN_ASAN`, `COCO_CL`→`KIRN_CL`; plus the niche `COCO_CXX`/`COCO_VERBOSE`/`COCO_LIB_TOOL`/`COCO_TARGET`.
- **Problem:** env/cmake identifiers still say Coco; a user setting `COCO_STDLIB` would be looking for Coco. Consistent rename in a dedicated phase avoids touching them mid-Phase 3/7.
- **Why it matters:** config surface parity; also avoids confusing the ASan build flag.
- **Design:**
  - `CMakeLists.txt:13` `option(COCO_ASAN ...)` → `KIRN_ASAN`, and the `if(COCO_ASAN)` → `if(KIRN_ASAN)`, plus comment.
  - `tools/coco.cpp:358` + `cocorun.cpp:47-48` `std::getenv("COCO_LIBS")` → `"KIRN_PETS"`.
  - `tools/coco.cpp:377` + `cocorun.cpp:62` `COCO_STDLIB` → `KIRN_STDLIB`.
  - `tools/coco.cpp:1916/2587-2588` `COCO_CL` → `KIRN_CL`.
  - `scripts/asanall.ps1` re-invokes with the renamed option.
- **Files:** `CMakeLists.txt`, `tools/coco.cpp`, `tools/cocorun.cpp`, `scripts/asanall.ps1`, docs.
- **Code example:**
  ```cmake
  # BEFORE
  option(COCO_ASAN "Build with AddressSanitizer" OFF)
  if(COCO_ASAN)
  # AFTER
  option(KIRN_ASAN "Build with AddressSanitizer" OFF)
  if(KIRN_ASAN)
  ```
  ```cpp
  // BEFORE
  if (const char* env = std::getenv("COCO_STDLIB")) dirs.push_back(env);
  // AFTER
  if (const char* env = std::getenv("KIRN_STDLIB")) dirs.push_back(env);
  ```
- **Testing:** rebuild with `-DKIRN_ASAN=ON` builds sanitizer targets and `asanall.ps1` uses the new flag; set `KIRN_PETS` to a dir and confirm `import` resolves there; clear the old `COCO_*` vars and confirm they no longer have effect; `G-VERIFY`.
- **Expected outcome:** config surface uses `KIRN_*` only.
- **Risks:** stale env vars lingering on dev machines won't apply (harmless). Commit after green.

---

### Phase 9 — CMake/library `.lib` name `coco_interp.lib` → `kirn_interp.lib` (native build linkage)
- **Goal:** Align the CMake output library names so `kirn build --native` links the right prebuilt `.lib`.
- **Problem:** `tools/coco.cpp:2519` checks for `coco_interp.lib` (and the link line `:2657` names `coco_interp.lib coco_vm.lib coco_sema.lib coco_parser.lib ...`). If Phase 3 renamed *namespaces* but we forgot these strings, native/cross builds break.
- **Why it matters:** the `go build`-like native path resolves the prebuilt runtime by exact lib filename.
- **Design:** update `tools/coco.cpp:2519` `coco_interp.lib` → `kirn_interp.lib`, the whole `coco_*.lib` link list `:2657` → `kirn_*.lib`, plus the CMake target renames from Phase 4 already emit `kirn_*.lib`.
- **Files:** `tools/coco.cpp`, `CMakeLists.txt` (already renamed in Phases 3–4, verify).
- **Code example:**
  ```cpp
  // BEFORE
  fs::exists(fs::path(binRoot) / "coco_interp.lib");
  // AFTER
  fs::exists(fs::path(binRoot) / "kirn_interp.lib");
  ```
- **Testing:** `kirn build --native` and cross `--target=windows-arm64` + `--release` both succeed and produce a working `.exe`; CI arm64 job green.
- **Expected outcome:** native/cross builds link the renamed libs.
- **Risks:** a hard-coded lib name is brittle; grep `\.lib"` in `tools/coco.cpp` to catch all. Commit after green.

---

### Phase 10 — Docs, plan documents, grammar, README, LICENSE, examples README (final prose pass)
- **Goal:** Final "Kirn/pets everywhere" pass over all human-facing documents and grammar, catching anything Phases 2–9 left and re-deriving titles.
- **Problem:** After the code renames, doc references to old tool names/features must be updated, and doc *titles* (`COCO_PLAN.md`, `WHY_USE_COCO_PLAN.md`, `COCO_CROSS_PLAN.md`) reference Coco.
- **Why it matters:** "replace coco with kirn everywhere" includes docs; a doc that says `coco run main.co` (README) after we renamed to `kirn run main.kn` is a lie.
- **Design:**
  1. Re-run a **prose** pass (like Phase 2) for any remaining `Coco`/`coco` (word) and `.co` in `.md`/`.ebnf`/`LICENSE` → `Kirn`/`kirn`/`.kn`.
  2. Update all inline command examples: `$ coco run main.co` → `$ kirn run main.kn`, `coco build` → `kirn build`, `coco test` → `kirn test`, `coco doc` → `kirn doc`, `coco install` → `kirn install`, `cococheck` → `kirncheck`, etc. (README.md, docs/COCO_PLAN.md, docs/FEATURE_GAP_ANALYSIS.md).
  3. `import lib.` → `import pet.` in all doc code samples.
  4. Brand URLs: `coco-lib.github.io` → `https://kirn-lang.github.io`; `github.com/coco-lib/coco-libs` → `https://pets-registry.github.io` (website) and `github.com/pets-registry/pets` (repo); `github.com/rkriad585/coco` → `github.com/rkriad585/kirn`; author website `https://rkriad585.github.io`.
  5. Boards: README logo switches from `logo/ryro-logo-orange-bg-removed.png` (deleted) to `kirn-logos/kirn-logo.png`.
  6. `grammar/coco.ebnf` — filename + all `.co`/`main.co`/`pin.co`/`mod.co` text → `.kn` variants, `import lib.` → `import pet.`, and the EBNF header describing the language name → Kirn. (Rename file `grammar/coco.ebnf` → `grammar/kirn.ebnf` too — see Phase 11.)
  7. Decide + execute the doc-file renames (see §8 recommendation): `docs/COCO_PLAN.md`→`docs/KIRN_PLAN.md`, `COCO_CROSS_PLAN.md`→`KIRN_CROSS_PLAN.md`, `WHY_USE_COCO_PLAN.md`→`WHY_USE_KIRN_PLAN.md`, and update all cross-links between plan docs and the README index.
- **Files:** all `*.md`, `grammar/*.ebnf`, `LICENSE`, `examples/README.md`, plan docs.
- **Code example:**
  ```markdown
  # README.md BEFORE
  ## Quick start
  $ coco run main.co
  # AFTER
  ## Quick start
  $ kirn run main.kn
  ```
- **Testing:** `kirn doc` builds a docs site with no `coco`/`.co` references; grep the whole repo (minus `build/`, `.git/`) for `coco` and `.co` word-boundary and assert only intentional historical/compat mentions remain; all cross-doc links resolve after the doc-file renames.
- **Expected outcome:** the repository reads as a fully "Kirn" project, docs commands are runnable as written.
- **Risks:** doc links breaking on file renames; fix in the same phase. Commit after green.

---

### Phase 11 — Project identity & external references (folder, repo, git, tooling, grammar filename)
- **Goal:** Rebrand the *project itself*: the top-level folder, `.git` metadata references, editor/LLM grammar files, and remaining `coco` in filenames.
- **Problem:** the workspace folder is `C:\Users\rkriad585\Projects\coco`, the grammar file is `grammar/coco.ebnf`, CI/hosted URLs may still say `coco`, and any `Coco.tmLanguage`/tree-sitter would be Coco-branded.
- **Why it matters:** this is the **outermost** identity; it's last because renaming the folder is disruptive and easiest once everything inside is already Kirn. (The GitHub repo name + homepage + local remote have **already** moved to `kirn` — as of 2026-09-18.)
- **Design:**
  1. `grammar/coco.ebnf` → `grammar/kirn.ebnf` (do here, after Phase 10 content edits), update references (docs, tooling, CI if referenced).
  2. Rename the repo folder `coco` → `kirn` (a filesystem-level `Rename-Item`, then `git remote set-url` already points at `https://github.com/rkriad585/kirn.git`). Update any path assumptions in scripts (they use `$PSScriptRoot`/relative paths, so safe).
  3. Confirm `.github`/CI hosted URLs say `rkriad585/kirn`, `kirn-lang.github.io`, `pets-registry.github.io`.
  4. Check for editor/LLM grammar files (`.tmLanguage`, `tree-sitter-*`): if present, rename/rebrand to `Kirn`/`.kn`.
  5. Confirm GitHub-language-detection: `.gitattributes` now lists `*.kn text eol=lf` (Phase 5) so GitHub tags `.kn`; optionally add a `linguist-language` if desired.
- **Files:** folder, `grammar/kirn.ebnf`, `.github/`, any grammar/tooling assets, remote URL (verify).
- **Code example (grammar file):**
  ```
  # grammar/kirn.ebnf (renamed from coco.ebnf)
  (* Encoding: UTF-8. File extension: .kn. Layout is free: ... *)
  ```
  ```powershell
  # folder rename (run once, last)
  Rename-Item -LiteralPath "C:\Users\rkriad585\Projects\coco" -NewName "kirn"
  git remote -v   # expected: https://github.com/rkriad585/kirn.git
  ```
- **Testing:** open the renamed folder, `cmake -S . -B build`, build, full `G-VERIFY` from the new path; `git remote -v` shows the new URL; grammar parses `.kn`.
- **Expected outcome:** the whole project, from folder name to grammar to remote, is "Kirn".
- **Risks:** renaming the working directory mid-session breaks open editors/PowerShell cwd; do as the final, deliberate step with a clean tree and commit first. Commit after green (and tag `rename-complete`).

---

### Phase 12 — Final sweep & regression: zero `coco`/`.co`, full-matrix green
- **Goal:** The verification capstone: a scripted assertion that *all* old tokens are gone (except explicitly-declared historical/compat notes) and every harness passes with the new names.
- **Problem:** a rename this broad needs an automated "are we done?" gate, so a later commit can't silently reintroduce `coco`/`.co`.
- **Why it matters:** "test everything to work perfectly" (§1) is only provable by a repeatable full-matrix run.
- **Design:**
  1. Add `scripts/rename_verify.ps1` that:
     - Fails if any tracked file under `examples/`,`stdlib/`,`tests/`,`tools/`,`src/`,`grammar/` matches `\bcoco\b`, `coco_`, `cocorun|cococheck|cocolex|cocoparse`, `\.co\b`, `coco\.toml`, `coco_libs`, `\.coco-`, `import lib\.`.
     - Explicitly **allowlists** `COCOB`/`.cob` (frozen bundle format — deliberate non-goal) and the allowed historical/compat `.co` mentions in docs.
     - Runs the full harness matrix with new names and reports pass/fail per suite.
  2. Wire `rename_verify.ps1` into `.github/workflows/ci.yml` as a `rename-regression` job so it runs on every future PR.
  3. Produce a final `_rename/report.md` summarizing before/after counts (from Phase 1 baseline vs now).
- **Files (new):** `scripts/rename_verify.ps1`; `.github/workflows/ci.yml` (+job).
- **Code example (regression grep, PowerShell):**
  ```powershell
  $banned = 'coco','coco_','\.co(?![A-Za-z0-9])|coco\.toml|coco_libs|\.coco-|import lib\.'
  $allow  = 'COCOB','\.cob'   # frozen bundle format (author decision)
  foreach ($f in Get-ChildItem -Recurse -File -Include *.kn,*.cpp,*.h,*.ps1,*.md,*.ebnf,*.yml |
             Where-Object { $_.FullName -notmatch '\\(build|\.git)\\' }) {
    $t = [IO.File]::ReadAllText($f.FullName)
    foreach ($pat in @('coco','.co','coco_','coco_libs')) {
      if ($t -match [regex]::Escape($pat)) { Write-Error "stale '$pat' in $($f.Name)" }
    }
  }
  ```
- **Testing:** run `rename_verify.ps1` locally — must be all-green; CI `rename-regression` passes.
- **Expected outcome:** a CI-enforced guarantee that the repo IS Kirn and STAYS Kirn.
- **Risks:** overstrict ban could flag a *legitimate* future string; keep an explicit allowlist of intentional historical mentions + frozen bundle tokens (documented in the script header). Commit after green.

---

### Phase 13 — Optional: `.co` backward-compat bridge (author decision)
- **Goal (optional, not default):** A documented, opt-in loader knob so legacy `.co` files can still be imported/produced during a transition period.
- **Problem:** if real users already have `.co` files, a hard cut to `.kn` breaks them; some projects value a grace window.
- **Design:** add a `--accept-co` flag (or `kirn.toml` `[tool] accept_legacy_extension = true`) that makes the loader's `rel += ".kn"` try `.kn` first, then fall back to `.co`, and lets `kirn build / kirn run` accept `.co` args. Keep the **default = pure `.kn`**.
- **Implementation:** touch the exact Phase-5 sites (`runtime.cpp` loader + `tools/coco.cpp` arg/glob handling + `cocorun` extension dispatch) behind the flag/bool read from the manifest or env `KIRN_LEGACY_CO`.
- **Files:** `src/interp/runtime.cpp`, `tools/coco.cpp`, `tools/cocorun.cpp`, `docs`.
- **Code example:**
  ```cpp
  // runtime.cpp (loader) behind acceptLegacyCo
  rel += ".kn";
  std::string alt = rel.substr(0, rel.size()-3) + ".co";  // fallback
  if (!acceptLegacyCo) alt.clear();
  ```
- **Testing:** with the flag on, import a `.co` module and `kirn run old.co` works; with the flag off (default), `.kn` only; `rename_verify.ps1` still passes with the flag off.
- **Expected outcome:** a safe, opt-in transition path — clearly documented as non-default.
- **Risks:** keeping two extensions alive adds maintenance surface; the author should decide *when* (now, later, or never) — Phase 13 exists so the choice is explicit rather than accidental. This phase is optional and does not block Phases 1–12.

---

## 6. Decision log (decisions made in this plan, with rationale)

| # | Decision | Rationale |
|---|---|---|
| D1 | `.co`→`.kn`, stdlib `import lib.`→`import pet.`, `.cocolib`→`.pet`; **`.cob`/`COCOB` frozen** (not renamed) | Author decision: source `.kn`, library = "pet(s)", bundle format "not important and not needed" |
| D2 | Pure `.kn` by default; `.co` compat is opt-in (Phase 13) | Simplicity + explicit transition; matches "replace everywhere" intent |
| D3 | Case forms mapped: `kirn` (id), `Kirn` (prose), `KIRN` (env/cmake); library term `pet` (prose) / `PETS` (env) | Prevents blind-case bugs; the repo already uses these forms distinctly |
| D4 | CMake targets + executables + namespaces + manifest all renamed, not just strings | Otherwise the codebase says "Kirn" while the C++/build/package identity says "Coco" |
| D5 | Doc plan filenames (`*_PLAN.md`) renamed in Phase 10/11 (recommended, not forced) | Historical artifacts; renaming is cleaner but must happen with cross-link fixes |
| D6 | Registry org `github.com/coco-lib/coco-libs`→`github.com/pets-registry/pets`; project site `coco-lib.github.io`→`kirn-lang.github.io` | Author-provided URLs: library website `https://pets-registry.github.io`, language website `https://kirn-lang.github.io` |
| D7 | No fractional naming (e.g. keeping `coco` in the namespace) after Phase 3 | "Everywhere" was requested; avoid half-migrated identifiers |
| D8 | Optional `--accept-co`/`KIRN_LEGACY_CO` bridge is **non-default** | Keeps the hard rename unambiguous while offering a documented escape hatch |
| D9 | Deps folder `coco_libs/`→`pets/`, global cache `~/.coco/coco-pkg`→`~/.kirn/pets`, `.coco-registry-lib.toml`→`.pets-registry.toml` | Aligns local names with the "pet(s)" library brand and `pets-registry` website |

---

## 7. Risks & mitigations (consolidated)

| Risk | Phase | Mitigation |
|---|---|---|
| Global blind replace breaks build | all | §2 phased approach + per-phase green gate; blocklist in Phase 2 rename script |
| `.co`→`.kn` breaks module `import`/convention resolution | 5 | Ship loader+resolver+glob edits atomically in one commit; `G-DIFF` as the canary |
| `lib`→`pet` breaks stdlib imports (dir + import paired) | 6 | Rename `stdlib/lib/` and all `import lib.` sites in the same commit; stdlib tests via `kirn test` are the gate |
| `.cocolib`→`.pet` writer/reader drift | 6 | Author+unpack+detect+`.gitattributes` in the same commit; reinstall a `.pet` in CI |
| Missed tool call-site breaks CI | 4 | Grep `coco|cocorun|cococheck|cocolex|cocoparse` in `scripts/`,`tests/`,`.github/`; assert zero |
| Native build links wrong `.lib` | 3,4,9 | Rename CMake targets + `coco_*.lib` strings together; full native/cross build test |
| Env/cmake flag mismatch | 8 | Rename `COCO_*`→`KIRN_*`/`KIRN_PETS` cohesively; ASan build test with `-DKIRN_ASAN=ON` |
| Docs say one thing, code another | 10 | Final prose pass + `kirn doc` build; grep whole repo post-migration; update brand URLs |
| Folder/remote rename disrupts work | 11 | Last phase, clean tree, commit first; repo URL already moved via `gh`; folder rename is filesystem-only |
| Frozen dead-records: `.cob`/`COCOB` still contain "coco" | 12 | Explicit allowlist in `rename_verify.ps1`; documented in plan header as deliberate non-goal |
| `.kn` niche collisions | 2,5 | Documented in §2; low risk; `.kn` is not a *code* extension anywhere mainstream |

---

## 8. Recommended execution order & acceptance

**Order (optimized for "green at every step" then "identity complete"):**
```
Phase 1  baseline + safety net          (read-only, quickest)
Phase 2  prose Coco→Kirn                (lowest risk, huge visible change)
Phase 3  C++ namespace coco→kirn        (mechanical, full rebuild gate)
Phase 4  CLI tools + CI/scripts names   (command line = kirn)
Phase 5  .co→.kn source ext + loader    (the core risk; atomic with globs)
Phase 6  lib→pet stdlib + deps layout   (import pet.*, pets/, .pet packs; pairs dir+import)
Phase 7  package manager + registry URL (kirn.toml/pets-registry)
Phase 8  COCO_*→KIRN_* env/cmake        (config surface)
Phase 9  kirn_interp.lib linkage        (native/cross build)
Phase 10 docs/grammar/plan final pass   (README, .ebnf, brand URLs, plan doc renames)
Phase 11 project folder + remote + grammar file name   (repo URL already moved via gh)
Phase 12 regression gate (rename_verify.ps1 + CI job)
Phase 13 OPTIONAL legacy-.co bridge     (decide explicitly; non-default)
```

**Acceptance criteria (definition of "test everything to work perfectly"):**
1. `cmake -S . -B build && cmake --build build --config Debug` succeeds from scratch.
2. `scripts/rename_verify.ps1` reports **zero** stale `coco`/`.co`/`coco.toml`/`coco_libs`/`import lib.` matches (allowlisted: `COCOB`/`.cob`) and all harnesses PASS.
3. Full corpus green on all backends: `runall.ps1` (examples), `types.ps1` (p/n), `negative.ps1`, `vm_diff.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`.
4. `kirn new demo` → `kirn run demo/code/main.kn` → `kirn test` → `kirn build` (native + `.cob` fallback) all work; `kirnrun demo.cob` runs the bundle.
5. CI (build-test + cross-arm64) passes end-to-end with renamed artifacts.
6. `git log` shows one clean commit per phase; `git -C <newfolder> remote -v` shows `https://github.com/rkriad585/kirn.git`.
7. Optional-if-adopted: legacy `.co` import works **only** with `--accept-co`/`KIRN_LEGACY_CO`.

**Note on the `WHY_USE_COCO_PLAN.md` artefact:** since it was authored under the Coco identity and the requirement is "replace coco with kirn everywhere," Phase 10 includes renaming it to `WHY_USE_KIRN_PLAN.md` and updating its internal `coco`/`.co` references — unless the author prefers to keep naming-history artefacts as-is (D5 marks this as recommend-rename).

---

*Plan v1 authored 2026-09-03 (Coco→Ryro) after a full source audit and 2026 web research; v2 retargeted 2026-09-18 to Coco→Kirn per author decision (`.kn`, `lib→pet`, `pets-registry.github.io`, `kirn-lang.github.io`, GitHub repo already renamed to `rkriad585/kirn`). Phases are ordered for a green commit after every step; the riskiest layer (`.co`→`.kn` in the module loader, Phase 5) is isolated and gated by the differential harnesses, with the `lib→pet` stdlib rebrand (Phase 6) as the second-highest-risk pairing.*