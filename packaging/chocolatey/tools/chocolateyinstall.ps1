$ErrorActionPreference = 'Stop'
$version = '0.0.1-b3'
$url = "https://github.com/rkriad585/kirn/releases/download/v$version/kirn-windows-amd64.zip"
$tools = Split-Path -Parent $MyInvocation.MyCommand.Definition

Install-ChocolateyZipPackage `
  -PackageName 'kirn' `
  -Url64bit $url `
  -UnzipLocation $tools

$archiveRoot = Join-Path $tools 'kirn-windows-amd64'
$bin = Join-Path $archiveRoot 'bin'
if (-not (Test-Path (Join-Path $bin 'kirn.exe'))) { throw "Kirn archive did not contain bin\kirn.exe" }
if (-not (Test-Path (Join-Path $bin 'kirnc.exe'))) { throw "Kirn archive did not contain bin\kirnc.exe" }

Install-BinFile -Name 'kirn' -Path (Join-Path $bin 'kirn.exe')
Install-BinFile -Name 'kirnc' -Path (Join-Path $bin 'kirnc.exe')
