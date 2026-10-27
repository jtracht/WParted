# SPDX-License-Identifier: GPL-2.0-or-later
# Authenticode-signs the executables inside the portable packages when a
# certificate is provided via PFX_B64 / PFX_PASS. Without a certificate it
# writes dist/UNSIGNED.txt and exits successfully - it never pretends to sign.
[CmdletBinding()]
param([Parameter(Mandatory)] [string]$DistDir)
$ErrorActionPreference = 'Stop'
if (-not $env:PFX_B64) {
    'This release is NOT code-signed (no signing certificate configured). Verify the SHA-256 checksums.' |
        Out-File -Encoding utf8 (Join-Path $DistDir 'UNSIGNED.txt')
    Write-Warning 'No signing certificate configured - release is unsigned.'
    exit 0
}
$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" | Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) { throw 'signtool.exe not found' }
$pfx = Join-Path $env:RUNNER_TEMP 'signing.pfx'
[IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:PFX_B64))
try {
    foreach ($zip in Get-ChildItem $DistDir -Filter '*-portable.zip') {
        $dir = Join-Path $env:RUNNER_TEMP $zip.BaseName
        Expand-Archive $zip.FullName $dir -Force
        $exes = Get-ChildItem $dir -Filter *.exe | ForEach-Object FullName
        & $signtool.FullName sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com /f $pfx /p $env:PFX_PASS @exes
        if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
        & $signtool.FullName verify /pa @exes
        if ($LASTEXITCODE -ne 0) { throw 'signature verification failed' }
        Remove-Item $zip.FullName
        Compress-Archive -Path (Join-Path $dir '*') -DestinationPath $zip.FullName
    }
} finally {
    Remove-Item $pfx -Force -ErrorAction SilentlyContinue
}
