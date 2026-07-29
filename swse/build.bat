@echo off
REM Build SWSE (32-bit dinput8.dll proxy). Requires VS2022+ C++ toolchain.
setlocal
cd /d "%~dp0"

REM --- locate vcvars for 32-bit (x86) builds ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community" set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined VSPATH (
  echo Could not find Visual Studio. Install the C++ toolchain.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul

echo Compiling SWSE proxy (x86)...
cl /nologo /LD /O2 /EHsc dllmain.cpp framehook.cpp gfx.cpp glspy.cpp console.cpp scriptvm.cpp input.cpp granny.cpp shaderspy.cpp foliage.cpp wind.cpp selftest.cpp uispy.cpp aitune.cpp features.cpp modregistry.cpp triggers.cpp positions.cpp /link /OUT:dinput8.dll
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

del /q dllmain.obj framehook.obj gfx.obj glspy.obj console.obj scriptvm.obj input.obj granny.obj shaderspy.obj foliage.obj wind.obj selftest.obj dinput8.exp dinput8.lib 2>nul
echo.
echo Built dinput8.dll
echo Install: copy dinput8.dll into the game's bin\ folder, and copy the real
echo system dinput8.dll there as dinput8_real.dll (build_install.bat does both).
endlocal
