# rename_native_ns.ps1 - Phase 3 (second half): kirn_native -> kirn_native
#
# Per KIRN_PLANS/REBRAND_PLAN.md, Phase 3 has two halves. First half
# (namespace kirn -> kirn, kirn:: -> kirn::) is DONE and committed (verified:
# namespace kirn { = 0, kirn:: = 0, namespace kirn { = 25).
#
# THIS is the second half: the `kirn_native` FAMILY. Ground-truth census
# (verified by exhaustive word-bounded inventory, NOT plan prose):
#   kirn_native is NOT a real C++ namespace in this repo - it only exists as
#   EMITTER-OUTPUT text: src/backend/native.cpp WRITES string literals into
#   the generated user-facing C++ (namespace opener native.cpp:291, closer
#   :321, kirn_native::co_* qualified emitted calls :547-563, the emitted
#   kirn_native_register :622, and kirn_native:: :639), native.h:48 is a
#   comment, and tools/kirn.cpp:2406 is the launcher's emitted call into
#   the generated kirn_native_register.
#
# EXACT census (authoritative, word-bounded `\bkirn_native\b` plus
# `kirn_native_register` family, src+tools only):
#   src\backend\native.cpp  :291, :321, :547, :551, :555, :556, :559, :562,
#                            :563, :622, :639   (11 emitter-output tokens)
#   src\backend\native.h    :48                  (1 comment token)
#   tools\kirn.cpp          :2406                (1 launcher-emitted token)
#   TOTAL = 13 tokens across 3 files.
#
# Because native.cpp is the EMITTER (writes the string) and tools/kirn.cpp is
# the LAUNCHER (reads/registers the emitted name), they must rename in
# LOCKSTEP - emitter-writer and launcher-reader atomically, else the emitted
# generated code goes stale (the jOOQ/LLVM dual-ABI banlist-to-dry-run-to-apply
# doctrine from Phase-3 research: "rename the writer AND the reader together,
# and any emitter that writes a namespace opener must be renamed along with
# every consumer of that namespace, else generated output is stale").
#
# We do NOT touch, in this phase:
#   - the real C++ `namespace kirn`/`kirn::` surface (Phase-3 first half -
#     already green, namespace kirn = 0, kirn:: = 0)
#   - kirn_libs, kirn.toml, kirn.lock, .kn, .cob, KIRNB, KIRN_, kirn-*
#     (later phases: 4/5/6/7/8/9)
#   - the kernel/atom tokens (kirnBuf, kirnExe, kirn_native_register is the
#     622 one BELOW - included; do not widen to kirn_native_* beyond these)
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
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# resolve the repo root from the script location so a workspace folder rename
# can never strand this on a stale absolute path
$targets = @(
    (Join-Path $RepoRoot 'src\backend\native.cpp'),
    (Join-Path $RepoRoot 'src\backend\native.h'),
    (Join-Path $RepoRoot 'tools\kirn.cpp')
)
$old = 'kirn_native'

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
    "[scan] {0} kirn_native token(s) across target files (expect 13)" -f $n
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
    "[apply] {0} kirn_native token(s) renamed -> kirn_native" -f $n
}

if ($Scan) { Invoke-Scan }
elseif ($Apply) { Invoke-Apply }
else { Write-Error "pass -Scan (dry-run) or -Apply (write). Never apply without a green -Scan first." }
