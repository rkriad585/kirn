# rename_native_emitter.ps1 - Phase 3 (second half): coco_native -> kirn_native
#
# What this does: renames the `coco_native` EMITTER-EMITTED family to
# `kirn_native`. Per COCO_PLANS/COCO_TO_KIRN_PLAN.md Phase-3 second half:
# "the emitted native C++ writes `coco_native` -> the emitted native C++
# writes `kirn_native`", and per the same plan's emitters section - the
# native backend EMITS C++ source text that names a namespace `coco_native`,
# and that emitted text must flip to `kirn_native` in the SAME commit as the
# emitter + launcher that reference it (writer/reader lockstep - jOOQ
# codegen + LLVM dual-ABI doctrine: emitted text and its consumer must change
# together or generated code goes stale).
#
# `coco_native` is NOT a real C++ namespace in this repo - it exists only as
# (a) string literals the emitter WRITES into generated user-facing C++, and
# (b) a comment in src/backend/native.h. Verified census (exhaustive, 13
# tokens in 3 files):
#   src\backend\native.cpp  :291  emitter writes 'namespace coco_native {'
#   src\backend\native.cpp  :321  emitter writes '}' + close-comment
#   src\backend\native.cpp  :547  emitter writes 'coco_native::co_fdiv('
#   src\backend\native.cpp  :551  emitter writes 'coco_native::co_floordiv('
#   src\backend\native.cpp  :555  emitter writes 'coco_native::co_mod('
#   src\backend\native.cpp  :556  emitter writes 'coco_native::co_fmodm('
#   src\backend\native.cpp  :559  emitter writes 'coco_native::co_ipow('
#   src\backend\native.cpp  :562  emitter writes 'coco_native::co_shl('
#   src\backend\native.cpp  :563  emitter writes 'coco_native::co_shr('
#   src\backend\native.cpp  :622  emitter writes 'coco_native_register{' decl
#   src\backend\native.cpp  :639  emitter writes 'coco_native::' (registerAll)
#   src\backend\native.h    :48   comment '...coco_native...'
#   tools\coco.cpp          :2406 launcher writes 'coco_native_register(const)'
# All 13 are emitter-output strings + 1 header comment. The C++ namespace
# (namespace coco / coco::) first half is ALREADY DONE (0 openers, 0
# qualified refs; 'namespace kirn {' = 25 green). Do NOT touch: coco_* other
# families (coco_libs/coco.toml/coco/.co etc. are later phases).
#
# Usage:
#   scripts/rename_native_emitter.ps1 -Scan    # dry-run inventory, no writes
#   scripts/rename_native_emitter.ps1 -Apply   # rewrite (after -Scan green)
#
# LF-only: every write is UTF-8 no-BOM, LF line endings, final newline.
# Idempotent: applying twice is a no-op (second scan = 0).
# Safety: blocklist protects 0 here (nothing in this family is a later-phase
# token); scope is exactly the 3 files. G-VERIFY after -Apply.

param()
$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$targets = @(
    (Join-Path $RepoRoot 'src\backend\native.cpp'),
    (Join-Path $RepoRoot 'src\backend\native.h'),
    (Join-Path $RepoRoot 'tools\coco.cpp')
)

$old = 'coco_native'
function Rename-Scan {
    $n = 0
    foreach ($f in $targets) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $text = [IO.File]::ReadAllText($f)
        $m = [regex]::Matches($text, [regex]::Escape($old))
        if ($m.Count -eq 0) { continue }
        $n += $m.Count
        "{0,3}  {1}" -f $m.Count, $f.Substring($RepoRoot.Length + 1)
    }
    "[scan] {0} coco_native token(s) across target files" -f $n
    if ($n -ne 13) {
        Write-Warning "expected 13 coco_native tokens; census says $n. If this phasing is off, the corpus changed - re-inventory before applying."
    }
}

function Rename-Apply {
    $n = 0
    foreach ($f in $targets) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $text = [IO.File]::ReadAllText($f)
        $c = ([regex]::Matches($text, [regex]::Escape($old))).Count
        if ($c -eq 0) { continue }
        $text = $text.Replace($old, 'kirn_native')
        [IO.File]::WriteAllText($f, $text, (New-Object System.Text.UTF8Encoding($false)))
        $n += $c
        "{0,3}  {1}  (applied)" -f $c, $f.Substring($RepoRoot.Length + 1)
    }
    "[apply] {0} coco_native token(s) renamed -> kirn_native ({1} file(s))" -f $n, ($targets | Where-Object { Test-Path -LiteralPath $_ }).Count
    if ($n -ne 13) { Write-Warning "expected 13; applied $n. Verify G-VERIFY for correctness." }
}

if ($args -contains '-Apply') { Rename-Apply }
elseif ($args -contains '-Scan') { Rename-Scan }
else { Write-Error "pass -Scan (dry-run) or -Apply (rewrite). Never apply without a green -Scan first." }
