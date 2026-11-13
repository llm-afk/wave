@echo off
setlocal
cd /d "%~dp0"
if not exist "build\WaveDemo.exe" (
    echo WaveDemo.exe has not been built yet.
    echo From this folder, run these commands in PowerShell:
    echo   powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
    echo   powershell -ExecutionPolicy Bypass -File build.ps1
    echo Then double-click run.cmd again.
    pause
    exit /b 1
)
start "Wave Demo" /D "%~dp0" "%~dp0build\WaveDemo.exe"
exit /b 0
