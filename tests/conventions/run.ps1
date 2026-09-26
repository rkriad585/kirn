# Phase 3 convention-file matrix tester.
#
# Verifies that the special convention files (main.kn, pin.kn) are resolved:
#   * `kirn run .` picks the right entry (manifest main -> code/main.kn ->
#     main.kn -> code/pin.kn -> pin.kn) and errors helpfully when none exist
#   * importing a package directory runs its pin.kn (initializer) exactly once
#     and exposes the package's pub surface

param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path)

$kirn = Join-Path $Root "build\kirn.exe"
if (-not (Test-Path $kirn)) { Write-Error "kirn.exe not found: $kirn" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kirn-conv-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

$pass = 0; $fail = 0
function Check($name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host ("  PASS  {0}" -f $name); $script:pass++ }
    else     { Write-Host ("  FAIL  {0}`n         {1}" -f $name, ($detail -replace "`r?`n", " ")); $script:fail++ }
}

function SetFile($path, $content) {
    $dir = Split-Path $path -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    [System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
}

# Run a kirn subcommand, capturing exit code and combined output without
# letting native stderr trips PowerShell's error handling.
function RunCoco {
    param([string[]]$CocoArgs)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $kirn
    $psi.Arguments = ($CocoArgs -join " ")
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $soT = $p.StandardOutput.ReadToEndAsync()
    $seT = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit() | Out-Null
    $so = $soT.GetAwaiter().GetResult()
    $se = $seT.GetAwaiter().GetResult()
    $code = $p.ExitCode
    $p.Dispose()
    return [pscustomobject]@{ Code = $code; Out = ($so + $se) }
}

Write-Host "== run-entry resolution =="

# 1. code/main.kn entry (the classic scaffold)
$p = Join-Path $tmp "a"
SetFile (Join-Path $p "code\main.kn") "def main() { print(`"A`"); }"
$r = RunCoco @("run", $p)
Check "code/main.kn entry" ($r.Code -eq 0 -and $r.Out -match "A") $r.Out

# 2. manifest main = other path wins over code/main.kn
$p = Join-Path $tmp "b"
SetFile (Join-Path $p "coco.toml") "[package]`nname=`"b`"`nmain=`"start.kn`"`n"
SetFile (Join-Path $p "start.kn") "def main() { print(`"Bmain`"); }"
SetFile (Join-Path $p "code\main.kn") "def main() { print(`"Bcode`"); }"
$r = RunCoco @("run", $p)
Check "manifest main preferred" ($r.Code -eq 0 -and $r.Out -match "Bmain") $r.Out

# 3. pin.kn as the only entry (library-style, run directly)
$p = Join-Path $tmp "c"
SetFile (Join-Path $p "pin.kn") "pub def main() { print(`"C`"); }"
$r = RunCoco @("run", $p)
Check "pin.kn as run entry" ($r.Code -eq 0 -and $r.Out -match "C") $r.Out

# 4. no entry -> helpful fix-it error
$p = Join-Path $tmp "d"
New-Item -ItemType Directory -Path $p -Force | Out-Null
SetFile (Join-Path $p "code\util.kn") "pub def u() -> int { return 1; }"
$r = RunCoco @("run", $p)
Check "no entry -> fix-it error" ($r.Code -ne 0 -and $r.Out -match "no entry point") $r.Out

Write-Host "== pin.kn package initializer =="

# 5. package run-once + pub surface across several imports
$p = Join-Path $tmp "proj"
SetFile (Join-Path $p "pets\greet\code\pin.kn") @"
var loads = 0;
loads = loads + 1;
pub def load_count() -> int { return loads; }
pub def hi(who: string) -> string { return "hi " + who; }
"@
SetFile (Join-Path $p "main.kn") @"
import greet;
import greet as g2;
import greet as g3;
def main() {
    print("count=", greet.load_count());
    print(greet.hi("world"));
}
"@
$r = RunCoco @("run", $p)
Check "pin.kn runs once (count=1)" ($r.Code -eq 0 -and $r.Out -match "count= 1") $r.Out
Check "pin.kn pub surface" ($r.Code -eq 0 -and $r.Out -match "hi world") $r.Out

# 6. pin.kn without a manifest, discovered in code/
$p = Join-Path $tmp "proj2"
SetFile (Join-Path $p "pets\nomanifest\code\pin.kn") @"
pub def poke() -> string { return "poked"; }
"@
SetFile (Join-Path $p "main.kn") @"
import nomanifest;
def main() { print(nomanifest.poke()); }
"@
$r = RunCoco @("run", $p)
Check "pin.kn discovered w/o manifest" ($r.Code -eq 0 -and $r.Out -match "poked") $r.Out

Write-Host ""
Write-Host ("{0} passed, {1} failed" -f $pass, $fail)
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($fail -gt 0) { exit 1 }
exit 0
