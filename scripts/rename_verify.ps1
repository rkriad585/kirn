#requires -Version 5.1
# rename_verify.ps1 - CI gate that keeps the language rename from eroding.
#
# The language was renamed; the old brand is now banned repo-wide. This script
# fails the build if a pre-rename token reappears in a git-tracked file, so a
# later commit cannot silently undo the rename.
#
# NOTE: this file deliberately never spells the banned word, not even in
# comments, so that the gate does not flag its own source. The brand is matched
# as a character class instead (see $brand below). It must therefore exempt
# itself - see scripts/rename_allowlist.txt.
#
# Run it locally (from anywhere) or let CI run it on every push and PR:
#     pwsh -NoProfile -File scripts/rename_verify.ps1
#     pwsh -NoProfile -File scripts/rename_verify.ps1 -Report _rename/gate.md
#
# Exit codes:
#     0  clean
#     1  violations found
#     2  the gate itself failed (bad git state, unreadable allowlist)
#
# DESIGN NOTES
# ------------
# * Tracked files only. `git grep` never sees untracked build output, so a stale
#   build/ tree can never fail this gate.
# * The allowlist is a BUDGET, not a registry (the check-spelling philosophy):
#   every entry must carry a reason, and an entry that no longer matches any
#   violation is itself reported as a failure so exemptions cannot silently rot
#   into a blanket suppression.
# * Patterns avoid `\b` on purpose. GNU grep and .NET disagree about it, and a
#   word-boundary pattern would MISS the attached forms that matter most
#   (old_thing, oldrun, OLD_ASAN, libold_rt) because those have no boundary
#   after the brand. The brand pattern therefore matches the standalone word OR
#   the identifier prefix/suffix.
# * The old-source-extension pattern must exclude a following "." so a domain
#   like foo.co.uk is not a violation, while a real old-extension file is.
# * This file cannot be free of the tokens: the extension pattern is a literal
#   regex, and the brand appears nowhere else. It therefore exempts ITSELF via
#   scripts/rename_allowlist.txt, which is why its own name is listed there.
# * Cross-platform: forward slashes, -LiteralPath, no 7.x-only syntax, and an
#   explicit exit code at every path (CI dot-sources the script and appends its
#   own `exit $LASTEXITCODE`, so a trailing no-match 1 would fail a green run).

