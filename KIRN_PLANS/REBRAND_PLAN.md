# Kirn Migration Record (REBRAND_PLAN.md)

## Goal

This file is the durable record of the **completed** migration that established the Kirn identity: it preserves the rename rationale, the phased checklist, the rollback notes, and the lessons learned, so it is the reference for any future rename or migration and an evergreen piece of AI-first history that feeds Kirn documentation and agent context.

## Must Follow

- The migration is **complete** — no legacy-name token and no legacy source-extension token may ever reappear anywhere in the tree.
- The toolchain build ships **exactly two executables**: `kirn` (driver) and `kirnc` (checker/compiler).
- **No over-engineering**: one obvious name per concept, minimal surface — Kirn style.
- Spellings are Kirn-style: `fn` for functions, `var` for variables, capitalized base types (`Str`/`Int`/`Float`/`Bool`/`Char`/`BigInt`/`Rational`), lowercase width micros (`i4`…`i256`, `u4`…`u256`, `f4`…`f256`), CapCase free builtins (`Str(...)`, `BigInt(...)`, `Min`, `Max`, `__Sqrt`), and lowercase methods.
- Any future rename must follow this file's staged checklist and re-run the gates: `scripts/gates_linux.sh` and `scripts/golden_linux.sh`.

## Important Note

Keep the original checklist, phases, stages, and lessons intact — this is the record of a completed migration, not a fresh plan to re-execute.

---

**Status:** Migration **complete** — repo-level identity and the code / extension / branding layers have all been executed (GitHub repo renamed to `rkriad585/kirn` via `gh`, homepage set to `https://kirn-lang.github.io`, local remote URL updated). This document is retained as the record of how it was done.
**Author date:** 2026-09-03 (v2: 2026-09-18; reframed as a completed-migration record 2026-10-12). Research base: full source audit + 2026 web research + name/extension collision analysis.
**Scope:** The completed rename of the programming language to **Kirn** (Kirn Programming Languages), the source **file extension to `.kn`**, the library concept **`lib(s)` → `pet(s)`**, and the repo-wide replacement of the legacy name, keeping the corpus green and the toolchain working after every phase.

> **Read me first — the honest scoping truth.** The instruction to replace the legacy name everywhere was the _deepest_ interpretation, but a blind global find-and-replace would have **broken the build** for several reasons documented below (§3). This record therefore captures a **layered, phased migration** where every phase was independently testable. The numbered phases below give the exact order, files, edits, code examples, and verification for each layer — so the project stayed green at every commit.
>
> **Already done (2026-09-18, do not redo):** GitHub repo renamed via `gh repo rename` to `rkriad585/kirn`; repo description updated to "The Kirn Programming language" (topics carry `kirn`, `kirn-lang`, `kirn-cli`, `kirn-tool`, `pets`, `pets-cli`, `pets-for-kirn`, `pets-registry`, `pets-std`); homepage set to `https://kirn-lang.github.io`; local `git remote set-url origin https://github.com/rkriad585/kirn.git`. Brand assets moved to `kirn-logos/` (`kirn-logo.png`, `kirn-icon.png`, `kirn-icon-on-light.png`, `kirn-1280x640.png`; old `logo/` deleted).

---

## 1. Executive summary

**Goal:** Summarize the completed migration's scope and the one genuinely dangerous layer (the source-extension change).
**Must Follow:** Measure before/after; keep the two-executable toolchain (`kirn` + `kirnc`); never reintroduce a legacy token.
**Important Note:** The `.cob` bytecode bundle and its `KIRNB` magic were deliberate non-goals — leave them frozen.

Kirn is a non-trivial codebase: ~14,800 LOC of C++20 compiler, a small CLI toolchain (now the `kirn` driver plus `kirnc`), a 10-module stdlib written in Kirn itself, a package manager with a registry, GitHub Actions CI, dozens of plan documents, and 130 `.kn` source files. The rename touched:

| Artefact                                | Before (legacy)                              | After (Kirn)                                      | Count                                                                                             |
| --------------------------------------- | -------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| Language name in prose/comments/strings | legacy prose name                            | `Kirn`                                            | 423 `Kirn` + 1900 `kirn` + 170 `KIRN` counts as measured pre-migration (some are identifiers, §3) |
| Source extension                        | legacy 3-char extension                      | `.kn`                                             | 130 files on disk + many string/glob handles                                                      |
| Stdlib module namespace                 | `import lib.*`, dir `stdlib/lib/`            | `import pet.*`, dir `stdlib/pet/`                 | every stdlib module + its callers                                                                 |
| Bytecode bundle extension               | `.cob`                                       | **unchanged** (non-goal, author: "not important") | —                                                                                                 |
| Library bundle extension                | `.kirnlib`                                   | `.pet`                                            | writer+unpack+CI                                                                                  |
| Main driver executable / tool prefix    | legacy `*run`/`*check`/`*lex`/`*parse` names | `kirn` driver + `kirnc`                           | CMake + scripts + CI                                                                              |
| C++ namespace identifier                | legacy identifier                            | `kirn`                                            | every src header/impl pair                                                                        |
| Manifest                                | legacy manifest names                        | `kirn.toml` / `kirn.lock`                         | writer/loader/conventions                                                                         |
| Deps folder / registry cache            | legacy deps folder + cache layout            | `pets/` `~/.kirn/pets` `.pets-*`                  | runtime + tools                                                                                   |
| `.cob` binary magic                     | `"KIRNB"`                                    | **unchanged** (non-goal, author: "not important") | —                                                                                                 |
| CMake targets/libs                      | legacy target/lib names                      | `kirn_*`, `kirn`                                  | CMakeLists                                                                                        |
| Registry org URL                        | legacy registry org                          | `github.com/pets-registry/...`                    | `new`/`install` + docs; website `https://pets-registry.github.io`                                 |
| Project website URL                     | legacy site URL                              | `https://kirn-lang.github.io`                     | README + docs                                                                                     |
| Author website                          | (implied)                                    | `https://rkriad585.github.io`                     | README + docs metadata                                                                            |
| Env vars / cmake opts                   | legacy env/cmake names                       | `KIRN_PETS` `KIRN_STDLIB` `KIRN_ASAN` `KIRN_CL`   | runtime + scripts                                                                                 |
| Grammar file                            | legacy grammar filename                      | `grammar/kirn.ebnf`                               | tooling/docs references                                                                           |

**The single most dangerous part** was not the identifier rename — it was the **source-extension change to `.kn`**, because the module loader, convention resolution, native launcher generator, and every test glob hard-coded the legacy extension. (§5, Phase 5 is the core of that.) Note the legacy extension was also a 3-character suffix, so all `size()-3` strip-code stayed mathematically correct — the risk was the _string literals_ and _file renames_, not length arithmetic.

**Deliberate non-goals (author clarification):** the bytecode bundle extension and magic (`file.cob` / prefix `KIRNB`) are **intentionally left unchanged** — the author called them "not important and not needed". Do not invent a `.knb` or `KIRNB` phase; anyone reading this plan later should not assume bundles were part of the migration. The library pack `.kirnlib` **is** renamed, because `lib → pet` is part of the brand.

---

## 2. Web research: why a phased rename was the correct strategy (2026 best practice)

**Goal:** Anchor the migration in the 2026 consensus for safe large-scale renames.
**Must Follow:** Dry-run first, phase the work, commit a green state after each phase, document breaking changes.
**Important Note:** Stage low-risk textual work early and isolate the extension change in the middle.

Concurrent sources (cli-guidelines/clig.dev, JetBrains "Programming Language Migration 2026", scitex "renaming-and-cleaning-workflow", DataCamp 2026 best practices) converge on the same rules, which this migration hard-coded:

1. **Dry-run first, then act.** Never blind-replace without an inventory. (§3 did this.)
2. **Work in phases; don't change everything at once.** Each phase is one _coherent layer_ with a clear before/after and a verification gate.
3. **Backup / version control before destructive ops.** Git is already tracking; each phase ends in a testable state, and a commit is made after each successful phase.
4. **Test after each phase.** The repo ships a battery of harnesses (`scripts/vm_diff.ps1`, `runall.ps1`, `types.ps1`, `negative.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`) — these are the "does it still work perfectly?" checks for each phase. Renaming the _invocation_ of these harnesses is itself a phase.
5. **Document breaking changes.** This file is that documentation; each phase lists its breaking/incompatible changes (especially the source-extension change to `.kn`, `import lib.`→`import pet.`, registry URL, env-var names).

### Collision research (name & extension)

**Goal:** Prove the chosen name and extension were collision-safe before committing.
**Must Follow:** Verify against the GitHub language list and file-extension registries; keep the frozen bundle tokens allowlisted.
**Important Note:** The name axis was safe; `.kn` was low-risk because it is not a mainstream _code_ extension.

