# Capture any application's window to a PNG, by process name.
#
# Generalised from shot.ps1 (which is hardcoded to the game) so that any
# window state can be inspected directly instead of described from memory.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\winshot.ps1 <process> <out.png>
#
# No param() block on purpose: with -File, a declared parameter steals
# position 0 and swallows the arguments (same reason swse.ps1 avoids one).

$proc = if ($args.Count -ge 1) { $args[0] } else { "stranger" }
$out  = if ($args.Count -ge 2) { $args[1] } else { "winshot.png" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WS {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    public struct R { public int L, T, Rt, B; }
}
"@

# Electron apps (Discord among them) run several processes; only one owns a
# visible main window, so filter for it rather than taking the first process.
$p = Get-Process -Name $proc -ErrorAction SilentlyContinue |
     Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no '$proc' process with a window"; exit 1 }

[void][WS]::ShowWindow($p.MainWindowHandle, 9)   # SW_RESTORE, in case minimised
[void][WS]::SetForegroundWindow($p.MainWindowHandle)
Start-Sleep -Milliseconds 800

$r = New-Object WS+R
[void][WS]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$w = $r.Rt - $r.L
$h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { Write-Output "bad window rect"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $h))
$g.Dispose()
if (-not [System.IO.Path]::IsPathRooted($out)) { $out = Join-Path (Get-Location) $out }
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "wrote $out  (${w}x${h})"
