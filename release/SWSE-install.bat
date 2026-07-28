@echo off
REM ================================================================
REM  SWSE installer helper  -  by Johnson Michaels
REM  Run this AFTER copying the "bin" and "SWSEMods" folders into
REM  your Stranger's Wrath game folder. It creates the forward DLL the
REM  loader needs (bin\dinput8_real.dll) from your Windows system file.
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
echo  In-game: F10 = graphics on/off, ` (tilde) = console.
echo.
pause
endlocal
