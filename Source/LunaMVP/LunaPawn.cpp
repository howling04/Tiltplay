#include "LunaPawn.h"
#include "LunaGame.h"
#include "LunaScale.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "LunaMotion.h"
#include "LunaPropulsion.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"

namespace
{
    constexpr double MetresToUU=LunaScale::RadiusUU/(1000.*LunaScale::RadiusKm);
    constexpr double MonumentHeight=8000.*MetresToUU;
    constexpr double StandardHeight=1200.*MetresToUU;

    void ConfigureExhaust(UStaticMeshComponent& Component)
    {
        Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component.SetCanEverAffectNavigation(false);
        Component.SetCastShadow(false);
        Component.SetVisibleInRayTracing(false);
        Component.SetAffectDistanceFieldLighting(false);
        Component.bReceivesDecals=false;
        Component.SetVisibility(false);
    }

    void SetPlumeInstances(UInstancedStaticMeshComponent& Component,const TArray<FTransform>& Transforms)
    {
        if (Component.GetInstanceCount()!=Transforms.Num())
        {
            Component.ClearInstances();
            if (!Transforms.IsEmpty()) Component.AddInstances(Transforms,false,false,false);
        }
        else if (!Transforms.IsEmpty())
            Component.BatchUpdateInstancesTransforms(0,Transforms,false,true,true);
        Component.SetVisibility(!Transforms.IsEmpty());
    }
}

