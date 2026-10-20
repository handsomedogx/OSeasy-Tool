@echo off
rem ===========================================================================
rem  swap.bat - install bin\OSeasy-Tool.new.exe as bin\OSeasy-Tool.exe and start it.
rem
rem  版本管理交给 git：旧版在提交历史里，文件系统不再堆 prev.exe。
rem  回滚 = git checkout <tag>  ->  build.bat  ->  再跑一次 swap.bat
rem
rem  It NEVER kills the running guardian: if OSeasy-Tool.exe is alive it just
rem  prints what to do. Exit it yourself from the panel/tray, then run again.
rem  ASCII only.
rem ===========================================================================
setlocal
tasklist /FI "IMAGENAME eq OSeasy-Tool.exe" /NH > "%TEMP%\ost_ps.txt" 2>nul
find /I "OSeasy-Tool.exe" "%TEMP%\ost_ps.txt" >nul
if not errorlevel 1 (
  echo [swap] OSeasy-Tool.exe is still running.
  echo [swap] Exit it first ^(panel button "exit" or tray menu^), then run swap.bat again.
  echo [swap] This script will not kill it for you.
  del "%TEMP%\ost_ps.txt" >nul 2>&1
  exit /b 1
)
del "%TEMP%\ost_ps.txt" >nul 2>&1

if not exist "%~dp0bin\OSeasy-Tool.new.exe" (
  echo [swap] bin\OSeasy-Tool.new.exe not found. Build it first:
  echo        build.bat OSeasy-Tool.new.exe
  exit /b 1
)

echo [swap] installing ...
move /Y "%~dp0bin\OSeasy-Tool.new.exe" "%~dp0bin\OSeasy-Tool.exe" >nul
if errorlevel 1 ( echo [swap] move failed - is bin\OSeasy-Tool.exe still open? & exit /b 1 )

start "" "%~dp0bin\OSeasy-Tool.exe"
echo [swap] done. Rollback: git checkout ^<tag^>  then  build.bat  then  swap.bat
endlocal
