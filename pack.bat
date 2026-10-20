@echo off
rem ===========================================================================
rem  pack.bat [version] - build, stage and zip a release artifact.
rem
rem  Output (dist\):
rem    OSeasy-Tool.exe                        the program itself (single-file runnable)
rem    OSeasy-Tool-v<version>-win-x86.zip     exe + README.md + LICENSE
rem    SHA256SUMS.txt                         sha256 of the two files above
rem
rem  Usage:  pack.bat             -> version 1.0.0
rem          pack.bat 1.0.1
rem  ASCII only.
rem ===========================================================================
setlocal
set "VER=%~1"
if "%VER%"=="" set "VER=1.0.0"
set "ROOT=%~dp0"
set "NAME=OSeasy-Tool-v%VER%-win-x86"
set "DIST=%ROOT%dist"
set "STAGE=%DIST%\%NAME%"

echo [pack] version %VER%
echo [pack] build ...
call "%ROOT%build.bat" OSeasy-Tool.exe
if errorlevel 1 ( echo [pack] build failed & exit /b 1 )

echo [pack] stage %STAGE%
if exist "%STAGE%" rmdir /S /Q "%STAGE%"
if not exist "%DIST%" mkdir "%DIST%"
mkdir "%STAGE%"
if not exist "%ROOT%bin\OSeasy-Tool.exe" ( echo [pack] exe missing & exit /b 1 )
copy /Y "%ROOT%bin\OSeasy-Tool.exe" "%STAGE%\" >nul
copy /Y "%ROOT%README.md" "%STAGE%\" >nul
copy /Y "%ROOT%LICENSE" "%STAGE%\" >nul

echo [pack] zip ...
if exist "%DIST%\%NAME%.zip" del /Q "%DIST%\%NAME%.zip"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%DIST%\%NAME%.zip' -Force"
if not exist "%DIST%\%NAME%.zip" ( echo [pack] zip failed & exit /b 1 )

copy /Y "%ROOT%bin\OSeasy-Tool.exe" "%DIST%\OSeasy-Tool.exe" >nul

rem  sha256 via certutil: Get-FileHash is not available under every PowerShell
rem  that may be picked up here, certutil always is.
echo [pack] sha256 ...
del /Q "%DIST%\SHA256SUMS.txt" 2>nul
call :addhash "%DIST%\OSeasy-Tool.exe"
call :addhash "%DIST%\%NAME%.zip"
for %%s in ("%DIST%\SHA256SUMS.txt") do if %%~zs LSS 130 ( echo [pack] sha256 failed & exit /b 1 )

rmdir /S /Q "%STAGE%"

echo.
echo [pack] done. dist\ :
dir /B "%DIST%"
echo.
echo [pack] next: git tag -a v%VER% -m "v%VER%"  ^&^&  git push origin v%VER%
endlocal
exit /b 0

:addhash
for /f "usebackq skip=1 delims=" %%h in (`certutil -hashfile "%~1" SHA256 ^| findstr /V /C:"CertUtil"`) do >>"%DIST%\SHA256SUMS.txt" echo %%h  %~nx1
exit /b 0
