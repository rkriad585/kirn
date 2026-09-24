# Kirn "What did we do so far?" - shared understanding of the project's health &
# safety netting tasks, kept up to date as the work progresses.

# Things the repo is currently set up for:
# - LF line endings EVERYWHERE (see .gitattributes "* text=auto eol=lf").
#   core.autocrlf is DISABLED for this repo (`git config core.autocrlf false`)
#   so git never auto-converts and never warns about LF/CRLF replacement.
# - 5-gate verification harness: scripts/runall.ps1, scripts/types.ps1
#   (-Check/-Run), scripts/negative.ps1 (-Runner), scripts/vm_diff.ps1,
#   tests/conventions/run.ps1. Phase-1 snapshot captured in scripts/
#   rename_baseline.ps1 (transcripts under _rename/baseline/).
#   Phase 4 renamed the tools + every harness call site: the CLI is now
#   `kirn` (driver), `kirnrun`, `kirncheck`, `kirnlex`, `kirnparse`.
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
#   * scripts/gates_linux.sh is the faithful bash port of the 5 gates
#     (runall 45 / types 31 / negative 18 / vm_diff 43 / conventions 7 =
#     144 checks); currently 144/144 green on the renamed binaries in build/
#     (CC=kirncheck, CR=kirnrun, CO=kirn). Native smoke: kirn build
#     --native + run must print `10 10 105 1024.0` for
#     examples/native_scalar_mix.co and exit 94 (=350 % 256) for
#     examples/native_main.co. (Compile with COCO_CXX=g++; that env var is
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
#   native_main.co (exit != 0) -- its loop ignores `# expect-exit`, a pre-*
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
# complete. NEXT is Phase 5: source extension .co -> .kn (loader + resolver +
# on-disk git mv of 130 files, atomic with every glob).
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
#     _rename/baseline/ transcripts, and the manifest tokens coco.toml/
#     coco.lock/coco_libs/~/.coco/ (Phases 6-7). tools/README.md still lists
#     the old tool table -> Phase 10 prose pass.

# docs/COCO_PLAN.md / docs/README.md / docs/FEATURE_GAP_ANALYSIS.md

# Line-ending rules of thumb (kept in sync between .gitattributes,
# .editorconfig, .prettierrc/.prettierignore, .husky/):
# * Every text file: LF, UTF-8, final newline, no trailing whitespace.
# * NEVER introduce CRLF; git never re-adds it on checkout now that
#   autocrlf is off.
# * New files created by any tool (Write/Edit) already come out LF.