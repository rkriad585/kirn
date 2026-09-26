# Kirn "What did we do so far?" - shared understanding of the project's health &
# safety netting tasks, kept up to date as the work progresses.

# Things the repo is currently set up for:
# - LF line endings EVERYWHERE (see .gitattributes "* text=auto eol=lf").
#   core.autocrlf is DISABLED for this repo (`git config core.autocrlf false`)
#   so git never auto-converts and never warns about LF/CRLF replacement.
# - 7-gate verification harness: scripts/runall.ps1, scripts/types.ps1
#   (-Check/-Run), scripts/negative.ps1 (-Runner), scripts/vm_diff.ps1,
#   tests/conventions/run.ps1, plus the Phase-6 gates `kirn test` on the
#   stdlib/pet suite and the .pet pack->install->consume round-trip.
#   Phase-1 snapshot captured in scripts/rename_baseline.ps1 (transcripts
#   under _rename/baseline/). Phase 4 renamed the tools + every harness call
#   site: the CLI is now `kirn` (driver), `kirnrun`, `kirncheck`, `kirnlex`,
#   `kirnparse`.
# - Linux dev environment (Phase 3 added this as a first-class path):
#   * no cmake installed yet -> build with direct g++; binaries live in
#     build/ (gitignored) because the kirn driver resolves runtime sources at
#     <exe-dir>/../src via /proc/self/exe.
#   * tools/kirn.cpp carries POSIX shims behind `#else` of the Win32 block
#     (_stricmp, GetEnvironmentVariableA, GetModuleFileNameA, MAX_PATH);
#     headers are stdint-self-contained (#include <cstdint> once, after the
#     phase MSVC pulled it transitively).
#   * tools/kirn.cpp uses `#ifdef COCO_HAS_NATIVE` and emits userspace
#     `kirn_native` (Phase 3), matching the C++ namespace in src/backend/.
#   * scripts/gates_linux.sh is the faithful bash port of the 7 gates
#     (runall 45 / types 31 / negative 18 / vm_diff 43 / conventions 8 /
#     test 1 / pets 3 = 149 checks); currently 149/149 green on the renamed
#     binaries in build/ (CC=kirncheck, CR=kirnrun, CO=kirn). Native smoke:
#     kirn build --native + run must print `10 10 105 1024.0` for
#     examples/native_scalar_mix.kn and exit 94 (=350 % 256) for
#     examples/native_main.kn. (Compile with COCO_CXX=g++; that env var is
#     Phase 8's COCO_*->KIRN_* rename.)
#   * emitted native launcher TU is retained at build/debug/linux-amd64/*.cpp
#     for postmortem inspection (it is where a `v_n = (v_n)` would betray a
#     dropped compound-assign operator).
# - ASan: build-asan/ (gitignored) = whole runtime instrumented with
#   -fsanitize=address. All 45 examples are ASan-clean (re-verified after the
#   Phase 4 tool rename: 0 AddressSanitizer memory errors; LeakSanitizer still
#   flags shared_ptr cycle leaks in the same 4 closure/module examples
#   (07, 21, 32, 44); native --asan smoke still prints `10 10 105 1024.0`).
#   Windows ASan has no LSan so these are Linux-only detections; worth a
#   future weak-ptr/cycle fix, not a Phase blocker. asanall.ps1 also flags
#   native_main.kn (exit != 0) -- its loop ignores `# expect-exit`, a pre-*
#   script quirk, mirror it in bash gates as expected behavior.
# - Node-based dev tooling (optional, husky v9 + lint-staged + commitlint +
#   prettier): activate with `npm install`. Hooks live in .husky/ and NOP
#   out when npx isn't available, so the C++ tree stays buildable with zero
#   JS dependency.

