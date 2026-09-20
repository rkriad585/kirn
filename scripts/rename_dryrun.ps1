# scripts/rename_dryrun.ps1 - Phase 1 inventory: Coco->Kirn touch-point census (read-only).
#
# COCO_PLANS/COCO_TO_KIRN_PLAN.md Phase 1 ("Baseline snapshot & safety netting",
# step 4) requires a reproducible dry-run inventory of every 'coco/Coco/COCO'
# case-form occurrence. This script produces that inventory and changes nothing.
#
# It counts RAW SUBSTRING occurrences (the same method as the plan's §1 table and
# §3.3 sample: [regex]::Matches(text, escaped pattern) summed per file) so the
# output diff can be compared against the §3 inventory. Excludes scratch/build
# trees and VCS/agent internals (build/, .git/, .cache/, .ruff_cache/,
# .opencode/, _rename/).
#
# Usage:
#     powershell -File scripts/rename_dryrun.ps1            # repo root
#     powershell -File scripts/rename_dryrun.ps1 -Root C:\path\to\coco
#     powershell -File scripts/rename_dryrun.ps1 -Pattern Coco -Pattern coco -Pattern COCO
#
# Exit code: always 0 (read-only census; a nonzero exit is reserved for IO errors).

param(
    [string]$Root = (Get-Location),
    [string[]]$Pattern = @('Coco', 'coco', 'COCO'),
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# Directories that are never census targets (names only; compared against path components).
$Skip = @('build', '.git', '.cache', '.ruff_cache', '.opencode', '_rename')

$files = Get-ChildItem -Path $Root -Recurse -File | Where-Object {
    $rel = $_.FullName.Substring((Resolve-Path -LiteralPath $Root).Path.Length).TrimStart('\', '/')
    $parts = $rel.Split(@('\', '/'), [StringSplitOptions]::RemoveEmptyEntries)
    -not ($parts | Where-Object { $Skip -contains $_ })
}

# Case-sensitive dictionary: @{} is case-insensitive in PowerShell, which would
# collapse 'Coco'/'coco'/'COCO' into one key.
$totals = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([System.StringComparer]::Ordinal)
foreach ($p in $Pattern) {
    $totals.Add($p, 0)
}

$fileCount = 0
foreach ($f in $files) {
    try {
        $bytes = [IO.File]::ReadAllBytes($f.FullName)
        # Treat files with NUL bytes as binary (mirrors rg's binary detection); exclude.
        if ($bytes -contains 0) { continue }
        $text = [Text.Encoding]::UTF8.GetString($bytes)
    } catch {
        # Unreadable files are not census targets.
        continue
    }
    $rel = $f.FullName.Substring((Resolve-Path -LiteralPath $Root).Path.Length).TrimStart('\', '/')
    $lineHits = 0
    foreach ($p in $Pattern) {
        $n = ([regex]::Matches($text, [regex]::Escape($p))).Count
        if ($n) {
            $totals[$p] = $totals[$p] + $n
            $lineHits += $n
            if (-not $Quiet) {
                "{0,-6} {1,6}  {2}" -f $p, $n, $rel
            }
        }
    }
    if ($lineHits) { $fileCount++ }
}

Write-Output ''
Write-Output ("TOTAL files with hits   : {0}" -f $fileCount)
foreach ($p in $Pattern) {
    Write-Output ("TOTAL {0,-6} occurrences  : {1,6}" -f $p, [int]$totals[$p])
}
$grand = 0
foreach ($p in $Pattern) { $grand += [int]$totals[$p] }
Write-Output ("TOTAL across all forms : {0,6}" -f $grand)
Write-Output ''
Write-Output 'dry-run only - nothing was modified.'
exit 0