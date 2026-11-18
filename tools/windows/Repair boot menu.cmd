@echo off
rem Puts SynthOS first in the boot menu again (the SynthOS / Windows menu at every start).
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0repair.ps1" %*
