function Get-LunaEngineRoot {
    if ($env:LUNA_UE_ROOT) {
        if (Test-Path -LiteralPath (Join-Path $env:LUNA_UE_ROOT 'Engine\Binaries\Win64\UnrealEditor.exe') -PathType Leaf) {
            return (Resolve-Path -LiteralPath $env:LUNA_UE_ROOT).Path
        }
        throw 'LUNA_UE_ROOT must point to the UE 5.8 installation directory.'
    }

    $candidates = @(
        (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.8' -ErrorAction SilentlyContinue).InstalledDirectory
        (Get-ItemProperty -LiteralPath 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue).'5.8'
    )
    $manifest = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path -LiteralPath $manifest -PathType Leaf) {
        $installations = (Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json).InstallationList
        $candidates += $installations | Where-Object AppName -eq 'UE_5.8' | ForEach-Object InstallLocation
    }
    $candidates += Join-Path $env:ProgramFiles 'Epic Games\UE_5.8'
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'Engine\Binaries\Win64\UnrealEditor.exe') -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'UE 5.8 was not found. Set LUNA_UE_ROOT to its installation directory.'
}
