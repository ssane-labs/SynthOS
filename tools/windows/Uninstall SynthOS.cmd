@echo off
rem Removes the SynthOS that Install SynthOS.cmd put next to Windows.
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
