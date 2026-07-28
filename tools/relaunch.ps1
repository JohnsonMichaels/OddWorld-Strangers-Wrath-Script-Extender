# Restart Stranger's Wrath with the current SWSE build and load the last save.
#
# The game MUST be started through Launcher.exe - running bin\stranger.exe
# directly produces a frozen window. The launcher then waits on its PLAY
# button, so that button is found by class/text and clicked.
#
# There is no in-game load command, so reloading a save means restarting and
# using the console's `continue` (which drives the main menu).
#
# USAGE
#   powershell -File tools/relaunch.ps1              # restart + continue
#   powershell -File tools/relaunch.ps1 -NoInstall   # skip copying the DLL
#   powershell -File tools/relaunch.ps1 -NoContinue  # leave it at the menu
$Install  = -not ($args -contains '-NoInstall')
$Continue = -not ($args -contains '-NoContinue')

$game = "C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath"
$here = Split-Path -Parent $PSScriptRoot

Get-Process -Name 'stranger','Launcher' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1500

if ($Install) {
    $src = Join-Path $here 'swse\dinput8.dll'
    if (Test-Path $src) {
        Copy-Item $src (Join-Path $game 'bin\dinput8.dll') -Force
        Write-Host "installed dinput8.dll ($((Get-Item $src).LastWriteTime))"
    } else {
        Write-Error "BUILD NOT FOUND: $src - refusing to launch a stale DLL"; exit 1
    }
}

Start-Process -FilePath (Join-Path $game 'Launcher.exe') -WorkingDirectory $game
Start-Sleep -Seconds 5

Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class LW {
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Auto)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Auto)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
}
"@ -ErrorAction SilentlyContinue

$launcher = Get-Process -Name 'Launcher' -ErrorAction SilentlyContinue
if ($launcher) {
    $script:play = [IntPtr]::Zero
    $cb = [LW+EnumProc]{ param($h,$p)
        $sb = New-Object Text.StringBuilder 256; [LW]::GetWindowText($h,$sb,256) | Out-Null
        $cn = New-Object Text.StringBuilder 256; [LW]::GetClassName($h,$cn,256) | Out-Null
        if ($cn.ToString() -eq 'Button' -and $sb.ToString() -match 'PLAY') { $script:play = $h }
        return $true }
    [LW]::EnumChildWindows($launcher.MainWindowHandle, $cb, [IntPtr]::Zero) | Out-Null
    if ($script:play -ne [IntPtr]::Zero) {
        [LW]::SendMessage($script:play, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        Write-Host "clicked PLAY"
    } else {
        Write-Host "PLAY button not found - click it manually"
    }
}

# wait for the game process, then for SWSE's console mailbox to answer
$deadline = (Get-Date).AddSeconds(90)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if (Get-Process -Name 'stranger' -ErrorAction SilentlyContinue) { break }
}
if (-not (Get-Process -Name 'stranger' -ErrorAction SilentlyContinue)) {
    Write-Error "game did not start"; exit 1
}
Write-Host "game running"
Start-Sleep -Seconds 10

if ($Continue) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'swse.ps1') "continue"
    Write-Host "loading save..."
    Start-Sleep -Seconds 30
}
Write-Host "ready"