ULunaMovement::ULunaMovement(){PrimaryComponentTick.bCanEverTick=true;}
void ULunaMovement::ResetMotion(){Velocity=FVector::ZeroVector;Command=FVector2D::ZeroVector;Accumulator=0;StableTime=0;BlockedTime=0;HitFeedback=0;CollisionEvents=0;}
void ULunaMovement::TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* ThisTick)
{
    Super::TickComponent(Dt,Type,ThisTick);
    if(!UpdatedComponent||ShouldSkipUpdate(Dt))return;
    HitFeedback=FMath::Max(0.f,HitFeedback-Dt);
    if(bLocked){Velocity=FVector::ZeroVector;Command=FVector2D::ZeroVector;Accumulator=0;UpdateComponentVelocity();return;}
    const ULunaSettings& S=*GetDefault<ULunaSettings>();
    const auto* Luna=Cast<ALunaPawn>(GetPawnOwner());
    const FTransform& Frame=Luna?Luna->FlightFrame():FTransform::Identity;
    const auto* Game=Cast<ALunaGame>(GetWorld()->GetAuthGameMode());
    const bool bFreeFlight=Game&&Game->bFreeFlight;
    const FVector2D Thrust=Luna?Luna->FlightCommand():Command;
    Accumulator+=FMath::Clamp(Dt,0.f,.1f);
    constexpr float H=1.f/120.f;
    bool bBlocked=false;
    while(Accumulator+1e-8>=H)
    {
        Accumulator-=H;
        const FVector Position=Frame.InverseTransformPositionNoScale(UpdatedComponent->GetComponentLocation());
        FVector LocalVelocity=Frame.InverseTransformVectorNoScale(Velocity);
        FVector LocalDelta=LunaMotion::Step(LocalVelocity,Thrust,Position.Z,TargetLayer,H,S,FlightSpeedLimit);
        if (bFreeFlight)
        {
            const double Fraction=Game->SolarSystem.TravelFraction(Position,Position+LocalDelta);
            if (Fraction<1.)
            {
                LocalDelta*=FMath::Max(0.,Fraction-0.001);
                LocalVelocity=FVector::ZeroVector;
                bBlocked=true;HitFeedback=.22f;
            }
        }
        Velocity=Frame.TransformVectorNoScale(LocalVelocity);
        const FVector Delta=Frame.TransformVectorNoScale(LocalDelta);
        FHitResult Hit;
        SafeMoveUpdatedComponent(Delta,UpdatedComponent->GetComponentQuat(),true,Hit);
        if(Hit.IsValidBlockingHit())
        {
            bBlocked=true;HitFeedback=.22f;++CollisionEvents;
            const float Inward=FVector::DotProduct(Velocity,Hit.Normal);
            if(Inward<0)Velocity-=Hit.Normal*Inward;
            SlideAlongSurface(Delta,1.f-Hit.Time,Hit.Normal,Hit,true);
        }
        const double Height=Frame.InverseTransformPositionNoScale(UpdatedComponent->GetComponentLocation()).Z;
        const double VerticalSpeed=Frame.InverseTransformVectorNoScale(Velocity).Z;
        StableTime=LunaMotion::HeightStable(Height,VerticalSpeed,TargetLayer)?StableTime+H:0.f;
    }
    BlockedTime=bBlocked?BlockedTime+Dt:0.f;
    UpdateComponentVelocity();
}
ALunaPawn::ALunaPawn()
{
    PrimaryActorTick.bCanEverTick=true;
    Collision=CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));SetRootComponent(Collision);
    Collision->InitSphereRadius(30);Collision->SetCollisionProfileName(TEXT("Pawn"));Collision->SetCanEverAffectNavigation(false);
    VisualRoot=CreateDefaultSubobject<USceneComponent>(TEXT("LunaAssembly"));VisualRoot->SetupAttachment(Collision);
    Surface=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LunarSurface"));Surface->SetupAttachment(VisualRoot);Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    HeroThruster=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SurfaceHeroThruster"));HeroThruster->SetupAttachment(VisualRoot);HeroThruster->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NearTerrain=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LunarForefield"));NearTerrain->SetupAttachment(VisualRoot);NearTerrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    NorthJet=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("NorthPolarEngine"));NorthJet->SetupAttachment(VisualRoot);NorthJet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SouthJet=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SouthPolarEngine"));SouthJet->SetupAttachment(VisualRoot);SouthJet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FarEngines=CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("DistantEngineArray"));
    FarEngines->SetupAttachment(VisualRoot);
    FarEngines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    FarEngines->SetCanEverAffectNavigation(false);
    FarEngines->SetCastShadow(false);
    FarEngines->SetVisibleInRayTracing(false);
    FarEngines->SetAffectDistanceFieldLighting(false);
    GlobalPlumes=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GlobalPlasma"));
    PolarPlumes=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("PolarPlasma"));
    PolarCores=CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("PolarCore"));
    for (auto* Plume:{GlobalPlumes.Get(),PolarPlumes.Get(),PolarCores.Get()})
    {
        Plume->SetupAttachment(VisualRoot);
        ConfigureExhaust(*Plume);
    }
    Movement=CreateDefaultSubobject<ULunaMovement>(TEXT("InertialMovement"));Movement->SetUpdatedComponent(Collision);
    CameraArm=CreateDefaultSubobject<USpringArmComponent>(TEXT("FollowCameraArm"));CameraArm->SetupAttachment(Collision);CameraArm->bDoCollisionTest=false;
    Camera=CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));Camera->SetupAttachment(CameraArm);
    AutoPossessPlayer=EAutoReceiveInput::Player0;
}
UPawnMovementComponent* ALunaPawn::GetMovementComponent()const{return Movement;}
void ALunaPawn::SetFlightFrame(const FTransform& NewFrame)
{
    Frame=NewFrame;
    SetActorRotation(Frame.GetRotation());
}
void ALunaPawn::SetFlightPosition(const FVector& Position)
{
    SetActorLocation(Frame.TransformPositionNoScale(Position),false,nullptr,ETeleportType::TeleportPhysics);
}
void ALunaPawn::SetFlightVelocity(const FVector& Velocity)
{
    Movement->Velocity=Frame.TransformVectorNoScale(Velocity);
}
void ALunaPawn::SetFreeView(bool bEnable)
{
    bFreeView=bEnable;
    ViewYaw=0.;
    ViewPitch=bEnable?10.:0.;
}
void ALunaPawn::RotateView(double Yaw,double Pitch)
{
    ViewYaw=FMath::UnwindDegrees(ViewYaw+Yaw);
    ViewPitch=FMath::Clamp(ViewPitch+Pitch,-25.,65.);
}
void ALunaPawn::FaceFlightDirection(const FVector& Direction)
{
    if (Direction.IsNearlyZero()) return;
    ViewYaw=FMath::RadiansToDegrees(FMath::Atan2(Direction.Y,Direction.X));
    ViewPitch=FMath::Clamp(-FMath::RadiansToDegrees(FMath::Atan2(Direction.Z,Direction.Size2D()))+5.,-25.,65.);
}
void ALunaPawn::ResetView(bool Menu)
{
    bSurfaceView=Menu;ViewTransition=-1;
    VisualRoot->SetRelativeScale3D(FVector(Menu?12000.:1.));
}
void ALunaPawn::BeginDeparture(){bSurfaceView=false;ViewTransition=0;}
void ALunaPawn::BeginPlay(){Super::BeginPlay();BuildVisual();ResetView(true);}
void ALunaPawn::BuildVisual()
{
    auto* Mesh=ReplacementMesh?ReplacementMesh.Get():LoadObject<UStaticMesh>(nullptr,TEXT("/Game/LunaArt/Meshes/SM_MoonUnified.SM_MoonUnified"));
    Surface->SetStaticMesh(Mesh);Surface->SetRelativeScale3D(FVector(.28));Surface->SetRelativeRotation(FRotator(0,0,90));
    EngineNearMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/LunaArt/Meshes/SM_EngineMonument.SM_EngineMonument"));
    EngineFarMesh=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/LunaArt/Meshes/SM_EngineMonument_Far.SM_EngineMonument_Far"));
    HeroThruster->SetStaticMesh(EngineNearMesh);
    FarEngines->SetStaticMesh(EngineFarMesh);
    NearTerrain->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Game/LunaArt/Meshes/SM_SurfaceRocks.SM_SurfaceRocks")));NearTerrain->SetRelativeRotation(FRotator(0,0,90));NearTerrain->SetRelativeScale3D(FVector(28./173740.));
    auto* PlasmaColumn=LoadObject<UStaticMesh>(nullptr,TEXT("/Game/LunaArt/Meshes/SM_PlasmaColumn.SM_PlasmaColumn"));
    for (auto* Plume:{GlobalPlumes.Get(),PolarPlumes.Get(),PolarCores.Get()}) Plume->SetStaticMesh(PlasmaColumn);
    GlobalPlumes->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/LunaArt/Materials/M_Plasma.M_Plasma")));
    PolarPlumes->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/LunaArt/Materials/M_PolarPlasma.M_PolarPlasma")));
    PolarCores->SetMaterial(0,LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/LunaArt/Materials/M_PolarCore.M_PolarCore")));
    FString Raw;
    if(FFileHelper::LoadFileToString(Raw,*(FPaths::ProjectContentDir()/TEXT("LunaArt/Data/thrusters.json"))))
    {
        TSharedPtr<FJsonObject> Doc;
        if(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw),Doc))
        {
            auto Vec=[](const TSharedPtr<FJsonObject>& O,const TCHAR* Key){const auto& A=O->GetArrayField(Key);return FVector(A[0]->AsNumber(),-A[1]->AsNumber(),A[2]->AsNumber());};
            const double S=MetresToUU;
            auto Hero=Doc->GetObjectField(TEXT("hero"));HeroBase=Vec(Hero,TEXT("position_m"))*S;HeroNormal=Vec(Hero,TEXT("normal")).GetSafeNormal();
            auto Cine=Doc->GetObjectField(TEXT("cinematic"));HeroBase=Vec(Cine,TEXT("base_m"))*S;SurfaceEye=Vec(Cine,TEXT("eye_m"))*S;SurfaceLook=Vec(Cine,TEXT("look_m"))*S;
            NearTerrain->SetRelativeLocation(HeroBase);
            HeroThruster->SetRelativeLocation(HeroBase);HeroThruster->SetRelativeRotation(FRotationMatrix::MakeFromZ(HeroNormal).ToQuat()*FRotator(0,0,90).Quaternion());
            HeroThruster->SetRelativeScale3D(FVector(S*10.));
            HeroNozzle={HeroBase+HeroNormal*MonumentHeight,HeroNormal,FRotationMatrix::MakeFromZ(HeroNormal).ToQuat()};
            TArray<FTransform> EngineTransforms;
            for(auto Value:Doc->GetArrayField(TEXT("instances")))
            {
                auto O=Value->AsObject();FVector P=Vec(O,TEXT("position_m"))*S,N=Vec(O,TEXT("normal")).GetSafeNormal();
                const double Height=O->GetNumberField(TEXT("height_m"));
                const FQuat Rotation=FRotationMatrix::MakeFromZ(N).ToQuat();
                const FExhaustNozzle Nozzle{P+N*(Height*S),N,Rotation};
                if(O->GetStringField(TEXT("kind"))==TEXT("polar_main"))
                {
                    auto* Body=N.Z>0?NorthJet.Get():SouthJet.Get();
                    Body->SetStaticMesh(EngineFarMesh);
                    Body->SetRelativeLocation(P);Body->SetRelativeRotation(Rotation*FRotator(0,0,90).Quaternion());Body->SetRelativeScale3D(FVector(S*10.));
                    PolarNozzles[N.Z>0?0:1]=Nozzle;
                }
                else if (O->GetIntegerField(TEXT("id"))!=Hero->GetIntegerField(TEXT("id")))
                {
                    StandardNozzles.Add(Nozzle);
                    EngineTransforms.Emplace(Rotation*FRotator(0,0,90).Quaternion(),P,FVector(S*10.*Height/8000.));
                }
            }
            FarEngines->AddInstances(EngineTransforms,false,false,false);
        }
    }
}
void ALunaPawn::UpdateEngineVisibility(const FVector& Eye,const FMinimalViewInfo& View)
{
    if (StandardNozzles.IsEmpty()) return;
    int32 Width=1600,Height=900;
    if (const auto* PC=Cast<APlayerController>(GetController())) PC->GetViewportSize(Width,Height);
    const double TanHorizontal=FMath::Tan(FMath::DegreesToRadians(View.FOV*.5));
    const double TanVertical=TanHorizontal*(Height>0?double(Height)/FMath::Max(1,Width):9./16.);
    const double FocalLength=.5*(Width>0?Width:1600)/TanHorizontal;
    const FRotationMatrix CameraAxes(View.Rotation);
    const FVector Forward=CameraAxes.GetScaledAxis(EAxis::X);
    const FVector Right=CameraAxes.GetScaledAxis(EAxis::Y);
    const FVector Up=CameraAxes.GetScaledAxis(EAxis::Z);
    auto InView=[&](const FVector& Center,double Bounds)
    {
        const FVector Delta=Center-Eye;
        const double Depth=FVector::DotProduct(Forward,Delta);
        if (Depth<-Bounds||FMath::Abs(FVector::DotProduct(Right,Delta))>Depth*TanHorizontal+2*Bounds
            ||FMath::Abs(FVector::DotProduct(Up,Delta))>Depth*TanVertical+2*Bounds) return false;
        const double ClosestT=-FVector::DotProduct(Eye,Delta)/FMath::Max(1.e-8,Delta.SizeSquared());
        const double OccluderRadius=LunaScale::RadiusUU*.98-Bounds;
        return OccluderRadius<=0.||ClosestT<=0.||ClosestT>=1.||(Eye+Delta*ClosestT).SizeSquared()>=FMath::Square(OccluderRadius);
    };
    auto ProjectedDiameter=[&](const FVector& Origin,double Radius)
    {
        return 2.*Radius*FocalLength/FMath::Max(.005,FVector::Dist(Eye,Origin));
    };
    auto UpdateBody=[&](UStaticMeshComponent& Body,const FExhaustNozzle& Nozzle)
    {
        const FVector Center=Nozzle.Origin-Nozzle.Normal*(MonumentHeight*.5);
        const double Pixels=ProjectedDiameter(Center,MonumentHeight*.5);
        const bool bVisible=Pixels>.65&&InView(Center,MonumentHeight);
        if (bVisible) Body.SetStaticMesh(Pixels>32.?EngineNearMesh:EngineFarMesh);
        Body.SetVisibility(bVisible);
    };
    UpdateBody(*HeroThruster,HeroNozzle);
    UpdateBody(*NorthJet,PolarNozzles[0]);
    UpdateBody(*SouthJet,PolarNozzles[1]);
    const double Nearest=FMath::Max(.005,Eye.Size()-LunaScale::RadiusUU-StandardHeight);
    FarEngines->SetVisibility(StandardHeight*FocalLength/Nearest>.65);
}

