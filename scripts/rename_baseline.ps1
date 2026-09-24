# scripts/rename_baseline.ps1 - Phase 1 safety net: build + record harness transcripts.
#
# COCO_PLANS/COCO_TO_KIRN_PLAN.md Phase 1 ("Baseline snapshot & safety netting")
# requires a reproducible baseline we can roll back to before ANY rename work:
#   1. record git state
#   2. confirm the build (`cmake -S . -B build` + `cmake --build build --config Debug`)
#   3. run every verification harness on the CURRENT tool names and record
#      transcripts to _rename/baseline/
#   4. (dry-run inventory is produced by scripts/rename_dryrun.ps1)
#
# The harnesses are invoked with the CURRENT executables (kirn.exe, kirnrun.exe,
# kirncheck.exe) exactly as Phase 1 orders - nothing is renamed yet.
#
# Usage:
#     powershell -File scripts/rename_baseline.ps1                    # auto-detect build\*.exe
#     powershell -File scripts/rename_baseline.ps1 -Runner build\Debug\kirnrun.exe
#     powershell -File scripts/rename_baseline.ps1 -IncludeAsan       # also build+run build-asan/ corpus
#     powershell -File scripts/rename_baseline.ps1 -BuildOnly         # skip harnesses
#
# Exit code: 0 if every gate passed; 1 if any gate failed (or the build failed).

param(
    [string]$Root = (Get-Location),
    [string]$RuntimeExe = '',   # kirnrun.exe (default: build\ then build\Debug\)
    [string]$CheckExe   = '',   # kirncheck.exe (default: build\ then build\Debug\)
    [string]$DriverExe  = '',   # kirn.exe     (default: build\ then build\Debug\)
    [switch]$IncludeAsan,
    [switch]$BuildOnly,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path -LiteralPath $Root).Path
$OutDir = Join-Path $Root '_rename\baseline'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Pick-Exe {
    param([string]$Name, [string]$Override)
    if ($Override) {
        $Full = if ([IO.Path]::IsPathRooted($Override)) { $Override } else { Join-Path $Root $Override }
        if (-not (Test-Path -LiteralPath $Full)) { throw "executable not found: $Full" }
        return (Resolve-Path -LiteralPath $Full).Path
    }
    foreach ($cand in @("build\$Name.exe", "build\Debug\$Name.exe")) {
        $Full = Join-Path $Root $cand
        if (Test-Path -LiteralPath $Full) { return (Resolve-Path -LiteralPath $Full).Path }
    }
    throw "no built $Name executable found under build\; rebuild first"
}

$Runtime = Pick-Exe 'kirnrun' $RuntimeExe
$Check   = Pick-Exe 'kirncheck' $CheckExe
$Driver  = Pick-Exe 'kirn' $DriverExe

# Write LF-only UTF-8 (no BOM). Transcripts are tracked as LF (.gitattributes
# *.log/*.txt eol=lf) and core.autocrlf is off, so regenerating a baseline must
# not leave CRLF files behind (which git would flag as modified).
function Write-Lf {
    param([string]$Path, [string[]]$Lines, [switch]$Append)
    $text = ($Lines -join "`n") + "`n"
    $enc = [System.Text.UTF8Encoding]::new($false)
    if ($Append -and (Test-Path -LiteralPath $Path)) {
        [IO.File]::AppendAllText($Path, $text, $enc)
    } else {
        [IO.File]::WriteAllText($Path, $text, $enc)
    }
}

function Invoke-And-Tee {
    param([string]$Name, [string]$LogName, [string]$ScriptPath, [string[]]$ScriptArgs = @())
    $logPath = Join-Path $OutDir $LogName
    $errPath = Join-Path $OutDir ($LogName + '.err')
    $sw = [Diagnostics.Stopwatch]::StartNew()
    # Run OUT OF PROCESS: the harnesses print progress via Write-Host, which
    # Windows PowerShell 5.1 cannot capture in-process (no information stream).
    # A child powershell.exe with redirected stdout/stderr captures everything.
    $pwrsh = (Get-Command powershell.exe).Source
    $argsFlat = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath) + $ScriptArgs
    $psi = $null
    $exitCode = 255
    $capturedErr = @()
    try {
        $psi = Start-Process -FilePath $pwrsh -ArgumentList $argsFlat -WindowStyle Hidden -Wait -PassThru `
            -RedirectStandardOutput $logPath -RedirectStandardError $errPath
        $exitCode = $psi.ExitCode
    } catch {
        $capturedErr += "HARNESS THREW: $_"
        Write-Lf -Path $logPath -Lines $capturedErr -Append
    }
    $sw.Stop()
    # Normalize transcripts to LF so regenerated baselines never dirty the tree.
    foreach ($p in @($logPath, $errPath)) {
        if (Test-Path -LiteralPath $p) {
            $t = [IO.File]::ReadAllText($p)
            if ($t.Contains("`r`n")) {
                [IO.File]::WriteAllText($p, ($t -replace "`r`n", "`n"), [System.Text.UTF8Encoding]::new($false))
            }
        }
    }
    $output = @()
    if (Test-Path -LiteralPath $logPath) { $output += Get-Content -LiteralPath $logPath }
    if (Test-Path -LiteralPath $errPath) {
        $errContent = Get-Content -LiteralPath $errPath
        if ($errContent) { $output += '--- stderr ---'; $output += $errContent }
    }
    Write-Host ("{0,2}. {1,-14} exit={2,-3} {3,6} ms  -> {4}" -f $script:step++, $Name, $exitCode, $sw.ElapsedMilliseconds, $LogName)
    if (-not $Quiet -and $output) { $output | ForEach-Object { Write-Host ("      | " + $_) } }
    return [pscustomobject]@{ Name = $Name; Code = $exitCode; Log = $LogName; Ms = $sw.ElapsedMilliseconds; Output = ($output -join "`n") }
}

