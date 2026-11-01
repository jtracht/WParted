# SPDX-License-Identifier: GPL-2.0-or-later
# Builds a WinPE ISO that contains the portable WParted files (ADR-0003).
# Needs the Windows ADK with the WinPE add-on, run from the "Deployment and
# Imaging Tools Environment" as administrator. Writes only below -WorkDir; it
# never writes to a USB stick or a disk, and it ships no Microsoft files.
# Status: not run yet.
param(
    [Parameter(Mandatory = $true)][string]$WParted,   # folder with wparted.exe, wparted-cli.exe, wparted-helper.exe
    [Parameter(Mandatory = $true)][string]$WorkDir,   # new, empty folder
    [ValidateSet('amd64', 'arm64')][string]$Arch = 'amd64'
)
$ErrorActionPreference = 'Stop'
foreach ($tool in 'copype.cmd', 'MakeWinPEMedia.cmd', 'dism.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool was not found: start this from the ADK's Deployment and Imaging Tools Environment."
    }
}
foreach ($exe in 'wparted.exe', 'wparted-cli.exe', 'wparted-helper.exe') {
    if (-not (Test-Path (Join-Path $WParted $exe))) { throw "$exe is missing in $WParted" }
}
if (Test-Path $WorkDir) { throw "$WorkDir exists already; give a new folder." }

& copype.cmd $Arch $WorkDir
if ($LASTEXITCODE -ne 0) { throw 'copype failed' }
$mount = Join-Path $WorkDir 'mount'
& dism.exe /Mount-Image "/ImageFile:$(Join-Path $WorkDir 'media\sources\boot.wim')" /Index:1 "/MountDir:$mount"
if ($LASTEXITCODE -ne 0) { throw 'mounting boot.wim failed' }
$commit = '/Discard'
try {
    $target = Join-Path $mount 'WParted'
    New-Item -ItemType Directory -Path $target | Out-Null
    Copy-Item -Path (Join-Path $WParted '*') -Destination $target -Recurse
    $commit = '/Commit'
} finally {
    & dism.exe /Unmount-Image "/MountDir:$mount" $commit
}
$iso = Join-Path $WorkDir 'WParted-WinPE.iso'
& MakeWinPEMedia.cmd /ISO $WorkDir $iso
if ($LASTEXITCODE -ne 0) { throw 'MakeWinPEMedia failed' }
Write-Output "Done: $iso. In WinPE, start X:\WParted\wparted.exe."
