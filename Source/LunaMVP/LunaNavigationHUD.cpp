#include "LunaHUD.h"
#include "LunaHUDDraw.h"
#include "LunaGame.h"
#include "LunaPawn.h"
#include "LunaFlightPlan.h"
#include "LunaScale.h"
#include "InputCoreTypes.h"
#include "Layout/Clipping.h"

using namespace LunaHUD;

namespace
{
const FSlateRect PlotBounds(56,154,874,572);
const FSlateRect ScannerBounds(40,523,272,657);
const FLinearColor PlotBackground=C(TEXT("0C1319"));
const FLinearColor PlotGrid=Alpha(Muted,.15f);
const FLinearColor ChartRoute=Blue;
const FLinearColor Accent=White;

enum EMapAction
{
    CloseMap=100, CourseChart, InnerChart, SolarChart, SystemChart, CenterMap,
    PlotApproach, ClearPlot, FaceRoute, FirstBody=1000
};

struct FPlot
{
    FSlateRect Bounds;
    FVector2D Center;
    FVector2D Origin;
    double UnitsPerPixel;

    FVector2D Middle() const
    {
        return {(Bounds.Left+Bounds.Right)*.5,(Bounds.Top+Bounds.Bottom)*.5};
    }
    FVector2D Project(FVector2D Point) const
    {
        const FVector2D Delta=(Point-Center)/UnitsPerPixel;
        return Middle()+FVector2D(Delta.X,-Delta.Y);
    }
    FVector2D Project(const FVector& Point) const { return Project(FVector2D(Point.X,Point.Y)); }
};

FString DistanceText(double Units)
{
    if (FMath::Abs(Units)>=.06*LunaScale::AstronomicalUnitUU)
        return FString::Printf(TEXT("%.2f AU"),Units/LunaScale::AstronomicalUnitUU);
    const double Km=LunaScale::DistanceKm(Units);
    return FString::Printf(TEXT("%.0f km"),FMath::Abs(Km)<.5?0.:Km);
}

double GridStep(double Minimum)
{
    const double Power=FMath::Pow(10.,FMath::FloorToDouble(FMath::LogX(10.,Minimum)));
    for (double Fraction:{1.,2.,5.,10.})
        if (Fraction*Power>=Minimum) return Fraction*Power;
    return 10.*Power;
}

void Frame(FDraw& D,const FSlateRect& Bounds)
{
    D.Rect(Bounds.Left,Bounds.Top,Bounds.Right-Bounds.Left,Bounds.Bottom-Bounds.Top,PlotBackground);
    D.Line(Bounds.Left,Bounds.Top,Bounds.Right,Bounds.Top,Rule);
    D.Line(Bounds.Left,Bounds.Bottom,Bounds.Right,Bounds.Bottom,Rule);
}

void ClipPlot(FDraw& D,const FSlateRect& Bounds)
{
    const FVector2D TL=D.G.LocalToAbsolute(FVector2D(D.Offset)+FVector2D(Bounds.Left,Bounds.Top)*D.S);
    const FVector2D BR=D.G.LocalToAbsolute(FVector2D(D.Offset)+FVector2D(Bounds.Right,Bounds.Bottom)*D.S);
    D.O.PushClip(FSlateClippingZone(FSlateRect(TL.X,TL.Y,BR.X,BR.Y)));
}

void DrawPath(FDraw& D,const TArray<FVector2D>& Points,FLinearColor Tint,float Width=1.f)
{
    TArray<FVector2f> SlatePoints;
    SlatePoints.Reserve(Points.Num());
    for (const auto& P:Points) SlatePoints.Add(FVector2f(P)*D.S+D.Offset);
    FSlateDrawElement::MakeLines(D.O,D.L++,D.G.ToPaintGeometry(),MoveTemp(SlatePoints),
        ESlateDrawEffect::None,Tint,true,Width*D.S);
}

void DrawOrbit(FDraw& D,const FPlot& Plot,const FVector& Origin,double Radius,FLinearColor Tint)
{
    const FVector2D Center=Plot.Project(Origin);
    const double Pixels=Radius/Plot.UnitsPerPixel;
    if (Pixels<4.) return;
    const double Span=FMath::Max(Plot.Bounds.Right-Plot.Bounds.Left,Plot.Bounds.Bottom-Plot.Bounds.Top);
    const FVector2D Offset=Plot.Middle()-Center;
    if (FMath::Abs(Offset.Size()-Pixels)>Span) return;
    const bool bArc=Pixels>2.*Span;
    const double Mid=FMath::Atan2(Offset.Y,Offset.X);
    const double Angle=bArc?FMath::Min(PI,2.*Span/Pixels):PI;
    TArray<FVector2D> Ring;
    constexpr int32 Sides=96;
    Ring.Reserve(Sides+1);
    for (int32 I=0;I<=Sides;++I)
    {
        const double A=Mid+Angle*(2.*I/Sides-1.);
        Ring.Add(Center+Pixels*FVector2D(FMath::Cos(A),FMath::Sin(A)));
    }
    DrawPath(D,Ring,Tint);
}

double DrawGrid(FDraw& D,const FPlot& Plot,bool bLabels)
{
    const auto& B=Plot.Bounds;
    const bool bAU=Plot.UnitsPerPixel*100.>=.04*LunaScale::AstronomicalUnitUU;
    const double Unit=bAU?LunaScale::AstronomicalUnitUU:1./LunaScale::KmPerUU;
    const double Step=GridStep(Plot.UnitsPerPixel*(bLabels?91.:43.)/Unit)*Unit;
    const double Left=Plot.Center.X-(B.Right-B.Left)*.5*Plot.UnitsPerPixel;
    const double Bottom=Plot.Center.Y-(B.Bottom-B.Top)*.5*Plot.UnitsPerPixel;
    int32 Index=FMath::CeilToInt((Left-Plot.Origin.X)/Step);
    for (int32 I=0;I<22;++I,++Index)
    {
        const double X=Plot.Project(FVector2D(Plot.Origin.X+Index*Step,Plot.Origin.Y)).X;
        if (X>B.Right) break;
        D.Line(X,B.Top,X,B.Bottom,Index==0?Alpha(Blue,.45f):PlotGrid);
        if (bLabels&&X>B.Left+36&&X<B.Right-66)
            D.Center((Step/Unit<1.?FString::Printf(TEXT("%+.2f"),Index*Step/Unit):FString::Printf(TEXT("%+.0f"),Index*Step/Unit)),X,B.Bottom-19,12,Muted,true);
    }
    Index=FMath::CeilToInt((Bottom-Plot.Origin.Y)/Step);
    for (int32 I=0;I<18;++I,++Index)
    {
        const double Y=Plot.Project(FVector2D(Plot.Origin.X,Plot.Origin.Y+Index*Step)).Y;
        if (Y<B.Top) break;
        D.Line(B.Left,Y,B.Right,Y,Index==0?Alpha(Blue,.45f):PlotGrid);
        if (bLabels&&Y>B.Top+44&&Y<B.Bottom-25)
            D.Text((Step/Unit<1.?FString::Printf(TEXT("%+.2f"),Index*Step/Unit):FString::Printf(TEXT("%+.0f"),Index*Step/Unit)),B.Left+7,Y-16,12,Muted,true);
    }
    return Step;
}

void DrawLuna(FDraw& D,const FPlot& Plot,const ALunaPawn& Moon,bool bLabel)
{
    const FVector2D P=Plot.Project(Moon.FlightPosition());
    if (!Plot.Bounds.ContainsPoint(P)) return;
    const double Angle=FMath::DegreesToRadians(Moon.FlightHeading());
    const FVector2D Forward(FMath::Cos(Angle),-FMath::Sin(Angle));
    const FVector2D Right(-Forward.Y,Forward.X);
    DrawPath(D,{P+Forward*11.,P-Forward*5.+Right*5.,P-Forward*2.,
        P-Forward*5.-Right*5.,P+Forward*11.},White,1.65f);
    const FVector2D Drift=FVector2D(Moon.FlightVelocity().X,-Moon.FlightVelocity().Y)*.65/Plot.UnitsPerPixel;
    const FVector2D End=P+Drift.GetClampedToMaxSize(47.);
    if (Drift.SizeSquared()>4.) D.Line(P.X,P.Y,End.X,End.Y,Alpha(White,.47f));
    if (bLabel) D.Text(TEXT("LUNA"),P.X+10,P.Y+6,13,White,true);
}

void DrawCourse(FDraw& D,const FPlot& Plot,const ALunaGame& Game,bool bLabels)
{
    using namespace LunaFlight;
    TArray<FVector2D> Corridor;
    for (const FVector P:{FVector(0,-500,PlaneZ),FVector(CorridorEndX,-500,PlaneZ),
        FVector(CorridorEndX,500,PlaneZ),FVector(0,500,PlaneZ),FVector(0,-500,PlaneZ)})
        Corridor.Add(Plot.Project(P));
    DrawPath(D,Corridor,Alpha(Blue,.27f));
    auto Mark=[&](const FVector& P,const TCHAR* Label,float Side)
    {
        const FVector2D Point=Plot.Project(P);
        if (!Plot.Bounds.ContainsPoint(Point)) return;
        D.Diamond(Point.X,Point.Y,3.0,Alpha(Blue,.7f));
        if (bLabels) D.Text(Label,Point.X-15,Point.Y+Side,12,Muted,true);
    };
    Mark({0,0,PlaneZ},TEXT("Departure"),-30);
    Mark({BrakeX,0,PlaneZ},TEXT("Brake"),15);
    Mark({LateralStartX,0,PlaneZ},TEXT("Lateral"),-40);
    Mark({PolarStartX,0,PlaneZ},TEXT("Polar"),-40);
    Mark(Arrival,TEXT("Arrival"),-40);
    for (int32 Index=0;Index<GateCount;++Index)
    {
        const FString Aperture=FString::Printf(TEXT("A%d"),Index+1);
        const FString Orbit=FString::Printf(TEXT("P%d"),Index+1);
        Mark(LateralGates[Index],*Aperture,Index%2?15.f:-22.f);
        Mark(PolarGates[Index],*Orbit,15.f);
    }
}

void DrawActiveRoute(FDraw& D,const FPlot& Plot,const ALunaGame& Game)
{
    if (!Game.HasNavigationTarget()) return;
    TArray<FVector2D> Path;
    if (Game.bFreeFlight)
    {
        Path.Add(Plot.Project(Game.Moon->FlightPosition()));
        for (int32 I=Game.RouteLeg;I<Game.Route.Num();++I)
        {
            const auto P=Plot.Project(Game.Route[I]);
            Path.Add(P);
            if (Plot.Bounds.ContainsPoint(P)) D.Diamond(P.X,P.Y,4.,Accent);
        }
    }
    else
    {
        for (int32 I=0;I<=28;++I) Path.Add(Plot.Project(Game.LocalPathPoint(I/28.)));
    }
    DrawPath(D,Path,ChartRoute,1.55f);
    const FVector2D Target=Plot.Project(Game.LocalNavigationTarget());
    if (Plot.Bounds.ContainsPoint(Target)) D.Diamond(Target.X,Target.Y,6.,Accent);
}

FString BodyCategory(const FLunaCelestialBody& Body)
{
    if (Body.IsSatellite()) return FString(TEXT("Satellite  /  "))+Body.Parent;
    if (Body.Name==TEXT("Sun")) return TEXT("Star");
    return Body.Name==TEXT("Pluto")?TEXT("Dwarf planet"):TEXT("Planet");
}
}

