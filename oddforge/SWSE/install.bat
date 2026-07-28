@echo off
REM Install SWSE into the game: copies our proxy dinput8.dll into bin\ and the
REM real system dinput8.dll beside it as dinput8_real.dll (our forward target).
setlocal
cd /d "%~dp0"

set "GAMEBIN=C:\Program Files (x86)\Steam\steamapps\common\Stranger's Wrath\bin"
if not exist "%GAMEBIN%\stranger.exe" (
  echo Game bin not found at "%GAMEBIN%".
  echo Edit GAMEBIN in this script to point at your install.
  exit /b 1
)
if not exist "dinput8.dll" (
  echo Build first: run build.bat
  exit /b 1
)

REM stash the real dinput8 as our forward target (only once)
if not exist "%GAMEBIN%\dinput8_real.dll" (
  copy "%WINDIR%\SysWOW64\dinput8.dll" "%GAMEBIN%\dinput8_real.dll" >nul
  echo copied real dinput8.dll -^> dinput8_real.dll
)
copy /y "dinput8.dll" "%GAMEBIN%\dinput8.dll" >nul
echo installed SWSE proxy dinput8.dll into game bin

echo.
echo Launch the game once, then check for bin\swse_log.txt to confirm injection.
echo Uninstall: delete dinput8.dll and dinput8_real.dll from bin\.
endlocal
