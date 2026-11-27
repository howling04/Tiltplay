@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Open-LunaEditor.ps1"
if errorlevel 1 pause