# Active work streams (check these before starting anything):
# COCO_PLANS/COCO_TO_KIRN_PLAN.md - the Coco->Kirn migration plan (Phases
# 1..13 + decision log D1..D8). Phases 1 (baseline snapshot), 3 (C++ symbol
# rename: coco -> kirn, coco_native -> kirn_native; 0 matches left in src/ +
# tools/) and 4 (CLI rename: tools + CMake targets + every harness/CI call
# site -> kirn/kirnrun/kirncheck/kirnlex/kirnparse; commit 3d538e0) are
# complete, as is Phase 5: source extension .co -> .kn (git mv of 130 files
# + loader/resolver/convention/glob literals in the same commit; commits
# 6a1a5f0 rename(phase5) + 6b07d9a fix(tools) nul-device probes) and
# Phase 6: lib -> pet + .cocolib -> .pet + coco_libs -> pets.
# NEXT is Phase 8: COCO_* environment variables -> KIRN_*.
#
# Phase 6 details worth remembering (commit rename(phase6), unpushed):
#   * One atomic rename commit; the loader has NO first-segment aliasing
#     (imporks probe <dir>/<name>/<rest>.kn literally) so dir + imports +
#     search-dirs were renamed together. New layout:
#       stdlib/lib -> stdlib/pet (namespace `lib.` -> `pet.`, 22 import sites)
#       coco_libs -> pets      (project deps; keeps `pets/libs` + `pets` roots)
#       ~/.coco/kirn-pkg -> ~/.kirn/pets (global cache; no-HOME fallback is
#       now `.kirn-pets`)
#       .cocolib -> .pet  magic header COCOLIB/1 -> PETLIB/1
#     Bundle format is unchanged otherwise: plain text, `@@FILE <path>` +
#     `@@END` framing, payload = coco.toml + code/ + docs/ + README + LICENSE,
#     output `build/{debug|release}/{target}/<name>-<ver>.pet`,
#     unpack dest `pets/libs/<name>`, function unpackCocolib -> unpackPet.
#   * `kirn build lib` subcommand, manifest `type="lib"`, and the registry
#     surface (github.com/coco-lib/*, kirn-libs) keep the `lib` name (Phase 7
#     owns coco.toml/lock + registry tokens; COCO_* env names are Phase 8).
#   * `kirn test .` (gate 6) revealed latent stdlib bugs, since fixed:
#     regexp_test asserted NOT-match for `a*b`/`acb` (standard glob says it
#     matches; now asserts that plus the `a*bx` negative); path.dirname
#     dropped the leading slash (join collapses the empty root segment; now
#     re-prefixes "/"); os_test assumed >=1 program arg (runners pass none).
#     io_test writes append-only scratch (no unlink primitive) -> gate 6
#     removes $ROOT/.io_tmp_test.txt before/after; other dirs (examples/pets,
#     pets/) are skipped by the test-file walker like build/.git.
#   * tools/j.kn was UTF-16LE + CRLF (pre-existing, invisible to text
#     tooling); normalized to UTF-8/LF during the `import pet.json` rewrite.
#   * ASan: build-asan/ tools predate Phase 6 (string renames + the 3 stdlib
#     behavior fixes only; no memory-semantics changes, but a rebuild of the
#     build-asan/* binaries is the next hygiene step before Phase 8).
#
# Phase 7 details worth remembering (commit rename(phase7), unpushed):
#   * Manifest/lockfile rename: coco.toml -> kirn.toml, coco.lock -> kirn.lock
#     (one commit; readManifest/writeManifest/readLock/writeLock, pack/unpack,
#     build/usage, comments). Hard rename per Cargo/npm/Bun precedent: NO
#     dual-read. Stale-file diagnostics instead: readManifest (tools/kirn.cpp
#     ~:163) and readLock (~:308) print a `mv coco.toml kirn.toml` /
#     `rm coco.lock` hint and return empty; the interpreter's
#     resolvePackageEntry (runtime.cpp ~:1131) prefers kirn.toml, prints a
#     note, but still parses a legacy coco.toml so stale installs keep running
#     until reinstalled (CLI never silently writes a mixed-state manifest).
#   * Registry retargeted to the REAL registry: github.com/pets-registry/pets
#     (org "Kirn's Pets"). CRITICAL: the plan's `registry/lib.toml` path
#     404'd; the live index is registry.toml at the repo ROOT with a NEW
#     schema: [registry] name/org/org_url/landing/docs/branch +
#     [pets."<name>"] repo="owner/repo" (req), latest, license, author,
#     website, branch (opt; currently zero entries). The old
#     github.com/coco-lib/coco-libs registry ([lib] rows with url shorthand)
#     is retired. tomlmini folds [pets."<name>"] into flat kv keys
#     ("pets.\"<name>\".repo") NOT Doc::tables (legacy [[lib]] rows), so
#     lookups now use a new enumPets() helper (tools/kirn.cpp ~:776);
#     repo is normalized to the `github.com/<repo>` clone shorthand via
#     petUrl() (clone = "https://"+spec+".git"), and cmdListOnline reads the
#     new keys (name/latest/license).
#   * Registry metadata dotfiles renamed: .kirn-registry-lib.toml ->
#     .pets-registry.toml and .kirn-sha -> .pets-sha everywhere (kirn.cpp
#     install/list sites + scaffold .gitignore template + gitIgnoreExtra) AND
#     the root .gitignore:14-16 entries were STALE (.coco-registry-lib.toml /
#     .coco-sha, dead code names) and now carry the live .pets-* names again.
#   * All registry fetches unified on
#     raw.githubusercontent.com/pets-registry/pets/main/registry.toml
#     (dropped the old refs/heads variant); `kirn-libs` prose -> "the Pets
#     Registry"; browse hint -> https://github.com/pets-registry/pets; scaffold
#     m.repo/m.homepage/install docs point at github.com/pets-registry/.
#   * CODEOWNERS root dropped the dead `@coco-lib` handle -> `* @rkriad585`
#     (.github/CODEOWNERS was already self-owned).
#   * Gate suite +1 for the stale-coco.toml diagnostic (conventions 7 -> 8
#     checks, total 149) and gate 7 now also asserts the scaffolded install
#     carries kirn.toml with no coco.toml present.
#
# Phase 5 details worth remembering:
#   * Zero-grep allowlist still in force: migration-toolkit scripts
#     (scripts/rename_kirn.py, rename_native_emitter.ps1, rename_native_ns.ps1),
#     .prettierignore:16 `*.co.ebnf` (grammar *filename* rename is Phase 10/11),
#     and prose docs (README/CONTRIBUTING/SECURITY/tools/src/stdlib/tests/
#     examples/grammar READMEs) -> Phase 10 prose pass.
#   * tools/kirn.cpp shell probes used to redirect to a literal `nul` file
#     (only meaningful on Windows); now routed through shellNullDevice()
#     (nul vs /dev/null) and all POSIX-reachable probes are clean -- native
#     builds/gates no longer litter a root `nul` artifact.
#   * Extension literals in C++: launcher "main.kn" (~:2436), package-entry
#     defaults /pin.kn,/code/pin.kn,/mod.kn,/code/mod.kn (runtime.cpp
#     ~:1143-1144), loader .kn strip at runtime.cpp ~:629/:1180,
#     checker.cpp ~:658, kirn.cpp ~:1729/:1957/:2740.
#
# Phase 4 details worth remembering:
#   * Folding plan Phase 9's code part INTO Phase 4 (per plan risk-row 5):
#     CMake lib targets renamed (coco_* -> kirn_* so CMake now emits
#     kirn_*.lib) AND the .lib link-list/probe strings in tools/kirn.cpp
#     (:1785 comment, :2474 probe, :2612-2613 cl.exe link list) were updated
#     in the same commit so the MSVC native-build surface stays coherent.
#     Plan Phase 9 now only files the native/cross build test.
#   * CMake edits are verified by INSPECTION only (no cmake on this box);
#     Windows CI (which uses cmake) is the real gate for them.
#   * Intentional-keeps for the phase-4 zero-grep (tool-name tokens still
#     present on purpose): migration-toolkit scripts (scripts/rename_*.ps1,
#     scripts/rename_kirn.py, scripts/run_phase3_half2.ps1), the frozen
#     _rename/baseline/ transcripts, and the migration-toolkit scripts
#     (the coco.toml/coco.lock manifest tokens named there were retired by
#     Phase 7; old coco_libs/ and ~/.coco/ paths were retired by Phase 6).
#     tools/README.md still lists the old tool table -> Phase 10
#     prose pass.

# docs/COCO_PLAN.md / docs/README.md / docs/FEATURE_GAP_ANALYSIS.md

# Line-ending rules of thumb (kept in sync between .gitattributes,
# .editorconfig, .prettierrc/.prettierignore, .husky/):
# * Every text file: LF, UTF-8, final newline, no trailing whitespace.
# * NEVER introduce CRLF; git never re-adds it on checkout now that
#   autocrlf is off.
# * New files created by any tool (Write/Edit) already come out LF.