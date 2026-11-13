@echo off
setlocal
cd /d "%~dp0"
if not exist "build\Wave.dll" (
    echo Build Wave.dll first. See README.md.
    pause
    exit /b 1
)
python -B examples\python_host_demo.py
if errorlevel 1 pause
