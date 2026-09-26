# CI corpus runner: executes every examples/*.kn through kirnrun with a
# per-example timeout. Prints a table (failures only when -Quiet), and
# EXITS NONZERO if any example fails or hangs.
#
#   scripts/runall.ps1 [-Runner build\kirnrun.exe] [-Dir examples] [-Quiet]
param(
    [string]$Runner = "",
    [string]$Dir = "examples",
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Runner) {
    $cfg = if (Test-Path "$root\build\kirnrun.exe") { "build" }
           elseif (Test-Path "$root\build\Debug\kirnrun.exe") { "build\Debug" }
           else { Write-Error "kirnrun.exe not found; pass -Runner" }
    $Runner = "$root\$cfg\kirnrun.exe"
}
if (-not (Test-Path $Runner)) { Write-Error "runner not found: $Runner" }
$dirPath = if ([IO.Path]::IsPathRooted($Dir) -and (Test-Path $Dir)) { $Dir }
           else { Join-Path $root $Dir }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kirn-ci-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$failures = @()
$rows = @()
foreach ($f in (Get-ChildItem $dirPath -Filter *.kn | Sort-Object Name)) {
    # raw .NET instead of Start-Process: ExitCode stays null there when
    # streams are redirected
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Runner
    $psi.Arguments = '"' + $f.FullName + '"'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $soT = $p.StandardOutput.ReadToEndAsync()
    $seT = $p.StandardError.ReadToEndAsync()
    $code = $null; $so = ""; $se = ""
    if (-not $p.WaitForExit(20000)) {
        try { $p.Kill() } catch {}
        $code = "HANG"
    } else {
        $so = ($soT.GetAwaiter().GetResult() -replace "`r`n", " ").Trim()
        $se = ($seT.GetAwaiter().GetResult() -replace "`r`n", " ").Trim()
        $code = $p.ExitCode
    }
    $p.Dispose()
    # an `# expect-exit: N` comment marks examples that intentionally return a
    # nonzero process exit code (e.g. a native `main() -> int` returning N).
    $expectExit = $null
    $expectRow = Select-String -LiteralPath $f.FullName -Pattern '#\s*expect-exit:\s*(\d+)' | Select-Object -First 1
    if ($expectRow) { $expectExit = [int]$expectRow.Matches[0].Groups[1].Value }
    $ok = if ($null -ne $expectExit) { $code -eq $expectExit } else { $code -eq 0 }
    $rows += [pscustomobject]@{ File = $f.Name; Code = $code; Stdout = $so; Stderr = $se }
    if (-not $ok) { $failures += $f.Name }

}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

$bad = $rows | Where-Object { $_.Code -ne 0 }
if ($bad) {
    $bad | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Host
}
if (-not $Quiet) {
    $rows | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Host
}
Write-Host ("{0}/{1} examples passed" -f ($rows.Count - $failures.Count), $rows.Count)
if ($failures.Count -gt 0) {
    Write-Host ("FAILED: " + ($failures -join ", "))
    exit 1
}
exit 0
