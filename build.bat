@echo off
rem ===========================================================================
rem  build.bat [main.exe] - OSeasy-Tool (x86 native, MSVC, static CRT). ASCII only.
rem
rem  Products:
rem    bin\OSeasy-Tool.exe       the program (tray guardian)          <- default
rem  Dev tools (not needed for normal use):
rem    bin\dev\FakeBox.exe       test harness: real StudentLogic.dll mouse hook
rem    bin\dev\relaytest.exe     standalone self-test of src/relay.cpp
rem    bin\dev\uitest.exe        status panel only (stubs), for UI work
rem  Objects go to bin\obj\ (throw-away).
rem
rem  Pass a different name when bin\OSeasy-Tool.exe is running (see swap.bat):
rem    build.bat OSeasy-Tool.new.exe
rem  Toolchain: try VS2022 Community first, else ask vswhere for whatever VC
rem  toolset is installed (VS2019/2022/2026 all work - plain Win32 + static CRT).
rem  Either can be overridden with the OST_VCVARS environment variable.
rem ===========================================================================
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if defined OST_VCVARS set "VCVARS=%OST_VCVARS%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VCVARS%" if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
set MAIN=%~1
if "%MAIN%"=="" set MAIN=OSeasy-Tool.exe
if not exist "%VCVARS%" ( echo [build] vcvarsall.bat not found - set OST_VCVARS & exit /b 1 )
echo [build] toolchain: %VCVARS%
call "%VCVARS%" x86 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0bin\obj" mkdir "%~dp0bin\obj"
if not exist "%~dp0bin\dev" mkdir "%~dp0bin\dev"

echo [build] version resource ...
rem  cl no longer accepts .rc as a source file (D9024), so run rc.exe separately
rem  and hand the .res to the linker.
rc /nologo /fo "%~dp0bin\obj\version.res" "%~dp0src\version.rc"
if errorlevel 1 ( echo [build] rc failed & exit /b 1 )

echo [build] %MAIN% ...
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE ^
   "%~dp0src\main.cpp" "%~dp0src\log.cpp" "%~dp0src\util.cpp" "%~dp0src\windowize.cpp" ^
   "%~dp0src\desktop.cpp" "%~dp0src\kbd.cpp" "%~dp0src\relay.cpp" "%~dp0src\bcast.cpp" ^
   "%~dp0src\statusui.cpp" "%~dp0bin\obj\version.res" ^
   /Fe:"%~dp0bin\%MAIN%" /Fo:"%~dp0bin\obj\\" ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X86 ^
   /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0src\OSeasy-Tool.manifest" /MANIFESTUAC:NO
if errorlevel 1 ( echo [build] OSeasy-Tool failed & exit /b 1 )

echo [build] dev\FakeBox.exe ...
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE ^
   "%~dp0test\FakeBox.cpp" ^
   /Fe:"%~dp0bin\dev\FakeBox.exe" /Fo:"%~dp0bin\obj\\" ^
   /link /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( echo [build] FakeBox failed & exit /b 1 )

echo [build] dev\relaytest.exe ...
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE ^
   "%~dp0test\relaytest.cpp" "%~dp0src\relay.cpp" "%~dp0src\log.cpp" ^
   /Fe:"%~dp0bin\dev\relaytest.exe" /Fo:"%~dp0bin\obj\\" ^
   /link /SUBSYSTEM:CONSOLE /MACHINE:X86
if errorlevel 1 ( echo [build] relaytest failed & exit /b 1 )

echo [build] dev\uitest.exe ^(status panel only^) ...
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE ^
   "%~dp0test\uitest.cpp" "%~dp0src\statusui.cpp" "%~dp0src\relay.cpp" "%~dp0src\log.cpp" ^
   /Fe:"%~dp0bin\dev\uitest.exe" /Fo:"%~dp0bin\obj\\" ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X86
if errorlevel 1 ( echo [build] uitest failed & exit /b 1 )

echo [build] OK: bin\%MAIN%  +  bin\dev\{FakeBox,relaytest,uitest}.exe
endlocal
