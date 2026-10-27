# SPDX-License-Identifier: GPL-2.0-or-later
# Builds WParted with MSVC + Ninja and runs the tests.
#   pwsh tools/build.ps1 [-Arch x64|x86|arm64] [-Config Release|Debug] [-NoTests] [-Package]
# Works in Windows PowerShell 5.1 and PowerShell 7. Used by CI and locally.
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')] [string]$Arch = 'x64',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string]$Config = 'Release',
    [switch]$NoTests,
    [switch]$Package,
    [string[]]$CMakeArgs = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build/$Arch-$($Config.ToLower())"

function Import-VsEnvironment([string]$arch) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found - install Visual Studio 2022 Build Tools (C++ workload).' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the MSVC x86/x64 toolset found.' }
    $hostArch = if ([Environment]::Is64BitOperatingSystem) { 'amd64' } else { 'x86' }
    $target = @{ x64 = 'amd64'; x86 = 'x86'; arm64 = 'arm64' }[$arch]
    $vcvarsArg = if ($hostArch -eq $target) { $target } else { "${hostArch}_$target" }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
    $envDump = cmd /c "`"$vcvars`" $vcvarsArg >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall.bat $vcvarsArg failed" }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
}

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # pip install --user cmake ninja puts them here
    $userScripts = & python -c "import sysconfig;print(sysconfig.get_path('scripts','nt_user'))" 2>$null
    if ($userScripts -and (Test-Path (Join-Path $userScripts "$name.exe"))) { return (Join-Path $userScripts "$name.exe") }
    throw "$name not found. Install it (e.g. 'python -m pip install --user cmake ninja')."
}

Import-VsEnvironment $Arch
$cmake = Find-Tool 'cmake'
$ninja = Find-Tool 'ninja'
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

& $cmake -S $root -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_BUILD_TYPE=$Config" @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$canRun = ($Arch -ne 'arm64') -or ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64')
if (-not $NoTests -and $canRun) {
    & $ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
} elseif (-not $NoTests) {
    Write-Warning "Tests skipped: $Arch binaries cannot run on this host."
}

if ($Package) {
    & python (Join-Path $PSScriptRoot 'package.py') --build-dir $buildDir --arch $Arch
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed' }
}
Write-Host "Build OK: $buildDir"
