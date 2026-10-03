@echo off
REM ================================================================
REM  SWSE installer helper  -  by Johnson Michaels
REM  OPTIONAL since SWSE 1.0.1: SWSE finds the Windows dinput8.dll by
REM  itself. Run this AFTER copying the "bin" and "SWSEMods" folders into
REM  your game folder if you want bin\dinput8_real.dll created anyway.
REM ================================================================
setlocal
cd /d "%~dp0"

if not exist "bin\dinput8.dll" (
  echo.
  echo  ERROR: bin\dinput8.dll not found next to this script.
  echo  Make sure you copied the "bin" folder into your GAME FOLDER,
  echo  and that this .bat is in the same GAME FOLDER. Then run again.
  echo.
  pause
  exit /b 1
)

if not exist "bin\stranger.exe" (
  echo.
  echo  WARNING: bin\stranger.exe not found. This script should be in
  echo  your Stranger's Wrath GAME FOLDER (the one containing bin and data).
  echo.
)

if exist "bin\dinput8_real.dll" (
  echo  bin\dinput8_real.dll already exists - nothing to do. You're set.
) else (
  if exist "%WINDIR%\SysWOW64\dinput8.dll" (
    copy "%WINDIR%\SysWOW64\dinput8.dll" "bin\dinput8_real.dll" >nul
    echo  Created bin\dinput8_real.dll  -  install complete!
  ) else (
    echo  Could not find the system dinput8.dll. Copy it manually from
    echo  C:\Windows\SysWOW64\dinput8.dll  to  bin\dinput8_real.dll
  )
)

echo.
echo  Done. Launch the game from Steam.
echo  In-game: ` (tilde) = console. SWSE 1.1.1 starts with ONLY the console on.
echo  Type  features  in the console to see every system, and e.g.
echo  features graphics on   (then F10 toggles the look)  or  features foliage on
echo  to switch one on, or  features preset full  for the classic SWSE in one
echo  word. SWSEMods\features.txt holds the same switches, and SWSE Setup.exe
echo  (in the download) ticks them for you.
echo.
pause
endlocal
