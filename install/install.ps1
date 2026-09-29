# Kirn installer for Windows, in the style of rustup-init:
#
#   irm https://github.com/rkriad585/kirn/releases/latest/download/install.ps1 | iex
#
# Detects the platform, downloads the matching release archive, verifies its
# SHA-256 against the release checksum file, and installs into $HOME\.kirn.
[CmdletBinding()]
param(
  [string] $Prefix = $(if ($env:KIRN_PREFIX) { $env:KIRN_PREFIX } else { Join-Path $HOME ".kirn" }),
  [string] $Version = "",
  [switch] $NoModifyPath,
  [switch] $Uninstall
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Repo = "rkriad585/kirn"

function Get-KirnTarget {
  # Windows on ARM64 hosts x64 binaries natively, so an ARM64 machine can use
  # the amd64 build. We only ever ship one Windows target: amd64.
  return "windows-amd64"
}

$target = Get-KirnTarget

function Remove-Existing {
  if (Test-Path -LiteralPath $Prefix) {
    Remove-Item -LiteralPath $Prefix -Recurse -Force
    Write-Host "removed $Prefix"
  }
  else {
    Write-Host "nothing installed at $Prefix"
  }
}

if ($Uninstall) { Remove-Existing; exit 0 }

$archive = "kirn-$target.zip"

if ($Version) {
  $tag = $Version
  $base = "https://github.com/$Repo/releases/download/$tag"
}
else {
  $tag = "latest"
  $base = "https://github.com/$Repo/releases/latest/download"
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("kirn-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

try {
  Write-Host "Kirn installer: $target ($tag)"

  Write-Host "  downloading $archive"
  $archivePath = Join-Path $tmp $archive
  try {
    Invoke-WebRequest -Uri "$base/$archive" -OutFile $archivePath
  }
  catch {
    throw "download failed: $base/$archive`n$($_.Exception.Message)"
  }

  # Verify integrity when the release publishes checksums.txt. A missing file is
  # not fatal: older releases may not have one.
  $sumsPath = Join-Path $tmp "checksums.txt"
  try {
    Invoke-WebRequest -Uri "$base/checksums.txt" -OutFile $sumsPath
    $line = Get-Content -LiteralPath $sumsPath |
      Where-Object { $_ -match "\s+$([regex]::Escape($archive))\s*$" } |
      Select-Object -First 1
    if ($line) {
      $expected = ($line -split '\s+')[0].ToLower()
      $actual = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLower()
      if ($expected -ne $actual) {
        throw "checksum mismatch for $archive`n  expected $expected`n  actual   $actual"
      }
      Write-Host "  checksum ok"
    }
    else {
      Write-Warning "no checksum entry for $archive, skipping verification"
    }
  }
  catch {
    if ($_.Exception.Message -like "checksum mismatch*") { throw }
    Write-Warning "checksums.txt not published, skipping verification"
  }

  Write-Host "  extracting to $Prefix"
  if (Test-Path -LiteralPath $Prefix) { Remove-Item -LiteralPath $Prefix -Recurse -Force }
  Expand-Archive -LiteralPath $archivePath -DestinationPath $Prefix -Force

  $exe = Join-Path $Prefix "bin\kirn.exe"
  if (-not (Test-Path -LiteralPath $exe)) {
    throw "archive did not contain bin\kirn.exe at $Prefix"
  }

  if (-not $NoModifyPath) {
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $bindir = Join-Path $Prefix "bin"
    if (-not $userPath) { $userPath = "" }
    $entries = $userPath -split ';' | Where-Object { $_ }
    if (-not ($entries | Where-Object { $_.TrimEnd('\') -eq $bindir.TrimEnd('\') })) {
      $newPath = (@($entries) + $bindir) -join ';'
      [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
      Write-Host "  added $bindir to your user PATH (restart the shell to pick it up)"
    }
    else {
      Write-Host "  $bindir already on your user PATH"
    }
  }

  $bindir = Join-Path $Prefix "bin"
  Write-Host ""
  Write-Host "Kirn installed to $Prefix"
  Write-Host ""
  Write-Host "  Add to PATH for this session:  `$env:Path = `"$bindir;`$env:Path`""
  Write-Host "  Point at the standard library:  `$env:KIRN_STDLIB = `"$(Join-Path $Prefix 'stdlib')`""
  Write-Host ""
  Write-Host "Try it:"
  Write-Host "  kirn run https://github.com/$Repo/raw/main/examples/01_hello.kn"
}
finally {
  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
