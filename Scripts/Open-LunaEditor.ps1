$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $projectRoot 'Saved\Verification'),(Join-Path $projectRoot 'Saved\Cache') | Out-Null
. (Join-Path $PSScriptRoot 'Luna-Engine.ps1')
$editor=Join-Path (Get-LunaEngineRoot) 'Engine\Binaries\Win64\UnrealEditor.exe'
$project=Join-Path $projectRoot 'Luna_v1.uproject'
$existing=@(Get-CimInstance Win32_Process -Filter "Name='UnrealEditor.exe'" | Where-Object {$_.CommandLine -and $_.CommandLine.Replace('/','\').Contains($project) -and $_.CommandLine -notmatch ' -game '})
if($existing.Count -gt 0){
    $shell=New-Object -ComObject WScript.Shell
    $null=$shell.AppActivate($existing[0].ProcessId)
    Write-Output 'The MVP editor is already open.'
    return
}
$arguments=@(('"'+(Join-Path $projectRoot 'Luna_v1.uproject')+'"'),'/Game/LunaArt/Maps/L_WanderingLuna','-ModelContextProtocolStartServer','-ModelContextProtocolPort=8000','-NoSplash','-ddc=InstalledNoZenLocalFallback',('"-LocalDataCachePath='+(Join-Path $projectRoot 'Saved\Cache')+'"'))
$p=Start-Process -FilePath $editor -ArgumentList $arguments -WorkingDirectory $projectRoot -WindowStyle Normal -PassThru
@{pid=$p.Id;project=$projectRoot;started_at=(Get-Date).ToString('o')} | ConvertTo-Json | Set-Content (Join-Path $projectRoot 'Saved\Verification\editor-process.json') -Encoding utf8
Write-Output ('Opened UE MVP editor: '+$p.Id)
