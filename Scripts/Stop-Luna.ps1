param([switch]$TiltPlayOnly)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'Saved\Verification'),(Join-Path $projectRoot 'Saved\Cache') | Out-Null
$record=Join-Path $projectRoot 'Saved\Verification\game-process.json'
if(-not (Test-Path -LiteralPath $record)){return}
$info=Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
$project=Join-Path $projectRoot 'Luna_v1.uproject'
if ($TiltPlayOnly -and $info.project -ne $project) {return}
$process=Get-CimInstance Win32_Process -Filter ('ProcessId='+[int]$info.pid)
if(-not $process){return}
$commandLine = [string]$process.CommandLine
if ($TiltPlayOnly -and $commandLine -notmatch '\s-LunaTiltPlay(?:\s|$)') {return}
if($process.Name -ne 'UnrealEditor.exe' -or -not $commandLine.Replace('/','\').Contains($project) -or $commandLine -notmatch '\s-game(?:\s|$)'){
    if($TiltPlayOnly){return}
    throw 'Recorded process is not this MVP game. Left running.'
}
$owned=Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
if(!$owned){return}
if($owned.MainWindowHandle -ne 0){$null=$owned.CloseMainWindow();$null=$owned.WaitForExit(5000)}
if(-not $owned.HasExited){
    Stop-Process -InputObject $owned
    if(!$owned.WaitForExit(5000)){throw 'The Luna game did not exit.'}
}
Write-Output 'Stopped only the MVP game window.'
