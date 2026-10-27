# SPDX-License-Identifier: GPL-2.0-or-later
# Packages a build directory into a portable ZIP plus SHA-256 checksum.
#   tools/package-portable.ps1 -BuildDir build/x64-release -Arch x64 [-OutDir dist]
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$BuildDir,
    [Parameter(Mandatory)] [ValidateSet('x64', 'x86', 'arm64')] [string]$Arch,
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$versionHeader = Get-Content (Join-Path $BuildDir 'apps/wparted-gui/wparted_version.h') -Raw
if ($versionHeader -notmatch 'WPARTED_VERSION_STRING "([^"]+)"') { throw 'version not found' }
$version = $Matches[1]
# File names state the tested compatibility range (see docs/compatibility).
$platform = if ($Arch -eq 'x86') { 'win7plus-x86' } elseif ($Arch -eq 'arm64') { 'win11-arm64' } else { 'win7plus-x64' }
$name = "WParted-v$version-$platform-portable"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

$files = @(
    @{ src = 'apps/wparted-gui/wparted.exe'; dst = 'wparted.exe' },
    @{ src = 'apps/wparted-cli/wparted-cli.exe'; dst = 'wparted-cli.exe' }
)
foreach ($f in $files) {
    $p = Join-Path $BuildDir $f.src
    if (-not (Test-Path $p)) { throw "missing build output: $p" }
    Copy-Item $p (Join-Path $stage $f.dst)
}
foreach ($doc in 'LICENSE', 'README.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', 'SECURITY.md') {
    Copy-Item (Join-Path $root $doc) $stage
}
# Smoke test: the packaged CLI must start and report the expected version.
$reported = & (Join-Path $stage 'wparted-cli.exe') version
if ($LASTEXITCODE -ne 0 -or $reported -ne $version) { throw "packaged CLI smoke test failed: '$reported'" }

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"$hash  $name.zip" | Out-File -Encoding ascii -Append (Join-Path $OutDir "WParted-v$version-checksums.sha256")
Write-Host "Packaged $zip ($hash)"
