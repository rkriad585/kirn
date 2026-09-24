# ASan coverage harness: builds the whole Kirn runtime/toolchain with
# AddressSanitizer into build-asan/, then runs every examples/*.co through the
# ASan-instrumented kirnrun. Any leak or memory error surfaces as a nonzero
# exit (with an ASan report) and is reported like the CI corpus runner.
#
#   scripts/asanall.ps1 [-Quiet] [-Runner build-asan\kirnrun.exe]
#
# Also does a `--native --asan` build+run smoke test for the native codegen
# path (whole-runtime instrumented).
param([string]$Runner = "", [switch]$Quiet)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# --- 1. configure + build an ASan-instrumented tree ------------------------
$bdir = "$root\build-asan"
if (-not (Test-Path "$bdir\CMakeCache.txt")) {
    & cmake -S $root -B $bdir -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCOCO_ASAN=ON
    if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed" }
}
& cmake --build $bdir --config Debug
if ($LASTEXITCODE -ne 0) { Write-Error "cmake build failed" }

if (-not $Runner) { $Runner = "$bdir\kirnrun.exe" }
if (-not (Test-Path $Runner)) { Write-Error "runner not found: $Runner" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kirn-asan-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$failures = @()
foreach ($f in (Get-ChildItem "$root\examples" -Filter *.co | Sort-Object Name)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Runner
    $psi.Arguments = '"' + $f.FullName + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEnd()
    $se = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $asanHit = $se -match "AddressSanitizer|LeakSanitizer|runtime error"
    if (-not $Quiet) {
        Write-Host ("{0,-28} code={1,-4} asan={2}" -f $f.Name, $p.ExitCode, ($asanHit ? "YES" : "no"))
    }
    if ($p.ExitCode -ne 0 -or $asanHit) {
        $failures += $f.Name
        if ($se) { Write-Host "--- stderr for $($f.Name) ---"; Write-Host $se }
    }
}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

$count = (Get-ChildItem "$root\examples" -Filter *.co | Measure-Object).Count
Write-Host ""
if ($failures.Count -eq 0) { Write-Host "ASan corpus: ALL PASS ($count examples)" }
else {
    Write-Host "ASan failures: $($failures -join ', ')"
    exit 1
}

# --- 2. native codegen ASan smoke test -------------------------------------
Write-Host "`nASan --native smoke test ..."
$asanExe = Join-Path $root "build\asan-native-smoke.exe"
& "$bdir\kirn.exe" build "$root\examples\native_scalar_mix.co" --native --asan -o $asanExe 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "native --asan build failed"; exit 1 }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $asanExe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Environment["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEnd(); $err = $p.StandardError.ReadToEnd(); $p.WaitForExit()
Write-Host ("native --asan output: {0}" -f $out.Trim())
if ($p.ExitCode -ne 0 -or ($err -match "AddressSanitizer|LeakSanitizer")) {
    Write-Host $err; Write-Host "native --asan FAILED"; exit 1
}
Write-Host "native --asan: OK"
