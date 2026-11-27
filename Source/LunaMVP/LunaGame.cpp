#include "LunaGame.h"
#include "LunaScale.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "LunaPawn.h"
#include "LunaCourse.h"
#include "LunaFlightPlan.h"
#include "LunaMotion.h"
#include "LunaHUD.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Components/SphereComponent.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "GameFramework/PlayerInput.h"
#include "InputKeyEventArgs.h"

using namespace LunaFlight;

ALunaController::ALunaController(){PrimaryActorTick.bCanEverTick=true;bShowMouseCursor=true;}
void ALunaController::BeginPlay()
{
    Super::BeginPlay();
    if(PlayerInput)PlayerInput->DebugExecBindings.RemoveAll([](const FKeyBind& Binding){return Binding.Key==EKeys::F1||Binding.Key==EKeys::F2;});
    if(IsLocalController()&&GEngine&&GEngine->GameViewport)
    {SAssignNew(Screen,SLunaHUD).Game(Cast<ALunaGame>(GetWorld()->GetAuthGameMode()));GEngine->GameViewport->AddViewportWidgetContent(Screen.ToSharedRef(),20);}
    FInputModeGameAndUI Mode;Mode.SetHideCursorDuringCapture(false);SetInputMode(Mode);
}
void ALunaController::EndPlay(const EEndPlayReason::Type Reason)
{if(Screen.IsValid()&&GEngine&&GEngine->GameViewport)GEngine->GameViewport->RemoveViewportWidgetContent(Screen.ToSharedRef());Screen.Reset();Super::EndPlay(Reason);}
void ALunaController::PlayerTick(float Dt)
{Super::PlayerTick(Dt);if(auto* G=Cast<ALunaGame>(GetWorld()->GetAuthGameMode()))G->UpdateControls(*this,Dt);}

ALunaGame::ALunaGame()
{
    PrimaryActorTick.bCanEverTick=true;DefaultPawnClass=ALunaPawn::StaticClass();PlayerControllerClass=ALunaController::StaticClass();
}
void ALunaGame::BeginPlay()
{
    Super::BeginPlay();StartedAt=FPlatformTime::Seconds();if(GEngine)GEngine->Exec(GetWorld(),TEXT("DisableAllScreenMessages"));
    for(TActorIterator<ALunaCourse> It(GetWorld());It;++It){Course=*It;break;}
    if(!Course)Course=GetWorld()->SpawnActor<ALunaCourse>();
    if(auto* PC=GetWorld()->GetFirstPlayerController())Moon=Cast<ALunaPawn>(PC->GetPawn());
    if(!Moon){Moon=GetWorld()->SpawnActor<ALunaPawn>(FVector(0,0,300),FRotator::ZeroRotator);if(auto* PC=GetWorld()->GetFirstPlayerController())PC->Possess(Moon);}
    Moon->SetFlightFrame(Course->GetActorTransform());
    if (!SolarSystem.Load(Moon->FlightFrame()))
        UE_LOG(LogTemp,Warning,TEXT("LUNA_SOLAR_CATALOG not available"));
    Device.Open(*GetDefault<ULunaSettings>());
    bVerify=FParse::Param(FCommandLine::Get(),TEXT("LunaVerify"));
    if (!bVerify) LoadControlSettings();
    if (FParse::Param(FCommandLine::Get(),TEXT("LunaTiltPlay"))) PlayerControls.bUseDevice=true;
    Narrator=NewObject<UAudioComponent>(this,TEXT("MissionNarrator"));Narrator->bIsUISound=true;Narrator->RegisterComponent();
    FString VoiceJson;if(FFileHelper::LoadFileToString(VoiceJson,*(FPaths::ProjectContentDir()/TEXT("LunaArt/Data/narration.json"))))
    {TSharedPtr<FJsonObject> O;if(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(VoiceJson),O))for(const auto& P:O->Values)VoiceAssets.Add(FString(P.Key.ToView()),P.Value->AsString());}
    if (USoundWave* Music = LoadObject<USoundWave>(nullptr, TEXT("/Game/LunaArt/Audio/SW_Luna_QuietMeridian.SW_Luna_QuietMeridian")))
    {
        BackgroundMusic = NewObject<UAudioComponent>(this, TEXT("LunaBackgroundMusic"));
        BackgroundMusic->bAutoActivate = false;
        BackgroundMusic->bAllowSpatialization = false;
        BackgroundMusic->bIsUISound = true;
        BackgroundMusic->SetSound(Music);
        BackgroundMusic->SetVolumeMultiplier(0.30f);
        BackgroundMusic->RegisterComponent();
        BackgroundMusic->Play();
        UE_LOG(LogTemp, Display, TEXT("LUNA_BGM asset=%s duration=%.3f looping=%d playing=%d"),
            *Music->GetName(), Music->Duration, Music->bLooping, BackgroundMusic->IsPlaying());
    }
    ShowMenu();UE_LOG(LogTemp,Display,TEXT("LUNA_MVP_READY version=2.1-orbital-instruments input_port=%d"),GetDefault<ULunaSettings>()->InputPort);
}
void ALunaGame::EndPlay(const EEndPlayReason::Type Reason)
{
    if (BackgroundMusic) BackgroundMusic->Stop();
    Device.Close();
    Super::EndPlay(Reason);
}
void ALunaGame::Speak(const FString& Text,bool Interrupt)
{
    if(Text.IsEmpty())return;
    if(!Interrupt&&Narrator&&Narrator->IsPlaying())return;
    const auto* Path=VoiceAssets.Find(Text);
    if(!Path){UE_LOG(LogTemp,Warning,TEXT("LUNA_VOICE_MISSING %s"),*Text);return;}
    if(auto* Voice=LoadObject<USoundBase>(nullptr,**Path))
    {
        Subtitle=Text;SubtitleRemaining=FMath::Max(2.f,Voice->Duration+.35f);
        if(Narrator){Narrator->Stop();Narrator->SetSound(Voice);Narrator->SetVolumeMultiplier(bMuted?0.f:.85f);Narrator->Play();}
        UE_LOG(LogTemp,Display,TEXT("LUNA_NARRATION %s duration=%.2f"),*Text,Voice->Duration);
    }
}
void ALunaGame::Notice(FString Text,float Seconds){Message=MoveTemp(Text);MessageRemaining=Seconds;Speak(Message,true);}
FVector ALunaGame::LocalNavigationTarget()const
{
    if (bFreeFlight) return Route.IsValidIndex(RouteLeg)?Route[RouteLeg]:Moon->FlightPosition();
    if (bInTransfer) return {Stage==1?LateralStartX:PolarStartX,0,300.0+200.0*Moon->Movement->TargetLayer};
    if (Stage==0) return {BrakeX,0,300};
    if (Stage==2&&HeightLesson<2) return {PolarPracticeX,0,HeightLesson==0?500.0:300.0};
    if (Gate>=GateCount) return Stage==1?FVector(LateralFinishX,0,300):Arrival;
    FVector Target=Stage==1?LateralGates[Gate]:PolarGates[Gate];
    const FVector Position=Moon->FlightPosition();
    if (Position.X>=Target.X&&FMath::Abs(Position.Y-Target.Y)<(Stage==1?100.:170.)
        &&FMath::Abs(Position.Z-Target.Z)<45.)
    {
        Target.X+=Stage==1?130.:185.;
    }
    return Target;
}

FVector ALunaGame::NavigationTarget() const
{
    return Moon->FlightFrame().TransformPositionNoScale(LocalNavigationTarget());
}

FVector ALunaGame::NavigationPathPoint(double T) const
{
    return Moon->FlightFrame().TransformPositionNoScale(LocalPathPoint(T));
}