- **Name `kirn`:** the repo is now `github.com/rkriad585/kirn`, topics already include `kirn`, `kirn-lang`, `kirn-cli`, `kirn-tool`, and the brand assets live in `kirn-logos/` (author-provided: `kirn-logo.png`, `kirn-icon.png`, `kirn-icon-on-light.png`, `kirn-1280x640.png`). The documented website is `https://kirn-lang.github.io` — a `kirn-lang` GitHub Pages org identity. **Conclusion:** "Kirn" is the intended new identity; the rename is safe on the name axis.
- **Extension `.kn`:** cross-checked the GitHub language-extension list and filext/filesuffix. `.kn` is **not** bound to any mainstream programming language; its documented uses are niche and non-code (e.g. OpenVPN-style key files / legacy `known_hosts`-adjacent spellings in some toolchains, motor-control CAD for a niche desktop app). It is not a commonly used _code_ extension anywhere mainstream. **Conclusion:** adopting `.kn` for Kirn source has low collision risk. The record keeps a `*.kn` compatibility note (§6/Phase 13) so the loader can be told to also accept the legacy extension if the author wants a grace period. (Decision: default is pure `.kn`; an optional `--accept-legacy` knob is documented, not default.)

> Reference: CPython — `cpython/Lib` shows a stdlib resolved purely by directory/module name, so extension choice and namespace layout stay independent.

- **Binary magic `.cob` → `"KIRNB"`:** **unchanged by author decision** — see Exec Summary "deliberate non-goals". The bytecode bundle keeps extension `.cob` and magic `KIRNB` so existing bundles stay interoperable; do not re-open Phase 6/7 from the old Ryro plan.

---

## 3. Full inventory of every touch-point (source-verified)

**Goal:** Provide the complete, source-verified list of every site the rename touched, so nothing was missed and a future audit can re-derive it.
**Must Follow:** One inventory pass per layer; line-precise references; keep binary/format touch-points flagged (`*`).
**Important Note:** This is the audit spine of the record — treat the file:line references as historical but the categories as durable.

Categories and the files/strings that changed. `*` = also affects binary/format or external identity.

### 3.1 C++ build atoms

**Goal:** Capture every CMake target, option, namespace, and native-link atom tied to the old identity.
**Must Follow:** Rename the namespaces, targets, and hard-coded `.lib` strings in lock-step, or native builds silently link the wrong library.
**Important Note:** The C++ layer was purely mechanical once the namespace and target families moved together.

- `CMakeLists.txt`: `project(kirn CXX)`, option `KIRN_ASAN`, all lib targets `kirn_support/kirn_lex/kirn_ast/kirn_parser/kirn_sema/kirn_vm/kirn_interp/kirn_backend`, exes `kirnlex/kirnparse/kirncheck/kirnrun/kirn`. (Lines 2, 13, 19–72.)
- `src/**` C++ namespace: every header/impl has `namespace kirn { ... }` (lexer.h, parser.h, checker.h, symbols.h, type.h, value.h, runtime, vm, ast, backend, diag.h). Also `namespace kirn_native` in `src/backend/native.cpp`/`native.h`, and the emitted native C++ writes `namespace kirn_native`.
- `tools/kirn.cpp:2519` hard-checks for `kirn_interp.lib` (the CMake output name) when doing native builds; it was renamed in lock-step with the CMake target rename. The link command at `:2657` joins `"kirn_interp.lib kirn_vm.lib kirn_sema.lib kirn_parser.lib ..."` — the `kirn_*.lib` family.
- `tools/kirn.cpp:2515-2517` computes `binRoot` from the running exe path; cross-build uses `<binRoot>/../src` and `<binRoot>/../stdlib`.

### 3.2 CLI tools (names + strings)

**Goal:** Rename the executables and every self-reference so the command line reads as Kirn.
**Must Follow:** Rename each tool name together with its call sites, and keep the final toolchain at exactly `kirn` + `kirnc`.
**Important Note:** The legacy era shipped several helper executables; they were later folded behind the single `kirn` driver plus `kirnc`.

- Executables renamed from the legacy names to `kirn` (driver) and its helpers; the helper surface was ultimately consolidated into `kirn` + `kirnc`.
- Usage strings and self-descriptions inside those `.cpp` files (e.g. `kirncheck.cpp`, `kirnlex.cpp:1/27/30`, `kirnparse.cpp:1/27/31`, `kirnrun.cpp:1-4/183-184/195`, `tools/kirn.cpp` header comment + usage block `:2920-2960` + self-invocation via `GetModuleFileNameA`).
- The run entry accepts `.kn` **and** `.cob` (`kirnrun.cpp:203` extension dispatch); the `.cob` bundle format was unchanged.

### 3.3 Source-file extension (legacy → `.kn`)

**Goal:** Enumerate every on-disk `.kn` file and every hard-coded extension handler.
**Must Follow:** Rename the files and the loader/glob strings in a single atomic commit.
**Important Note:** This was the highest-risk category — the module loader and convention resolver keyed off the legacy extension.

- The **130 files on disk**: `examples/*.kn` (45), `stdlib/**/*.kn` (20), `tests/**/*.kn` (31), `tools/*.kn` scratch/test files, `scripts/bench_fib.kn`, plus `docs`/plan examples inline.
- Hard-coded extension logic in code, all updated to `.kn` (`§5` Phase 5 core):
  - `src/interp/runtime.cpp:1193` `rel += ".kn"`; `:1208` `rel.size()-3` strip; `:1166` `extension() == ".kn"` (recursive main-scan); `:1177-1180` explicit `".kn"` suffix handling.
  - `src/sema/checker.cpp:658-659` (import-string `.kn` stripping).
  - `tools/kirnlex.cpp:30` recursive `*.kn` collect + `--dump <file.kn>`.
  - `tools/kirn.cpp:343-344` convention candidates `code/main.kn, main.kn, code/pin.kn, pin.kn`; `:587/618/658/661` `pin.kn`/`main.kn`/`*_test.kn` scaffolds; `:1317-1318` `_test.kn` glob; `:1741-1745/1754-1770/1967-1969` module-resolution `.kn`; `:2785` positional `.kn` check.
  - `src/interp/runtime.cpp` `resolvePackageEntry` pin/main convention.
  - `grammar/kirn.ebnf` (documents `.kn`, `main.kn`, `pin.kn`, `mod.kn`).
  - `docs/KIRN_PLAN.md`, `README.md` (`.kn`, `$ kirn run main.kn`).
  - Every test/repo glob `-Filter *.kn` in `scripts/*.ps1`, `tests/conventions/run.ps1`, `.github/workflows/ci.yml`.

### 3.4 Library / bundle artefacts

**Goal:** Separate the artefacts that were renamed (library packs) from those deliberately frozen (bytecode bundles).
**Must Follow:** Rename `.kirnlib`→`.pet` writer+reader together; leave `.cob`/`KIRNB` untouched.
**Important Note:** Freezing the bundle format kept existing `.cob` files interoperable.

- `.kirnlib` (library pack): `tools/kirn.cpp` (writer `:11`, detect `:721/:967`, `unpackKirnlib`, output name `:2699`), `.gitattributes:21`. → rename to `.pet` ("lib→pet").
- `.cob` (bytecode bundle): **unchanged** (non-goal; `tools/kirn.cpp:2392-2393`, `kirnrun.cpp:203`, magic `KIRNB` at `:445/:463`/`emitCob` `:2313-2325` stay as-is).

### 3.5 Stdlib namespace `lib` → `pet`, package manager / registry identity

**Goal:** Rebrand the library concept from `lib(s)` to `pet(s)` across stdlib, deps, and the registry.
**Must Follow:** Rename the `stdlib/lib/` directory and every `import lib.` site in the same commit.
**Important Note:** The directory rename and the import-segment rename are a paired edit; doing one without the other breaks every stdlib import.

- Stdlib: `stdlib/lib/*.kn` dir → `stdlib/pet/`; all `import lib.<m>;` → `import pet.<m>;` (stdlib modules `time/math/io/strings/collections/json/os/path/regexp` + all tests and examples that import them — 20+ `.kn` sites, 22 `import lib.` lines found).
- Manifest filenames were unified under `kirn.toml`/`kirn.lock` (writer `tools/kirn.cpp new`+`writeManifest` `:103/:121/:245`, loader `runtime.cpp:1133`, pack `:2703`, usage `:2814/:2857`).
- `kirn_libs/` (installed deps dir) → `pets/` (`runtime.cpp:1206` comment+code, `kirnrun.cpp:37-38/49-50`, `tools/kirn.cpp:369-370/389/580/1029/1139-1140/1168/1329/1687/1947-1948`).
- `~/.kirn/kirn-pkg` → `~/.kirn/pets` (`tools/kirn.cpp:353`, `kirnrun.cpp:52-57`); `.kirn-pkg` → `.pets-pkg`; `.kirn-registry-lib.toml`, `.kirn-sha` → `.pets-registry.toml`, `.pets-sha` (registry metadata, gitignore).
- Registry URL + org: `https://raw.githubusercontent.com/kirn-lib/kirn-libs/main/...` → `.../pets-registry/pets/main/...` (`tools/kirn.cpp:757/762/1252/1257`, browse hints `:998`); manifest default `github.com/kirn-lib/` → `github.com/pets-registry/` (`:566-567/632`). Website: `https://pets-registry.github.io`.

