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
REM The version resource (swse.rc, FileVersion 1.1.0.0) lets tools read the
REM installed version from bin\dinput8.dll without running the game.
rc /nologo /fo swse.res swse.rc
if errorlevel 1 (
  echo RESOURCE COMPILE FAILED
  exit /b 1
)
REM /I..\sdk: plugins.cpp is the host side of sdk\swse_plugin_api.h (1.1).
cl /nologo /LD /O2 /EHsc /I..\sdk dllmain.cpp framehook.cpp gfx.cpp glspy.cpp console.cpp scriptvm.cpp input.cpp granny.cpp shaderspy.cpp foliage.cpp wind.cpp materials.cpp gpucompute.cpp geocapture.cpp raytrace.cpp selftest.cpp uispy.cpp aitune.cpp features.cpp modregistry.cpp triggers.cpp positions.cpp levelwatch.cpp playertune.cpp prefsedit.cpp playnpc.cpp freecam.cpp gamebuild.cpp mute.cpp menu.cpp plugins.cpp hookreg.cpp swse.res /link /OUT:dinput8.dll
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

REM ABI check: the example plugin must still compile, warning-free, against the
REM same header SWSE was just built from - a cheap guard against an edit that
REM breaks the plugin ABI (the header's layout asserts fail to compile).
cl /nologo /c /W4 /WX /EHsc /I..\sdk /Fo"%TEMP%\swse_hello_abi.obj" ..\sdk\examples\hello_plugin\hello.cpp >nul
if errorlevel 1 (
  echo SDK ABI CHECK FAILED - sdk\examples\hello_plugin\hello.cpp no longer compiles against sdk\swse_plugin_api.h
  exit /b 1
)
del /q "%TEMP%\swse_hello_abi.obj" 2>nul

del /q *.obj swse.res dinput8.exp dinput8.lib dllmain.exp dllmain.lib 2>nul
echo.
echo Built dinput8.dll (SWSE 1.1)
echo Install: copy dinput8.dll into the game's bin\ folder - install.bat does it
echo for the default Steam path. dinput8_real.dll is optional since 1.0.1: SWSE
echo loads the system dinput8.dll itself when it is not there.
endlocal
