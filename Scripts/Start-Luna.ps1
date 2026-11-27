param([switch]$Verify,[switch]$TiltPlay,[string]$RuntimeLog)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'Saved\Verification'),(Join-Path $projectRoot 'Saved\Cache') | Out-Null
. (Join-Path $PSScriptRoot 'Luna-Engine.ps1')
$engine = Get-LunaEngineRoot
$editor = Join-Path $engine 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $projectRoot 'Luna_v1.uproject'
$existing=@(Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe'" | Where-Object {$_.CommandLine -and $_.CommandLine.Replace('/','\').Contains($project) -and $_.CommandLine -match ' -game '})
if($existing.Count -gt 0){
    if($Verify -or $TiltPlay){throw 'Close the existing Luna game window first.'}
    $shell=New-Object -ComObject WScript.Shell
    $null=$shell.AppActivate($existing[0].ProcessId)
    Write-Output 'The MVP game is already open.'
    return
}
$binary = Join-Path $projectRoot 'Binaries\Win64\UnrealEditor-LunaMVP.dll'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Build the UE MVP first using Scripts\Build-Luna.ps1.' }
$localTemp = Join-Path $projectRoot 'Saved\Temp'
New-Item -ItemType Directory -Force -Path $localTemp | Out-Null
$env:TEMP = $env:TMP = $localTemp
if (!$RuntimeLog) { $RuntimeLog = Join-Path $projectRoot 'Saved\Verification\runtime.log' }
$args = @('"'+$project+'"','/Game/LunaArt/Maps/L_WanderingLuna','-game','-windowed','-ResX=1600','-ResY=900','-NoSplash','-ddc=InstalledNoZenLocalFallback',
    '"-LocalDataCachePath='+(Join-Path $projectRoot 'Saved\Cache')+'"',
    '-DisablePlugins=ModelContextProtocol,EditorToolset,ToolsetRegistry,PythonScriptPlugin,Terminal',
    '"-abslog='+$RuntimeLog+'"')
if ($Verify) { $args += '-LunaVerify' }
if ($TiltPlay) { $args += '-LunaTiltPlay' }
$process = Start-Process -FilePath $editor -ArgumentList $args -WorkingDirectory $projectRoot -WindowStyle Normal -PassThru
@{pid=$process.Id;verify=[bool]$Verify;project=$project;started_at=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $projectRoot 'Saved\Verification\game-process.json') -Encoding utf8
Write-Output ('UE TiltPlay MVP started. Process '+$process.Id)