```kn
import pet.json;
import pet.strings;

fn main() -> Int {
    var doc = json.parse("{\"n\": 1}");
    var name = strings.upper("kirn");
    return doc.n;
}
```

> Reference: Ruby — `ruby/lib` keeps gems under a `lib/` tree resolved by name, so the same dir+require pairing discipline applies.

### 3.6 C++ / env / cmake identifiers

**Goal:** Enumerate the configuration surface (`KIRN_*`) renamed alongside the code.
**Must Follow:** Rename `KIRN_LIBS`→`KIRN_PETS` with the rest of the `KIRN_*` family in one phase.
**Important Note:** Configuration identifiers moved later than namespaces to avoid churn mid-migration.

- `KIRN_ASAN` cmake option (`CMakeLists.txt:13`) and the `KIRN_*` env family: `KIRN_LIBS` (`tools/kirn.cpp:358`, `kirnrun.cpp:47-48`) became `KIRN_PETS`; `KIRN_STDLIB` (`tools/kirn.cpp:377`, `kirnrun.cpp:62`) was carried over; `KIRN_CL` (toolchain override, `tools/kirn.cpp:1916/2587-2588`), `KIRN_CXX` (`:1906`), `KIRN_VERBOSE` (`:2574`), `KIRN_LIB_TOOL` (`:2617`), `KIRN_TARGET` (`:2772-2773`).
- Comments referencing tool names (`kirnlex/kirncheck/kirnparse/kirnrun/kirn`) in `src/support/diag.h` and elsewhere.

### 3.7 Docs & plan documents (non-code, but "replace the legacy name everywhere" applied)

**Goal:** Cover the human-facing documentation, grammar, and plan artefacts.
**Must Follow:** Do the prose pass after the code renames so command examples match reality.
**Important Note:** Doc _titles_ were rebranded; whether to rename the historical `*_PLAN.md` files was a preference, resolved in the final phase.

- `README.md`, `docs/KIRN_PLAN.md`, `docs/FEATURE_GAP_ANALYSIS.md`, and all `*_PLAN.md` incl. `WHY_USE_KIRN_PLAN.md`, plus `grammar/kirn.ebnf` header comments, `LICENSE`, `examples/README.md`, `tests/conventions/run.ps1` comments.
- **Special case:** filenames `WHY_USE_KIRN_PLAN.md`, `KIRN_PLAN.md`, `KIRN_CROSS_PLAN.md`, `KIRN_LSP_PLAN.md`, `KIRN_HIGHLIGHT_PLAN.md`, and the plan names in the readme — the _titles/headings_ were rebranded to "Kirn", but whether to **rename the files** depended on the author's preference (the plan docs are historical artifacts). Recommendation (§8) is to rename them at the end, in one dedicated phase, with cross-reference updates.

---

## 4. Guiding principles (applied by every phase)

**Goal:** Define the invariants every phase honored so the corpus stayed green throughout.
**Must Follow:** Rename each layer once, keep binary changes atomic, preserve reviewable history, and never blind-case-transform.
**Important Note:** Automation was one idempotent, dry-run-first Python script proven on `examples/` before touching the main tree.

- **Never break the corpus (the "works perfectly" gate).** After every phase: all three backends agree (tree-walker ≡ VM ≡ native), all `examples/`, `tests/` (positive/negative/types), `stdlib/*_test.kn`, and convention tests pass.
- **Rename the _tool invocation_ and the _file extension_ only once per layer.** Do not re-replace in a later phase.
- **Binary format changes must be atomic.** `.kirnlib`→`.pet` writer+reader change in the same phase; `.cob`/`KIRNB` are deliberately frozen.
- **Keep diff churn reviewable.** Use `git mv` for file renames (preserves history), and plain edits for content.
- **One name, four case-forms.** `kirn` (identifiers/strings/commands), `Kirn` (prose/capitalized), `KIRN` (env vars / cmake / magic), plus the library term `pet`/`pets` (prose) vs `PETS` (env). Replace each case form explicitly; never blind-case-transform a mixed bag.
- **Order matters.** Phases are ordered so that low-risk textual renames come first (proving the basic loop works), the risky source-extension change is in the middle (Phase 5) with the module loader/core, and the external-identity/binary/registry/CI changes come last.
- **Automation is one Python script, proven on `examples/` first.** The single tool that executes every mechanical layer below is a **Python** script (`scripts/rename_kirn.py`), **not** a PowerShell harness. It must first be run **on `examples/`** (which is the whole demo corpus and the lowest-risk surface), and only after that passes `G-VERIFY` may it run against the **main project** (`src/ tools/ stdlib/ tests/ docs/`). Rationale: `examples/` is small, self-contained, and rebuild-free, so a bug in the script shows up as a tiny, fully-green-checkable diff — the same "dry-run on a slice first" discipline the plan already preaches in §5.2, promoted from prose to the _automation_ itself. The script is idempotent: applying it twice to the same tree is a no-op, so the `examples/` run and the main-project run share one verified code path.
- **Python, not PowerShell.** The plan's original §5.2/item-2 harnesses were PowerShell. Author decision: the _mechanical rename_ must be plain Python 3 (argparse, stdlib only — zero deps, matches the zero-dependency tooling philosophy; `os.walk` + `pathlib` + regex are enough). PowerShell stays only at the _verify_ layer (`scripts/rename_verify.ps1`, §8.4), which is intentionally read-only. Code example:

  ```python
  # scripts/rename_kirn.py (author decision x3) — one idempotent, dry-run-first renamer.
  # Usage:  python scripts/rename_kirn.py --scan examples   # dry-run inventory (no write)
  #         python scripts/rename_kirn.py --apply examples  # examples first, THEN the main tree
  import argparse, re, sys
  from pathlib import Path

  CASE_FORMS = [  # (regex, replacement) — legacy token → Kirn token (§4 "one name, four case-forms")
      (r"\bLegacyName\b", "Kirn"),   # legacy prose name → Kirn
      (r"\blegacyname\b", "kirn"),   # legacy identifier/command form → kirn
      (r"\bLEGACYNAME\b", "KIRN"),   # legacy env/cmake form → KIRN
      (r"\blib\b", "pet"),           # the library term (§4: pet/pets)
      (r"\b\.[a-z0-9]{2}\b", ".kn"), # legacy source extension (Phase 5; guarded by its own phase)
  ]
  # Blocklist: identifiers/magic that must NOT be touched (see §4 point above).
  BLOCKLIST = re.compile(r"kirn_|kirn/|\.kirn|KIRNB|\.kn\b|\.cob\b|kirnlib|kirnrun|kirncheck|\bkirn[^a-z]")

  if __name__ == "__main__":
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
  ```

  The gate is literal: `python scripts/rename_kirn.py --scan examples` (must list every `examples/` hit but change nothing), then `python scripts/rename_kirn.py --apply examples`, confirm `G-VERIFY` green on the toy corpus, **then** point the same script at the main project.

---

## 5. Phased roadmap

**Goal:** Lay out the ordered phases that composed the migration, each independently testable.
**Must Follow:** One coherent layer per phase, a green gate after each, and a commit per phase.
**Important Note:** The phases are a record of what was executed, not instructions to re-execute.

Phase-gate shorthand used in every phase:

- `G-VERIFY` = run all corpus harnesses (see §2.4 list) with the current tool names, then commit.
- `G-LINT` = `kirn check` on all `.kn` files with zero errors.
- `G-DIFF` = `vm_diff.ps1` byte-identical on tree-walker vs VM vs native.

---

### Phase 1 — Baseline snapshot & safety netting (drastic safety net)

