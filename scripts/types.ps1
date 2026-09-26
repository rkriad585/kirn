# Runs the PLAN Phase 5 type-system test suite:
#   - tests/types/p*.kn  (positive) must PASS kirncheck and kirnrun (exit 0)
#   - tests/types/n*.kn  (negative) must FAIL kirncheck with the substring on
#     the `# expect:` comment line.
#   scripts/types.ps1 [-Check <kirncheck-path>] [-Run <kirnrun-path>]
param(
    [Alias("cc")]
    [string]$Check = "kirncheck.exe",
    [Alias("rr")]
    [string]$Run = "kirnrun.exe"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$typesDir = Join-Path $root "tests\types"

function Resolve-Exe([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }
    if (-not (Test-Path -LiteralPath $p)) { Write-Error "executable not found at '$p'" }
    return $p
}
$CheckExe = Resolve-Exe $Check
$RunExe = Resolve-Exe $Run

$positive = Get-ChildItem -LiteralPath $typesDir -Filter "p*.kn" | Sort-Object Name
$negative = Get-ChildItem -LiteralPath $typesDir -Filter "n*.kn" | Sort-Object Name

$pass = 0; $fail = 0
function Report([string]$status, [string]$name, [string]$msg) {
    if ($status -eq "PASS") { Write-Host ("PASS  {0}: {1}" -f $name, $msg) }
    else { Write-Output ("FAIL  {0}: {1}" -f $name, $msg); $script:fail++ }
}

foreach ($t in $positive) {
    $name = $t.BaseName
    $cout = (& $CheckExe $t.FullName 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        Report "FAIL" $name "kirncheck rejected positive test (exit $LASTEXITCODE)";
        $cout.Trim() -split "`r?`n" | ForEach-Object { Write-Output "          $_" }
        continue
    }
    $rout = (& $RunExe $t.FullName 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        Report "FAIL" $name "kirnrun did not reach exit 0 (exit $LASTEXITCODE)";
        $rout.Trim() -split "`r?`n" | ForEach-Object { Write-Output "          $_" }
        continue
    }
    Report "PASS" $name "check + run clean"
    $pass++
}

foreach ($t in $negative) {
    $name = $t.BaseName
    $expectRow = Select-String -LiteralPath $t.FullName -Pattern '#\s*expect:' | Select-Object -First 1
    if (-not $expectRow) {
        Report "FAIL" $name "missing '# expect:' comment line"
        continue
    }
    $expected = ($expectRow.Line -replace '^.*#\s*expect:\s*', '').Trim()
    if (-not $expected) {
        Report "FAIL" $name "empty '# expect:' comment line"
        continue
    }
    $out = (& $CheckExe $t.FullName 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) {
        Report "FAIL" $name "kirncheck exited 0, expected a diagnostic"
        continue
    }
    if ($out -notmatch [regex]::Escape($expected)) {
        Report "FAIL" $name "output does not contain expected error"
        Write-Output "          expected substring: $expected"
        $out.Trim() -split "`r?`n" | ForEach-Object { Write-Output "            $_" }
        continue
    }
    Report "PASS" $name $expected
    $pass++
}

Write-Output ("RESULT: {0} passed, {1} failed" -f $pass, $fail)
exit ($(if ($fail -eq 0) { 0 } else { 1 }))
