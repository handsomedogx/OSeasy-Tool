@echo off
rem ===========================================================================
rem  swap.bat - put the newly built bin\OeWinFix2.new.exe in place.
rem
rem  It NEVER kills the running guardian: if OeWinFix2.exe is alive it just
rem  prints what to do. Exit it yourself from the tray menu (that is the only
rem  thing keeping your keyboard released), then run this again.
rem  ASCII only.
rem ===========================================================================
setlocal
tasklist /FI "IMAGENAME eq OeWinFix2.exe" /NH > "%TEMP%\owf2_ps.txt" 2>nul
find /I "OeWinFix2.exe" "%TEMP%\owf2_ps.txt" >nul
if not errorlevel 1 (
  echo [swap] OeWinFix2.exe is still running.
  echo [swap] Exit it from the tray menu first ^(tray icon -> right click -> "exit program"^),
  echo [swap] then run swap.bat again. This script will not kill it for you.
  del "%TEMP%\owf2_ps.txt" >nul 2>&1
  exit /b 1
)
del "%TEMP%\owf2_ps.txt" >nul 2>&1

if not exist "%~dp0bin\OeWinFix2.new.exe" (
  echo [swap] bin\OeWinFix2.new.exe not found. Build it first:
  echo        build.bat OeWinFix2.new.exe
  exit /b 1
)

echo [swap] stopping standalone MouseRelay.exe ^(its job is now built into OeWinFix2^) ...
taskkill /IM MouseRelay.exe /F >nul 2>&1

echo [swap] backing up current exe -^> bin\OeWinFix2.prev.exe
if exist "%~dp0bin\OeWinFix2.exe" move /Y "%~dp0bin\OeWinFix2.exe" "%~dp0bin\OeWinFix2.prev.exe" >nul

echo [swap] installing ...
move /Y "%~dp0bin\OeWinFix2.new.exe" "%~dp0bin\OeWinFix2.exe" >nul
if errorlevel 1 ( echo [swap] move failed - is bin\OeWinFix2.exe still open? & exit /b 1 )

start "" "%~dp0bin\OeWinFix2.exe"
echo [swap] done: new OeWinFix2 started.
echo [swap] rollback: exit the new one, rename bin\OeWinFix2.prev.exe back to bin\OeWinFix2.exe
endlocal