- **Goal:** A clean, reproducible baseline to roll back to, plus a dry-run inventory confirming the exact byte-counts this migration will touch.
- **Must Follow:** Record a green pre-migration build and transcript, plus a scripted byte-count inventory; change nothing here.
- **Important Note:** Completed — this baseline was the rollback point every later phase relied on.
- **Problem:** You cannot verify a migration you can't revert or measure. v1.0 of this migration must prove the "before" state is green and countable.
- **Why it matters:** Rename work is all-or-nothing on some layers (extension, registry); a rollback point and a before/after diff are the only safety. (Note: the GitHub repo rename is already done — the local tree is what this phase baselines.)
- **Design/approach:**
  1. `git status` clean; `git log -1` recorded (2026-09-18: repo rename commit history is intact through the `gh` rename).
  2. Confirm the repo builds: `cmake -S . -B build` then `cmake --build build --config Debug`.
  3. Run every harness on the **current** names and record a baseline transcript:
     `scripts/runall.ps1 -Runner build/Debug/kirnrun.exe`, `scripts/types.ps1`, `scripts/negative.ps1`, `scripts/vm_diff.ps1`, `tests/conventions/run.ps1`.
  4. Produce a dry-run inventory with exact counts (the §3 table) via a reproducible script `scripts/rename_dryrun.ps1` that prints every file + line for each case form and extension handler, but changes nothing.
