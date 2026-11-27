#pragma once
#include "CoreMinimal.h"

struct FLunaCelestialBody
{
    FString Name;
    FString Parent;
    FVector Position=FVector::ZeroVector; // Flight-frame coordinates.
    double Radius=0.;
    double RingRadius=0.;

    double SafetyRadius() const;
    bool IsSatellite() const { return !Parent.IsEmpty(); }
};

struct FLunaSolarSystem
{
    TArray<FLunaCelestialBody> Bodies;
    FString Epoch;
    bool Load(const FTransform& FlightFrame);
    int32 Find(const FString& Name) const;
    int32 Nearest(const FVector& Position) const;
    FVector Approach(int32 BodyIndex,const FVector& Start) const;
    double Clearance(const FVector& Position) const;
    double TravelFraction(const FVector& Start,const FVector& End) const;
    bool PlanRoute(FVector Start,FVector Goal,TArray<FVector>& Points) const;
};
