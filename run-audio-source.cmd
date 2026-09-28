@echo off
setlocal
cd /d "%~dp0"
if not exist "build\AudioSource.exe" (
    echo Run build.ps1 first.
    pause
    exit /b 1
)
echo Start Wave separately with run.cmd. Close the CPU/memory source first.
echo If Wave was registered to another source, close and reopen Wave.
echo Play music/video, then select Audio Left / Audio Right in Wave.
echo Close this console or press Ctrl+C to stop capture. No audio file is saved.
"%~dp0build\AudioSource.exe" %*
if errorlevel 1 pause
