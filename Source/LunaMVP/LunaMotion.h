#pragma once
#include "CoreMinimal.h"
#include "LunaSettings.h"
#include "LunaPropulsion.h"

namespace LunaMotion
{
    inline float TiltAxis(float Degrees, const ULunaSettings& S)
    {
        const float Range = FMath::Max(.01f, S.FullTiltDegrees - S.DeadZoneDegrees);
        return FMath::Sign(Degrees) * FMath::Clamp((FMath::Abs(Degrees)-S.DeadZoneDegrees)/Range,0.f,1.f);
    }
    inline FVector2D Tilt(float Pitch, float Roll, const ULunaSettings& S)
    {
        return FVector2D(TiltAxis(Pitch,S),TiltAxis(Roll,S)).GetClampedToMaxSize(1.);
    }
    inline FVector2D DeviceTilt(FVector2D Input, bool bDegrees, float DeadZoneDegrees, float FullTiltDegrees, FVector2D Gain)
    {
        constexpr double BridgeDeadZone = 3.0;
        constexpr double BridgeFullTilt = 25.0;
        const double DeadZone = bDegrees ? double(DeadZoneDegrees) : FMath::Max(double(DeadZoneDegrees), BridgeDeadZone);
        const double Range = FMath::Max(0.01, FullTiltDegrees - DeadZone);
        auto Axis = [&](double Value, double AxisGain)
        {
            const double Magnitude = FMath::Abs(Value);
            if (Magnitude == 0.0) return 0.0;
            const double Degrees = bDegrees ? Magnitude : BridgeDeadZone + (BridgeFullTilt - BridgeDeadZone) * Magnitude;
            return FMath::Sign(Value) * FMath::Clamp((Degrees - DeadZone) / Range * AxisGain, 0.0, 1.0);
        };
        return FVector2D(Axis(Input.X, Gain.X), Axis(Input.Y, Gain.Y)).GetClampedToMaxSize(1.0);
    }
    inline FVector Step(FVector& Velocity, FVector2D Input, float Z, int32 Layer, float Dt,
        const ULunaSettings& S, double FlightSpeedLimit=0.)
    {
        Input = Input.GetClampedToMaxSize(1.);
        FVector2D XY(Velocity.X,Velocity.Y);
        const double Limit=FlightSpeedLimit>0.?FlightSpeedLimit:S.MaxSpeed;
        const double Acceleration=S.Acceleration*(Limit/FMath::Max(1.f,S.MaxSpeed));
        XY += (Acceleration*Input-S.Drag*XY)*Dt;
        XY = XY.GetClampedToMaxSize(Limit);
        Velocity.X=XY.X; Velocity.Y=XY.Y;
        const float Az=LunaPropulsion::VerticalAcceleration(Z,Velocity.Z,Layer,S);
        Velocity.Z=FMath::Clamp(Velocity.Z+Az*Dt,-S.MaxVerticalSpeed,S.MaxVerticalSpeed);
        return Velocity*Dt;
    }
    inline bool HeightStable(float Z, float Vz, int32 Layer)
    {
        return FMath::Abs(Z-(300.f+200.f*Layer))<=15.f && FMath::Abs(Vz)<=20.f;
    }
}
