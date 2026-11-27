param([Parameter(Mandatory=$true)][string]$ReportDirectory,[switch]$Apply)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$dependencyFile=Join-Path $projectRoot 'Saved\Verification\dependencies.json'
$dependencies=Get-Content -LiteralPath $dependencyFile -Raw | ConvertFrom-Json
if(-not $dependencies.passed){throw 'A successful asset dependency audit is required'}
if(@(Get-CimInstance Win32_Process | Where-Object {$_.Name -in @('UnrealEditor.exe','UnrealEditor-Cmd.exe') -and $_.CommandLine -and $_.CommandLine.Contains((Join-Path $projectRoot 'Luna_v1.uproject'))}).Count){throw 'Close this project before cleanup'}
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$candidates=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-Candidate([string]$relative){
    $absolute=[IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if(-not $absolute.StartsWith($projectRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw ('Unsafe cleanup target: '+$absolute)}
    if(Test-Path -LiteralPath $absolute){$null=$candidates.Add($absolute)}
}
foreach($package in $dependencies.unused){
    if(-not $package.StartsWith('/Game/')){throw 'Unexpected package outside /Game'}
    $relative='Content\'+$package.Substring(6).Replace('/','\')
    foreach($extension in @('.uasset','.umap','.uexp','.ubulk')){Add-Candidate ($relative+$extension)}
}
foreach($relative in @('Docs','DesignResource','Intermediate','DerivedDataCache','Luna','UE_ACCESS.md','StartCodex.cmd','StartUE-MCP.cmd','Open-MVPEditor.cmd','Play-MVP.cmd','Content\LunaArt\HUD')){Add-Candidate $relative}
foreach($name in @('__ExternalActors__','__ExternalObjects__','Collections','Developers')){
    if(-not @($dependencies.keep | Where-Object {$_.StartsWith('/Game/'+$name+'/')}).Count){Add-Candidate ('Content\'+$name)}
}
$keepScripts=@('Assets-Luna.ps1','Build-Luna.ps1','Clean-Luna.ps1','Luna-Engine.ps1','Open-LunaEditor.ps1','Play-LunaTiltPlay.ps1','Start-Luna.ps1','Stop-Luna.ps1','Test-Luna.ps1','Test-Editor.py','ue_mcp.py')
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Scripts') -Force | Where-Object {$_.Name -notin $keepScripts} | ForEach-Object {Add-Candidate ('Scripts\'+$_.Name)}
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Saved') -Force | Where-Object {$_.Name -ne 'Cache'} | ForEach-Object {Add-Candidate ('Saved\'+$_.Name)}
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Binaries\Win64') -Filter '*.pdb' | ForEach-Object {Add-Candidate ('Binaries\Win64\'+$_.Name)}
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Content\LunaArt\Fonts') -File | Where-Object {$_.Name -notin @('NotoSansCJKsc-Regular.otf','Rajdhani-Medium.ttf')} | ForEach-Object {Add-Candidate ('Content\LunaArt\Fonts\'+$_.Name)}
foreach($folder in @('SourceArt','Integration')){Add-Candidate ($folder+'\__pycache__')}
$records=@();[long]$bytes=0
foreach($absolute in @($candidates | Sort-Object)){
    $parentCandidate=$false
    foreach($other in $candidates){if($absolute.StartsWith($other+'\',[StringComparison]::OrdinalIgnoreCase)){$parentCandidate=$true;break}}
    if($parentCandidate){continue}
    $item=Get-Item -LiteralPath $absolute -Force
    if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw ('Refusing to traverse a link: '+$absolute)}
    if($item.PSIsContainer){
        $files=@(Get-ChildItem -LiteralPath $absolute -File -Force -Recurse)
        $size=($files | Measure-Object Length -Sum).Sum
    }else{$size=$item.Length}
    $bytes+=$size;$records+=@{path=$absolute.Substring($projectRoot.Length+1);bytes=$size;directory=$item.PSIsContainer}
}
$report=@{project=$projectRoot;approved_scope='User requested removal of nonessential project resources and history';applied=[bool]$Apply;bytes=$bytes;targets=$records;runtime_packages_kept=$dependencies.keep}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ReportDirectory 'cleanup.json') -Encoding utf8
Copy-Item -LiteralPath $dependencyFile -Destination (Join-Path $ReportDirectory 'dependencies.json')
if($Apply){
    foreach($record in $records){
        $absolute=[IO.Path]::GetFullPath((Join-Path $projectRoot $record.path))
        if(-not $absolute.StartsWith($projectRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup target escaped workspace'}
        Remove-Item -LiteralPath $absolute -Recurse -Force
    }
    Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Content') -Directory -Recurse | Sort-Object {$_.FullName.Length} -Descending | ForEach-Object {if(-not @(Get-ChildItem -LiteralPath $_.FullName -Force).Count){Remove-Item -LiteralPath $_.FullName -Force}}
}
$verb=if($Apply){'Removed '}else{'Planned '}
Write-Output ($verb+[math]::Round($bytes/1GB,3)+' GiB; '+$dependencies.keep.Count+' runtime packages retained')
