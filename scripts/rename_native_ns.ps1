# rename_native_ns.ps1 - Phase 3 (second half): coco_native -> kirn_native
#
# Per COCO_PLANS/COCO_TO_KIRN_PLAN.md, Phase 3 has two halves. First half
# (namespace coco -> kirn, coco:: -> kirn::) is DONE and committed (verified:
# namespace coco { = 0, coco:: = 0, namespace kirn { = 25).
#
# THIS is the second half: the `coco_native` FAMILY. Ground-truth census
# (verified by exhaustive word-bounded inventory, NOT plan prose):
#   coco_native is NOT a real C++ namespace in this repo - it only exists as
#   EMITTER-OUTPUT text: src/backend/native.cpp WRITES string literals into
#   the generated user-facing C++ (namespace opener native.cpp:291, closer
#   :321, coco_native::co_* qualified emitted calls :547-563, the emitted
#   coco_native_register :622, and coco_native:: :639), native.h:48 is a
#   comment, and tools/coco.cpp:2406 is the launcher's emitted call into
#   the generated coco_native_register.
#
# EXACT census (authoritative, word-bounded `\bcoco_native\b` plus
# `coco_native_register` family, src+tools only):
#   src\backend\native.cpp  :291, :321, :547, :551, :555, :556, :559, :562,
#                            :563, :622, :639   (11 emitter-output tokens)
#   src\backend\native.h    :48                  (1 comment token)
#   tools\coco.cpp          :2406                (1 launcher-emitted token)
#   TOTAL = 13 tokens across 3 files.
#
# Because native.cpp is the EMITTER (writes the string) and tools/coco.cpp is
# the LAUNCHER (reads/registers the emitted name), they must rename in
# LOCKSTEP - emitter-writer and launcher-reader atomically, else the emitted
# generated code goes stale (the jOOQ/LLVM dual-ABI banlist-to-dry-run-to-apply
# doctrine from Phase-3 research: "rename the writer AND the reader together,
# and any emitter that writes a namespace opener must be renamed along with
# every consumer of that namespace, else generated output is stale").
#
# We do NOT touch, in this phase:
#   - the real C++ `namespace coco`/`coco::` surface (Phase-3 first half -
#     already green, namespace coco = 0, coco:: = 0)
#   - coco_libs, coco.toml, coco.lock, .co, .cob, COCOB, COCO_, coco-*
#     (later phases: 4/5/6/7/8/9)
#   - the kernel/atom tokens (cocoBuf, cocoExe, coco_native_register is the
#     622 one BELOW - included; do not widen to coco_native_* beyond these)
#
# LF-only: this repo is LF (`* text=auto eol=lf`, autocrlf off). We write
# UTF-8 (no BOM) + LF via .NET, no CRLF ever. Idempotent: applying twice is
# a no-op (second scan = 0).
#
# Usage:
#   scripts/rename_native_ns.ps1 -Scan   # dry-run inventory, reads only
#   scripts/rename_native_ns.ps1 -Apply  # apply ONLY after -Scan green (=13)

param(
    [switch]$Scan,
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$RepoRoot = 'C:\Users\rkriad585\Projects\coco'
if (-not (Test-Path -LiteralPath $RepoRoot)) {
    # fall back to resolving from script location (avoids Join-Path doubling)
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

# hardcoded absolute paths - no Join-Path, no double-src bug
$targets = @(
    (Join-Path $RepoRoot 'src\backend\native.cpp'),
    (Join-Path $RepoRoot 'src\backend\native.h'),
    (Join-Path $RepoRoot 'tools\coco.cpp')
)
$old = 'coco_native'

function Invoke-Scan {
    $n = 0
    foreach ($f in $targets) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $t = [IO.File]::ReadAllText($f)
        $m = [regex]::Matches($t, [regex]::Escape($old))
        if ($m.Count -eq 0) { continue }
        $n += $m.Count
        "{0,3}  {1}" -f $m.Count, $f.Substring($RepoRoot.Length + 1)
    }
    "[scan] {0} coco_native token(s) across target files (expect 13)" -f $n
    if ($n -ne 13) { Write-Warning "expected 13; census says $n. Re-inventory before any Apply." }
}

function Invoke-Apply {
    $n = 0
    foreach ($f in $targets) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $t = [IO.File]::ReadAllText($f)
        $m = [regex]::Matches($t, [regex]::Escape($old))
        if ($m.Count -eq 0) { continue }
        $n += $m.Count
        $t = $t.Replace($old, 'kirn_native')
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($f, $t, $utf8)
        "{0,3}  {1}  (applied)" -f $m.Count, $f.Substring($RepoRoot.Length + 1)
    }
    "[apply] {0} coco_native token(s) renamed -> kirn_native" -f $n
}

if ($Scan) { Invoke-Scan }
elseif ($Apply) { Invoke-Apply }
else { Write-Error "pass -Scan (dry-run) or -Apply (write). Never apply without a green -Scan first." }
