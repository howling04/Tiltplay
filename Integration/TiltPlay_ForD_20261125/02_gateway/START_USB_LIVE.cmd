@echo off
setlocal
cd /d "%~dp0"
if "%~1"=="" (
  echo Usage: START_USB_LIVE.cmd COM7
  echo        (replace COM7 with your board port, run CHECK_ENV.cmd to list them)
  echo.
  call CHECK_ENV.cmd
  exit /b 1
)
set "PY=python"
where python >nul 2>nul || set "PY=py -3"
echo [TiltPlay] board=%1  gateway -> 127.0.0.1:7001
%PY% record_live.py --port %1 --ue
echo.
echo [TiltPlay] recorder exited with code %ERRORLEVEL%
pause
