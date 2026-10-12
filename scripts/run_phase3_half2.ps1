# run_phase3_half2.ps1 - ONE deterministic driver for Phase-3 (second half)
#
# Sequence (per KIRN_PLANS/REBRAND_PLAN.md Phase 3, second half, and the
# repo's writer/reader-lockstep doctrine):
#   1.  scripts/rename_native_ns.ps1 -Scan     -> must be green: 13/3
#   2.  REPO G-BUILD (clean)                   -> must be green (rename is
#       emitter-writer only; C++ first-half already at 0 openers / 0 kirn::,
#       so a build must stay green even BEFORE apply -- this is the "dry-run
#       must not change behavior" gate)
#   3.  scripts/rename_native_ns.ps1 -Apply    -> writes 13 tokens, 3 files
#   4.  scripts/rename_native_ns.ps1 -Scan     -> must be green AGAIN (idempotent,
#       second scan = 0 -> "applied twice is a no-op")
#   5.  REPO G-BUILD (clean, from scratch; LF-only preserved)
#   6.  scripts/runall.ps1 -Runner build\Debug\kirnrun.exe   -> 45/45
#   7.  scripts/types.ps1 / negative.ps1 / tests\conventions\run.ps1 / vm_diff
#   8.  commit ONE atomic commit (emitter + launcher call-site lockstep)
#
# The rename: emitter (src/backend/native.cpp) WRITES string literals that
# name the emitted user-facing namespace `kirn_native` (native.cpp:291 opener,
# :321 closer, `kirn_native::co_*` qualified emitted calls at :547-563,
# `kirn_native_register` emitters at :622/:639, launcher csv at :623), plus
# native.h:48 comment and tools/kirn.cpp:2406 launcher-emitted call. Writer
# and reader must flip in lockstep (jOOQ/LLVM dual-ABI doctrine: emitted text
# and the launcher that calls it change together or stale). C++ namespace
# first half (kirn -> kirn, kirn:: -> kirn::) is already committed/green.

$ErrorActionPreference = 'Stop'

function Invoke-Step($label, $script) {
    Write-Output ""
    Write-Output ("---------------- {0} ----------------" -f $label)
    & $script
    if ($LASTEXITCODE -ne 0) { throw "step FAILED (exit=$LASTEXITCODE): $label" }
    Write-Output ("[ok] {0}" -f $label)
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location -LiteralPath $repo

# 1) dry-run scan (reads only) - THE GATE. Census must be 13/3 before any write.
Invoke-Step "Phase3.2 scan (dry-run, expect 13/3)" '.\scripts\rename_native_ns.ps1 -Scan'
