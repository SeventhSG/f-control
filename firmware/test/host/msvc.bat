@echo off
REM Builds one host test suite with MSVC.
REM   msvc.bat <outdir> <name> <includedir> <src...>
REM Quoting a vcvars call through a POSIX shell is a losing game, so the whole
REM invocation lives here where cmd owns the quoting rules.
setlocal enabledelayedexpansion

set "OUT=%~1"
set "NAME=%~2"
set "INC=%~3"
shift
shift
shift

set "SRCS="
:collect
if "%~1"=="" goto collected
set SRCS=!SRCS! "%~1"
shift
goto collect
:collected

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio not found. Install the C++ build tools, gcc, or clang. 1>&2
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo Could not load the Visual Studio build environment. 1>&2
  exit /b 1
)

set "COMMON=/nologo /W4 /WX /std:c11 /Zi /I"%INC%" /Fe:"%OUT%\%NAME%.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\%NAME%.pdb""

REM Address sanitizer turns an out of bounds read in the fuzz pass into a hard
REM failure instead of silence, which is the entire point of running it. It is
REM an optional VS component, so fall back rather than refuse to build.
cl %COMMON% /fsanitize=address %SRCS% >"%OUT%\build.log" 2>&1
if not errorlevel 1 (
  echo   built with address sanitizer
  goto run
)

cl %COMMON% %SRCS% >"%OUT%\build.log" 2>&1
if errorlevel 1 (
  echo   build failed 1>&2
  type "%OUT%\build.log" 1>&2
  exit /b 1
)
echo   built without address sanitizer, install the ASan component for stronger checks

REM The suite runs here rather than back in the shell because the address
REM sanitizer runtime DLL is only on PATH inside the Visual Studio environment
REM this script just loaded.
:run
"%OUT%\%NAME%.exe"
exit /b %errorlevel%
