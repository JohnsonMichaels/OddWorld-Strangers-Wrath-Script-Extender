@echo off
REM Build the loader test plugins into out\ (x86, static runtime), for
REM tools\swse_plugin_tests.ps1, which copies them into mod folders in the
REM game. NEVER SHIP THESE: several misbehave on purpose.
REM   idle1..idle6     empty TICK + OVERLAY callbacks (frame cost; idle6 is the
REM                    one added while the game runs, for `mods reload`)
REM   qaapi99, qadecline, qaqueryfault, qaloadfault, qaenablefault,
REM   qarollback, qamany, qaoverlay, qaevents, qagl, qagl2   (qaplug.cpp)
REM   crashy, crashy2..crashy6    (..\mockhost\crashy.cpp, one per fault)
REM   hello, and copies of it as hi.dll (name mismatch) and graphics.dll
REM   (a built-in's name); qanoexp.dll (no exports); qa64.dll (x64)
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
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul

set "SDK=%~dp0..\.."
if not exist out mkdir out
if not exist obj mkdir obj
set "CL_PLUG=cl /nologo /LD /MT /O2 /W4 /EHsc /I"%SDK%""

for %%n in (idle1 idle2 idle3 idle4 idle5 idle6) do (
  %CL_PLUG% /DQA_NAME=\"%%n\" /DQA_MODE_IDLE qaplug.cpp /Fo"obj\%%n.obj" /link /OUT:out\%%n.dll || goto :fail
)
call :qa qaapi99       API99       || goto :fail
call :qa qadecline     DECLINE     || goto :fail
call :qa qaqueryfault  QUERYFAULT  || goto :fail
call :qa qaloadfault   LOADFAULT   || goto :fail
call :qa qaenablefault ENABLEFAULT || goto :fail
call :qa qarollback    ROLLBACK    || goto :fail
call :qa qamany        MANY        || goto :fail
call :qa qaoverlay     OVERLAY     || goto :fail
call :qa qaevents      EVENTS      || goto :fail
call :qa qagl          GL          || goto :fail
call :qa qagl2         GL          || goto :fail

cl /nologo /LD /MT /O2 /W4 /EHsc /I"%SDK%" ..\mockhost\crashy.cpp /Fo"obj\crashy.obj" /link /OUT:out\crashy.dll || goto :fail
for %%n in (crashy2 crashy3 crashy4 crashy5 crashy6) do (
  %CL_PLUG% /DCRASHY_NAME=\"%%n\" ..\mockhost\crashy.cpp /Fo"obj\%%n.obj" /link /OUT:out\%%n.dll || goto :fail
)

rc /nologo /fo obj\hello.res "%SDK%\examples\hello_plugin\hello.rc" || goto :fail
%CL_PLUG% "%SDK%\examples\hello_plugin\hello.cpp" obj\hello.res /Fo"obj\hello.obj" /link /OUT:out\hello.dll || goto :fail
copy /y out\hello.dll out\hi.dll >nul
copy /y out\hello.dll out\graphics.dll >nul

cl /nologo /LD /MT /O2 /W4 noexports.c /Fo"obj\qanoexp.obj" /link /OUT:out\qanoexp.dll || goto :fail

REM Last, because it switches the environment to the x64 toolchain.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /LD /MT /O2 /W4 noexports.c /Fo"obj\qa64.obj" /link /OUT:out\qa64.dll || goto :fail

del /q out\*.exp out\*.lib 2>nul
echo Built the loader test plugins in %~dp0out
endlocal
exit /b 0

:qa
%CL_PLUG% /DQA_NAME=\"%1\" /DQA_MODE_%2 qaplug.cpp /Fo"obj\%1.obj" /link /OUT:out\%1.dll
exit /b %errorlevel%

:fail
echo BUILD FAILED
endlocal
exit /b 1
