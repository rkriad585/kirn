# Official root installer for Windows PowerShell.
[CmdletBinding()]
param([string]$Prefix=$(if ($env:KIRN_PREFIX) {$env:KIRN_PREFIX} else {Join-Path $HOME ".kirn"}), [string]$Version="", [switch]$NoModifyPath, [switch]$Uninstall)
$ErrorActionPreference="Stop"; $Repo="rkriad585/kirn"; $target="windows-amd64"
if ($Uninstall) { if (Test-Path $Prefix) { Remove-Item $Prefix -Recurse -Force }; exit 0 }
$tag=if ($Version) {$Version} else {"latest"}; $base=if ($Version) {"https://github.com/$Repo/releases/download/$tag"} else {"https://github.com/$Repo/releases/latest/download"}
$tmp=Join-Path ([IO.Path]::GetTempPath()) ("kirn-"+[Guid]::NewGuid().ToString("N")); New-Item -ItemType Directory $tmp -Force | Out-Null
try {
  $archive="kirn-$target.zip"; $archivePath=Join-Path $tmp $archive
  Invoke-WebRequest "$base/$archive" -OutFile $archivePath
  $sums=Join-Path $tmp "checksums.txt"
  try { Invoke-WebRequest "$base/checksums.txt" -OutFile $sums; $line=Get-Content $sums | Where-Object {$_ -match "\s+$([regex]::Escape($archive))\s*$"} | Select-Object -First 1; if ($line) { $want=($line -split '\s+')[0].ToLower(); $got=(Get-FileHash $archivePath -Algorithm SHA256).Hash.ToLower(); if ($want -ne $got) {throw "checksum mismatch"} } } catch { if ($_.Exception.Message -eq "checksum mismatch") {throw} }
  if (Test-Path $Prefix) {Remove-Item $Prefix -Recurse -Force}; Expand-Archive $archivePath $Prefix -Force
  if (-not (Test-Path (Join-Path $Prefix "bin\kirn.exe"))) {throw "archive did not contain bin\kirn.exe"}
  if (-not (Test-Path (Join-Path $Prefix "bin\kirnc.exe"))) {throw "archive did not contain bin\kirnc.exe"}
  if (-not $NoModifyPath) { $p=[Environment]::GetEnvironmentVariable("Path","User"); $b=Join-Path $Prefix "bin"; if ($p -notlike "*$b*") {[Environment]::SetEnvironmentVariable("Path", (($p,$b) -ne "") -join ';',"User")} }
  Write-Host "Kirn installed to $Prefix ($tag)"
} finally {Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue}
