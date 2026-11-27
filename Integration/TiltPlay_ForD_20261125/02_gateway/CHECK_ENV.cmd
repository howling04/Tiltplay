@echo off
setlocal
cd /d "%~dp0"
set "PY=python"
where python >nul 2>nul || set "PY=py -3"
%PY% check_env.py
echo.
echo [TiltPlay] if port 7001/7002 is BUSY, close the other UE instance or the old recorder.
pause