$script:step = 1
$gates = @()

# --- 1. metadata ---------------------------------------------------------------
$meta = @()
$meta += "baseline snapshot (Phase 1)"
$meta += "timestamp : " + (Get-Date).ToUniversalTime().ToString('o')
$meta += "root      : $Root"
$meta += "version   : " + ((Get-Content -LiteralPath (Join-Path $Root '.version') -ErrorAction SilentlyContinue) -join '')
$meta += "git head  : " + ((git -C $Root rev-parse HEAD) 2>$null)
$meta += "git branch: " + ((git -C $Root branch --show-current) 2>$null)
$meta += "git desc  : " + ((git -C $Root describe --tags) 2>$null)
$meta += "git status:"
$meta += ((git -C $Root status --short) 2>$null)
$meta += "remote    : " + ((git -C $Root remote get-url origin) 2>$null)
$meta += "cmake     : " + (((cmake --version) 2>$null | Select-Object -First 1))
$meta += "runtime   : $Runtime"
$meta += "check     : $Check"
$meta += "driver    : $Driver"
$meta += "cl        : " + (((where.exe cl 2>$null) | Select-Object -First 1))
$meta | Write-Lf (Join-Path $OutDir '00-metadata.txt')

# --- 2. build ------------------------------------------------------------------
Write-Output '==> Building (cmake -S . -B build; cmake --build build --config Debug)'
$buildLog = Join-Path $OutDir '01-build.log'
$buildOut = @('--- cmake -S . -B build ---')
$buildOut += (& cmake -S $Root -B (Join-Path $Root 'build') 2>&1 | ForEach-Object { "$_" })
$buildOut += '--- cmake --build build --config Debug ---'
$buildOut += (& cmake --build (Join-Path $Root 'build') --config Debug 2>&1 | ForEach-Object { "$_" })
$buildCode = $LASTEXITCODE
$buildOut | Write-Lf $buildLog
Write-Output ("   build exit={0}  -> 01-build.log" -f $buildCode)
if ($buildCode -ne 0) {
    Write-Output 'baseline build FAILED; recording transcript and exiting 1'
    exit 1
}

if ($BuildOnly) {
    Write-Output '==> -BuildOnly: skipping harnesses'
    exit 0
}

# --- 3. harness transcripts (currently-named tools) -----------------------------
$gates += Invoke-And-Tee 'runall'   '02-runall.log'   (Join-Path $Root 'scripts\runall.ps1')   @('-Runner', $Runtime)
$gates += Invoke-And-Tee 'types'    '03-types.log'    (Join-Path $Root 'scripts\types.ps1')    @('-Check', $Check, '-Run', $Runtime)
$gates += Invoke-And-Tee 'negative' '04-negative.log' (Join-Path $Root 'scripts\negative.ps1') @('-Runner', $Check)
$gates += Invoke-And-Tee 'vm_diff'  '05-vm_diff.log'  (Join-Path $Root 'scripts\vm_diff.ps1')
$gates += Invoke-And-Tee 'conventions' '06-conventions.log' (Join-Path $Root 'tests\conventions\run.ps1')

if ($IncludeAsan) {
    $gates += Invoke-And-Tee 'asanall' '07-asanall.log' (Join-Path $Root 'scripts\asanall.ps1')
}

# --- 4. summary -----------------------------------------------------------------
$bad = @()
$lines = @('baseline harness summary')
foreach ($g in $gates) {
    $ok = $true
    if ($g.Name -eq 'vm_diff') {
        $m = [regex]::Match($g.Output, 'RESULT:\s*([0-9]+) matched,\s*([0-9]+) failed,\s*([0-9]+) hung')
        $fail = if ($m.Success) { [int]$m.Groups[2].Value + [int]$m.Groups[3].Value } else { 1 }
        $ok = ($fail -eq 0)
    } elseif ($g.Code -ne 0) {
        $ok = $false
    }
    $status = if ($ok) { 'PASS' } else { 'FAIL' }
    if (-not $ok) { $bad += $g.Name }
    $lines += ("{0,-14} {1,-4} exit={2,-3}  {3} ms  {4}" -f $g.Name, $status, $g.Code, $g.Ms, $g.Log)
}
$lines += '---'
$lines += ("baseline {0}: {1}/{2} gates passed" -f $(if ($bad) { 'FAILED' } else { 'OK' }), ($gates.Count - $bad.Count), $gates.Count)
$lines | Write-Lf (Join-Path $OutDir 'SUMMARY.txt')
Write-Output '==> baseline summary:'
$lines | ForEach-Object { Write-Output ("   " + $_) }
Write-Output "==> transcripts under $OutDir"
if ($bad) { exit 1 }
exit 0