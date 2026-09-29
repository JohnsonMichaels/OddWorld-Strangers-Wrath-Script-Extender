@echo off
REM Build the hello example plugin -> hello.dll (32-bit x86, static C runtime).
REM Same toolchain discovery as swse\build.bat: Visual Studio 2022+ with the
REM C++ workload. Run it from anywhere; it works in its own folder.
setlocal
cd /d "%~dp0"

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
REM vcvars32, not vcvars64: stranger.exe is a 32-bit process and can only load
REM 32-bit DLLs. The SDK header refuses to compile for anything else.
REM On some installs vcvars prints "'vswhere.exe' is not recognized ...": that
REM comes from Visual Studio's own script and is harmless (SWSE's build too).
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul

rc /nologo /fo hello.res hello.rc
if errorlevel 1 (
  echo RESOURCE COMPILE FAILED
  exit /b 1
)

REM /LD  a DLL.
REM /MT  the static C runtime: no redistributable needed, and no C runtime
REM      state shared with SWSE (the plugin ABI never shares any).
REM /W4  the SDK header is warning-free at /W4.
REM /I   where swse_plugin_api.h lives (the sdk folder).
cl /nologo /LD /MT /O2 /W4 /EHsc /I"%~dp0..\.." hello.cpp hello.res /link /OUT:hello.dll
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
del /q hello.obj hello.res hello.exp hello.lib 2>nul

echo.
echo Built hello.dll. To install, copy it to
echo     ^<game^>\SWSEMods\SWSE Hello\plugins\hello.dll
echo then in the game console type:  features hello on   and then:  hello
endlocal
