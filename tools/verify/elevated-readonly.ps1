# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Manual verification on real hardware (WP-012, WP-014, WP-071): runs WParted's
# read-only commands from an elevated prompt and stores their output. Nothing is
# written to any disk - WParted's builds contain no device write path (CI
# read-only guard), and this script only reads. The Windows view of disks,
# partitions and volumes is recorded before and after, so that "unchanged" can
# be shown. BitLocker: `manage-bde -status` (read-only) for comparison with the
# state WParted reports.
#
# Usage (administrator PowerShell):
#   powershell -ExecutionPolicy Bypass -File tools\verify\elevated-readonly.ps1 [-Cli <wparted-cli.exe>] [-Out <folder>]
#Requires -RunAsAdministrator
param(
    [string]$Cli = "$PSScriptRoot\..\..\build\x64-release\apps\wparted-cli\wparted-cli.exe",
    [string]$Out = "$PSScriptRoot\..\..\build\elevated-check"
)
$ErrorActionPreference = 'Stop'
$Cli = (Resolve-Path $Cli).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

function Snapshot([string]$name) {
    $disks = Get-Disk | Select-Object Number, FriendlyName, PartitionStyle, Size, OperationalStatus, IsReadOnly
    $parts = Get-Partition | Select-Object DiskNumber, PartitionNumber, Offset, Size, Type, GptType, DriveLetter
    $vols = Get-Volume | Select-Object UniqueId, DriveLetter, FileSystem, FileSystemLabel, Size
    @{ disks = $disks; partitions = $parts; volumes = $vols } | ConvertTo-Json -Depth 4 |
        Out-File -Encoding utf8 (Join-Path $Out "$name.json")
}

"WParted elevated read-only check, $(Get-Date -Format o)" | Out-File -Encoding utf8 (Join-Path $Out 'summary.txt')
"CLI: $Cli" | Out-File -Append -Encoding utf8 (Join-Path $Out 'summary.txt')
Snapshot 'windows-before'
$t0 = Get-Date
& $Cli version 2>&1 | Out-File -Encoding utf8 (Join-Path $Out 'version.txt')
& $Cli list 2>&1 | Out-File -Encoding utf8 (Join-Path $Out 'list.txt')
& $Cli list --json 2>&1 | Out-File -Encoding utf8 (Join-Path $Out 'list.json')
$report = Get-Content -Raw (Join-Path $Out 'list.json') | ConvertFrom-Json
foreach ($d in $report.disks) {
    & $Cli rescue-scan $d.number 2>&1 | Out-File -Encoding utf8 (Join-Path $Out "rescue-disk$($d.number).txt")
}
$seconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
try { manage-bde -status 2>&1 | Out-File -Encoding utf8 (Join-Path $Out 'manage-bde-status.txt') } catch { "manage-bde not available: $_" | Out-File -Encoding utf8 (Join-Path $Out 'manage-bde-status.txt') }
Snapshot 'windows-after'
$same = (Get-Content -Raw (Join-Path $Out 'windows-before.json')) -eq (Get-Content -Raw (Join-Path $Out 'windows-after.json'))
"elevated: $($report.elevated); disks: $($report.disks.Count); WParted commands took $seconds s" |
    Out-File -Append -Encoding utf8 (Join-Path $Out 'summary.txt')
"Windows view of disks, partitions and volumes unchanged: $same" | Out-File -Append -Encoding utf8 (Join-Path $Out 'summary.txt')
Get-Content (Join-Path $Out 'summary.txt')
"Results in $Out"