void SLunaHUD::DrawMinimap(FDraw& D,const ALunaGame& Flight) const
{
    const FVector Position=Flight.Moon->FlightPosition();
    const FVector Target=Flight.LocalNavigationTarget();
    const int32 Nearest=Flight.SolarSystem.Nearest(Position);
    const FLunaCelestialBody* Nearby=Flight.SolarSystem.Bodies.IsValidIndex(Nearest)?&Flight.SolarSystem.Bodies[Nearest]:nullptr;
    const double Range=Flight.bFreeFlight
        ? FMath::Clamp(Nearby?FVector::Dist2D(Position,Nearby->Position):2600.,2100.,2.8*LunaScale::AstronomicalUnitUU)
        : FMath::Clamp(FVector::Dist2D(Position,Target),1150.,5900.);
    const FVector Focus=Flight.bFreeFlight?Position:FMath::Lerp(Position,Target,.30);
    const FPlot Plot{ScannerBounds,FVector2D(Focus.X,Focus.Y),FVector2D::ZeroVector,Range/162.};
    D.Panel(32,492,248,195,.76f);
    const bool bInVicinity=Nearby&&FVector::Dist(Position,Nearby->Position)<.24*LunaScale::AstronomicalUnitUU;
    D.Text(Flight.bFreeFlight?(bInVicinity?Nearby->Name+TEXT(" vicinity"):TEXT("Solar scanner")):TEXT("Course scanner"),
        45,501,13,White);
    D.Text(TEXT("TAB"),240,501,14,Blue,true);
    D.Line(44,523,268,523,Rule);
    ClipPlot(D,Plot.Bounds);
    const FVector2D Center=Plot.Middle();
    for (float Scale:{.33f,.66f,1.f})
        D.Ellipse(Center.X,Center.Y,107.f*Scale,61.f*Scale,Alpha(Blue,Scale==1.f?.40f:.20f));
    D.Line(49,Center.Y,263,Center.Y,Rule);
    D.Line(Center.X,529,Center.X,651,Rule);
    if (!Flight.bFreeFlight) DrawCourse(D,Plot,Flight,false);
    else
    {
        for (const auto& Body:Flight.SolarSystem.Bodies)
        {
            const auto P=Plot.Project(Body.Position);
            if (!Plot.Bounds.ContainsPoint(P)) continue;
            D.Diamond(P.X,P.Y,Body.IsSatellite()?2.f:4.f,Blue);
            if (&Body==Nearby) D.Text(Body.Name,P.X+6,P.Y-8,12,Blue,true);
        }
    }
    DrawActiveRoute(D,Plot,Flight);
    DrawLuna(D,Plot,*Flight.Moon,false);
    D.Text(TEXT("+Y"),Center.X+6,527,11,Muted,true);
    D.Text(TEXT("+X"),248,Center.Y+6,11,Muted,true);
    D.O.PopClip();
    D.Line(44,659,268,659,Rule);
    D.Text(FString(TEXT("Range  "))+DistanceText(107.*Plot.UnitsPerPixel),45,665,12,Muted,true);
    if (Flight.HasNavigationTarget())
    {
        FString Distance=DistanceText(FVector::Dist2D(Position,Target));
        D.Text(Distance,266-D.Width(Distance,14,true),663,14,Accent,true);
    }
    Actions.Add({FSlateRect(32,492,280,687),19});
}

