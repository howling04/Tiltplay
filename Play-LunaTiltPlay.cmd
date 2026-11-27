@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Play-LunaTiltPlay.ps1" %*
set "LUNA_EXIT=%ERRORLEVEL%"
pause
exit /b %LUNA_EXIT%
