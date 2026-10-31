@echo off
rem Installs SynthOS next to Windows: no USB stick and no firmware settings needed.
cd /d "%~dp0"
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
