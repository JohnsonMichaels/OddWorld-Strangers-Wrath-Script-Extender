@echo off
REM Build the mock host, the fault-injection plugin and the hello example, then
REM run the self-check: every rule the host enforces is exercised at least once.
REM   build.bat          build and run the self-check
REM   build.bat nocheck  build only
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
REM "'vswhere.exe' is not recognized" from vcvars itself is harmless.
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul

set "SDK=%~dp0..\.."
cl /nologo /W4 /WX /EHsc /O2 /MT /I"%SDK%" mockhost.cpp user32.lib /Fe:mockhost.exe
if errorlevel 1 goto :fail
cl /nologo /LD /W4 /EHsc /O2 /MT /I"%SDK%" crashy.cpp /link /OUT:crashy.dll
if errorlevel 1 goto :fail
cl /nologo /LD /W4 /EHsc /O2 /MT /I"%SDK%" "%SDK%\examples\hello_plugin\hello.cpp" /link /OUT:hello.dll
if errorlevel 1 goto :fail
del /q *.obj *.exp *.lib 2>nul

if /i "%~1"=="nocheck" goto :done
echo.
echo ==== self-check ====
REM Load order is the order on the command line: crashy first, hello later.
REM (.\ because cmd may be told not to search the current folder.)
echo --- 1: lifecycle, switch gating, a command two plugins offer, host rules
.\mockhost.exe crashy.dll hello.dll -- ^
  "hello" "features hello on" "frame 3" "levelup" "hello world" "plugins" ^
  "features hello off" "frame 2" "hello" "features hello on" "frame" "hello again" ^
  "features crashy on" "hello both-on" "crashy thread" "crashy recurse" ^
  "crashy slow" "frame" "plugins" "crashy throw" "features crashy on" "hello still-alive" "plugins"
echo.
echo --- 2: switched on in the other order - same owner; off falls back to the other provider
.\mockhost.exe crashy.dll hello.dll -- "features crashy on" "features hello on" "hello same-owner" ^
  "features hello off" "hello" "features hello on" "hello back"
echo.
echo --- 3: a fault in a frame callback
.\mockhost.exe crashy.dll hello.dll -- "features crashy on" "crashy frame" "frame" "crash" "features hello on" "hello"
echo.
echo --- 4: stack overflow, then a fault inside ntdll in a call the plugin made
.\mockhost.exe crashy.dll hello.dll -- "features crashy on" "crashy overflow" "features crashy on"
.\mockhost.exe crashy.dll hello.dll -- "features crashy on" "crashy callee" "plugins"
:done
endlocal
exit /b 0
:fail
echo BUILD FAILED
endlocal
exit /b 1
