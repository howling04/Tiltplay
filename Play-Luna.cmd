@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Start-Luna.ps1"
if errorlevel 1 pause
