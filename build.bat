@echo off
rem ===========================================================================
rem  build.bat [main.exe] - OeWinFix2 (x86 native, MSVC, static CRT). ASCII only.
rem
rem  Products:
rem    bin\OeWinFix2.exe         the program (tray guardian)          <- default
rem  Dev tools (not needed for normal use):
rem    bin\dev\FakeBox.exe       test harness: real StudentLogic.dll mouse hook
rem    bin\dev\relaytest.exe     standalone self-test of src/relay.cpp
rem    bin\dev\uitest.exe        status panel only (stubs), for UI work
rem  Objects go to bin\obj\ (throw-away).
rem
rem  Pass a different name when bin\OeWinFix2.exe is running (see swap.bat):
rem    build.bat OeWinFix2.new.exe
rem  Edit VCVARS below if your Visual Studio lives elsewhere.
rem ===========================================================================
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
set MAIN=%~1
if "%MAIN%"=="" set MAIN=OeWinFix2.exe
if not exist "%VCVARS%" ( echo [build] vcvarsall.bat not found, edit VCVARS & exit /b 1 )
call "%VCVARS%" x86 >nul
if errorlevel 1 ( echo [build] vcvarsall failed & exit /b 1 )

if not exist "%~dp0bin" mkdir "%~dp0bin"
if not exist "%~dp0bin\obj" mkdir "%~dp0bin\obj"
if not exist "%~dp0bin\dev" mkdir "%~dp0bin\dev"

echo [build] %MAIN% ...
cl /nologo /utf-8 /O2 /MT /W3 /EHsc /DUNICODE /D_UNICODE ^
   "%~dp0src\main.cpp" "%~dp0src\log.cpp" "%~dp0src\util.cpp" "%~dp0src\windowize.cpp" ^
   "%~dp0src\desktop.cpp" "%~dp0src\kbd.cpp" "%~dp0src\relay.cpp" "%~dp0src\bcast.cpp" ^
   "%~dp0src\mirror.cpp" "%~dp0src\statusui.cpp" ^
   /Fe:"%~dp0bin\%MAIN%" /Fo:"%~dp0bin\obj\\" ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X86 ^
   /MANIFEST:EMBED /MANIFESTINPUT:"%~dp0src\OeWinFix2.manifest" /MANIFESTUAC:NO
if errorlevel 1 ( echo [build] OeWinFix2 failed & exit /b 1 )

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
