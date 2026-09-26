# Differential harness: runs every example via the tree-walker and via the VM,
# comparing stdout + exit code. Detects hangs via a per-file watchdog.
param(
    [string]$Dir = "examples",
    [int]$TimeoutSec = 20,
    [string]$Pattern = "^[0-9]"
)

$ErrorActionPreference = "Continue"
$runner = Join-Path (Resolve-Path "$PSScriptRoot\..\build") "kirnrun.exe"
$files = Get-ChildItem (Join-Path (Resolve-Path "$PSScriptRoot\..") $Dir) -Filter *.kn -Name |
         Where-Object { $_ -match $Pattern }

$fails = 0
$matches_ = 0
$hangs = 0

function Run-One([string]$exe, [string]$file) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = $file
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardInput = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $p.StandardInput.Close()
    $so = $p.StandardOutput.ReadToEndAsync()
    $se = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $p.Kill()
        return @{ ok = $true; hang = $true; out = ""; code = -99 }
    }
    $o = $so.Result
    $err = $se.Result
    $code = $p.ExitCode
    if ($err) { $o = $o + "`n[stderr]`n" + $err }
    return @{ ok = $true; hang = $false; out = $o; code = $code }
}

foreach ($f in $files) {
    $fpath = Join-Path (Resolve-Path "$PSScriptRoot\..") (Join-Path $Dir $f)
    $a = Run-One $runner ("--no-vm " + $fpath)   # tree-walker reference
    $b = Run-One $runner $fpath                  # VM (default)
    if (-not $b.ok) { Write-Output "RUNERR $f"; $fails++; continue }
    if ($b.hang) { Write-Output "HANG   $f"; $hangs++; $fails++; continue }
    $sa = "$($a.code)::" + $a.out
    $sb = "$($b.code)::" + $b.out
    if ($sa -eq $sb) { Write-Output "MATCH  $f"; $matches_++ }
    else {
        Write-Output "DIFF   $f  (tree=$($a.code) vm=$($b.code))"
        if ($a.out -ne $b.out) {
            Write-Output "  --- tree stdout ---"; Write-Output $a.out
            Write-Output "  --- vm stdout   ---"; Write-Output $b.out
        }
        $fails++
    }
}
Write-Output ("RESULT: $matches_ matched, $fails failed, $hangs hung")