FVector ALunaGame::LocalPathPoint(double T) const
{
    const FVector Start=Moon->FlightPosition();
    const FVector End=LocalNavigationTarget();
    if (bFreeFlight) return FMath::Lerp(Start,End,T);
    const FVector Delta=End-Start;
    const bool bAperture=!bInTransfer&&Gate<GateCount&&(Stage==1||(Stage==2&&HeightLesson==2));
    if (bAperture)
    {
        const FVector Opening=Stage==1?LateralGates[Gate]:PolarGates[Gate];
        const double ApproachX=Opening.X-(Stage==1?240.:270.);
        const bool bAligned=FMath::Abs(Start.Y-Opening.Y)<(Stage==1?90.:145.)
            && FMath::Abs(Start.Z-Opening.Z)<(Stage==1?45.:40.);
        if (!bAligned&&Start.X>ApproachX)
        {
            // Rejoin the open approach before turning through the asteroid wall.
            const FVector Entry(ApproachX,Opening.Y,Opening.Z);
            return T<.6
                ? FMath::Lerp(Start,Entry,FMath::SmoothStep(0.,.6,T))
                : FMath::Lerp(Entry,End,FMath::SmoothStep(.6,1.,T));
        }
        const double AlignBy=FMath::Clamp((ApproachX-Start.X)/FMath::Max(1.,Delta.X),.25,1.);
        const double Blend=FMath::SmoothStep(0.,AlignBy,T);
        return Start+FVector(Delta.X*T,Delta.Y*Blend,Delta.Z*Blend);
    }
    const double Bend=FMath::Min(85.,Delta.Size()*.10);
    const double Side=Start.Y>0.?-1.:1.;
    return Start+Delta*T+FVector(0.,Side*Bend*FMath::Sin(PI*T),0.);
}
void ALunaGame::ShowMenu()
{
    if(Narrator)Narrator->Stop();Subtitle.Empty();SubtitleRemaining=0;LastGuidance.Empty();
    Phase=ELunaPhase::Menu;bControlSettingsOpen=false;bWaitRelease=true;bSafetyLocked=false;bDeparturePending=false;
    bInTransfer=false;bDeviceMode=PlayerControls.bUseDevice;ControlSettingsError.Reset();Device.InvalidateCenter();
    bFreeFlight=false;bMapOpen=false;bCruise=false;ClearRoute();
    Course->SetTrainingActive(true);
    if(Moon){Moon->SetFlightPosition({0,0,PlaneZ});Moon->SetFreeView(false);Moon->Movement->ResetMotion();Moon->Movement->bLocked=true;Moon->ResetView(true);}
    if(auto* PC=GetWorld()->GetFirstPlayerController())PC->bShowMouseCursor=true;
}
void ALunaGame::StartRun(bool UseDevice,int32 FromStage,bool Practice,bool FreeFlight)
{
    bDeviceMode=UseDevice;bPractice=Practice;RunTime=0;Resets=0;bSecondCheckpoint=false;FailDelay=0;MessageRemaining=0;
    bFreeFlight=FreeFlight;bMapOpen=false;bCruise=false;ClearRoute();
    Course->SetTrainingActive(!bFreeFlight);
    Moon->SetFreeView(bFreeFlight);
    Moon->Movement->FlightSpeedLimit=0.;
    if (Narrator) Narrator->Stop();
    SubtitleRemaining=0;
    EnterStage(FMath::Clamp(FromStage,0,2));bWaitRelease=true;Device.InvalidateCenter();
    bDeparturePending=Stage==0&&!bVerify&&!bFreeFlight;
    if (UseDevice)
    {
        Phase=ELunaPhase::DeviceSetup;
        LockDevice();
        if (bDeparturePending) Moon->ResetView(true);
        Notice(TEXT("Connect controller. Press C to center."),3);
    }
    else
    {
        Phase=ELunaPhase::Playing;
        bSafetyLocked=false;
        StartDepartureIfPending();
    }
    LastGuidance.Empty();
    if (bFreeFlight) UE_LOG(LogTemp,Display,TEXT("LUNA_FREE_FLIGHT bodies=%d local_height=%.4f"),
        SolarSystem.Bodies.Num(),Moon->FlightPosition().Z-PlaneZ);
    if(auto* PC=GetWorld()->GetFirstPlayerController())PC->bShowMouseCursor=UseDevice;
}
void ALunaGame::StartDepartureIfPending()
{
    if (!bDeparturePending) return;
    bDeparturePending=false;
    Moon->BeginDeparture();
    Speak(TEXT("Engines ready. Leaving the lunar surface."),true);
}
void ALunaGame::EnterStage(int32 NewStage,bool bTransfer)
{
    bInTransfer=bTransfer;
    Stage=NewStage;Gate=0;CheckpointGate=0;HeightLesson=0;CheckpointLayer=0;DockHold=0;
    if (bTransfer)
    {
        Checkpoint={Stage==1?BrakeX:LateralFinishX+160.,0,300};
        if (Narrator) Narrator->Stop();
        SubtitleRemaining=0;MessageRemaining=0;LastGuidance.Empty();
    }
    else
    {
        Checkpoint={Stage==0?0.:Stage==1?LateralStartX:PolarStartX,0,300};
        Moon->SetFlightPosition(Checkpoint);Moon->Movement->ResetMotion();Moon->Movement->TargetLayer=0;Moon->Movement->bLocked=true;Moon->ResetView();
    }
    Course->SetDoors(Stage,false);
    UE_LOG(LogTemp,Display,TEXT("LUNA_STAGE %d"),Stage+1);
}
void ALunaGame::ResetCheckpoint()
{
    if(!Moon||Phase==ELunaPhase::Menu||Phase==ELunaPhase::Complete)return;
    Moon->Movement->bLocked=true;Moon->Movement->ResetMotion();Moon->SetFlightPosition(Checkpoint);
    Moon->ResetView();
    Moon->Movement->TargetLayer=CheckpointLayer;Gate=CheckpointGate;DockHold=0;FailDelay=0;++Resets;bWaitRelease=true;
    Device.ClearEvents();if(bDeviceMode)LockDevice();
    if (bFreeFlight)
    {
        ClearRoute();
        RouteStatus=TEXT("Returned to departure");
        Moon->SetFreeView(true);
    }
    else Notice(TEXT("Checkpoint restored. Release controls to resume."));
}
void ALunaGame::LockDevice(){bSafetyLocked=true;Device.InvalidateCenter();if(Moon){Moon->Movement->ResetMotion();Moon->Movement->bLocked=true;}}
void ALunaGame::RequestCenter()
{
    if(!bDeviceMode||(Phase!=ELunaPhase::Playing&&Phase!=ELunaPhase::DeviceSetup))return;
    LockDevice();Device.RequestCenter(FPlatformTime::Seconds());Notice(TEXT("Hold controller still. Centering."));
}
void ALunaGame::TogglePause()
{
    if (Phase==ELunaPhase::Playing)
    {
        Phase=ELunaPhase::Paused;
        Moon->Movement->ResetMotion();
        Moon->Movement->bLocked=true;
        Device.ClearEvents();
        if (bDeviceMode) LockDevice();
    }
    else if (Phase==ELunaPhase::Paused)
    {
        Phase=bDeviceMode&&bSafetyLocked?ELunaPhase::DeviceSetup:ELunaPhase::Playing;
        bWaitRelease=true;
        if (bDeviceMode) Notice(TEXT("Press C to center and resume."));
    }
    else return;
    if (auto* PC=GetWorld()->GetFirstPlayerController()) PC->bShowMouseCursor=Phase!=ELunaPhase::Playing;
}

void ALunaGame::ToggleNavigationMap()
{
    if (bControlSettingsOpen||Phase==ELunaPhase::Menu||Phase==ELunaPhase::Complete) return;
    bMapOpen=!bMapOpen;
    bWaitRelease=true;
    Moon->Movement->ResetMotion();
    Moon->Movement->bLocked=true;
    Device.ClearEvents();
    if (auto* PC=GetWorld()->GetFirstPlayerController()) PC->bShowMouseCursor=bMapOpen||Phase!=ELunaPhase::Playing;
}

void ALunaGame::ClearRoute()
{
    Route.Reset();
    RouteLeg=0;
    DestinationBody=INDEX_NONE;
    RouteStatus.Reset();
}