[CmdletBinding()]
param(
    # Repo root. Defaults to the parent of this script's directory.
    [string] $Root,
    # Optional markdown report path.
    [string] $Report
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if (-not $Root) { $Root = Split-Path -Parent $PSCommandPath }
$Root = (Resolve-Path -LiteralPath $Root).Path
$RepoRoot = Split-Path -Parent $Root

$AllowlistPath = Join-Path $RepoRoot 'scripts/rename_allowlist.txt'

# Run a native command, capturing stdout/stderr and the exit code. $LASTEXITCODE
# is read inside the function because a function's own commands reset it.
function Invoke-Git {
    param([Parameter(Mandatory = $true)][string[]] $Arguments)
    $global:LASTEXITCODE = 0
    $output = @(& git -C $RepoRoot @Arguments 2>&1)
    $code = $LASTEXITCODE
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

# git grep exit codes: 0 = match, 1 = no match, >1 = real error. Collapsing 1
# and an error into the same answer is how a broken gate reads as green, so the
# two are kept distinct all the way to the exit code.
function Get-GrepMatches {
    param([Parameter(Mandatory = $true)][string] $Pattern)
    $result = Invoke-Git @('grep', '-I', '-n', '-i', '-E', '-e', $Pattern, '--', '.')
    switch ($result.ExitCode) {
        0 { return @($result.Output) }
        1 { return @() }
        default {
            [Console]::Error.WriteLine("FATAL: git grep '$Pattern' failed (rc=$($result.ExitCode))")
            $result.Output | ForEach-Object { [Console]::Error.WriteLine("  $_") }
            exit 2
        }
    }
}

# Parse the allowlist. Format: <path-glob><TAB><reason>. Blank lines and
# #-comments are ignored. A reason may wrap onto following indented lines. An
# entry without a reason is a hard error, so an exemption can never be granted
# silently.
function Read-Allowlist {
    if (-not (Test-Path -LiteralPath $AllowlistPath)) {
        [Console]::Error.WriteLine("FATAL: allowlist not found: $AllowlistPath")
        exit 2
    }
    $entries = New-Object System.Collections.Generic.List[object]
    $n = 0
    foreach ($raw in [IO.File]::ReadAllLines($AllowlistPath)) {
        $n++
        # an indented line continues the reason above it
        if ($raw.Length -gt 0 -and ($raw.StartsWith(' ') -or $raw.StartsWith("`t"))) {
            $cont = $raw.Trim()
            if ($entries.Count -eq 0) {
                [Console]::Error.WriteLine("FATAL: ${AllowlistPath}:${n} indented before any entry")
                exit 2
            }
            if ($cont.Length -gt 0) { $entries[$entries.Count - 1].Reason += " $cont" }
            continue
        }
        $trimmed = $raw.Trim()
        if (($trimmed.Length -eq 0) -or $trimmed.StartsWith('#')) { continue }
        $parts = $trimmed -split "`t"
        if ($parts.Count -lt 2) {
            [Console]::Error.WriteLine("FATAL: ${AllowlistPath}:${n} needs a tab-separated reason")
            exit 2
        }
        if ([string]::IsNullOrWhiteSpace($parts[1])) {
            [Console]::Error.WriteLine("FATAL: ${AllowlistPath}:${n} has an empty reason")
            exit 2
        }
        $entries.Add([pscustomobject]@{
            Glob   = $parts[0].Trim()
            Reason = $parts[1].Trim()
            Line   = $n
        })
    }
    return $entries
}

# Turn a `git ls-files` line into a full path. git always emits forward slashes;
# normalize for the host filesystem.
function Resolve-RepoFile {
    param([Parameter(Mandatory = $true)][string] $Relative)
    $rel = $Relative -replace '\\', '/'
    return Join-Path $RepoRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
}

$brand = '([Cc][Oo][Cc][Oo])([^A-Za-z0-9]|$)|[Cc][Oo][Cc][Oo][_-]'
# The old CLI tool names concatenated the brand directly onto a verb
# (brand+run / brand+check / brand+lex / brand+parse), leaving no separator for
# the branch above to match. They are listed explicitly rather than loosening
# the boundary, which would re-admit ordinary words like "coconut".
$brandTool = '[Cc][Oo][Cc][Oo](run|check|lex|parse|doc|test)'
$oldExtension = '\.co([^A-Za-z0-9.]|$)'
$oldImport = 'import[[:space:]]+lib\.'

$allow = Read-Allowlist

Write-Host "rename_verify: scanning tracked files under $RepoRoot"
Write-Host "rename_verify: $($allow.Count) allowlist entrie(s)"

$violations = New-Object System.Collections.Generic.List[string]
$usedGlobs = New-Object System.Collections.Generic.HashSet[string]

# ---- 1. PATHS -------------------------------------------------------------
# git grep searches contents only, so a resurrected file named with the old
# source extension or an old tool name would otherwise pass forever. Check tracked paths too.
$tracked = Invoke-Git @('ls-files')
if ($tracked.ExitCode -ne 0) {
    [Console]::Error.WriteLine("FATAL: git ls-files failed (rc=$($tracked.ExitCode))")
    $tracked.Output | ForEach-Object { [Console]::Error.WriteLine("  $_") }
    exit 2
}

$pathViolations = @()
foreach ($raw in $tracked.Output) {
    $line = [string]$raw
    if ($line.Length -eq 0) { continue }
    $line = $line -replace '\\', '/'
    $isStale = ($line -match $brand) -or ($line -match $brandTool) -or
               ($line -match $oldExtension) -or ($line -match $oldImport)
    if (-not $isStale) { continue }
    $exempt = $false
    foreach ($entry in $allow) {
        if ($line -like $entry.Glob) {
            $exempt = $true
            [void]$usedGlobs.Add($entry.Glob)
            break
        }
    }
    if (-not $exempt) { $pathViolations += $line }
}
foreach ($p in $pathViolations) {
    $violations.Add("$p`n    -> stale name in the FILE PATH (git mv it)")
}

# ---- 2. CONTENT -----------------------------------------------------------
# One scan per pattern; the patterns are not redundant with each other.
foreach ($pattern in @($brand, $brandTool, $oldExtension, $oldImport)) {
    foreach ($hit in (Get-GrepMatches $pattern)) {
        $text = [string]$hit
        # git grep emits `path:line:content`. A Windows path could contain a
        # colon, so split off the first two fields from the left.
        $firstColon = $text.IndexOf(':')
        if ($firstColon -lt 0) { $violations.Add($text); continue }
        $file = $text.Substring(0, $firstColon)
        $rest = $text.Substring($firstColon + 1)
        $secondColon = $rest.IndexOf(':')
        if ($secondColon -lt 0) { $violations.Add($text); continue }
        $lineNo = $rest.Substring(0, $secondColon)
        $content = $rest.Substring($secondColon + 1).Trim()

        $fileNorm = $file -replace '\\', '/'
        $exempt = $false
        foreach ($entry in $allow) {
            if ($fileNorm -like $entry.Glob) {
                $exempt = $true
                [void]$usedGlobs.Add($entry.Glob)
                break
            }
        }
        if ($exempt) { continue }

        $violations.Add("$fileNorm`:$lineNo`: $content")
    }
}

# ---- 3. ALLOWLIST STALENESS ----------------------------------------------
# An exemption that no longer matches anything is dead weight. Report it so the
# budget only ever reflects reality.
$stale = @()
foreach ($entry in $allow) {
    if (-not $usedGlobs.Contains($entry.Glob)) { $stale += $entry }
}

# ---- 4. REPORT ------------------------------------------------------------
if ($Report) {
    $lines = @('# rename_verify report', '')
    $lines += ('generated  : {0}' -f (Get-Date).ToUniversalTime().ToString('o'))
    $lines += ('repo root  : {0}' -f $RepoRoot)
    $lines += ('violations : {0}' -f $violations.Count)
    $lines += ('allowlist  : {0} entrie(s), {1} unused' -f $allow.Count, $stale.Count)
    $lines += ''
    if ($violations.Count -gt 0) {
        $lines += '## Violations'
        $lines += ''
        foreach ($v in $violations) { $lines += ('- ' + ($v -replace "`n", ' ')) }
        $lines += ''
    }
    if ($stale.Count -gt 0) {
        $lines += '## Unused allowlist entries'
        $lines += ''
        foreach ($s in $stale) {
            $lines += ('- `{0}` (line {1}) - {2}' -f $s.Glob, $s.Line, $s.Reason)
        }
        $lines += ''
    }
    # UTF-8 without BOM, LF line endings, identical on Windows and Linux.
    $enc = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Report, ($lines -join "`n") + "`n", $enc)
}

# ---- 5. VERDICT -----------------------------------------------------------
if ($violations.Count -gt 0) {
    foreach ($v in $violations) {
        $flat = $v -replace "`n", ' '
        Write-Host "::error file=$($flat.Split(':')[0]),title=Pre-rename token::" `
            "the old brand is banned here. Use the current names, or add a" `
            "reasoned entry to scripts/rename_allowlist.txt if this reference is" `
            "intentional."
    }
    [Console]::Error.WriteLine("")
    [Console]::Error.WriteLine("FAIL: $($violations.Count) pre-rename token(s) found.")
    exit 1
}

if ($stale.Count -gt 0) {
    foreach ($s in $stale) {
        # build the message first: a .NET method call cannot span lines with a
        # backtick continuation the way Write-Host can.
        $msg = "::warning file=scripts/rename_allowlist.txt,title=Stale exemption::" +
            " '$($s.Glob)' (line $($s.Line)) no longer matches any violation - remove it."
        [Console]::Error.WriteLine($msg)
    }
    [Console]::Error.WriteLine("FAIL: $($stale.Count) stale allowlist entr(y/ies).")
    exit 1
}

Write-Host "OK: no pre-rename tokens in tracked files (allowlist fully used)."
exit 0
