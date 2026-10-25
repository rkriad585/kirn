# TSan coverage harness: builds the whole Kirn runtime/toolchain with
# ThreadSanitizer into build-tsan/, then runs every examples/*.kn through
# the TSan-instrumented kirnrun. Data races surface as nonzero exit (with
# a TSan report) and are reported.
#
#   scripts/tsanall.ps1 [-Quiet] [-Runner build-tsan\kirnrun.exe]
param([string]$Runner = "", [switch]$Quiet)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$bdir = "$root\build-tsan"
if (-not (Test-Path "$bdir\CMakeCache.txt")) {
    & cmake -S $root -B $bdir -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKIRN_TSAN=ON
    if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed" }
}
& cmake --build $bdir --config Debug
if ($LASTEXITCODE -ne 0) { Write-Error "cmake build failed" }

if (-not $Runner) { $Runner = "$bdir\kirnrun.exe" }
if (-not (Test-Path $Runner)) { Write-Error "runner not found: $Runner" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kirn-tsan-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$failures = @()
foreach ($f in (Get-ChildItem "$root\examples" -Filter *.kn | Sort-Object Name)) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Runner
    $psi.Arguments = '"' + $f.FullName + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Environment["TSAN_OPTIONS"] = "second_deadlock_stack=1:halt_on_error=1"
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEnd()
    $se = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $tsanHit = $se -match "ThreadSanitizer|data race|lock-order-inversion"
    if (-not $Quiet) {
        Write-Host ("{0,-28} code={1,-4} tsan={2}" -f $f.Name, $p.ExitCode, ($tsanHit ? "YES" : "no"))
    }
    if ($p.ExitCode -ne 0 -or $tsanHit) {
        $failures += $f.Name
        if ($se) { Write-Host "--- stderr for $($f.Name) ---"; Write-Host $se }
    }
}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

$count = (Get-ChildItem "$root\examples" -Filter *.kn | Measure-Object).Count
Write-Host ""
if ($failures.Count -eq 0) { Write-Host "TSan corpus: ALL PASS ($count examples)" }
else {
    Write-Host "TSan failures: $($failures -join ', ')"
    exit 1
}
