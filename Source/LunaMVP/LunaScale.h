#pragma once
#include "CoreMinimal.h"

// Spatial scale follows the supplied mean Moon radius, independently of camera zoom.
namespace LunaScale
{
    constexpr double RadiusUU=28.;
    constexpr double RadiusKm=1737.4;
    constexpr double KmPerUU=RadiusKm/RadiusUU;
    constexpr double AstronomicalUnitKm=149597870.7;
    constexpr double AstronomicalUnitUU=AstronomicalUnitKm/KmPerUU;
    constexpr double SimulationSecondsPerRealSecond=3600.;
    inline double DistanceKm(double UU){return UU*KmPerUU;}
    inline double SpeedKmS(double UUPerSecond){return UUPerSecond*KmPerUU/SimulationSecondsPerRealSecond;}
    inline double AccelerationMS2(double UUPerSecond2){return UUPerSecond2*KmPerUU*1000./FMath::Square(SimulationSecondsPerRealSecond);}
}
