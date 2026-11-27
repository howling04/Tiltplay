#pragma once
#include "CoreMinimal.h"

namespace LunaFlight
{
    constexpr double PlaneZ = 300.0;
    constexpr double BrakeX = 1900.0;
    constexpr double BrakeHalfDepth = 150.0;
    constexpr double BrakeInterlockX = 2460.0;
    constexpr double LateralStartX = 7000.0;
    constexpr double LateralFinishX = 10000.0;
    constexpr double PolarStartX = 15200.0;
    constexpr double PolarPracticeX = 15500.0;
    constexpr double PolarInterlockX = 15940.0;
    constexpr double ArrivalX = 17900.0;
    constexpr double CorridorEndX = 18400.0;
    constexpr int32 GateCount = 3;

    inline const FVector LateralGates[GateCount] = {
        {8000, -250, 300}, {8700, 250, 300}, {9400, -200, 300}
    };
    inline const FVector PolarGates[GateCount] = {
        {16200, 0, 500}, {16800, 0, 300}, {17400, 0, 100}
    };
    constexpr int32 PolarLayers[GateCount] = {1, 0, -1};
    inline const FVector Arrival = {ArrivalX, 0, 100};
    inline const FVector CollisionProbe = {8000, 0, 300};
}
