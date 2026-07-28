# Capture the game window to a PNG.
#
# Verifying a visual change by asking the user to look is not verification.
# This grabs the window so a change can be inspected directly.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\shot.ps1 out.png
#
# No param() block on purpose: with -File, a declared parameter steals
# position 0 and swallows the path (same reason swse.ps1 avoids one).

$out = if ($args.Count -ge 1) { $args[0] } else { "shot.png" }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern IntPtr FindWindowA(string c, string n);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct R { public int L, T, Rt, B; }
    public struct P { public int X, Y; }
}
"@

$proc = Get-Process stranger -ErrorAction SilentlyContinue
if (-not $proc) { Write-Output "game not running"; exit 1 }
$h = $proc.MainWindowHandle
if ($h -eq 0) { Write-Output "no main window"; exit 1 }

# The game must be frontmost or the capture reads whatever occludes it.
[void][W]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 700

$r = New-Object W+R
[void][W]::GetClientRect($h, [ref]$r)
$p = New-Object W+P
[void][W]::ClientToScreen($h, [ref]$p)
$w = $r.Rt - $r.L
$ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Output "bad client rect"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($p.X, $p.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$g.Dispose()

if (-not [System.IO.Path]::IsPathRooted($out)) {
    $out = Join-Path (Get-Location) $out
}
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "wrote $out  (${w}x${ht})"
