param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port = 'COM7',
    [ValidateRange(0,3600)][int]$Seconds = 0,
    [switch]$Replay,
    [switch]$Probe
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$gateway = Join-Path $root 'Integration\TiltPlay_ForD_20261125\02_gateway\record_live.py'
$serialProbe = Join-Path $root 'Integration\probe_tiltplay_serial.py'
$run = Join-Path $root ('Saved\Verification\TiltPlay-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$python = Get-Command python.exe -ErrorAction SilentlyContinue
$pythonArgs = @()
if (!$python -or $python.Source -like '*\WindowsApps\*') {
    $python = Get-Command py.exe -ErrorAction Stop
    $pythonArgs = @('-3')
}
$env:PYTHONUTF8 = '1'
$env:PYTHONUNBUFFERED = '1'
& $python.Source @pythonArgs -B -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
if ($LASTEXITCODE -ne 0) { throw 'Python 3.10 or newer is required for the gateway.' }

New-Item -ItemType Directory -Force -Path $run | Out-Null
if ($Probe) {
    & $python.Source @pythonArgs -B $serialProbe $Port (Join-Path $run 'serial_probe.json')
    exit $LASTEXITCODE
}
& (Join-Path $PSScriptRoot 'Stop-Luna.ps1') -TiltPlayOnly
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Build-Luna.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Luna could not be built. See Saved\Verification\build-Editor.log.' }

Write-Host "Capture folder: $run"
if ($Replay) {
    Write-Host 'Recorded input: press 2 in Luna for practice, then press C to begin playback.'
} else {
    Write-Host "Leave the board flat and still. Close any serial monitor. Board: $Port."
    Write-Host 'In Luna, enter Nano mode and press C with the board flat to enable flight.'
    Write-Host 'Tilt for horizontal thrust. In polar training or free flight, push the board up/down to shift orbit. Q/E also works.'
    Write-Host 'Press Ctrl+C in this console to end the gateway and save the capture.'
}
& (Join-Path $PSScriptRoot 'Start-Luna.ps1') -TiltPlay -RuntimeLog (Join-Path $run 'runtime.log')
if ($Replay) {
    & $python.Source @pythonArgs -B (Join-Path $root 'Integration\replay_tiltplay.py')
    exit $LASTEXITCODE
}
& $python.Source @pythonArgs -B $gateway --port $Port --ue --seconds $Seconds --out $run
$gatewayExit = $LASTEXITCODE
if ($gatewayExit -eq 3) {
    Write-Host 'Gateway did not reach ready state. Collecting an 8-second serial diagnostic.'
    & $python.Source @pythonArgs -B $serialProbe $Port (Join-Path $run 'serial_probe.json')
}
exit $gatewayExit
