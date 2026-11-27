@echo off
setlocal
cd /d "%~dp0"
set "PY=python"
where python >nul 2>nul || set "PY=py -3"
echo [TiltPlay] replaying 40s recorded data to 127.0.0.1:7001 at 60 Hz
echo            start UE (or the packaged exe) FIRST, then run this.
%PY% replay_to_ue.py "sample_input_40s.jsonl" 7001 60
echo.
pause
