param([ValidateSet('Editor','Game')][string]$Target = 'Editor')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$saved = Join-Path $projectRoot 'Saved'
$verification = Join-Path $saved 'Verification'
$buildTemp = Join-Path $saved 'Temp'
New-Item -ItemType Directory -Force -Path $verification,$buildTemp | Out-Null
$env:TEMP = $env:TMP = $buildTemp
$env:DOTNET_CLI_HOME = Join-Path $saved 'DotNet'
$env:DOTNET_NOLOGO = '1'
. (Join-Path $PSScriptRoot 'Luna-Engine.ps1')
$engine = Get-LunaEngineRoot
$dotnet = Join-Path $engine 'Engine\Binaries\ThirdParty\DotNet\10.0\win-x64\dotnet.exe'
$ubt = Join-Path $engine 'Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.dll'
if (!(Test-Path -LiteralPath $dotnet) -or !(Test-Path -LiteralPath $ubt)) {
    throw 'UE 5.8 build tools were not found. Set LUNA_UE_ROOT to the UE 5.8 installation folder.'
}
$name = if ($Target -eq 'Editor') { 'LunaMVPEditor' } else { 'LunaMVP' }
# Launch UBT as a top-level build so its local UBA executor can initialize.
& $dotnet $ubt $name Win64 Development ('-Project=' + (Join-Path $projectRoot 'Luna_v1.uproject')) `
    ('-Log=' + (Join-Path $verification "build-$Target.log")) ('-UBARootDir=' + (Join-Path $saved 'UBA')) `
    -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
exit $LASTEXITCODE
