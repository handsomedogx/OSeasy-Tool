@echo off
rem ===========================================================================
rem  swap.bat - install bin\OeWinFix2.new.exe as bin\OeWinFix2.exe and start it.
rem
rem  版本管理交给 git：旧版在提交历史里，文件系统不再堆 prev.exe。
rem  回滚 = git checkout <tag>  ->  build.bat  ->  再跑一次 swap.bat
rem
rem  It NEVER kills the running guardian: if OeWinFix2.exe is alive it just
rem  prints what to do. Exit it yourself from the panel/tray, then run again.
rem  ASCII only.
rem ===========================================================================
setlocal
tasklist /FI "IMAGENAME eq OeWinFix2.exe" /NH > "%TEMP%\owf2_ps.txt" 2>nul
find /I "OeWinFix2.exe" "%TEMP%\owf2_ps.txt" >nul
if not errorlevel 1 (
  echo [swap] OeWinFix2.exe is still running.
  echo [swap] Exit it first ^(panel button "exit" or tray menu^), then run swap.bat again.
  echo [swap] This script will not kill it for you.
  del "%TEMP%\owf2_ps.txt" >nul 2>&1
  exit /b 1
)
del "%TEMP%\owf2_ps.txt" >nul 2>&1

if not exist "%~dp0bin\OeWinFix2.new.exe" (
  echo [swap] bin\OeWinFix2.new.exe not found. Build it first:
  echo        build.bat OeWinFix2.new.exe
  exit /b 1
)

echo [swap] installing ...
move /Y "%~dp0bin\OeWinFix2.new.exe" "%~dp0bin\OeWinFix2.exe" >nul
if errorlevel 1 ( echo [swap] move failed - is bin\OeWinFix2.exe still open? & exit /b 1 )

start "" "%~dp0bin\OeWinFix2.exe"
echo [swap] done. Rollback: git checkout ^<tag^>  then  build.bat  then  swap.bat
endlocal
