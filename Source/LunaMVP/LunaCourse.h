#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LunaCourse.generated.h"
class UProceduralMeshComponent;
class UBoxComponent;

UCLASS(Blueprintable)
class LUNAMVP_API ALunaCourse : public AActor
{
    GENERATED_BODY()
public:
    ALunaCourse();
    virtual void OnConstruction(const FTransform& Transform) override;
    void SetDoors(int32 Stage,bool bHeightLessonDone);
    void SetTrainingActive(bool bActive);
    void CenterStarfield(const FVector& Position);
    UPROPERTY(EditAnywhere,BlueprintReadWrite,Category="Space") int32 FieldSeed=568;
    UPROPERTY(EditAnywhere,BlueprintReadWrite,Category="Space") float AsteroidDensity=1.f;
    UPROPERTY(VisibleAnywhere,BlueprintReadOnly,Category="Space") int32 AsteroidCount=0;
private:
    UPROPERTY() TArray<TObjectPtr<UActorComponent>> Generated;
    UPROPERTY() TObjectPtr<UBoxComponent> StageDoorBlock;
    UPROPERTY() TObjectPtr<UBoxComponent> LessonDoorBlock;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> StageDoor;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> LessonDoor;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Starfield;
    UProceduralMeshComponent* Geometry(FString Name,const TArray<FVector>& V,const TArray<int32>& I,UMaterialInterface* M);
    UProceduralMeshComponent* Ring(FString Name,FVector Center,float Radius,float Width,UMaterialInterface* M);
    void Rock(FVector Center,float Radius,FRandomStream& Random);
};
