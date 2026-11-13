@echo off
setlocal
cd /d "%~dp0"
if not exist "build\CppHostDemo.exe" (
    echo Build Wave.dll and CppHostDemo.exe first. See README.md.
    pause
    exit /b 1
)
start "Wave C++ Host" /D "%~dp0build" "%~dp0build\CppHostDemo.exe"