void SLunaHUD::SetMapView(EMapView View)
{
    MapView=View;
    MapZoom=1.;
    MapPan=FVector2D::ZeroVector;
}

FReply SLunaHUD::MapMouseDown(const FPointerEvent& Event)
{
    if (Event.GetEffectingButton()==EKeys::RightMouseButton&&PlotBounds.ContainsPoint(Pointer))
    {
        const FVector2D Delta=Pointer-FVector2D((PlotBounds.Left+PlotBounds.Right)*.5,(PlotBounds.Top+PlotBounds.Bottom)*.5);
        const FVector2D Point=PaintedCenter+FVector2D(Delta.X,-Delta.Y)*PaintedUnitsPerPixel;
        Game->PlotPoint(FVector(Point.X,Point.Y,LunaFlight::PlaneZ));
        return FReply::Handled();
    }
    if (Event.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Handled();
    for (const auto& Action:Actions)
    {
        if (!Action.Rect.ContainsPoint(Pointer)) continue;
        if (Action.Id>=FirstBody) MapSelection=Action.Id-FirstBody;
        else switch (Action.Id)
        {
        case CloseMap: Game->ToggleNavigationMap(); break;
        case CourseChart: SetMapView(EMapView::Course); break;
        case InnerChart: SetMapView(EMapView::Inner); break;
        case SolarChart: SetMapView(EMapView::Solar); break;
        case SystemChart:
            if (Game->SolarSystem.Bodies.IsValidIndex(MapSelection))
            {
                const auto& Body=Game->SolarSystem.Bodies[MapSelection];
                MapPrimary=Body.IsSatellite()?Game->SolarSystem.Find(Body.Parent):MapSelection;
                SetMapView(EMapView::System);
            }
            break;
        case CenterMap:
            MapPan+=FVector2D(Game->Moon->FlightPosition())-PaintedCenter;
            break;
        case PlotApproach: Game->PlotDestination(MapSelection); break;
        case ClearPlot: Game->ClearRoute(); break;
        case FaceRoute: Game->FaceDestination();Game->ToggleNavigationMap(); break;
        }
        return FReply::Handled();
    }
    if (PlotBounds.ContainsPoint(Pointer))
    {
        bDraggingMap=true;
        return FReply::Handled().CaptureMouse(SharedThis(this));
    }
    return FReply::Handled();
}

FReply SLunaHUD::OnMouseWheel(const FGeometry& Geometry,const FPointerEvent& Event)
{
    if (!Game.IsValid()||!Game->bMapOpen) return FReply::Unhandled();
    OnMouseMove(Geometry,Event);
    if (PlotBounds.ContainsPoint(Pointer))
    {
        const double NewZoom=FMath::Clamp(MapZoom*FMath::Pow(1.27,Event.GetWheelDelta()),.30,25600.);
        const FVector2D Middle((PlotBounds.Left+PlotBounds.Right)*.5,(PlotBounds.Top+PlotBounds.Bottom)*.5);
        const FVector2D Offset(Pointer.X-Middle.X,Middle.Y-Pointer.Y);
        MapPan+=Offset*PaintedUnitsPerPixel*(1.-MapZoom/NewZoom);
        MapZoom=NewZoom;
    }
    return FReply::Handled();
}

void SLunaHUD::DrawNavigationMap(FDraw& D,const ALunaGame& Flight) const
{
    const auto& System=Flight.SolarSystem;
    const FVector Luna=Flight.Moon->FlightPosition();
    if (!bMapInitialized)
    {
        MapView=Flight.bFreeFlight?EMapView::Inner:EMapView::Course;
        MapSelection=System.Find(TEXT("Earth"));
        bMapInitialized=true;
    }
    const int32 SunIndex=System.Find(TEXT("Sun"));
    const FVector SolarOrigin=System.Bodies.IsValidIndex(SunIndex)?System.Bodies[SunIndex].Position:FVector::ZeroVector;
    const bool bCourse=MapView==EMapView::Course;
    const bool bSystem=MapView==EMapView::System&&System.Bodies.IsValidIndex(MapPrimary);
    const bool bInner=MapView==EMapView::Inner;
    const FVector Ref=bCourse?FVector::ZeroVector:bSystem?System.Bodies[MapPrimary].Position:SolarOrigin;
    const FVector2D Origin(Ref.X,Ref.Y);
    FVector2D Center=Origin;
    double UnitsPerPixel=1.;
    if (bCourse)
    {
        Center.X=LunaFlight::ArrivalX*.5;
        UnitsPerPixel=25.;
    }
    else if (bSystem)
    {
        double Extent=FMath::Max(System.Bodies[MapPrimary].SafetyRadius()*1.4,450.);
        for (const auto& B:System.Bodies)
            if (B.Parent==System.Bodies[MapPrimary].Name) Extent=FMath::Max(Extent,FVector::Dist2D(Ref,B.Position)*1.12);
        UnitsPerPixel=Extent/191.;
    }
    else
    {
        UnitsPerPixel=(bInner?2.:35.)*LunaScale::AstronomicalUnitUU/193.;
    }
    PaintedCenter=Center+MapPan;
    PaintedUnitsPerPixel=UnitsPerPixel/MapZoom;
    const FPlot Plot{PlotBounds,PaintedCenter,Origin,PaintedUnitsPerPixel};
    D.Rect(0,0,1280,720,FLinearColor(.002f,.004f,.007f,.92f));
    D.Panel(32,27,1216,666,.98f);
    D.Text(TEXT("Navigation"),56,39,43,White,true);
    D.Text(TEXT("Milky Way  /  Solar System"),296,56,14,Blue);
    D.Text(TEXT("J2000 ECLIPTIC"),889,43,18,Blue,true);
    D.Text(System.Epoch.Left(10)+TEXT("  •  positions at epoch"),889,70,12,Muted);

    auto Button=[&](float X,float Y,float W,const FString& Label,int32 Action,bool Active=false)
    {
        const FSlateRect R(X,Y,X+W,Y+29);
        const bool Hover=R.ContainsPoint(Pointer);
        if (Active||Hover) D.Rect(X,Y,W,29,Alpha(White,Active?.10f:.06f));
        D.Line(X,Y+29,X+W,Y+29,Active?Blue:Rule);
        D.Text(Label,X+8,Y+5,13,Active||Hover?White:Muted);
        Actions.Add({R,Action});
    };
    if (!Flight.bFreeFlight) Button(56,106,108,TEXT("Training course"),CourseChart,bCourse);
    Button(172,106,112,TEXT("Inner system"),InnerChart,bInner);
    Button(292,106,116,TEXT("Solar system"),SolarChart,MapView==EMapView::Solar);
    if (System.Bodies.IsValidIndex(MapSelection)) Button(416,106,126,TEXT("Body system"),SystemChart,bSystem);
    Button(699,106,175,TEXT("Center on LUNA"),CenterMap);
    Button(1120,51,100,TEXT("Close  /  Tab"),CloseMap);

    Frame(D,PlotBounds);
    ClipPlot(D,PlotBounds);
    const double Step=DrawGrid(D,Plot,true);
    D.Text(bCourse?TEXT("DEPARTURE FRAME"):bSystem?System.Bodies[MapPrimary].Name.ToUpper()+TEXT(" FRAME"):TEXT("SOL-CENTERED"),67,160,14,Blue,true);
    D.Text(TEXT("+Y"),841,162,13,Muted,true);
    D.Text(TEXT("+X"),840,543,13,Muted,true);
    TArray<FSlateRect> Labels;
    if (bCourse) DrawCourse(D,Plot,Flight,true);
    else
    {
        for (const auto& Body:System.Bodies)
        {
            if (bSystem&&Body.Parent==System.Bodies[MapPrimary].Name)
                DrawOrbit(D,Plot,Ref,FVector::Dist2D(Ref,Body.Position),Alpha(Blue,.2f));
            else if (!bSystem&&!Body.IsSatellite()&&Body.Name!=TEXT("Sun"))
                DrawOrbit(D,Plot,SolarOrigin,FVector::Dist2D(SolarOrigin,Body.Position),Alpha(Blue,.22f));
            if (bSystem&&Body.Name==System.Bodies[MapPrimary].Name&&Body.RingRadius>0.)
                DrawOrbit(D,Plot,Ref,Body.RingRadius,Alpha(Warning,.52f));
        }
        for (int32 Index=0;Index<System.Bodies.Num();++Index)
        {
            const auto& Body=System.Bodies[Index];
            if (bSystem?Index!=MapPrimary&&Body.Parent!=System.Bodies[MapPrimary].Name:Body.IsSatellite()) continue;
            const auto P=Plot.Project(Body.Position);
            if (!PlotBounds.ContainsPoint(P)) continue;
            const float Symbol=Body.Name==TEXT("Sun")?6.f:Body.IsSatellite()?3.f:4.5f;
            const bool bSelected=Index==MapSelection;
            const FLinearColor Tint=bSelected?White:Body.Name==TEXT("Sun")?Warning:Blue;
            D.Diamond(P.X,P.Y,Symbol,Tint);
            if (bSelected) D.Diamond(P.X,P.Y,Symbol+4.f,Alpha(Blue,.65f));
            if (bSystem&&Body.Radius/Plot.UnitsPerPixel>6.)
                DrawOrbit(D,Plot,Body.Position,Body.Radius,Alpha(Tint,.68f));
            const FSlateRect Label(P.X+9,P.Y-10,P.X+12+D.Width(Body.Name,13,true),P.Y+6);
            if (Label.Right<PlotBounds.Right&&Label.Top>PlotBounds.Top+20
                &&!Labels.ContainsByPredicate([&](const FSlateRect& Other){return FSlateRect::DoRectanglesIntersect(Other,Label);}))
            {
                D.Text(Body.Name,Label.Left,Label.Top,13,Tint,true);
                Labels.Add(Label);
            }
            Actions.Add({FSlateRect(P.X-9,P.Y-9,P.X+9,P.Y+9),FirstBody+Index});
        }
    }
    DrawActiveRoute(D,Plot,Flight);
    DrawLuna(D,Plot,*Flight.Moon,bCourse||bSystem||PaintedUnitsPerPixel<100.);
    D.O.PopClip();

    const auto* Selected=System.Bodies.IsValidIndex(MapSelection)?&System.Bodies[MapSelection]:nullptr;
    D.Line(892,154,892,571,Rule);
    if (bCourse)
    {
        D.Text(TEXT("Training corridor"),910,158,27,White,true);
        D.Text(Flight.StageName(),910,195,14,Blue);
        D.Text(TEXT("Next marker"),910,229,12,Muted);
        D.Text(DistanceText(FVector::Dist2D(Luna,Flight.LocalNavigationTarget())),1030,222,21,Accent,true);
        D.Text(TEXT("A1–A3   Lateral apertures"),910,279,14,White);
        D.Text(TEXT("P1–P3   Polar apertures"),910,311,14,White);
        D.Text(TEXT("Blue line   Current approach"),910,349,13,Blue);
        D.Text(TEXT("Triangle   LUNA and heading"),910,376,13,Muted);
        D.Text(TEXT("Diamond   Active waypoint"),910,402,13,Muted);
        D.Text(TEXT("Course complete"),910,469,12,Muted);
        D.Line(910,501,1198,501,Rule,2);
        D.Line(910,501,910+288*Flight.Progress(),501,Blue,2);
    }
    else if (Selected)
    {
        D.Text(Selected->Name,910,155,30,White,true);
        D.Text(BodyCategory(*Selected),910,193,13,Blue);
        auto Field=[&](float Y,const TCHAR* Label,const FString& Value)
        {
            D.Text(Label,910,Y,12,Muted);
            D.Text(Value,1216-D.Width(Value,16,true),Y-3,16,White,true);
        };
        Field(222,TEXT("Range from LUNA"),DistanceText(FVector::Dist(Luna,Selected->Position)));
        Field(245,TEXT("Height from ecliptic"),DistanceText(Selected->Position.Z-LunaFlight::PlaneZ));
        Field(268,TEXT("Body radius"),DistanceText(Selected->Radius));
        if (Selected->RingRadius>0.) Field(291,TEXT("Outer ring reference"),DistanceText(Selected->RingRadius));
        D.Line(910,319,1217,319,Rule);
        D.Text(bSystem?TEXT("Local bodies"):TEXT("Body catalog"),910,331,14,White);
        int32 Slot=0;
        for (int32 Index=0;Index<System.Bodies.Num();++Index)
        {
            const auto& Body=System.Bodies[Index];
            if (bSystem?Index!=MapPrimary&&Body.Parent!=System.Bodies[MapPrimary].Name:Body.IsSatellite()) continue;
            Button(910+154*(Slot%2),355+32*(Slot/2),145,Body.Name,FirstBody+Index,Index==MapSelection);
            ++Slot;
        }
        if (Flight.bFreeFlight)
        {
            Button(910,532,141,TEXT("Plot approach"),PlotApproach,
                Flight.HasNavigationTarget()&&Flight.DestinationBody==MapSelection);
            Button(1064,532,145,TEXT("Clear route"),ClearPlot);
        }
    }

    D.Line(56,595,1216,595,Rule);
    const FVector2D Relative(Luna.X-Origin.X,Luna.Y-Origin.Y);
    D.Text(TEXT("LUNA POSITION"),56,609,12,Muted);
    D.Text(FString::Printf(TEXT("X  %s     Y  %s     Z  %s"),*DistanceText(Relative.X),*DistanceText(Relative.Y),
        *DistanceText(Luna.Z-LunaFlight::PlaneZ)),56,633,17,White,true);
    D.Text(FString::Printf(TEXT("Grid  %s   /   %s"),*DistanceText(Step),bCourse?TEXT("Course bounds"):TEXT("Orbit reference")),
        472,613,13,Muted,true);
    D.Text(Flight.bFreeFlight?TEXT("Wheel zoom   •   Drag pan   •   Right-click plot point"):TEXT("Wheel zoom   •   Drag pan"),
        472,643,12,Muted);
    if (Flight.bFreeFlight&&Flight.HasNavigationTarget())
    {
        double Remaining=FVector::Dist2D(Luna,Flight.Route[Flight.RouteLeg]);
        for (int32 I=Flight.RouteLeg+1;I<Flight.Route.Num();++I) Remaining+=FVector::Dist2D(Flight.Route[I-1],Flight.Route[I]);
        D.Text(FString::Printf(TEXT("%s  /  %d leg%s"),*DistanceText(Remaining),
            Flight.Route.Num()-Flight.RouteLeg,Flight.Route.Num()-Flight.RouteLeg==1?TEXT(""):TEXT("s")),910,604,18,Accent,true);
        Button(1064,627,145,TEXT("Face route  /  F"),FaceRoute);
    }
    if (!Flight.RouteStatus.IsEmpty())
        D.Text(Flight.RouteStatus.Len()>31?Flight.RouteStatus.Left(28)+TEXT("…"):Flight.RouteStatus,910,653,12,Blue);
}
