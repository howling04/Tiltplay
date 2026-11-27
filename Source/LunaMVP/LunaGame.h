#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "LunaDevice.h"
#include "LunaSolarSystem.h"
#include "LunaGame.generated.h"
class ALunaPawn;
class ALunaCourse;
class SLunaHUD;

struct FLunaPlayerControls
{
    float ForwardGain=1.f, RightGain=1.f, DeadZoneDegrees=3.f;
    FLunaGestureProfile Gesture;
    bool bUseDevice=false;
    bool bShowTrajectory=true;
};

UENUM(BlueprintType)
enum class ELunaPhase:uint8 { Menu, DeviceSetup, Playing, Paused, Complete };

UCLASS()
class LUNAMVP_API ALunaController:public APlayerController
{
    GENERATED_BODY()
public:
    ALunaController();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void PlayerTick(float Dt) override;
private:
    friend class ALunaGame;
    TSharedPtr<SLunaHUD> Screen;
};

UCLASS(Blueprintable)
class LUNAMVP_API ALunaGame:public AGameModeBase
{
    GENERATED_BODY()
public:
    ALunaGame();
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    void StartRun(bool DeviceMode,int32 FromStage=0,bool Practice=false,bool FreeFlight=false);
    void StartSelectedRun(int32 FromStage=0,bool Practice=false) { StartRun(PlayerControls.bUseDevice,FromStage,Practice); }
    void StartFreeFlight() { StartRun(PlayerControls.bUseDevice,0,false,true); }
    void SelectInputMode(bool bUseDevice);
    bool ShowsTrajectory() const { return PlayerControls.bShowTrajectory; }
    void ShowMenu();
    void TogglePause();
    void ToggleNavigationMap();
    bool PlotDestination(int32 BodyIndex);
    bool PlotPoint(const FVector& Point);
    void ClearRoute();
    void FaceDestination();
    bool HasNavigationTarget() const { return !bFreeFlight||Route.IsValidIndex(RouteLeg); }
    FVector LocalNavigationTarget() const;
    FVector LocalPathPoint(double T) const;
    void OpenControlSettings();
    void CloseControlSettings(bool bApply);
    FVector2D ControlCommand(const FLunaPlayerControls& Controls) const;
    void ResetCheckpoint();
    void RequestCenter();
    void ShiftLayer(int32 Direction, bool bFromDevice=false);
    void UpdateControls(ALunaController& PC,float Dt);
    void Notice(FString Text,float Seconds=2.5f);
    FString Task()const;
    FString StageName()const;
    float Progress()const;
    void Snapshot(FString Name);
    void Speak(const FString& Text,bool Interrupt=false);
    FVector NavigationTarget()const;
    FVector NavigationPathPoint(double T) const;
    FString Subtitle;
    float SubtitleRemaining=0;
    bool bMuted=false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) ELunaPhase Phase=ELunaPhase::Menu;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Stage=0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 Gate=0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 HeightLesson=0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bDeviceMode=false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bSafetyLocked=false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bInTransfer=false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bFreeFlight=false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) bool bMapOpen=false;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<ALunaPawn> Moon;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<ALunaCourse> Course;
    FLunaDevice Device;
    FLunaSolarSystem SolarSystem;
    TArray<FVector> Route;
    int32 RouteLeg=0,DestinationBody=INDEX_NONE;
    FString RouteStatus;
    bool bCruise=false;
    bool bControlSettingsOpen=false;
    FLunaPlayerControls DraftControls;
    FString ControlSettingsError;
    bool bDiagnostics=false,bWaitRelease=false,bVerify=false,bPractice=false;
    FString Message;
    float MessageRemaining=0,RunTime=0,DockHold=0;
    int32 Resets=0;
private:
    FLunaPlayerControls PlayerControls;
    void LoadControlSettings();
    bool SaveControlSettings(const FLunaPlayerControls& Controls);
    void ApplyInputMode(bool bUseDevice);
    void StartDepartureIfPending();
    FVector Checkpoint=FVector(0,0,300);
    int32 CheckpointLayer=0,CheckpointGate=0;
    bool bSecondCheckpoint=false,bWasFocused=true,bDeparturePending=false;
    float FailDelay=0;
    double StartedAt=0,VerifyPhaseTime=0;
    float VerifiedCampaignSeconds=0;
    int32 VerifyStep=0;
    TArray<FString> VerifyResults;
    UPROPERTY() TObjectPtr<class UAudioComponent> Narrator;
    UPROPERTY(Transient) TObjectPtr<class UAudioComponent> BackgroundMusic;
    TMap<FString,FString> VoiceAssets;
    FString LastGuidance;
    float ObstacleCueCooldown=0;
    bool bFieldShot=false,bPolarShot=false;
    void EnterStage(int32 NewStage,bool bTransfer=false);
    void FinishTraining();
    void LockDevice();
    void AdvanceCourse(float Dt);
    void AdvanceFreeFlight();
    void VerifyTick(float Dt);
    void WriteVerification(bool Pass);
};