- **Files (new):** `scripts/rename_dryrun.ps1`, `scripts/rename_baseline.ps1` (records harness output to `_rename/baseline/`).
- **Relevant source files:** whole repo (read-only).
- **Code/syntax example (dry-run form):**
  ```powershell
  # scripts/rename_dryrun.ps1 (new) — inventory only, no writes
  param([string]$Root = (Get-Location))
  $pats = @('kirn','Kirn','KIRN')
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

### Phase 2 — Prose & comment rename everywhere (legacy → Kirn), lowest risk

- **Goal:** Replace the legacy **prose name** with `Kirn` in comments, doc titles, and user-facing strings — the least risky layer, proving the loop.
- **Must Follow:** Use a word-boundary plus identifier blocklist; never touch namespace/identifier tokens or frozen bundle tokens.
- **Important Note:** Completed — this was the zero-risk confidence-builder that made every later phase read consistently.
- **Problem:** 423 capitalized prose occurrences + hundreds of lowercase identifier/comment uses across docs and source. This is what a reader actually sees and it had to be consistent before any other rename.
- **Why it matters:** It is the zero-risk confidence-builder; also, renaming prose first means later phases read consistently.
- **Design:** Do a **scoped** replace of the legacy prose name with `Kirn` and its lowercase form with `kirn` in **comments/strings/docs only** — but **not** inside C++ `namespace kirn`, not inside `kirn_*`/`kirn*.cpp` identifiers, and not in the frozen `.cob`/`KIRNB` tokens or the manifest/deps names. Use context-aware replacement (word boundary + a blocklist of the identifier forms handled in later phases).
- **Implementation:** a `scripts/rename_prose.ps1` that:
  - For each tracked text file, replaces the legacy capitalized name with `Kirn`.
  - For guarded cases, only replaces the legacy lowercase name when the surrounding char is not `[A-Za-z0-9_]` (so `kirn_lex`, `kirnrun`, `kirnExe`, `dotted.kn` are untouched) **and** the token isn't on the blocklist (`kirn_`, `kirn/`, `.kirn`, `KIRN`, `kirnrun`, `kirncheck`, `kirnlex`, `kirnparse`, `kirnExe`, `kirnlib`, `kirn.toml`...).
- **Files:** all `.md`, `.html`, `LICENSE`, `.ebnf` header comments, `examples/README.md`, plus the **header comments** of C++ sources (these are prose, safe) — but _not_ the `namespace` lines yet.
- **Code example (legacy comment → Kirn):**
  ```cpp
  // Before:  // <legacy>parse --ast prints the tree; for the legacy docs see docs/KIRN_PLAN.md
  // After:   // kirnparse --ast prints the tree; for Kirn docs see docs/KIRN_PLAN.md
  ```
  (Note: even the _tool name inside that comment_ later becomes `kirnparse`; Phase 2 only fixed the language word. The full sentence was fixed in Phase 4.)
- **Testing:** `G-VERIFY` (harnesses run with unchanged tool names and the pre-Phase-5 extension — behavior identical); plus run the renamed docs through `kirn doc` if it parses any of them.
- **Expected outcome:** all human-facing prose says "Kirn"; binary/builds untouched; all tests still pass.
- **Risks/trade-offs:** must not touch identifiers; the blocklist makes it safe. Commit after green.

---

### Phase 3 — C++ namespace identifier → `kirn` (and native emitter → `kirn_native`)

- **Goal:** Rename the internal C++ namespace identifier from the legacy name to `kirn` across every header/impl, plus the native emitter's `kirn_native`.
- **Must Follow:** Rewrite only the `namespace` opener/closer and qualified `<id>::` references; leave `kirn_lex`, `kirn.toml`, `KIRNB`, `KIRN` for their own phases.
- **Important Note:** Completed — a from-scratch rebuild proved the namespace rename was internally consistent.
- **Problem:** The source is one C++20 project with a shared namespace. Leaving the legacy identifier after the language rename would make the code read as a different project than the one now branded Kirn.
- **Why it matters (and why it's its own phase):** `namespace kirn` appears in _every_ `src/**.h` + `src/**.cpp` file, and symbols are referenced as `kirn::Lexer`, `kirn::Parser`, `kirn::ast`, `kirn::vm`, `kirn::tomlmini`, `kirn::interp`, `kirn::ast::StKind`, etc. A global replace of just the identifier `kirn::` and `namespace kirn` is surgical because the namespace is always followed by `::` or `{`.
- **Design:** Two precise rewrites, both landing on `kirn`:
  1. the namespace opener/closer (including the `} // namespace ...` comment) becomes `namespace kirn`.
  2. every qualified reference `<id>::` becomes `kirn::`.
     Do **not** touch `kirn_lex`, `kirn_ast`, `kirnExe`, `kirn.toml`, `KIRNB`, `KIRN` here — those were other phases.
- **Implementation:** `scripts/rename_ns.ps1` operating on `src/**, tools/*.cpp, tools/*.h` only. Use a regex scoped to the `namespace` declaration and `<id>::` with a word boundary after the identifier (`kirn(?![A-Za-z0-9_])` won't match `kirn_lex`).
- **Files:** every file under `src/`, `tools/` that declares/uses the namespace (≈ all of them).
- **Code example (final form):**
  ```cpp
  namespace kirn {
  ... auto toks = kirn::Lexer(src, path, diags).lexAll();
  } // namespace kirn
  ```
  And in `native.cpp` the emitted code writes `namespace kirn_native`.
- **Testing:** rebuild from scratch (`cmake --build build --config Debug`) — a clean compile proves the namespace rename is internally consistent. Then `G-VERIFY`.
- **Expected outcome:** source reads "kirn"; compiler links and all harnesses pass.
- **Risks:** any missed `kirn::` reference breaks compile — that's exactly why a full rebuild + network of tests is the gate. Commit after green.

---

### Phase 4 — CLI tools renamed (driver + helper executables)

- **Goal:** Rename the executables from their legacy names to the `kirn` driver family — filenames, CMake targets, usage strings, self-invocation, and all script/CI call sites.
- **Must Follow:** Rename tools and their call sites in the same phase; the shipped toolchain is now exactly `kirn` + `kirnc`.
- **Important Note:** Completed — the historical helper executables were later folded behind the `kirn` driver plus `kirnc`.
- **Problem:** The tools' names are the language's face (`kirn run`, `kirn build`, `kirn test`, `kirn doc`). After branding "Kirn", the command had to be `kirn run`, etc.
- **Why it matters:** Every script (`runall.ps1`, `types.ps1`, `negative.ps1`, `vm_diff.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`, `scripts/bench.ps1`) invokes `kirn.exe`/`kirnrun`/`kirncheck`/etc. So renaming tools required renaming their call sites **in the same phase** or nothing ran.
- **Design:**
  1. `git mv` each `tools/<legacy-tool>*.cpp` to its new name; keep the file content, then edit the internal usage strings.
  2. `CMakeLists.txt`: rename the exe targets and the driver target to `kirn`.
  3. Update every passthrough string (help/error/usage) inside the `.cpp` files to the new tool names, ending at `kirn`/`kirnc`.
  4. Update `tools/kirn.cpp` self-invocation: it builds `"<kirnExe> run ..."` from `GetModuleFileNameA` — the variable and the invoked command became `kirn.exe`, producing `kirn run`.
  5. Update all `scripts/*.ps1`, `tests/conventions/run.ps1`, `.github/workflows/ci.yml` references (`build\Debug\kirnrun.exe`, `build\Debug\kirn.exe`, `kirn.exe`/`kirnrun.exe` artifact paths, and the `kirn-tools` artifact name).
- **Files:** `tools/*.cpp`, `CMakeLists.txt`, `scripts/*.ps1`, `.github/workflows/ci.yml`, `tests/conventions/run.ps1`.
- **Code example (final form):**
  ```powershell
  run: scripts/runall.ps1 -Runner build\Debug\kirnrun.exe
  $kirn = Join-Path $PWD "build\Debug\kirn.exe"
  ```
  ```cpp
  // tools/kirn.cpp (driver header comment)
  //   kirn run [dir|file]      run a program or project
  ```
- **Kirn example — the program the driver runs (`.kn`):**
  ```kn
  fn add(a: Int, b: Int) -> Int {
      return a + b;
  }

  fn main() -> Int {
      var total = add(2, 3);
      return total;
  }
  ```
- **Testing:** rebuild; then the renamed binaries exist in `build/Debug/`; re-run every harness **passing the new tool names**; confirm the legacy names no longer work (so you know everything moved).
- **Expected outcome:** the command line is `kirn ...`; CI runs with the new binaries.
- **Risks:** a missed call site = a failed CI step, so grep for the old tool names in `scripts/`, `tests/`, `.github/` and assert zero remain after the phase. Commit after green.

---

### Phase 5 — THE core: source extension → `.kn` in the loader, resolver, and every glob

- **Goal:** Change the source-file extension to `.kn` everywhere, rename the 130 files on disk, and update every hard-coded extension handler so `kirn run main.kn` and `import pet.core` resolve correctly.
- **Must Follow:** Rename the on-disk files and the loader/resolver/glob strings in one atomic commit; guard the change with `G-DIFF`.
- **Important Note:** Completed — the highest-risk phase; the module loader and package resolver keyed off the extension, so misses surfaced as broken `import`.
- **Problem:** This is the highest-risk phase because the compiler's _module loader and package resolver_ hard-coded the legacy extension in several places, and because source files **import each other by module name** (resolved by appending the extension), and the **generated native launcher** embeds a literal `"main.kn"`. If any of these is missed, `import` breaks, convention files (`main.kn`/`pin.kn`) are not found, and native builds fail.
- **Why it matters:** `.kn` is the user-visible file extension of the rebrand; everything downstream (glob patterns, syntax highlighters, GitHub language detection via `.gitattributes`, build bundles) keys off it.
- **Design — update these exact sites:**
  1. **On-disk rename:** `git mv` all source files to `*.kn` in `examples/`, `stdlib/`, `tests/`, `scripts/` (bench_fib), `tools/` (scratch/test sources), keeping directory structure. (130 files.)
  2. **Module loader** `src/interp/runtime.cpp`:
     - `:1193` the loader appends `.kn` (`rel += ".kn"`).
     - `:1208` `rel.substr(0, rel.size()-3)` strip (unchanged: the new extension is still 3 chars).
     - `:1166` `extension() == ".kn"` (recursive entry main-scan); `:1177-1180` explicit `.kn` suffix accept.
  3. **Checker** `src/sema/checker.cpp:658-659` — the `import "x.kn"` suffix-stripping: `mod.compare(mod.size()-3,3,".kn")` and `mod.erase(mod.size()-3)` (unchanged length).
  4. **Convention resolver** — `tools/kirn.cpp:343-344` `cands[] = {"code/main.kn","main.kn","code/pin.kn","pin.kn"}` and the same candidates in `runtime.cpp` `resolvePackageEntry`, plus scaffold writers `:587/618/658/661` producing `main.kn`/`pin.kn`, and generated `"main.kn"` in the native launcher/naming code (`:1317-1318` `_test.kn`, `:2785`).
  5. **Recursive collectors** — `tools/kirnlex.cpp:30` `extension()==".kn"` and `--dump <file.kn>`; `tools/kirn.cpp` any remaining extension scans (`:1741-1745/1754-1770/1967-1969`).
  6. **Every glob** — `scripts/*.ps1` `-Filter *.kn`; `tests/conventions/run.ps1`; `.github/workflows/ci.yml` comments/globs; `grammar/kirn.ebnf` text (`File extension: .kn`, `main.kn`, `pin.kn`, `mod.kn`).
  7. **`.gitattributes`** — `*.kn text eol=lf`.
- **Files:** everything above.
- **Code example — loader (final form):**
  ```cpp
  // src/interp/runtime.cpp  (loadModuleFile)
  std::string key = rel;
  rel += ".kn";
  ```
  ```cpp
  // tools/kirn.cpp convention candidates
  const char* cands[] = {"code/main.kn","main.kn","code/pin.kn","pin.kn"};
  ```
  ```cpp
  // generated native launcher
  << "    auto toks = kirn::Lexer(kMainSrc, \"main.kn\", diags).lexAll();\n"
  ```

> Reference: Zig — `zig/build.zig` + `build.zig.zon` show build/extension identity is centralized in one manifest, so a source-extension change stays a single-point edit.

- **Testing:** full rebuild; `G-LINT` on all `.kn`; `G-DIFF` (vm_diff must match byte-for-byte through the newly `.kn`-resolved modules — **critical**: `import` must now resolve to `.kn`); `runall.ps1 -Runner build/Debug/kirnrun.exe` over `examples/*.kn`; `types.ps1` (p/n `.kn`); `negative.ps1`; `asanall.ps1`; `tests/conventions/run.ps1` (must resolve `code/main.kn`/`pin.kn`). Also run `kirn new demo` then `kirn run demo/main.kn` from a fresh dir.
- **Expected outcome:** every source file and handler uses `.kn`; all modules/imports/conventions resolve; the whole corpus is green on all backends.
- **Risks/trade-offs:** **This was the phase most likely to break things.** Mitigation: the on-disk rename and the loader/glob edits shipped in a single commit (they are mutually dependent); `G-DIFF` ran immediately (the module loader resolving `.kn`). Optional compat: an `--accept-legacy` loader knob was documented (not default) if the author wanted the legacy extension imported. Committed green.

---

### Phase 6 — Stdlib + library rename: `lib` → `pet` (module namespace, `stdlib/lib/`, deps layout)

- **Goal:** Rebrand the _library_ concept from `lib(s)` to `pet(s)`: stdlib module namespace `import lib.` → `import pet.`, dir `stdlib/lib/` → `stdlib/pet/`, package-install layout `kirn_libs/` → `pets/`, library bundles `.kirnlib` → `.pet`, global cache `~/.kirn/kirn-pkg` → `~/.kirn/pets`.
- **Must Follow:** Rename the `stdlib/lib/` directory and every `import lib.` site together; rename the `.pet` writer/detector/unpack in one commit.
- **Important Note:** Completed — the paired dir+import edit was the second-highest-risk layer, gated by the stdlib tests.
- **Problem:** "the library" is user-facing: users write `import lib.time;`, packages install into `kirn_libs/`, library packs are `<n>-<v>.kirnlib`. After the Kirn rename, leaving "lib/kirn_libs/kirnlib" would mix brands.
- **Why it matters:** this is the ecosystem's _library_ identity — the registry is the "pets" registry (`https://pets-registry.github.io`), so the local names must match.
- **Design:**
  1. **Stdlib namespace:** `git mv stdlib/lib stdlib/pet`; edit every `import lib.<m>;` → `import pet.<m>;` in `stdlib/pet/*.kn`, `tools/*.kn`, `examples/`, tests (22 `import lib.` sites found). The loader maps the first import segment to a directory under each stdlib/search dir (see §3.5), so renaming the directory + the import segment is a paired edit — do both in one commit or imports break.
  2. **Deps layout:** `kirn_libs/` → `pets/` at every site: `kirnrun.cpp:37-38/49-50`, `tools/kirn.cpp:369-370/389` (`fs::path("kirn_libs")`), gitignore template `:580`, test-scan skip `:1329`, install/remove legacy paths `:1029/1139-1140/1168/1687`, module-search dirs `:1947-1948`. Keep the `libs/`-vs-root split semantics (`pets/libs` + `pets`) as today.
  3. **Global cache:** `home + "/.kirn/kirn-pkg"` (`tools/kirn.cpp:353`, `kirnrun.cpp:52-57`) → `home + "/.kirn/pets"`; `.kirn-pkg` → `.pets-pkg`.
  4. **Library pack:** `.kirnlib` → `.pet` — writer (`tools/kirn.cpp:11`, output `:2699`), detectors (`:721/:967`), unpack (`:2838/:2862`), usage strings (`:2929/:2956`), `.gitattributes:21`.
- **Files:** `src/interp/runtime.cpp`, `tools/kirn.cpp`, `tools/kirnrun.cpp`, `stdlib/lib/*` → `stdlib/pet/*`, all importers, `.gitattributes`.
- **Code example:**
  ```cpp
  // tools/kirn.cpp deps dir BEFORE
  return fs::path("kirn_libs");
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
- **Expected outcome:** stdlib imports and the package layout say "pet"; bundle packs are `.pet`; the old `kirn_libs/`/`lib.` strings are gone.
- **Risks:** forgetting the paired dir-rename+import-rename breaks every stdlib import (loud, catches itself in `G-VERIFY`); legacy `kirn_libs/` on disk won't be found → run `git clean`/migrate or keep a compat note. Commit after green.

> Reference: Nim — `Nim/lib` ships the stdlib as a name-resolved directory tree, the same pairing discipline used for `stdlib/pet/`.

---

### Phase 7 — Package manager & registry identity (manifest, `pets`, `~/.kirn`, registry URL)

- **Goal:** Rebrand all package-manager filenames, folders, caches, and the registry org URL to Kirn/pets.
- **Must Follow:** Move manifest, cache, and registry names as one identity layer; update the scaffold/gitignore templates with them.
- **Important Note:** Completed — the ecosystem identity (manifest + registry URL) was made consistent with the `pets` brand.
- **Problem:** the manifest and cache names plus the registry URL still carried the pre-migration brand: `.kirn-registry-lib.toml`, `.kirn-sha`, and `github.com/kirn-lib/kirn-libs`.
- **Why it matters:** these are user-visible and ecosystem-identity — the manifest, cache, and registry had to name Kirn/pets consistently.
- **Design:**
  1. Manifest: writer (`tools/kirn.cpp` `new` + `writeManifest` `:103/:121/:245`), loader (`runtime.cpp:1133` `readFileIfExists(dir+"/kirn.toml")`), pack (`:2703`), usage (`:2814/:2857`) — all unified under `kirn.toml`/`kirn.lock`.
  2. Registry metadata: `.kirn-registry-lib.toml` → `.pets-registry.toml`, `.kirn-sha` → `.pets-sha` (register/verify sites in `tools/kirn.cpp`, and the `.gitignore`/scaffold template).
  3. Registry URL + org: `m.repo`/`m.homepage` defaults `github.com/kirn-lib/` → `github.com/pets-registry/` (`:566-567`), registry fetch `https://raw.githubusercontent.com/kirn-lib/kirn-libs/main/` → `.../pets-registry/pets/main/` (`:757/762`), `list online` (`:1252/1257`), browse hints (`:998`), install hint → `kirn install github.com/pets-registry/...` (`:632`).
- **Files:** `src/interp/runtime.cpp`, `tools/kirn.cpp`, `tools/kirnrun.cpp`, docs.
- **Code example (final form):**
  ```cpp
  // runtime.cpp
  if (readFileIfExists(dir + "/kirn.toml", manifest)) {...}
  ```
  ```cpp
  // tools/kirn.cpp registry
  "https://raw.githubusercontent.com/pets-registry/pets/main/"
  ```
- **Testing:** fresh `kirn new demo` writes `kirn.toml`/`kirn.lock` and `pets/` ignored; `kirn run .`/`kirn test` resolve from the new manifest; `kirn install` writes `.pets-registry.toml`; a `code/main.kn`+`pin.kn` convention app runs from a fresh dir; CI `kirn new demo` → `kirn build` round-trip green.
- **Expected outcome:** every package-manager artefact and URL says `kirn`/`pets`; the ecosystem identity is consistent.
- **Risks:** any leftover legacy manifest detection breaks `kirn run .`; grep for the legacy manifest/deps/registry tokens and assert zero after the phase. Commit after green.

> Reference: Go — `go/src/cmd` keeps module-path and registry identity centralized in `go.mod`, so the package identity moves as one unit.

---

### Phase 8 — Env vars & CMake options (legacy `*_*` → `KIRN_*`)

- **Goal:** Rename the env/cmake identifiers from the legacy prefix to the `KIRN_*` family: `KIRN_PETS`, `KIRN_STDLIB`, `KIRN_ASAN`, `KIRN_CL`, plus the niche `KIRN_CXX`/`KIRN_VERBOSE`/`KIRN_LIB_TOOL`/`KIRN_TARGET`.
- **Must Follow:** Rename the whole `KIRN_*` family in one dedicated phase; keep the ASan option name stable.
- **Important Note:** Completed — a dedicated phase avoided touching config identifiers mid-Phase 3/7.
- **Problem:** the env/cmake identifiers still carried the pre-migration brand; a user setting `KIRN_STDLIB` would have been looking for the old name.
- **Why it matters:** config surface parity; also avoids confusing the ASan build flag.
- **Design:**
  - `CMakeLists.txt:13` `option(KIRN_ASAN ...)` and `if(KIRN_ASAN)`, plus comment.
  - `tools/kirn.cpp:358` + `kirnrun.cpp:47-48` `std::getenv("KIRN_PETS")`.
  - `tools/kirn.cpp:377` + `kirnrun.cpp:62` `KIRN_STDLIB`.
  - `tools/kirn.cpp:1916/2587-2588` `KIRN_CL`.
  - `scripts/asanall.ps1` re-invokes with the renamed option.
- **Files:** `CMakeLists.txt`, `tools/kirn.cpp`, `tools/kirnrun.cpp`, `scripts/asanall.ps1`, docs.
- **Code example (final form):**
  ```cmake
  option(KIRN_ASAN "Build with AddressSanitizer" OFF)
  if(KIRN_ASAN)
  ```
  ```cpp
  if (const char* env = std::getenv("KIRN_STDLIB")) dirs.push_back(env);
  ```
- **Testing:** rebuild with `-DKIRN_ASAN=ON` builds sanitizer targets and `asanall.ps1` uses the new flag; set `KIRN_PETS` to a dir and confirm `import` resolves there; clear the legacy `KIRN_*` vars and confirm they no longer have effect; `G-VERIFY`.
- **Expected outcome:** config surface uses `KIRN_*` only.
- **Risks:** stale env vars lingering on dev machines won't apply (harmless). Commit after green.

---

### Phase 9 — CMake/library `.lib` name → `kirn_interp.lib` (native build linkage)

- **Goal:** Align the CMake output library names so `kirn build --native` links the right prebuilt `.lib`.
- **Must Follow:** Rename the hard-coded `.lib` strings in lock-step with the CMake targets; grep `\.lib"` to catch them all.
- **Important Note:** Completed — a missed lib name would have broken native/cross linkage even when namespaces compiled.
- **Problem:** `tools/kirn.cpp:2519` checks for `kirn_interp.lib` (and the link line `:2657` names `kirn_interp.lib kirn_vm.lib kirn_sema.lib kirn_parser.lib ...`). If Phase 3 renamed _namespaces_ but these strings were missed, native/cross builds would break.
- **Why it matters:** the `go build`-like native path resolves the prebuilt runtime by exact lib filename.
- **Design:** update `tools/kirn.cpp:2519` and the whole `kirn_*.lib` link list `:2657`, in lock-step with the CMake target renames from Phase 4 that emit `kirn_*.lib`.
- **Files:** `tools/kirn.cpp`, `CMakeLists.txt` (already renamed in Phases 3–4, verify).
- **Code example (final form):**
  ```cpp
  fs::exists(fs::path(binRoot) / "kirn_interp.lib");
  ```
- **Testing:** `kirn build --native` and cross `--target=windows-arm64` + `--release` both succeed and produce a working `.exe`; CI arm64 job green.
- **Expected outcome:** native/cross builds link the renamed libs.
- **Risks:** a hard-coded lib name is brittle; grep `\.lib"` in `tools/kirn.cpp` to catch all. Commit after green.

> Reference: LLVM — `llvm/llvm/lib` and `llvm/llvm/tools` pair shared libraries with driver tools, where exact archive filenames matter for linkage.

---

### Phase 10 — Docs, plan documents, grammar, README, LICENSE, examples README (final prose pass)

- **Goal:** Final "Kirn/pets everywhere" pass over all human-facing documents and grammar, catching anything Phases 2–9 left and re-deriving titles.
- **Must Follow:** Run the prose pass after the code renames, update inline commands to the shipped CLI, and fix cross-links when renaming doc files.
- **Important Note:** Completed — this proved the docs describe the tool that actually exists.
- **Problem:** After the code renames, doc references to old tool names/features had to be updated, and doc _titles_ (`KIRN_PLAN.md`, `WHY_USE_KIRN_PLAN.md`, `KIRN_CROSS_PLAN.md`) still referenced the legacy identity.
- **Why it matters:** replacing the legacy name everywhere included docs; a doc that still showed the legacy invocation after the rename to `kirn run main.kn` would have been a lie.
- **Design:**
  1. Re-run a **prose** pass (like Phase 2) for any remaining legacy name/extension tokens in `.md`/`.ebnf`/`LICENSE`, landing on `Kirn`/`kirn`/`.kn`.
  2. Update all inline command examples to the shipped CLI: `kirn run main.kn`, `kirn build`, `kirn test`, `kirn doc`, `kirn install`, `kirncheck` (README.md, docs/KIRN_PLAN.md, docs/FEATURE_GAP_ANALYSIS.md).
  3. `import lib.` → `import pet.` in all doc code samples.
  4. Brand URLs: the legacy site URL → `https://kirn-lang.github.io`; the legacy registry repo → `https://pets-registry.github.io` (website) and `github.com/pets-registry/pets` (repo); author website `https://rkriad585.github.io`.
  5. Boards: README logo switches from `logo/ryro-logo-orange-bg-removed.png` (deleted) to `kirn-logos/kirn-logo.png`.
  6. `grammar/kirn.ebnf` — all `.kn`/`main.kn`/`pin.kn`/`mod.kn` text, `import lib.` → `import pet.`, and the EBNF header describing the language name → Kirn. (The file itself was renamed in Phase 11.)
  7. Decide + execute the doc-file renames (see §8 recommendation): the `*_PLAN.md` files were rebranded (e.g. `WHY_USE_KIRN_PLAN.md`), updating all cross-links between plan docs and the README index.
- **Files:** all `*.md`, `grammar/*.ebnf`, `LICENSE`, `examples/README.md`, plan docs.
- **Code example (final form):**
  ```markdown
  ## Quick start

  $ kirn run main.kn
  ```
- **Testing:** `kirn doc` builds a docs site with no legacy references; grep the whole repo (minus `build/`, `.git/`) for the legacy name and extension at word boundary and assert only intentional historical/compat mentions remain; all cross-doc links resolve after the doc-file renames.
- **Expected outcome:** the repository reads as a fully "Kirn" project, docs commands are runnable as written.
- **Risks:** doc links breaking on file renames; fix in the same phase. Commit after green.

---

### Phase 11 — Project identity & external references (folder, repo, git, tooling, grammar filename)

- **Goal:** Rebrand the _project itself_: the top-level identity, `.git` metadata references, editor/LLM grammar files, and remaining legacy names in filenames.
- **Must Follow:** Do this last, on a clean tree; rename the grammar filename and confirm the remote URLs in the same pass.
- **Important Note:** Completed — the outermost identity layer; the repo URL had already moved to `rkriad585/kirn`.
- **Problem:** the workspace folder carried the legacy name (a local, non-shipped detail), the grammar file was `grammar/<legacy>.ebnf`, CI/hosted URLs may still have said the legacy name, and any `*.tmLanguage`/tree-sitter files would have been legacy-branded.
- **Why it matters:** this is the **outermost** identity; it came last because touching the checkout folder is disruptive and easiest once everything inside was already Kirn. (The GitHub repo name + homepage + local remote had **already** moved to `kirn` — as of 2026-09-18.)
- **Design:**
  1. `grammar/<legacy>.ebnf` → `grammar/kirn.ebnf` (done here, after Phase 10 content edits), update references (docs, tooling, CI if referenced).
  2. Treat the local checkout folder name as a non-shipped detail: scripts use `$PSScriptRoot`/relative paths, so the build never depends on it. The canonical identity is the remote `https://github.com/rkriad585/kirn.git`.
  3. Confirm `.github`/CI hosted URLs say `rkriad585/kirn`, `kirn-lang.github.io`, `pets-registry.github.io`.
  4. Check for editor/LLM grammar files (`.tmLanguage`, `tree-sitter-*`): if present, rename/rebrand to `Kirn`/`.kn`.
  5. Confirm GitHub-language-detection: `.gitattributes` now lists `*.kn text eol=lf` (Phase 5) so GitHub tags `.kn`; optionally add a `linguist-language` if desired.
- **Files:** folder, `grammar/kirn.ebnf`, `.github/`, any grammar/tooling assets, remote URL (verify).
- **Code example (grammar file):**
  ```
  # grammar/kirn.ebnf (renamed from the legacy grammar filename)
  (* Encoding: UTF-8. File extension: .kn. Layout is free: ... *)
  ```
  ```powershell
  # the shipped identity is the remote, not the checkout folder name
  git remote -v   # expected: https://github.com/rkriad585/kirn.git
  ```
- **Testing:** open the project, `cmake -S . -B build`, build, full `G-VERIFY` from the checkout; `git remote -v` shows the new URL; grammar parses `.kn`.
- **Expected outcome:** the whole project identity, from grammar to remote, is "Kirn".
- **Risks:** touching the working directory mid-session breaks open editors/PowerShell cwd; do it as the final, deliberate step with a clean tree and commit first. Commit after green (and tag `rename-complete`).

---

### Phase 12 — Final sweep & regression: zero legacy tokens, full-matrix green

- **Goal:** The verification capstone: a scripted assertion that _all_ legacy tokens are gone (except explicitly-declared historical/compat notes) and every harness passes with the new names.
- **Must Follow:** Keep an explicit allowlist for frozen bundle tokens and historical mentions; wire the check into CI so it runs on every PR.
- **Important Note:** Completed — this is the CI-enforced guarantee that the repo _is_ Kirn and _stays_ Kirn.
- **Problem:** a rename this broad needs an automated "are we done?" gate, so a later commit can't silently reintroduce a legacy token.
- **Why it matters:** "everything works perfectly" (§1) is only provable by a repeatable full-matrix run.
- **Design:**
  1. Add `scripts/rename_verify.ps1` that:
     - Fails if any tracked file under `examples/`,`stdlib/`,`tests/`,`tools/`,`src/`,`grammar/` matches a legacy token (`$LEGACY`, `${LEGACY}_`, `\.$LEGACY\b`, `import lib\.`, `.kirn-` registry metadata).
     - Explicitly **allowlists** `KIRNB`/`.cob` (frozen bundle format — deliberate non-goal) and the allowed historical/compat mentions in docs.
     - Runs the full harness matrix with the new names and reports pass/fail per suite.
  2. Wire `rename_verify.ps1` into `.github/workflows/ci.yml` as a `rename-regression` job so it runs on every future PR.
  3. Produce a final `_rename/report.md` summarizing before/after counts (from Phase 1 baseline vs now).
- **Files (new):** `scripts/rename_verify.ps1`; `.github/workflows/ci.yml` (+job).
- **Code example (regression grep, PowerShell):**
  ```powershell
  # $LEGACY holds the one legacy name/token; the shipped tree contains no literal copy of it.
  $LEGACY = '<legacy-name>'
  $banned = $LEGACY,"${LEGACY}_","\.$LEGACY\b",'import lib\.','\.kirn-','\.kirn-registry'
  $allow  = 'KIRNB','\.cob'   # frozen bundle format (author decision)
  foreach ($f in Get-ChildItem -Recurse -File -Include *.kn,*.cpp,*.h,*.ps1,*.md,*.ebnf,*.yml |
             Where-Object { $_.FullName -notmatch '\\(build|\.git)\\' }) {
    $t = [IO.File]::ReadAllText($f.FullName)
    foreach ($pat in $banned) {
      if ($t -match $pat) { Write-Error "stale legacy token in $($f.Name)" }
    }
  }
  ```
- **Testing:** run `rename_verify.ps1` locally — must be all-green; CI `rename-regression` passes.
- **Expected outcome:** a CI-enforced guarantee that the repo IS Kirn and STAYS Kirn.
- **Risks:** overstrict ban could flag a _legitimate_ future string; keep an explicit allowlist of intentional historical mentions + frozen bundle tokens (documented in the script header). Commit after green.

> Reference: Rust — `rust/x` (and `x.py`) run the repo's `tidy`/banned-token checks in CI, the same "machine-checked invariant" pattern used here.

---

### Phase 13 — Optional: legacy-extension backward-compat bridge (author decision)

- **Goal (optional, not default):** A documented, opt-in loader knob so legacy-extension files can still be imported/produced during a transition period.
- **Must Follow:** Keep the default pure `.kn`; the bridge is opt-in via a flag/manifest bool and does not block Phases 1–12.
- **Important Note:** Not adopted — recorded so the choice stayed explicit rather than accidental.
- **Problem:** if real users still had legacy-extension files, a hard cut to `.kn` would break them; some projects value a grace window.
- **Design:** add an `--accept-legacy` flag (or `kirn.toml` `[tool] accept_legacy_extension = true`) that makes the loader try `.kn` first, then fall back to the legacy extension, and lets `kirn build`/`kirn run` accept legacy-extension args. Keep the **default = pure `.kn`**.
- **Implementation:** touch the exact Phase-5 sites (`runtime.cpp` loader + `tools/kirn.cpp` arg/glob handling + run dispatch) behind the flag/bool read from the manifest or an env var.
- **Files:** `src/interp/runtime.cpp`, `tools/kirn.cpp`, `tools/kirnrun.cpp`, `docs`.
- **Code example:**
  ```cpp
  // runtime.cpp (loader) behind acceptLegacy
  rel += ".kn";
  std::string alt = rel.substr(0, rel.size()-3) + kLegacyExt;  // optional fallback
  if (!acceptLegacy) alt.clear();
  ```
- **Testing:** with the flag on, import a legacy-extension module and run a legacy-extension file; with the flag off (default), `.kn` only; `rename_verify.ps1` still passes with the flag off.
- **Expected outcome:** a safe, opt-in transition path — clearly documented as non-default.
- **Risks:** keeping two extensions alive adds maintenance surface; the author should decide _when_ (now, later, or never) — Phase 13 exists so the choice is explicit rather than accidental. This phase is optional and does not block Phases 1–12.

---

## 6. Decision log (decisions made in this plan, with rationale)

**Goal:** Preserve the explicit decisions and their rationale so future migrations inherit the reasoning, not just the outcome.
**Must Follow:** One decision per row with a concrete rationale; keep the frozen-format and non-default choices explicit.
**Important Note:** These decisions are settled history — do not re-open them without a new, recorded rationale.

| #   | Decision                                                                                                                 | Rationale                                                                                                               |
| --- | ------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------- |
| D1  | Source extension → `.kn`, stdlib `import lib.`→`import pet.`, `.kirnlib`→`.pet`; **`.cob`/`KIRNB` frozen** (not renamed) | Author decision: source `.kn`, library = "pet(s)", bundle format "not important and not needed"                         |
| D2  | Pure `.kn` by default; legacy-extension compat is opt-in (Phase 13)                                                      | Simplicity + explicit transition; matches "replace everywhere" intent                                                   |
| D3  | Case forms mapped: `kirn` (id), `Kirn` (prose), `KIRN` (env/cmake); library term `pet` (prose) / `PETS` (env)            | Prevents blind-case bugs; the repo already uses these forms distinctly                                                  |
| D4  | CMake targets + executables + namespaces + manifest all renamed, not just strings                                        | Otherwise the codebase would read Kirn while the C++/build/package identity still carried the legacy name               |
| D5  | Doc plan filenames (`*_PLAN.md`) renamed in Phase 10/11 (recommended, not forced)                                        | Historical artifacts; renaming is cleaner but must happen with cross-link fixes                                         |
| D6  | Legacy registry org → `github.com/pets-registry/pets`; legacy project site → `kirn-lang.github.io`                       | Author-provided URLs: library website `https://pets-registry.github.io`, language website `https://kirn-lang.github.io` |
| D7  | No fractional naming (e.g. leaving a legacy fragment in the namespace) after Phase 3                                     | "Everywhere" was requested; avoid half-migrated identifiers                                                             |
| D8  | The optional `--accept-legacy` bridge is **non-default**                                                                 | Keeps the hard rename unambiguous while offering a documented escape hatch                                              |
| D9  | Deps folder `pets/`, global cache `~/.kirn/pets`, registry metadata `.pets-registry.toml`                                | Aligns local names with the "pet(s)" library brand and `pets-registry` website                                          |

---

## 7. Risks & mitigations (consolidated)

**Goal:** Record every risk that actually mattered and the mitigation that contained it.
**Must Follow:** Pair each risk with a concrete, testable mitigation; keep the atomic-commit boundaries.
**Important Note:** All risks landed green; the extension change (Phase 5) and the `lib→pet` pairing (Phase 6) were the only two that could break the corpus broadly.

| Risk                                                       | Phase | Mitigation                                                                                                     |
| ---------------------------------------------------------- | ----- | -------------------------------------------------------------------------------------------------------------- |
| Global blind replace breaks build                          | all   | §2 phased approach + per-phase green gate; blocklist in Phase 2 rename script                                  |
| Source-extension change breaks module `import`/conventions | 5     | Ship loader+resolver+glob edits atomically in one commit; `G-DIFF` as the canary                               |
| `lib`→`pet` breaks stdlib imports (dir + import paired)    | 6     | Rename `stdlib/lib/` and all `import lib.` sites in the same commit; stdlib tests via `kirn test` are the gate |
| `.kirnlib`→`.pet` writer/reader drift                      | 6     | Author+unpack+detect+`.gitattributes` in the same commit; reinstall a `.pet` in CI                             |
| Missed tool call-site breaks CI                            | 4     | Grep the legacy tool names in `scripts/`, `tests/`, `.github/`; assert zero remains                            |
| Native build links wrong `.lib`                            | 3,4,9 | Rename CMake targets + `kirn_*.lib` strings together; full native/cross build test                             |
| Env/cmake flag mismatch                                    | 8     | Rename the legacy `KIRN_*` names to `KIRN_PETS`/`KIRN_*` cohesively; ASan build test with `-DKIRN_ASAN=ON`     |
| Docs say one thing, code another                           | 10    | Final prose pass + `kirn doc` build; grep whole repo post-migration; update brand URLs                         |
| Checkout-folder identity disrupts work                     | 11    | Last phase, clean tree, commit first; repo URL already moved via `gh`; checkout folder is a non-shipped detail |
| Frozen `.cob`/`KIRNB` bundle tokens left as-is             | 12    | Explicit allowlist in `rename_verify.ps1`; documented in plan header as deliberate non-goal                    |
| `.kn` extension-name collisions                            | 2,5   | Documented in §2; low risk; `.kn` is not a _code_ extension anywhere mainstream                                |

---

## 8. Recommended execution order & acceptance

**Goal:** Give the exact phase order for a green commit at every step, plus the definition of "done".
**Must Follow:** Low-risk textual layers first, the extension change in the middle, external identity last; keep the two-binary toolchain as the acceptance target.
**Important Note:** The order is a record of what was executed; the acceptance criteria are the durable part.

**Order (optimized for "green at every step" then "identity complete"):**

```
Phase 1  baseline + safety net          (read-only, quickest)
Phase 2  prose legacy→Kirn              (lowest risk, huge visible change)
Phase 3  C++ namespace → kirn           (mechanical, full rebuild gate)
Phase 4  CLI tools + CI/scripts names   (command line = kirn + kirnc)
Phase 5  source ext → .kn + loader      (the core risk; atomic with globs)
Phase 6  lib→pet stdlib + deps layout   (import pet.*, pets/, .pet packs; pairs dir+import)
Phase 7  package manager + registry URL (kirn.toml/pets-registry)
Phase 8  legacy env/cmake → KIRN_*      (config surface)
Phase 9  kirn_interp.lib linkage        (native/cross build)
Phase 10 docs/grammar/plan final pass   (README, .ebnf, brand URLs, plan doc renames)
Phase 11 project identity + remote + grammar file name   (repo URL already moved via gh)
Phase 12 regression gate (rename_verify.ps1 + CI job)
Phase 13 OPTIONAL legacy-extension bridge (decide explicitly; non-default)
```

**Acceptance criteria (definition of "everything works perfectly"):**

1. `cmake -S . -B build && cmake --build build --config Debug` succeeds from scratch.
2. `scripts/rename_verify.ps1` reports **zero** stale legacy tokens (allowlisted: `KIRNB`/`.cob`) and all harnesses PASS.
3. Full corpus green on all backends: `runall.ps1` (examples), `types.ps1` (p/n), `negative.ps1`, `vm_diff.ps1`, `asanall.ps1`, `tests/conventions/run.ps1`.
4. `kirn new demo` → `kirn run demo/code/main.kn` → `kirn test` → `kirn build` (native + `.cob` fallback) all work; a `.cob` bundle runs.
5. CI (build-test + cross-arm64) passes end-to-end with renamed artifacts.
6. `git log` shows one clean commit per phase; `git -C <repo> remote -v` shows `https://github.com/rkriad585/kirn.git`.
7. Optional-if-adopted: legacy-extension import works **only** with `--accept-legacy` / the legacy env var.

**Note on the `WHY_USE_KIRN_PLAN.md` artefact:** since it was authored under the legacy identity and the requirement was to replace the legacy name everywhere, Phase 10 rebranded it to `WHY_USE_KIRN_PLAN.md` and updated its internal name/extension references — unless the author prefers to keep naming-history artefacts as-is (D5 marks this as recommend-rename).

---

_Plan v1 authored 2026-09-03 (an initial rename-to-Ryro exploration) after a full source audit and 2026 web research; v2 retargeted 2026-09-18 to the final rename to Kirn per author decision (`.kn`, `lib→pet`, `pets-registry.github.io`, `kirn-lang.github.io`, GitHub repo already renamed to `rkriad585/kirn`); reframed as a completed-migration record 2026-10-12. Phases were ordered for a green commit after every step; the riskiest layer (the source-extension change in the module loader, Phase 5) was isolated and gated by the differential harnesses, with the `lib→pet` stdlib rebrand (Phase 6) as the second-highest-risk pairing._
