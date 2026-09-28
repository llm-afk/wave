@echo off
setlocal
cd /d "%~dp0"
if not exist "build\SystemSource.exe" (
    echo Please run build.ps1 first.
    pause
    exit /b 1
)
echo External data source only. Open Wave separately with run.cmd.
echo Close this console or press Ctrl+C to stop this source.
"%~dp0build\SystemSource.exe" %*
if errorlevel 1 pause