bool ALunaGame::PlotPoint(const FVector& Point)
{
    if (!bFreeFlight) return false;
    TArray<FVector> Plan;
    if (!SolarSystem.PlanRoute(Moon->FlightPosition(),Point,Plan))
    {
        RouteStatus=TEXT("Choose a clear point outside the exclusion zones.");
        return false;
    }
    Route=MoveTemp(Plan);
    RouteLeg=0;
    DestinationBody=INDEX_NONE;
    RouteStatus=TEXT("Waypoint plotted");
    return true;
}

bool ALunaGame::PlotDestination(int32 BodyIndex)
{
    if (!SolarSystem.Bodies.IsValidIndex(BodyIndex)) return false;
    if (!PlotPoint(SolarSystem.Approach(BodyIndex,Moon->FlightPosition()))) return false;
    DestinationBody=BodyIndex;
    RouteStatus=SolarSystem.Bodies[BodyIndex].Name+TEXT(" approach plotted");
    return true;
}

void ALunaGame::FaceDestination()
{
    if (!bFreeFlight) return;
    const FVector Position=Moon->FlightPosition();
    if (HasNavigationTarget())
        Moon->FaceFlightDirection(LocalNavigationTarget()-Position);
    else if (SolarSystem.Bodies.IsValidIndex(DestinationBody))
        Moon->FaceFlightDirection(SolarSystem.Bodies[DestinationBody].Position-Position);
}

void ALunaGame::AdvanceFreeFlight()
{
    const FVector Position=Moon->FlightPosition();
    double Limit=bCruise?1800000.:6000.;
    Limit=FMath::Min(Limit,FMath::Max(480.,SolarSystem.Clearance(Position)*.8));
    if (Route.IsValidIndex(RouteLeg))
    {
        const double Distance=FVector::Dist2D(Position,Route[RouteLeg]);
        Limit=FMath::Min(Limit,FMath::Max(280.,Distance*.72));
        if (Distance<90. && ++RouteLeg==Route.Num())
        {
            Moon->Movement->ResetMotion();
            RouteStatus=TEXT("Approach reached");
        }
    }
    Moon->Movement->FlightSpeedLimit=Limit;
}

namespace
{
    constexpr const TCHAR* ControlSection=TEXT("Luna.Controls");
    FString ControlSettingsPath(bool bVerify)
    {
        return FPaths::ProjectSavedDir()/(bVerify?TEXT("Verification/LunaControls.ini"):TEXT("Config/LunaControls.ini"));
    }
}

void ALunaGame::LoadControlSettings()
{
    FConfigFile Config;
    Config.Read(ControlSettingsPath(bVerify));
    Config.GetFloat(ControlSection,TEXT("ForwardGain"),PlayerControls.ForwardGain);
    Config.GetFloat(ControlSection,TEXT("RightGain"),PlayerControls.RightGain);
    Config.GetFloat(ControlSection,TEXT("DeadZoneDegrees"),PlayerControls.DeadZoneDegrees);
    Config.GetBool(ControlSection,TEXT("UseNanoRP2040"),PlayerControls.bUseDevice);
    Config.GetBool(ControlSection,TEXT("ShowTrajectory"),PlayerControls.bShowTrajectory);
    auto& Gesture=PlayerControls.Gesture;
    Config.GetFloat(ControlSection,TEXT("GestureThresholdMps2"),Gesture.ThresholdMps2);
    Config.GetInt(ControlSection,TEXT("GestureHoldMs"),Gesture.HoldMs);
    Config.GetInt(ControlSection,TEXT("GestureQuietMs"),Gesture.QuietMs);
    Config.GetInt(ControlSection,TEXT("GestureCooldownMs"),Gesture.CooldownMs);

    PlayerControls.ForwardGain=FMath::Clamp(PlayerControls.ForwardGain,0.5f,2.f);
    PlayerControls.RightGain=FMath::Clamp(PlayerControls.RightGain,0.5f,2.f);
    PlayerControls.DeadZoneDegrees=FMath::Clamp(PlayerControls.DeadZoneDegrees,0.f,12.f);
    Gesture.ThresholdMps2=FMath::Clamp(Gesture.ThresholdMps2,1.5f,8.f);
    Gesture.HoldMs=FMath::Clamp(Gesture.HoldMs,20,120);
    Gesture.QuietMs=FMath::Clamp(Gesture.QuietMs,100,600);
    Gesture.CooldownMs=FMath::Clamp(Gesture.CooldownMs,200,1000);
    if (!bVerify) Device.SetGestureProfile(Gesture);
}

void ALunaGame::OpenControlSettings()
{
    if (Phase!=ELunaPhase::Menu&&Phase!=ELunaPhase::Paused&&Phase!=ELunaPhase::DeviceSetup) return;
    DraftControls=PlayerControls;
    ControlSettingsError.Reset();
    bControlSettingsOpen=true;
    Device.ClearEvents();
}

void ALunaGame::CloseControlSettings(bool bApply)
{
    if (bApply)
    {
        if (!SaveControlSettings(DraftControls)) return;
        PlayerControls=DraftControls;
        if (!bVerify) Device.SetGestureProfile(PlayerControls.Gesture);
        ApplyInputMode(PlayerControls.bUseDevice);
    }
    Device.ClearEvents();
    bControlSettingsOpen=false;
}

bool ALunaGame::SaveControlSettings(const FLunaPlayerControls& Controls)
{
    const auto& Gesture=Controls.Gesture;
    const FString Path=ControlSettingsPath(bVerify);
    FConfigFile Config;
    Config.SetFloat(ControlSection,TEXT("ForwardGain"),Controls.ForwardGain);
    Config.SetFloat(ControlSection,TEXT("RightGain"),Controls.RightGain);
    Config.SetFloat(ControlSection,TEXT("DeadZoneDegrees"),Controls.DeadZoneDegrees);
    Config.SetBool(ControlSection,TEXT("UseNanoRP2040"),Controls.bUseDevice);
    Config.SetBool(ControlSection,TEXT("ShowTrajectory"),Controls.bShowTrajectory);
    Config.SetFloat(ControlSection,TEXT("GestureThresholdMps2"),Gesture.ThresholdMps2);
    Config.SetString(ControlSection,TEXT("GestureHoldMs"),*FString::FromInt(Gesture.HoldMs));
    Config.SetString(ControlSection,TEXT("GestureQuietMs"),*FString::FromInt(Gesture.QuietMs));
    Config.SetString(ControlSection,TEXT("GestureCooldownMs"),*FString::FromInt(Gesture.CooldownMs));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    if (Config.Write(Path,false))
    {
        ControlSettingsError.Reset();
        return true;
    }
    ControlSettingsError=TEXT("Could not save control settings");
    return false;
}

void ALunaGame::SelectInputMode(bool bUseDevice)
{
    if (PlayerControls.bUseDevice!=bUseDevice)
    {
        FLunaPlayerControls Updated=PlayerControls;
        Updated.bUseDevice=bUseDevice;
        if (!SaveControlSettings(Updated)) return;
        PlayerControls=Updated;
    }
    ApplyInputMode(bUseDevice);
}

void ALunaGame::ApplyInputMode(bool bUseDevice)
{
    if (bDeviceMode==bUseDevice) return;
    bDeviceMode=bUseDevice;
    LastGuidance.Empty();
    Device.InvalidateCenter();
    if (Phase==ELunaPhase::Menu||Phase==ELunaPhase::Complete) return;

    Moon->Movement->ResetMotion();
    Moon->Movement->bLocked=true;
    bWaitRelease=true;
    bSafetyLocked=bUseDevice;
    if (Phase!=ELunaPhase::Paused)
    {
        Phase=bUseDevice?ELunaPhase::DeviceSetup:ELunaPhase::Playing;
        if (auto* PC=GetWorld()->GetFirstPlayerController()) PC->bShowMouseCursor=bUseDevice;
    }
    Notice(bUseDevice?TEXT("Controller selected. Press C to center."):TEXT("Keyboard selected."));
    if (Phase==ELunaPhase::Playing) StartDepartureIfPending();
}

