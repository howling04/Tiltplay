#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LunaSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="TiltPlay MVP"))
class LUNAMVP_API ULunaSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(Config, EditAnywhere, Category="Movement", meta=(ToolTip="Acceleration in cm/s^2")) float Acceleration = 700.f;
    UPROPERTY(Config, EditAnywhere, Category="Movement") float Drag = 1.2f;
    UPROPERTY(Config, EditAnywhere, Category="Movement", meta=(Units="cm/s")) float MaxSpeed = 500.f;
    UPROPERTY(Config, EditAnywhere, Category="Movement") float DeadZoneDegrees = 3.f;
    UPROPERTY(Config, EditAnywhere, Category="Movement") float FullTiltDegrees = 25.f;
    UPROPERTY(Config, EditAnywhere, Category="Height") float HeightKp = 3.f;
    UPROPERTY(Config, EditAnywhere, Category="Height") float HeightKd = 3.5f;
    UPROPERTY(Config, EditAnywhere, Category="Height") float MaxVerticalAcceleration = 600.f;
    UPROPERTY(Config, EditAnywhere, Category="Height") float MaxVerticalSpeed = 250.f;
    UPROPERTY(Config, EditAnywhere, Category="Network") FString ListenAddress = TEXT("127.0.0.1");
    UPROPERTY(Config, EditAnywhere, Category="Network", meta=(ClampMin="1", ClampMax="65535")) int32 InputPort = 7001;
    UPROPERTY(Config, EditAnywhere, Category="Network") FString BridgeAddress = TEXT("127.0.0.1");
    UPROPERTY(Config, EditAnywhere, Category="Network", meta=(ClampMin="1", ClampMax="65535")) int32 ControlPort = 7002;
    UPROPERTY(Config, EditAnywhere, Category="Network") float StaleSeconds = .25f;
    UPROPERTY(Config, EditAnywhere, Category="Network") FString ExpectedDeviceId;
    UPROPERTY(Config, EditAnywhere, Category="Network") bool bGatewayGestureProfile = true;
    UPROPERTY(Config, EditAnywhere, Category="Appearance") TSoftObjectPtr<UStaticMesh> MoonReplacement;
    UPROPERTY(Config, EditAnywhere, Category="Appearance") TSoftObjectPtr<UStaticMesh> BlockReplacement;
    UPROPERTY(Config, EditAnywhere, Category="Appearance") bool bReduceMotion = true;
};
