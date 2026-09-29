@echo off
rem Double-click launcher for the OrcSDR settings-safe installer.
title OrcSDR installer
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
echo.
pause
