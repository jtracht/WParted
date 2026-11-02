# SPDX-License-Identifier: GPL-2.0-or-later
# Takes the screenshots of the user guides from a synthetic disk image, so they
# show no real disk, serial number or user name. Run: & tools\verify\make_screenshots.ps1
param([string]$Build = "$PSScriptRoot\..\..\build\x64-release")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Build = (Resolve-Path $Build).Path
$Out = Join-Path $root 'docs\images'
New-Item -ItemType Directory -Force $Out | Out-Null
# A neutral folder: the details pane shows the image's full path in the picture.
$work = Join-Path $env:SystemDrive 'WParted-Beispiel'
New-Item -ItemType Directory -Force $work | Out-Null
$img = Join-Path $work 'beispiel.img'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class Sh {
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
 public delegate bool EnumProc(IntPtr h, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
 public struct RECT { public int L,T,R,B; }
 public static IntPtr Child(IntPtr parent, string cls) { IntPtr found = IntPtr.Zero;
   EnumChildWindows(parent, (h, l) => { var sb = new StringBuilder(64); GetClassName(h, sb, 64);
     if (sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero); return found; }
 public static IntPtr Top(uint pid, string cls) { IntPtr found = IntPtr.Zero;
   EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
     if (p == pid && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero); return found; }
}
"@
[Sh]::SetProcessDPIAware() | Out-Null
function Shot([IntPtr]$h, [string]$name) {
  $r = New-Object Sh+RECT; [Sh]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
  [Sh]::PrintWindow($h, $hdc, 2) | Out-Null
  $g.ReleaseHdc($hdc); $bmp.Save((Join-Path $Out "$name.png")); $g.Dispose(); $bmp.Dispose()
}
$CMD = 0x111; $KEYDOWN = 0x100
foreach ($lang in 'en', 'de') {
  & "$Build\tests\wparted_make_test_image.exe" $img
  $env:WPARTED_LANG = $lang
  $p = Start-Process "$Build\apps\wparted-gui\wparted.exe" -ArgumentList "`"$img`"" -PassThru
  $main = [IntPtr]::Zero
  for ($i = 0; $i -lt 60 -and $main -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $main = [Sh]::Top([uint32]$p.Id, 'WPartedMainWindow') }
  if ($main -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id -Force; throw 'no main window' }
  [Sh]::SetWindowPos($main, [IntPtr]::Zero, 40, 40, 1180, 760, 0x14) | Out-Null
  Start-Sleep -Seconds 9   # discovery, then the image is opened
  [Sh]::PostMessage($main, $CMD, [IntPtr]1011, [IntPtr]0) | Out-Null   # light theme
  Start-Sleep -Milliseconds 700
  Shot $main "main-$lang"
  $map = [Sh]::Child($main, 'WPartedDiskMap')
  [Sh]::PostMessage($map, $KEYDOWN, [IntPtr]0x23, [IntPtr]0) | Out-Null   # End: last partition
  Start-Sleep -Milliseconds 400
  [Sh]::PostMessage($main, $CMD, [IntPtr]1018, [IntPtr]0) | Out-Null      # Delete: one planned operation
  Start-Sleep -Milliseconds 700
  Shot $main "planned-$lang"
  [Sh]::PostMessage($main, $CMD, [IntPtr]1006, [IntPtr]0) | Out-Null      # Apply: the summary, nothing is confirmed
  Start-Sleep -Milliseconds 1000
  $td = [Sh]::Top([uint32]$p.Id, '#32770')
  if ($td -ne [IntPtr]::Zero) {
    Shot $td "apply-$lang"
    [Sh]::PostMessage($td, 0x466, [IntPtr]8, [IntPtr]0) | Out-Null        # Close
    Start-Sleep -Milliseconds 500
  }
  [Sh]::PostMessage($main, $CMD, [IntPtr]1012, [IntPtr]0) | Out-Null      # dark theme
  Start-Sleep -Milliseconds 700
  Shot $main "dark-$lang"
  [Sh]::PostMessage($main, $CMD, [IntPtr]1010, [IntPtr]0) | Out-Null      # back to the system theme
  Start-Sleep -Milliseconds 300
  Stop-Process -Id $p.Id -Force
  Start-Sleep -Milliseconds 500
}
Remove-Item -Recurse -Force $work
Remove-Item Env:\WPARTED_LANG
Get-ChildItem $Out | Select-Object Name, Length