void ALunaPawn::UpdatePlumes()
{
    if (StandardNozzles.IsEmpty()) return;

    const auto& Settings=*GetDefault<ULunaSettings>();
    const float Az=LunaPropulsion::VerticalAcceleration(FlightPosition().Z,FlightVelocity().Z,Movement->TargetLayer,Settings);
    const bool bLocked=Movement->bLocked||IsDeparting()||bSurfaceView;
    const FVector2D Power=LunaPropulsion::PolarPower(Az,Settings.MaxVerticalAcceleration,bLocked);
    NorthThrust=Power.X;
    SouthThrust=Power.Y;

    TArray<FTransform> ShellTransforms,CoreTransforms;
    for (int32 Pole=0;Pole<2;++Pole)
    {
        const double Drive=Power[Pole];
        if (Drive<.003) continue;
        const FExhaustNozzle& Nozzle=PolarNozzles[Pole];
        const double Length=(80000.+5420000.*Drive)*MetresToUU;
        const double Width=(6000.+450000.*FMath::Sqrt(Drive))*MetresToUU;
        ShellTransforms.Emplace(Nozzle.Rotation,Nozzle.Origin,FVector(Width/100.,Width/100.,Length/100.));
        CoreTransforms.Emplace(Nozzle.Rotation,Nozzle.Origin,FVector(Width/380.,Width/380.,Length/150.));
    }
    SetPlumeInstances(*PolarPlumes,ShellTransforms);
    SetPlumeInstances(*PolarCores,CoreTransforms);

    const FVector2D PlanarCommand=bLocked?FVector2D::ZeroVector:FlightCommand().GetClampedToMaxSize(1.);
    if (LastPlumeCommand.Equals(PlanarCommand,.015)) return;
    LastPlumeCommand=PlanarCommand;

    TArray<FTransform> PlanarTransforms;
    PlanarTransforms.Reserve(StandardNozzles.Num()+1);
    auto AddColumn=[&](const FExhaustNozzle& Nozzle,bool bHero)
    {
        const double Drive=LunaPropulsion::PlanarPower(Nozzle.Normal,PlanarCommand);
        const double Length=600000.*MetresToUU*(.005+.195*Drive);
        const double Width=bHero?.0008:.00015;
        PlanarTransforms.Emplace(Nozzle.Rotation,Nozzle.Origin,FVector(Width,Width,Length*(bHero?.08:.01)));
    };
    for (const FExhaustNozzle& Nozzle:StandardNozzles) AddColumn(Nozzle,false);
    AddColumn(HeroNozzle,true);
    SetPlumeInstances(*GlobalPlumes,PlanarTransforms);
}
void ALunaPawn::CalcCamera(float Dt,FMinimalViewInfo& Out)
{
    const FVector Center=GetActorLocation();
    const FRotator Heading(0.,ViewYaw,0.);
    const FVector OrbitEye=Heading.RotateVector(FVector(-680,0,bFreeView?790.*FMath::Tan(FMath::DegreesToRadians(ViewPitch)):480.));
    const FVector OrbitTarget=Heading.RotateVector(FVector(110,0,0));
    FVector Eye,Target;FVector Up=FVector::UpVector;
    double Scale=1.;
    if(bSurfaceView||IsDeparting())
    {
        const float T=bSurfaceView?0.f:DepartureProgress();
        const float Ease=T*T*(3-2*T);
        Scale=FMath::Pow(12000.,1.-double(Ease));
        // Surface and camera shrink together. Angular image remains continuous while clearance grows.
        const double Clearance=210.*FMath::Pow(double(T),8.);
        double Mix=FMath::Clamp((double(T)-.45)/.55,0.,1.);Mix=Mix*Mix*(3-2*Mix);
        const FVector LiftEye=SurfaceEye+HeroNormal*Clearance;
        Eye=FMath::Lerp(LiftEye,OrbitEye,Mix)*Scale;
        Target=FMath::Lerp(SurfaceLook,OrbitTarget,Mix)*Scale;
        Up=FMath::Lerp(HeroNormal,FVector::UpVector,Mix).GetSafeNormal();
        VisualRoot->SetRelativeScale3D(FVector(Scale));
    }
    else{Eye=OrbitEye;Target=OrbitTarget;VisualRoot->SetRelativeScale3D(FVector::OneVector);}
    Surface->SetCustomPrimitiveDataVector3(0,VisualRoot->GetComponentTransform().TransformPosition(HeroBase));
    Surface->SetCustomPrimitiveDataFloat(4,42000.f*28.f/1737400.f*VisualRoot->GetComponentScale().X);
    Out.Location=Center+Frame.TransformVectorNoScale(Eye);
    Out.Rotation=FRotationMatrix::MakeFromXZ((Target-Eye).GetSafeNormal(),Up).Rotator();
    Out.FOV=bSurfaceView?58.f:IsDeparting()?FMath::Lerp(58.f,60.f,DepartureProgress()):60.f;Out.AspectRatio=16.f/9.f;Out.bConstrainAspectRatio=false;
    UpdateEngineVisibility(Eye/Scale,Out);
    Out.Rotation=(Frame.GetRotation()*Out.Rotation.Quaternion()).Rotator();
}
bool ALunaPawn::HasContinuousSurface()const{return Surface&&Surface->IsVisible()&&Surface->GetStaticMesh()&&Surface->GetStaticMesh()->GetName()==TEXT("SM_MoonUnified")&&NearTerrain->IsVisible();}
int32 ALunaPawn::EngineCount()const{return FarEngines->GetInstanceCount()+3;}
void ALunaPawn::Tick(float Dt)
{
    Super::Tick(Dt);
    auto* Game=Cast<ALunaGame>(GetWorld()->GetAuthGameMode());
    if(IsDeparting()&&(!Game||(Game->Phase!=ELunaPhase::Paused&&!Game->bMapOpen)))
    {
        ViewTransition=FMath::Min(9.f,ViewTransition+Dt);
        if(ViewTransition>=9)ResetView(false);
    }
    UpdatePlumes();
}
