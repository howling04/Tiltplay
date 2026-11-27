#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "LunaPawn.generated.h"
class USphereComponent;
class UCameraComponent;
class USpringArmComponent;
class UProceduralMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;

UCLASS(ClassGroup=Movement, meta=(BlueprintSpawnableComponent))
class LUNAMVP_API ULunaMovement : public UPawnMovementComponent
{
    GENERATED_BODY()
public:
    ULunaMovement();
    virtual void TickComponent(float Dt,ELevelTick Type,FActorComponentTickFunction* ThisTick) override;
    void ResetMotion();
    FVector2D Command=FVector2D::ZeroVector;
    double FlightSpeedLimit=0.;
    int32 TargetLayer=0;
    bool bLocked=true;
    float StableTime=0,BlockedTime=0,HitFeedback=0;
    int32 CollisionEvents=0;
    bool Stable() const {return StableTime>=.3f;}
private:
    double Accumulator=0;
};

UCLASS(Blueprintable)
class LUNAMVP_API ALunaPawn : public APawn
{
    GENERATED_BODY()
public:
    ALunaPawn();
    virtual UPawnMovementComponent* GetMovementComponent() const override;
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    virtual void CalcCamera(float DeltaTime,FMinimalViewInfo& OutResult) override;
    void ResetView(bool Menu=false);
    void BeginDeparture();
    void SetFlightFrame(const FTransform& Frame);
    void SetFreeView(bool bEnable);
    void RotateView(double Yaw,double Pitch);
    void FaceFlightDirection(const FVector& Direction);
    double FlightHeading() const { return ViewYaw; }
    const FTransform& FlightFrame() const { return Frame; }
    FVector FlightPosition() const { return Frame.InverseTransformPositionNoScale(GetActorLocation()); }
    FVector FlightVelocity() const { return Frame.InverseTransformVectorNoScale(Movement->Velocity); }
    void SetFlightPosition(const FVector& Position);
    void SetFlightVelocity(const FVector& Velocity);
    FVector2D FlightCommand() const { return Movement->Command.GetRotated(ViewYaw); }
    bool IsDeparting()const{return ViewTransition>=0&&ViewTransition<9.f;}
    float DepartureProgress()const{return FMath::Clamp(ViewTransition/9.f,0.f,1.f);}
    bool HasContinuousSurface()const;
    int32 EngineCount()const;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<USphereComponent> Collision;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<ULunaMovement> Movement;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<USpringArmComponent> CameraArm;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Replaceable Appearance") TObjectPtr<UStaticMesh> ReplacementMesh;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Propulsion") float NorthThrust=0;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Propulsion") float SouthThrust=0;
private:
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> LowPolyMoon;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> NorthJet;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> SouthJet;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Surface;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> HeroThruster;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> NearTerrain;
    UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FarEngines;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> GlobalPlumes;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> PolarPlumes;
    UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> PolarCores;
    UPROPERTY() TObjectPtr<UStaticMesh> EngineNearMesh;
    UPROPERTY() TObjectPtr<UStaticMesh> EngineFarMesh;
    FVector HeroBase=FVector(-22,0,17),HeroNormal=FVector(-.78,0,.625);
    FVector SurfaceEye,SurfaceLook;
    bool bSurfaceView=true;
    bool bFreeView=false;
    double ViewYaw=0.,ViewPitch=0.;
    FTransform Frame=FTransform::Identity;
    float ViewTransition=-1;
    struct FExhaustNozzle
    {
        FVector Origin=FVector::ZeroVector;
        FVector Normal=FVector::UpVector;
        FQuat Rotation=FQuat::Identity;
    };
    TArray<FExhaustNozzle> StandardNozzles;
    FExhaustNozzle HeroNozzle,PolarNozzles[2];
    FVector2D LastPlumeCommand=FVector2D(100,100);
    void UpdatePlumes();
    void UpdateEngineVisibility(const FVector& Eye,const FMinimalViewInfo& View);
    void BuildVisual();
};