FVector2D ALunaGame::ControlCommand(const FLunaPlayerControls& Controls) const
{
    const FVector2D Input=Device.bHasTiltDegrees?FVector2D(Device.Pitch,Device.Roll):Device.Move;
    return LunaMotion::DeviceTilt(Input,Device.bHasTiltDegrees,Controls.DeadZoneDegrees,
        GetDefault<ULunaSettings>()->FullTiltDegrees,{Controls.ForwardGain,Controls.RightGain});
}
void ALunaGame::ShiftLayer(int32 Direction, bool bFromDevice)
{
    if(Phase!=ELunaPhase::Playing||bSafetyLocked||bWaitRelease||!Moon)return;
    if(Stage<2&&!bFreeFlight){Notice(TEXT("Maintain altitude. Polar control follows in sector three."));return;}
    const int32 Old=Moon->Movement->TargetLayer;Moon->Movement->TargetLayer=FMath::Clamp(Old+Direction,-1,1);if(Old!=Moon->Movement->TargetLayer)Moon->Movement->StableTime=0;
    if (bFromDevice && Old!=Moon->Movement->TargetLayer)
        UE_LOG(LogTemp, Display, TEXT("LUNA_ORBIT_STEP direction=%+d layer=%+d"), Direction, Moon->Movement->TargetLayer);
    Notice(Old==Moon->Movement->TargetLayer?TEXT("Altitude limit reached."):Direction>0?TEXT("Raising orbit."):TEXT("Lowering orbit."),1.5f);
}
void ALunaGame::UpdateControls(ALunaController& PC,float Dt)
{
    if(!Moon)return;
    if(bFreeFlight&&Course) Course->CenterStarfield(Moon->GetActorLocation());
    const bool Focused=!FSlateApplication::IsInitialized()||FSlateApplication::Get().IsActive();
    if(!bVerify&&!Focused&&bWasFocused&&Phase==ELunaPhase::Playing){TogglePause();Notice(TEXT("Flight paused."));}
    bWasFocused=Focused;
    if (bControlSettingsOpen)
    {
        if (PC.WasInputKeyJustPressed(EKeys::Escape)) CloseControlSettings(false);
        else if (PC.WasInputKeyJustPressed(EKeys::Enter)) CloseControlSettings(true);
        return;
    }
    if (bMapOpen)
    {
        if (PC.WasInputKeyJustPressed(EKeys::Tab)||PC.WasInputKeyJustPressed(EKeys::Escape)) ToggleNavigationMap();
        return;
    }
    if (PC.WasInputKeyJustPressed(EKeys::O)&&
        (Phase==ELunaPhase::Menu||Phase==ELunaPhase::Paused||Phase==ELunaPhase::DeviceSetup))
    {
        OpenControlSettings();
        return;
    }
    if(PC.WasInputKeyJustPressed(EKeys::F1))bDiagnostics=!bDiagnostics;
    if(PC.WasInputKeyJustPressed(EKeys::V)){bMuted=!bMuted;if(Narrator)Narrator->SetVolumeMultiplier(bMuted?0.f:.85f);}
    if(PC.WasInputKeyJustPressed(EKeys::M)){ShowMenu();return;}
    if(Phase==ELunaPhase::Menu||Phase==ELunaPhase::Complete)
    {
        if (PC.WasInputKeyJustPressed(EKeys::G)) StartFreeFlight();
        else if (PC.WasInputKeyJustPressed(EKeys::I)) { SelectInputMode(true);StartSelectedRun(); }
        else if (PC.WasInputKeyJustPressed(EKeys::K)) { SelectInputMode(false);StartSelectedRun(); }
        else if (PC.WasInputKeyJustPressed(EKeys::Enter)) StartSelectedRun();
        else if (PC.WasInputKeyJustPressed(EKeys::One)) StartSelectedRun(0,true);
        else if (PC.WasInputKeyJustPressed(EKeys::Two)) StartSelectedRun(1,true);
        else if (PC.WasInputKeyJustPressed(EKeys::Three)) StartSelectedRun(2,true);
        return;
    }
    if (PC.WasInputKeyJustPressed(EKeys::Tab)) { ToggleNavigationMap();return; }
    if(PC.WasInputKeyJustPressed(EKeys::P)||PC.WasInputKeyJustPressed(EKeys::Escape))TogglePause();
    if(PC.WasInputKeyJustPressed(EKeys::C))RequestCenter();
    if(PC.WasInputKeyJustPressed(EKeys::R))ResetCheckpoint();
    if(PC.WasInputKeyJustPressed(EKeys::F2))SelectInputMode(!bDeviceMode);
    if (bFreeFlight&&Phase==ELunaPhase::Playing)
    {
        if (PC.WasInputKeyJustPressed(EKeys::SpaceBar)) bCruise=!bCruise;
        if (PC.WasInputKeyJustPressed(EKeys::F)) FaceDestination();
        const double Yaw=(PC.IsInputKeyDown(EKeys::Right)?1.:0.)-(PC.IsInputKeyDown(EKeys::Left)?1.:0.);
        const double Pitch=(PC.IsInputKeyDown(EKeys::Down)?1.:0.)-(PC.IsInputKeyDown(EKeys::Up)?1.:0.);
        if (Yaw!=0.||Pitch!=0.) Moon->RotateView(Yaw*62.*Dt,Pitch*35.*Dt);
    }
    const bool Held=PC.IsInputKeyDown(EKeys::W)||PC.IsInputKeyDown(EKeys::A)||PC.IsInputKeyDown(EKeys::S)||PC.IsInputKeyDown(EKeys::D)||PC.IsInputKeyDown(EKeys::Q)||PC.IsInputKeyDown(EKeys::E);
    if(bWaitRelease&&!Held)bWaitRelease=false;
    if(Phase!=ELunaPhase::Playing||bWaitRelease||bSafetyLocked||bVerify||Moon->IsDeparting()){if(!bVerify)Moon->Movement->Command=FVector2D::ZeroVector;return;}
    if(!bDeviceMode)
    {
        Moon->Movement->Command=FVector2D((PC.IsInputKeyDown(EKeys::W)?1.:0.)-(PC.IsInputKeyDown(EKeys::S)?1.:0.),(PC.IsInputKeyDown(EKeys::D)?1.:0.)-(PC.IsInputKeyDown(EKeys::A)?1.:0.)).GetClampedToMaxSize(1.);
    }
    if (!bDeviceMode || !Device.bGatewayGestureProfile)
    {
        if(PC.WasInputKeyJustPressed(EKeys::Q))ShiftLayer(1);
        if(PC.WasInputKeyJustPressed(EKeys::E))ShiftLayer(-1);
    }
}
void ALunaGame::Tick(float Dt)
{
    Super::Tick(Dt);const double Now=FPlatformTime::Seconds();Device.Poll(Now);MessageRemaining=FMath::Max(0.f,MessageRemaining-Dt);
    if(!Moon)return;
    SubtitleRemaining=FMath::Max(0.f,SubtitleRemaining-Dt);
    ObstacleCueCooldown=FMath::Max(0.f,ObstacleCueCooldown-Dt);
    if(Phase==ELunaPhase::Playing&&Moon->Movement->BlockedTime>1&&ObstacleCueCooldown<=0)
    {Notice(bFreeFlight?TEXT("Exclusion zone. Adjust your heading."):TEXT("Path blocked. Change course, or press R to reset."),4);ObstacleCueCooldown=12;}
    if(Phase==ELunaPhase::Playing&&!bControlSettingsOpen&&!bMapOpen&&!bFreeFlight&&!Moon->IsDeparting())
    {
        FString Guidance=Task();
        if(Guidance!=LastGuidance&&(!Narrator||!Narrator->IsPlaying())){Speak(Guidance);LastGuidance=Guidance;}
    }
    if(bDeviceMode&&!bControlSettingsOpen&&!bMapOpen&&(Phase==ELunaPhase::Playing||Phase==ELunaPhase::DeviceSetup))
    {
        if(!Device.Healthy(Now)||!Device.bCentered)
        {if(!bSafetyLocked)LockDevice();}
        else if(bSafetyLocked)
        {
            bSafetyLocked=false;Moon->Movement->TargetLayer=FMath::Clamp(FMath::RoundToInt((Moon->FlightPosition().Z-300)/200),-1,1);
            Phase=ELunaPhase::Playing;bWaitRelease=false;Notice(TEXT("Controller centered. Flight control restored."));
            UE_LOG(LogTemp, Display, TEXT("LUNA_CONTROLLER_READY source_seq=%lld"), Device.SourceSeq);
            if(auto* PC=GetWorld()->GetFirstPlayerController())PC->bShowMouseCursor=false;
            StartDepartureIfPending();
        }
        if (!bSafetyLocked&&Phase==ELunaPhase::Playing)
        {
            Moon->Movement->Command=ControlCommand(PlayerControls);
            bool bUp=false,bDown=false;
            Device.ConsumeGesture(bUp,bDown);
            if (!bWaitRelease&&FailDelay<=0&&!Moon->IsDeparting())
            {
                if (bUp) ShiftLayer(1, true);
                if (bDown) ShiftLayer(-1, true);
            }
        }
    }
    const bool Locked=Phase!=ELunaPhase::Playing||bSafetyLocked||bWaitRelease||bMapOpen||FailDelay>0||Moon->IsDeparting();
    Moon->Movement->bLocked=Locked;
    if(Phase==ELunaPhase::Playing&&!Locked)
    {
        RunTime+=FMath::Min(Dt,.1f);
        if (bFreeFlight) AdvanceFreeFlight();
        else
        {
            AdvanceCourse(Dt);
            const FVector P=Moon->FlightPosition();
            if(FMath::Abs(P.Y)>500||P.Z<32||P.Z>850||P.X< -200||P.X>CorridorEndX)
            {FailDelay=.5f;Moon->Movement->ResetMotion();Moon->Movement->bLocked=true;Notice(TEXT("Outside flight corridor. Restoring checkpoint."));}
        }
    }
    if(FailDelay>0){FailDelay-=Dt;if(FailDelay<=0)ResetCheckpoint();}
    if(bVerify)VerifyTick(Dt);
}
void ALunaGame::AdvanceCourse(float Dt)
{
    const FVector P=Moon->FlightPosition();const float Speed=Moon->FlightVelocity().Size2D();
    if (bInTransfer)
    {
        const double NextLessonX=Stage==1?LateralStartX:PolarStartX;
        if (P.X<NextLessonX) return;
        bInTransfer=false;
        Checkpoint={NextLessonX,0,300};
        if (Stage==1) Notice(TEXT("Braking complete. Enter the asteroid corridor."),3);
        return;
    }
    if(Stage==0)
    {
        if(P.X>=1200&&!bSecondCheckpoint){Checkpoint={1240,0,300};bSecondCheckpoint=true;Notice(TEXT("Checkpoint set. Reverse thrust to brake."));}
        const bool Docked=FMath::Abs(P.X-BrakeX)<=BrakeHalfDepth&&FMath::Abs(P.Y)<=250&&Speed<40;
        DockHold=Docked?DockHold+FMath::Min(Dt,.1f):0;
        if(DockHold>=1.f)
        {
            if(bPractice){FinishTraining();return;}
            EnterStage(1,true);
            if(bVerify)Snapshot(TEXT("02_lateral"));
        }
    }
    else if(Stage==1)
    {
        if (Gate<GateCount&&P.X>LateralGates[Gate].X+85&&FMath::Abs(P.Y-LateralGates[Gate].Y)<100)
        {
            Checkpoint=LateralGates[Gate]+FVector(170,0,0);
            CheckpointGate=++Gate;
            Notice(FString::Printf(TEXT("Gate %d clear."),Gate),1.7f);
        }
        if (Gate==GateCount&&P.X>=LateralFinishX)
        {
            if(bPractice){FinishTraining();return;}
            EnterStage(2,true);
            if(bVerify)Snapshot(TEXT("03_height"));
        }
    }
    else
    {
        if(HeightLesson==0&&Moon->Movement->TargetLayer==1&&Moon->Movement->Stable())
        {
            HeightLesson=1;
            if (!bDeviceMode) Notice(TEXT("Upper orbit reached. Press E to return."));
        }
        else if(HeightLesson==1&&Moon->Movement->TargetLayer==0&&Moon->Movement->Stable()){HeightLesson=2;Course->SetDoors(Stage,true);Notice(TEXT("Polar control ready. Follow the altitude markers."));}
        if(HeightLesson==2&&Gate<GateCount&&P.X>PolarGates[Gate].X+140&&FMath::Abs(P.Y)<170&&FMath::Abs(P.Z-PolarGates[Gate].Z)<45)
        {
            Checkpoint=PolarGates[Gate]+FVector(220,0,0);
            CheckpointLayer=PolarLayers[Gate];
            CheckpointGate=++Gate;
            Notice(FString::Printf(TEXT("Orbit gate %d clear."),Gate),1.7f);
        }
        if(HeightLesson==2&&Gate==GateCount&&P.X>=ArrivalX)
        {FinishTraining();}
    }
}
void ALunaGame::FinishTraining()
{Phase=ELunaPhase::Complete;Moon->SetFlightPosition({0,0,PlaneZ});Moon->Movement->ResetMotion();Moon->Movement->bLocked=true;Moon->ResetView(false);Speak(TEXT("Flight complete. Luna is on course."),true);Device.ClearEvents();if(auto* PC=GetWorld()->GetFirstPlayerController())PC->bShowMouseCursor=true;UE_LOG(LogTemp,Display,TEXT("LUNA_COMPLETE seconds=%.2f resets=%d practice=%d"),RunTime,Resets,bPractice);}
FString ALunaGame::StageName()const
{return bFreeFlight?TEXT("Free navigation"):bInTransfer?TEXT("Free flight"):Stage==0?TEXT("Braking"):Stage==1?TEXT("Lateral control"):TEXT("Polar control");}
FString ALunaGame::Task()const
{
    if(bSafetyLocked)return TEXT("Controls locked. Connect controller and press C.");
    if(Phase==ELunaPhase::Paused)return bDeviceMode?TEXT("Press P to resume, then C to center."):TEXT("Press P to resume. Release flight controls.");
    if (bFreeFlight)
        return HasNavigationTarget()?RouteStatus:TEXT("Tab  Map     /     Space  Cruise     /     Arrow keys  View");
    // Inter-lesson flight leaves the narration channel available for future story cues.
    if(bInTransfer)return {};
    if (Stage==0)
    {
        const double X=Moon->FlightPosition().X;
        if (X<600) return bDeviceMode?TEXT("Tilt forward to accelerate."):TEXT("Hold W to accelerate.");
        if (X<1200) return bDeviceMode?TEXT("Return to level. Luna keeps drifting."):TEXT("Release W. Luna keeps drifting.");
        return bDeviceMode?TEXT("Tilt back to brake. Hold position inside the marker."):TEXT("Hold S to brake. Hold position inside the marker.");
    }
    if (Gate>=GateCount) return TEXT("Continue to the arrival marker.");
    if (Stage==1)
        return bDeviceMode?FString::Printf(TEXT("Tilt left or right. Pass through gate %d."),Gate+1)
            :FString::Printf(TEXT("Use A and D. Pass through gate %d."),Gate+1);
    const bool bUsePushGestures = bDeviceMode && (Device.bHasVerticalStep || Device.bGatewayGestureProfile);
    if (HeightLesson==0)
        return bUsePushGestures?TEXT("Push up to rise. Wait for orbital lock."):TEXT("Press Q to rise. Wait for orbital lock.");
    if (HeightLesson==1)
        return bUsePushGestures?TEXT("Push down to descend. Wait for orbital lock."):TEXT("Press E to descend. Wait for orbital lock.");
    const TCHAR* Layer=Gate==0?TEXT("Upper"):Gate==1?TEXT("Mid"):TEXT("Lower");
    return bUsePushGestures?FString::Printf(TEXT("%s orbit ahead. Push up to rise, down to descend."),Layer)
        :FString::Printf(TEXT("%s orbit ahead. Q to rise, E to descend."),Layer);
}
float ALunaGame::Progress()const
{if(Phase==ELunaPhase::Complete)return 1;return Moon?FMath::Clamp(float(Moon->FlightPosition().X/ArrivalX),0.f,1.f):0;}
void ALunaGame::Snapshot(FString Name)
{IFileManager::Get().MakeDirectory(*(FPaths::ProjectSavedDir()/TEXT("Verification/Screenshots")),true);FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir()/TEXT("Verification/Screenshots")/(Name+TEXT(".png")),true,false);}
void ALunaGame::WriteVerification(bool Pass)
{
    for(const auto& Result:VerifyResults)if(Result.StartsWith(TEXT("FAIL:")))Pass=false;
    TSharedPtr<FJsonObject> Report=MakeShared<FJsonObject>();Report->SetBoolField(TEXT("passed"),Pass);Report->SetNumberField(TEXT("stage"),Stage+1);Report->SetNumberField(TEXT("gate"),Gate);Report->SetNumberField(TEXT("run_seconds"),RunTime);Report->SetNumberField(TEXT("resets"),Resets);
    Report->SetNumberField(TEXT("campaign_seconds"),VerifiedCampaignSeconds);Report->SetBoolField(TEXT("verification_finished"),true);
    TArray<TSharedPtr<FJsonValue>> Checks;for(const auto& Result:VerifyResults)Checks.Add(MakeShared<FJsonValueString>(Result));Report->SetArrayField(TEXT("checks"),Checks);
    FString Text;auto Writer=TJsonWriterFactory<>::Create(&Text);FJsonSerializer::Serialize(Report.ToSharedRef(),Writer);
    FFileHelper::SaveStringToFile(Text,*(FPaths::ProjectSavedDir()/TEXT("Verification/runtime.json")));
    UE_LOG(LogTemp,Display,TEXT("LUNA_RUNTIME_VERIFICATION %s"),Pass?TEXT("PASS"):TEXT("FAIL"));
}
void ALunaGame::VerifyTick(float Dt)
{
    const double Elapsed=FPlatformTime::Seconds()-StartedAt;
    const auto ClickMap=[&](const FVector2D& Point,const FKey& Button)
    {
        const auto* PC=Cast<ALunaController>(GetWorld()->GetFirstPlayerController());
        if (!PC||!PC->Screen||!bMapOpen) return false;
        const FGeometry& Geometry=PC->Screen->GetCachedGeometry();
        const FVector2D Size=Geometry.GetLocalSize();
        const double Scale=FMath::Min(Size.X/1280.,Size.Y/720.);
        const FVector2D At=Geometry.LocalToAbsolute((Size-FVector2D(1280.,720.)*Scale)*.5+Point*Scale);
        const TSet<FKey> Pressed{Button};
        const FPointerEvent Mouse(0,At,At,Pressed,Button,0.f,FModifierKeysState());
        return PC->Screen->OnMouseButtonDown(Geometry,Mouse).IsEventHandled();
    };
    if(VerifyStep==90&&Elapsed>VerifyPhaseTime+1)
    {StartRun(false,0,true);bWaitRelease=false;Moon->SetFlightPosition({BrakeX,0,PlaneZ});Moon->ResetView();VerifyStep=91;return;}
    if(VerifyStep==91&&Phase==ELunaPhase::Complete)
    {
        VerifyResults.Add(bPractice&&Stage==0?TEXT("PASS: single-stage practice stops at its own finish"):TEXT("FAIL: practice progression"));
        Snapshot(TEXT("05_practice_complete"));
        VerifyStep=92;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==92&&Elapsed>VerifyPhaseTime+.3)
    {
        const int32 Sun=SolarSystem.Find(TEXT("Sun")),Earth=SolarSystem.Find(TEXT("Earth"));
        const bool bPlane=SolarSystem.Bodies.IsValidIndex(Sun)
            && FMath::Abs(SolarSystem.Bodies[Sun].Position.Z-PlaneZ)<.01
            && FMath::Abs(Moon->FlightPosition().Z-PlaneZ)<.01;
        const bool bSingleLuna=!SolarSystem.Bodies.ContainsByPredicate([](const FLunaCelestialBody& Body)
            {return Body.Name==TEXT("Moon")||Body.Parent==TEXT("Earth");});
        VerifyResults.Add(bPlane&&bSingleLuna&&SolarSystem.Bodies.Num()==31
            ?TEXT("PASS: one Luna and a shared heliocentric ecliptic with 31 catalogued bodies")
            :TEXT("FAIL: ecliptic transform or celestial catalog"));
        StartRun(true,0,false,true);
        VerifyResults.Add(Phase==ELunaPhase::DeviceSetup&&bSafetyLocked
            ?TEXT("PASS: Nano free flight waits for centering"):TEXT("FAIL: Nano free-flight hold"));
        StartRun(false,0,false,true);
        const bool bPlan=PlotDestination(Earth);
        VerifyResults.Add(bPlan&&!Course->GetActorEnableCollision()
            ?TEXT("PASS: Earth approach plotted outside the training corridor"):TEXT("FAIL: free-flight destination"));
        GEngine->GameViewport->InputKey(FInputKeyEventArgs(GEngine->GameViewport->Viewport,FInputDeviceId::CreateFromInternalId(0),
            EKeys::Tab,IE_Pressed,FPlatformTime::Cycles64()));
        VerifyStep=93;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==93&&Elapsed>VerifyPhaseTime+.35)
    {
        GEngine->GameViewport->InputKey(FInputKeyEventArgs(GEngine->GameViewport->Viewport,FInputDeviceId::CreateFromInternalId(0),
            EKeys::Tab,IE_Released,FPlatformTime::Cycles64()));
        VerifyResults.Add(bMapOpen&&Moon->Movement->bLocked?TEXT("PASS: Tab opens a map and holds flight")
            :TEXT("FAIL: navigation map input or hold"));
        Snapshot(TEXT("06_solar_chart"));
        VerifyStep=94;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==94&&Elapsed>VerifyPhaseTime+.4)
    {
        ClickMap({1100,430},EKeys::LeftMouseButton);
        ClickMap({460,117},EKeys::LeftMouseButton);
        VerifyStep=97;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==97&&Elapsed>VerifyPhaseTime+.4)
    {
        ClickMap({1100,365},EKeys::LeftMouseButton);
        ClickMap({970,543},EKeys::LeftMouseButton);
        VerifyResults.Add(DestinationBody==SolarSystem.Find(TEXT("Io"))&&HasNavigationTarget()
            ?TEXT("PASS: body-system catalog selects Io and plots an approach")
            :TEXT("FAIL: body-system selection or approach click"));
        Snapshot(TEXT("06a_jupiter_chart"));
        VerifyStep=98;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==98&&Elapsed>VerifyPhaseTime+.4)
    {
        ClickMap({700,264},EKeys::RightMouseButton);
        VerifyResults.Add(DestinationBody==INDEX_NONE&&HasNavigationTarget()&&RouteStatus==TEXT("Waypoint plotted")
            ?TEXT("PASS: map coordinates plot a free-flight waypoint"):TEXT("FAIL: waypoint map click"));
        PlotDestination(SolarSystem.Find(TEXT("Earth")));
        if (bMapOpen) ToggleNavigationMap();
        FaceDestination();
        bWaitRelease=false;
        bCruise=true;
        Snapshot(TEXT("07_free_flight"));
        VerifyStep=96;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==96)
    {
        if (HasNavigationTarget()&&Elapsed<VerifyPhaseTime+16.)
        {
            Moon->Movement->Command={1,0};
            return;
        }
        const FVector Position=Moon->FlightPosition();
        const bool bArrived=!Route.IsEmpty()&&!HasNavigationTarget()&&Resets==0
            && FMath::Abs(Position.Z-PlaneZ)<.01&&FMath::Abs(Position.Y)>500.;
        VerifyResults.Add(bArrived?TEXT("PASS: free flight reaches Earth approach and stays on the ecliptic")
            :TEXT("FAIL: free-flight travel or plane hold"));
        Snapshot(TEXT("08_earth_approach"));
        WriteVerification(bArrived);
        VerifyStep=95;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==95&&Elapsed>VerifyPhaseTime+1){bVerify=false;LoadControlSettings();ShowMenu();return;}
    if(Elapsed>180&&VerifyStep<90){VerifyResults.Add(TEXT("FAIL: runtime verification timeout"));WriteVerification(false);VerifyStep=99;return;}
    if(VerifyStep==0&&Elapsed>8){VerifyStep=108;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==108&&Elapsed>VerifyPhaseTime+.75){Snapshot(TEXT("00_menu"));VerifyStep=109;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==109&&Elapsed>VerifyPhaseTime+.23){Snapshot(TEXT("00_menu_plume_later"));VerifyStep=110;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==110&&Elapsed>VerifyPhaseTime+.5)
    {
        OpenControlSettings();
        DraftControls.bUseDevice=true;
        VerifyStep=111;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==111&&Elapsed>VerifyPhaseTime+.5)
    {
        VerifyResults.Add(bControlSettingsOpen?TEXT("PASS: control settings opened from the main menu"):TEXT("FAIL: control settings entry"));
        Snapshot(TEXT("00a_control_settings"));
        VerifyStep=112;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==112&&Elapsed>VerifyPhaseTime+.5)
    {
        const FLunaPlayerControls Before=PlayerControls;
        DraftControls={1.35f,.85f,4.5f,{4.2f,60,350,650},true,false};
        const FLunaPlayerControls Expected=DraftControls;
        CloseControlSettings(true);
        const bool bApplied=!bControlSettingsOpen;
        PlayerControls=FLunaPlayerControls();
        LoadControlSettings();
        const auto& Reopened=PlayerControls;
        const bool bPersisted=bApplied
            && FMath::IsNearlyEqual(Reopened.ForwardGain,Expected.ForwardGain)
            && FMath::IsNearlyEqual(Reopened.RightGain,Expected.RightGain)
            && FMath::IsNearlyEqual(Reopened.DeadZoneDegrees,Expected.DeadZoneDegrees)
            && FMath::IsNearlyEqual(Reopened.Gesture.ThresholdMps2,Expected.Gesture.ThresholdMps2)
            && Reopened.Gesture.HoldMs==Expected.Gesture.HoldMs
            && Reopened.Gesture.QuietMs==Expected.Gesture.QuietMs
            && Reopened.Gesture.CooldownMs==Expected.Gesture.CooldownMs
            && Reopened.bUseDevice && !ShowsTrajectory();
        VerifyResults.Add(bPersisted?TEXT("PASS: input mode, route visibility and tuning survive save and reload"):TEXT("FAIL: control settings save and reload"));
        StartSelectedRun(2,true);
        const bool bNanoStartsLocked=Phase==ELunaPhase::DeviceSetup&&bDeviceMode&&bSafetyLocked;
        OpenControlSettings();
        DraftControls.bUseDevice=false;
        DraftControls.bShowTrajectory=true;
        CloseControlSettings(false);
        const bool bCancelled=Phase==ELunaPhase::DeviceSetup&&bDeviceMode&&!ShowsTrajectory();
        OpenControlSettings();
        DraftControls.bUseDevice=false;
        DraftControls.bShowTrajectory=true;
        CloseControlSettings(true);
        const bool bKeyboardResumes=Phase==ELunaPhase::Playing&&!bDeviceMode&&!bSafetyLocked&&bWaitRelease&&ShowsTrajectory();
        VerifyResults.Add(bNanoStartsLocked&&bCancelled&&bKeyboardResumes
            ?TEXT("PASS: selected Nano mode requires centering; cancel preserves settings; applying keyboard resumes")
            :TEXT("FAIL: input selection or settings transition"));
        PlayerControls=Before;
        ShowMenu();
        VerifyStep=1;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==1&&Elapsed>VerifyPhaseTime+1)
    {StartRun(false);Moon->BeginDeparture();Speak(TEXT("Engines ready. Leaving the lunar surface."),true);VerifyStep=10;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==10&&Elapsed>VerifyPhaseTime+.8){Snapshot(TEXT("00b_surface_lift"));VerifyStep=11;return;}
    if(VerifyStep==11&&Elapsed>VerifyPhaseTime+2.3){Snapshot(TEXT("00f_continuous_before"));VerifyStep=101;return;}
    if(VerifyStep==101&&Elapsed>VerifyPhaseTime+2.5){Snapshot(TEXT("00g_continuous_after"));VerifyStep=102;return;}
    if(VerifyStep==102&&Elapsed>VerifyPhaseTime+3.5){Snapshot(TEXT("00c_lunar_reveal"));VerifyStep=12;return;}
    if(VerifyStep==12&&Elapsed>VerifyPhaseTime+5.2){Snapshot(TEXT("00e_luna_full"));VerifyStep=14;return;}
    if(VerifyStep==14&&Elapsed>VerifyPhaseTime+6.8){Snapshot(TEXT("00d_orbital_reveal"));VerifyStep=13;return;}
    if(VerifyStep==13&&Elapsed>VerifyPhaseTime+9.8)
    {
        VerifyResults.Add(!Moon->IsDeparting()?TEXT("PASS: continuous surface-to-orbit transition completes and unlocks flight"):TEXT("FAIL: departure camera"));
        VerifyResults.Add(Moon->HasContinuousSurface()?TEXT("PASS: one lunar surface and rocks persist through departure"):TEXT("FAIL: surface changed"));
        VerifyResults.Add(Moon->EngineCount()==12674?TEXT("PASS: all 12674 engines use anchored monument geometry"):TEXT("FAIL: missing engine bodies"));
        GEngine->GameViewport->InputKey(FInputKeyEventArgs(GEngine->GameViewport->Viewport,FInputDeviceId::CreateFromInternalId(0),EKeys::F1,IE_Pressed,FPlatformTime::Cycles64()));
        VerifyStep=15;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==15&&Elapsed>VerifyPhaseTime+.15)
    {
        GEngine->GameViewport->InputKey(FInputKeyEventArgs(GEngine->GameViewport->Viewport,FInputDeviceId::CreateFromInternalId(0),EKeys::F1,IE_Released,FPlatformTime::Cycles64()));
        VerifyResults.Add(bDiagnostics&&!GEngine->GameViewport->EngineShowFlags.Wireframe?TEXT("PASS: F1 input shows diagnostics without changing rendering"):TEXT("FAIL: F1 rendering conflict"));
        Snapshot(TEXT("01c_F1_diagnostics"));VerifyStep=16;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==16&&Elapsed>VerifyPhaseTime+.2)
    {bDiagnostics=false;StartRun(false,1);bWaitRelease=false;Moon->SetFlightPosition(CollisionProbe-FVector(300,0,0));VerifyStep=2;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==2)
    {
        Moon->Movement->Command={1,0};
        if(Elapsed>VerifyPhaseTime+2)
        {
            bool Pass=Moon->Movement->CollisionEvents>0;
            UE_LOG(LogTemp,Display,TEXT("LUNA_ASTEROID_SWEEP contacts=%d position=%s"),Moon->Movement->CollisionEvents,*Moon->FlightPosition().ToString());
            VerifyResults.Add(Pass?TEXT("PASS: swept lunar sphere blocked by real convex asteroid"):TEXT("FAIL: asteroid swept collision"));
            if(!Pass){WriteVerification(false);VerifyStep=99;return;}
            TogglePause();Snapshot(TEXT("01b_paused"));VerifyStep=3;VerifyPhaseTime=Elapsed;
        }return;
    }
    if(VerifyStep==3&&Elapsed>VerifyPhaseTime+.3)
    {
        bool Pass=Moon->Movement->Velocity.IsNearlyZero();VerifyResults.Add(Pass?TEXT("PASS: pause clears velocity"):TEXT("FAIL: pause velocity"));
        ResetCheckpoint();Pass=Moon->Movement->Velocity.IsNearlyZero()&&Moon->Movement->TargetLayer==CheckpointLayer&&FVector::Dist(Moon->FlightPosition(),Checkpoint)<.1;
        VerifyResults.Add(Pass?TEXT("PASS: checkpoint reset restores safe position, layer and zero velocity"):TEXT("FAIL: checkpoint reset"));
        StartRun(false,2);bWaitRelease=false;ShiftLayer(1);ShiftLayer(1);Pass=Moon->Movement->TargetLayer==1;ShiftLayer(-1);ShiftLayer(-1);ShiftLayer(-1);Pass=Pass&&Moon->Movement->TargetLayer==-1;
        VerifyResults.Add(Pass?TEXT("PASS: layer events clamp to [-1,+1]"):TEXT("FAIL: layer boundaries"));
        StartRun(false,1);bWaitRelease=false;Moon->SetFlightPosition({LateralStartX+500,501,PlaneZ});VerifyStep=30;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==30&&Elapsed>VerifyPhaseTime+.7)
    {
        const bool Pass=Resets==1&&FVector::Dist(Moon->FlightPosition(),Checkpoint)<.1;
        VerifyResults.Add(Pass?TEXT("PASS: leaving the navigation corridor restores the checkpoint"):TEXT("FAIL: course side boundary"));
        if(!Pass){WriteVerification(false);VerifyStep=99;return;}
        bDeviceMode=true;bSafetyLocked=false;Device.bCentered=true;Device.bValid=true;Device.bCalibrated=true;Device.SourceAge=1.;Device.LastSourceTime=FPlatformTime::Seconds();Moon->Movement->Velocity={120,70,100};
        VerifyStep=31;return;
    }
    if(VerifyStep==31)
    {
        bool Pass=bSafetyLocked&&Moon->Movement->Velocity.IsNearlyZero();VerifyResults.Add(Pass?TEXT("PASS: stale device clears horizontal and vertical velocity on next update"):TEXT("FAIL: device failsafe"));
        if(!Pass){WriteVerification(false);VerifyStep=99;return;}
        StartRun(false,2);bWaitRelease=false;ShiftLayer(1);VerifyStep=32;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==32&&Elapsed>VerifyPhaseTime+.12)
    {
        const bool Pass=Moon->SouthThrust>.35f&&Moon->NorthThrust==0;
        VerifyResults.Add(Pass?TEXT("PASS: rising orbit fires strong south polar exhaust"):TEXT("FAIL: south polar exhaust"));
        Snapshot(TEXT("03c_south_thrust"));
        VerifyStep=33;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==33&&Elapsed>VerifyPhaseTime+.25)
    {
        Moon->SetFlightPosition({PolarStartX,0,PlaneZ});Moon->Movement->Velocity=FVector::ZeroVector;Moon->Movement->TargetLayer=-1;
        VerifyStep=34;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==34&&Elapsed>VerifyPhaseTime+.12)
    {
        const bool Pass=Moon->NorthThrust>.35f&&Moon->SouthThrust==0;
        VerifyResults.Add(Pass?TEXT("PASS: descending orbit fires strong north polar exhaust"):TEXT("FAIL: north polar exhaust"));
        Snapshot(TEXT("03d_north_thrust"));VerifyStep=35;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==35&&Elapsed>VerifyPhaseTime+.25)
    {
        Moon->SetFlightPosition({PolarStartX,0,490});Moon->SetFlightVelocity({0,0,150});Moon->Movement->TargetLayer=1;
        VerifyStep=36;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==36&&Elapsed>VerifyPhaseTime+.1)
    {
        VerifyResults.Add(Moon->NorthThrust>0&&Moon->SouthThrust==0?TEXT("PASS: upward approach brakes with north polar exhaust"):TEXT("FAIL: polar braking direction"));
        Snapshot(TEXT("03e_polar_braking"));VerifyStep=37;VerifyPhaseTime=Elapsed;return;
    }
    if(VerifyStep==37&&Elapsed>VerifyPhaseTime+.25){TogglePause();VerifyStep=38;VerifyPhaseTime=Elapsed;return;}
    if(VerifyStep==38&&Elapsed>VerifyPhaseTime+.15)
    {
        VerifyResults.Add(Moon->NorthThrust==0&&Moon->SouthThrust==0?TEXT("PASS: pause shuts down both active polar plumes"):TEXT("FAIL: paused polar thrust"));
        StartRun(false);bWaitRelease=false;
        Snapshot(TEXT("01_inertia"));VerifyStep=380;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==380&&Elapsed>VerifyPhaseTime+.25)
    {
        PlayerControls.bShowTrajectory=false;
        Snapshot(TEXT("01a_trail_hidden"));VerifyStep=381;VerifyPhaseTime=Elapsed;return;
    }
    if (VerifyStep==381&&Elapsed>VerifyPhaseTime+.25)
    {
        PlayerControls.bShowTrajectory=true;
        VerifyStep=4;
        return;
    }
    if(VerifyStep!=4&&VerifyStep!=5)return;
    bWaitRelease=false;
    if(Phase==ELunaPhase::Complete&&VerifyStep==4)
    {
        VerifiedCampaignSeconds=RunTime;
        if (!VerifyResults.Contains(TEXT("PASS: quiet braking-to-lateral transfer"))||!VerifyResults.Contains(TEXT("PASS: quiet lateral-to-polar transfer")))
            VerifyResults.Add(TEXT("FAIL: missing free-flight narration interval"));
        VerifyResults.Add(TEXT("PASS: three stages completed in order with swept movement and PD height"));
        Snapshot(TEXT("04_complete"));VerifyStep=90;VerifyPhaseTime=Elapsed;return;
    }
    if(Phase!=ELunaPhase::Playing)return;
    FVector P=Moon->FlightPosition(),Target=P;const FVector V=Moon->FlightVelocity();
    if(Stage==1&&!bInTransfer&&Gate==0&&P.X>LateralGates[0].X-480&&!bFieldShot){Snapshot(TEXT("02b_asteroid_aperture"));bFieldShot=true;}
    if(Stage==2&&HeightLesson==2&&Gate==0&&P.X>PolarGates[0].X-390&&!bPolarShot){Snapshot(TEXT("03b_polar_aperture"));bPolarShot=true;}
    if (bInTransfer)
    {
        Target=LocalNavigationTarget()+FVector(200,0,0);
        const TCHAR* Result=Stage==1?TEXT("PASS: quiet braking-to-lateral transfer"):TEXT("PASS: quiet lateral-to-polar transfer");
        if (Task().IsEmpty()&&(!Narrator||!Narrator->IsPlaying())&&Target.X-P.X>4500)
            VerifyResults.AddUnique(Result);
    }
    else if(Stage==0)Target={BrakeX,0,300};
    else if(Stage==1)
    {
        if (Gate<GateCount)
        {
            const FVector Opening=LateralGates[Gate];
            Target=Opening+FVector(200,0,0);
            if(FMath::Abs(P.Y-Opening.Y)>40&&P.X<Opening.X-170)Target.X=FMath::Min(P.X+80.,Opening.X-240);
        }
        else Target={LateralFinishX+100,0,300};
    }
    else
    {
        if(HeightLesson<2)
        {
            Target={PolarPracticeX,0,300};
            const int32 Desired=HeightLesson==0?1:0;
            if(Moon->Movement->TargetLayer!=Desired)ShiftLayer(Desired>Moon->Movement->TargetLayer?1:-1);
        }
        else if(Gate<GateCount)
        {
            if(Moon->Movement->TargetLayer!=PolarLayers[Gate])ShiftLayer(PolarLayers[Gate]>Moon->Movement->TargetLayer?1:-1);
            Target=PolarGates[Gate]+FVector(240,0,0);
            if(!Moon->Movement->Stable())Target.X=FMath::Min(P.X+30.,PolarGates[Gate].X-270);
        }
        else Target=Arrival+FVector(100,0,0);
    }
    FVector2D Desired=(FVector2D(Target.X-P.X,Target.Y-P.Y)*1.6).GetClampedToMaxSize(240.);
    const auto& S=*GetDefault<ULunaSettings>();FVector2D Velocity(V.X,V.Y);
    Moon->Movement->Command=((Desired-Velocity)*4.+S.Drag*Velocity)/S.Acceleration;
}
