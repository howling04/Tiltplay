#include "LunaSolarSystem.h"
#include "LunaFlightPlan.h"
#include "LunaScale.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
double SphereEntry(const FVector& Start,const FVector& End,const FVector& Center,double Radius)
{
    const FVector Delta=End-Start, ToCenter=Center-Start;
    const double Length=Delta.Size();
    if (Length<UE_DOUBLE_SMALL_NUMBER) return 1.;
    const double Along=FVector::DotProduct(ToCenter,Delta/Length);
    const double DistanceSquared=ToCenter.SizeSquared();
    const double RadiusSquared=FMath::Square(Radius);
    if (DistanceSquared<=RadiusSquared) return Along>0.?0.:1.;
    if (Along<=0.) return 1.;
    const double Chord=RadiusSquared-(ToCenter-Delta*(Along/Length)).SizeSquared();
    return Chord>0.?FMath::Clamp((Along-FMath::Sqrt(Chord))/Length,0.,1.):1.;
}
}

double FLunaCelestialBody::SafetyRadius() const
{
    return FMath::Max(Radius,RingRadius)+LunaScale::RadiusUU+4.;
}

bool FLunaSolarSystem::Load(const FTransform& FlightFrame)
{
    Bodies.Reset();
    FString Text;
    const FString Path=FPaths::ProjectContentDir()/TEXT("LunaArt/Data/solar_navigation.json");
    TSharedPtr<FJsonObject> Document;
    const TArray<TSharedPtr<FJsonValue>>* Entries=nullptr;
    if (!FFileHelper::LoadFileToString(Text,*Path)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Document)
        || !Document || !Document->TryGetArrayField(TEXT("bodies"),Entries)) return false;
    Document->TryGetStringField(TEXT("epoch_utc"),Epoch);
    for (const auto& Entry:*Entries)
    {
        const TSharedPtr<FJsonObject>* Object=nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Center=nullptr;
        FLunaCelestialBody Body;
        if (!Entry->TryGetObject(Object)
            || !(*Object)->TryGetStringField(TEXT("name"),Body.Name)
            || !(*Object)->TryGetArrayField(TEXT("position_uu"),Center) || Center->Num()!=3
            || !(*Object)->TryGetNumberField(TEXT("radius_uu"),Body.Radius) || Body.Radius<=0.) continue;
        (*Object)->TryGetStringField(TEXT("parent"),Body.Parent);
        (*Object)->TryGetNumberField(TEXT("ring_radius_uu"),Body.RingRadius);
        const FVector World((*Center)[0]->AsNumber(),(*Center)[1]->AsNumber(),(*Center)[2]->AsNumber());
        Body.Position=FlightFrame.InverseTransformPositionNoScale(World);
        Bodies.Add(MoveTemp(Body));
    }
    return Bodies.Num()>0;
}

int32 FLunaSolarSystem::Find(const FString& Name) const
{
    return Bodies.IndexOfByPredicate([&](const FLunaCelestialBody& Body){return Body.Name==Name;});
}

int32 FLunaSolarSystem::Nearest(const FVector& Position) const
{
    int32 Best=INDEX_NONE;
    double Minimum=TNumericLimits<double>::Max();
    for (int32 Index=0;Index<Bodies.Num();++Index)
    {
        const double Distance=FVector::Dist(Bodies[Index].Position,Position)-Bodies[Index].Radius;
        if (Distance<Minimum) { Minimum=Distance;Best=Index; }
    }
    return Best;
}

FVector FLunaSolarSystem::Approach(int32 BodyIndex,const FVector& Start) const
{
    const auto& Body=Bodies[BodyIndex];
    const FVector Direction=(Start-Body.Position).GetSafeNormal2D(UE_DOUBLE_SMALL_NUMBER,FVector::ForwardVector);
    const double Distance=Body.SafetyRadius()+FMath::Max(210.,.25*Body.Radius);
    FVector Goal=Body.Position+Direction*Distance;
    Goal.Z=LunaFlight::PlaneZ;
    return Goal;
}

double FLunaSolarSystem::Clearance(const FVector& Position) const
{
    double Distance=TNumericLimits<double>::Max();
    for (const auto& Body:Bodies)
        Distance=FMath::Min(Distance,FVector::Dist(Body.Position,Position)-Body.SafetyRadius());
    return Distance;
}

double FLunaSolarSystem::TravelFraction(const FVector& Start,const FVector& End) const
{
    double Fraction=1.;
    for (const auto& Body:Bodies)
        Fraction=FMath::Min(Fraction,SphereEntry(Start,End,Body.Position,Body.SafetyRadius()));
    return Fraction;
}

bool FLunaSolarSystem::PlanRoute(FVector Start,FVector Goal,TArray<FVector>& Points) const
{
    Start.Z=Goal.Z=LunaFlight::PlaneZ;
    if (Goal.ContainsNaN()||FVector::Dist2D(Start,Goal)<70.||Clearance(Goal)<12.) return false;
    TArray<FVector> Path={Start,Goal};
    // Resolve hazards in path order using two approach points outside the projected sphere.
    for (int32 Attempts=0;Attempts<8;++Attempts)
    {
        bool bChanged=false;
        for (int32 Leg=1;Leg<Path.Num();++Leg)
        {
            const FVector A=Path[Leg-1],B=Path[Leg];
            double First=1.;
            const FLunaCelestialBody* Blocker=nullptr;
            for (const auto& Body:Bodies)
            {
                const double Hit=SphereEntry(A,B,Body.Position,Body.SafetyRadius()+14.);
                if (Hit<First) { First=Hit;Blocker=&Body; }
            }
            if (!Blocker) continue;
            const FVector Center(Blocker->Position.X,Blocker->Position.Y,LunaFlight::PlaneZ);
            const double Radius=FMath::Sqrt(FMath::Max(0.,FMath::Square(Blocker->SafetyRadius()+18.)
                -FMath::Square(Blocker->Position.Z-LunaFlight::PlaneZ)))+30.;
            const FVector Direction=(B-A).GetSafeNormal2D();
            FVector Side(-Direction.Y,Direction.X,0.);
            if (FVector::DotProduct(A-Center,Side)<0.) Side=-Side;
            bool bInserted=false;
            for (double Sign:{1.,-1.})
            {
                const FVector Entry=Center+Sign*Side*(2.*Radius)-Direction*(1.35*Radius);
                const FVector Exit=Center+Sign*Side*(2.*Radius)+Direction*(1.35*Radius);
                if (TravelFraction(A,Entry)<1.||TravelFraction(Entry,Exit)<1.||Clearance(Exit)<12.)
                    continue;
                Path.Insert({Entry,Exit},Leg);
                bInserted=true;
                break;
            }
            if (!bInserted) return false;
            bChanged=true;
            break;
        }
        if (!bChanged)
        {
            Path.RemoveAt(0);
            Points=MoveTemp(Path);
            return true;
        }
    }
    return false;
}
