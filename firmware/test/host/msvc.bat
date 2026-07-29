@echo off
REM Builds one host test suite with MSVC.
REM   msvc.bat <outdir> <name> <includedir> <src...>
REM Quoting a vcvars call through a POSIX shell is a losing game, so the whole
REM invocation lives here where cmd owns the quoting rules. Flags that embed a
REM path are written directly on the cl.exe command line rather than stored in
REM a %VAR%, because a quoted path inside a `set "VAR=..."` value is exactly
REM the kind of thing that quietly breaks cmd's quote parsing.
setlocal enabledelayedexpansion

set "OUT=%~1"
set "NAME=%~2"
set "INC=%FCP_INC%"
shift
shift

REM Anything under a "vendor" directory is third-party and is compiled with
REM relaxed warnings. Our own code is compiled with /W4 /WX, and a warning
REM inside somebody else's vetted library is not ours to fix or silence at
REM the point of use.
set "OWN_SRCS="
set "VENDOR_SRCS="
:collect
if "%~1"=="" goto collected
set "ISVENDOR="
echo %~1 | findstr /I "vendor" >nul
if not errorlevel 1 set "ISVENDOR=1"
if defined ISVENDOR (set VENDOR_SRCS=!VENDOR_SRCS! "%~1") else (set OWN_SRCS=!OWN_SRCS! "%~1")
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

REM INC may list several directories separated by semicolons. Prepending them
REM to INCLUDE handles that without building a /I switch per entry.
set "INCLUDE=%INC%;%INCLUDE%"

if not exist "%OUT%\vendor_" mkdir "%OUT%\vendor_"
del /q "%OUT%\*.obj" "%OUT%\vendor_\*.obj" >nul 2>&1

REM Address sanitizer turns an out of bounds read in the fuzz pass into a hard
REM failure instead of silence, which is the entire point of running it. It is
REM an optional VS component, so fall back rather than refuse to build.
cl /nologo /W4 /WX /std:c11 /Zi /c /fsanitize=address /Fo:"%OUT%\\" %OWN_SRCS% >"%OUT%\build.log" 2>&1
set "OWN_ERR=%errorlevel%"
set "VENDOR_ERR=0"
if not "%VENDOR_SRCS%"=="" (
  cl /nologo /W1 /std:c11 /Zi /c /fsanitize=address /Fo:"%OUT%\vendor_\\" %VENDOR_SRCS% >>"%OUT%\build.log" 2>&1
  set "VENDOR_ERR=!errorlevel!"
)
if "%OWN_ERR%"=="0" if "%VENDOR_ERR%"=="0" (
  cl /nologo /Zi /Fe:"%OUT%\%NAME%.exe" /Fd:"%OUT%\%NAME%.pdb" "%OUT%\*.obj" "%OUT%\vendor_\*.obj" >>"%OUT%\build.log" 2>&1
  if not errorlevel 1 (
    echo   built with address sanitizer
    goto run
  )
)

del /q "%OUT%\*.obj" "%OUT%\vendor_\*.obj" >nul 2>&1
cl /nologo /W4 /WX /std:c11 /Zi /c /Fo:"%OUT%\\" %OWN_SRCS% >"%OUT%\build.log" 2>&1
set "OWN_ERR=%errorlevel%"
set "VENDOR_ERR=0"
if not "%VENDOR_SRCS%"=="" (
  cl /nologo /W1 /std:c11 /Zi /c /Fo:"%OUT%\vendor_\\" %VENDOR_SRCS% >>"%OUT%\build.log" 2>&1
  set "VENDOR_ERR=!errorlevel!"
)
if not "%OWN_ERR%"=="0" (
  echo   build failed 1>&2
  type "%OUT%\build.log" 1>&2
  exit /b 1
)
if not "%VENDOR_ERR%"=="0" (
  echo   build failed 1>&2
  type "%OUT%\build.log" 1>&2
  exit /b 1
)

cl /nologo /Zi /Fe:"%OUT%\%NAME%.exe" /Fd:"%OUT%\%NAME%.pdb" "%OUT%\*.obj" "%OUT%\vendor_\*.obj" >>"%OUT%\build.log" 2>&1
if errorlevel 1 (
  echo   link failed 1>&2
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
