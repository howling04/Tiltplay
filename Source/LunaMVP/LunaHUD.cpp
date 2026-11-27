#include "LunaHUD.h"
#include "LunaHUDDraw.h"
#include "LunaGame.h"
#include "LunaPawn.h"
#include "LunaScale.h"
#include "LunaSettings.h"
#include "Camera/PlayerCameraManager.h"
#include "InputCoreTypes.h"
// UI craft workflow: unslop by Damir Sokolovsky, CC BY 4.0, https://heydamir.design/unslop.

using namespace LunaHUD;

namespace
{
constexpr float SliderLeft=650.f, SliderWidth=190.f;
enum ESettingsAction : int32
{
    OpenSettings=20, BackSettings, ApplySettings, ResetSettings, SelectKeyboard, SelectNano,
    ShowTrajectory, HideTrajectory, FirstSlider=32
};

struct FControlRow
{
    const TCHAR* Label;
    const TCHAR* Unit;
    float Min, Max, Step;
    int32 Decimals;
};

const FControlRow ControlRows[]={
    {TEXT("Forward / back"),TEXT("x"),0.5f,2.f,0.05f,2},
    {TEXT("Left / right"),TEXT("x"),0.5f,2.f,0.05f,2},
    {TEXT("Tilt dead zone"),TEXT("°"),0.f,12.f,0.5f,1},
    {TEXT("Activation threshold"),TEXT(" m/s²"),1.5f,8.f,0.1f,1},
    {TEXT("Minimum hold"),TEXT(" ms"),20.f,120.f,5.f,0},
    {TEXT("Quiet window"),TEXT(" ms"),100.f,600.f,10.f,0},
    {TEXT("Cooldown"),TEXT(" ms"),200.f,1000.f,25.f,0}
};


}
void SLunaHUD::Construct(const FArguments& Args){Game=Args._Game;SetVisibility(EVisibility::Visible);ForceVolatile(true);}
FReply SLunaHUD::OnMouseMove(const FGeometry& G,const FPointerEvent& E)
{
    const float S=FMath::Min(G.GetLocalSize().X/1280.,G.GetLocalSize().Y/720.);const auto Off=(G.GetLocalSize()-FVector2D(1280*S,720*S))*.5;
    if (S<=0.f) return FReply::Unhandled();
    const FVector2D Next=(G.AbsoluteToLocal(E.GetScreenSpacePosition())-Off)/S;
    const FVector2D Delta=Next-Pointer;
    Pointer=Next;
    if (bDraggingMap&&Game.IsValid()&&Game->bMapOpen&&HasMouseCapture())
    {
        MapPan+=FVector2D(-Delta.X,Delta.Y)*PaintedUnitsPerPixel;
        return FReply::Handled();
    }
    if (DragControl!=INDEX_NONE&&HasMouseCapture())
    {
        MoveControlSlider();
        return FReply::Handled();
    }
    return FReply::Unhandled();
}

void SLunaHUD::MoveControlSlider()
{
    if (!Game.IsValid()||!Game->bControlSettingsOpen||DragControl==INDEX_NONE) return;
    const auto& Row=ControlRows[DragControl];
    const float Position=FMath::Clamp(float((Pointer.X-SliderLeft)/SliderWidth),0.f,1.f);
    const float Value=FMath::GridSnap(FMath::Lerp(Row.Min,Row.Max,Position),Row.Step);
    auto& Draft=Game->DraftControls;
    switch (DragControl)
    {
        case 0: Draft.ForwardGain=Value; break;
        case 1: Draft.RightGain=Value; break;
        case 2: Draft.DeadZoneDegrees=Value; break;
        case 3: Draft.Gesture.ThresholdMps2=Value; break;
        case 4: Draft.Gesture.HoldMs=FMath::RoundToInt(Value); break;
        case 5: Draft.Gesture.QuietMs=FMath::RoundToInt(Value); break;
        case 6: Draft.Gesture.CooldownMs=FMath::RoundToInt(Value); break;
    }
}

FReply SLunaHUD::OnMouseButtonUp(const FGeometry& Geometry,const FPointerEvent& Event)
{
    if (bDraggingMap&&Event.GetEffectingButton()==EKeys::LeftMouseButton)
    {
        bDraggingMap=false;
        return FReply::Handled().ReleaseMouseCapture();
    }
    if (Event.GetEffectingButton()!=EKeys::LeftMouseButton||DragControl==INDEX_NONE) return FReply::Unhandled();
    OnMouseMove(Geometry,Event);
    DragControl=INDEX_NONE;
    bDraggingMap=false;
    return FReply::Handled().ReleaseMouseCapture();
}

void SLunaHUD::OnMouseCaptureLost(const FCaptureLostEvent& Event)
{
    DragControl=INDEX_NONE;
    bDraggingMap=false;
    SCompoundWidget::OnMouseCaptureLost(Event);
}
FReply SLunaHUD::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)
{
    OnMouseMove(G,E);
    if(!Game.IsValid())return FReply::Unhandled();
    if(Game->bMapOpen) return MapMouseDown(E);
    if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    for(const auto& A:Actions)if(A.Rect.ContainsPoint(Pointer))
    {
        if (Game->bControlSettingsOpen)
        {
            if (A.Id>=FirstSlider&&A.Id<FirstSlider+UE_ARRAY_COUNT(ControlRows))
            {
                DragControl=A.Id-FirstSlider;
                MoveControlSlider();
                return FReply::Handled().CaptureMouse(SharedThis(this));
            }
            switch (A.Id)
            {
                case BackSettings: Game->CloseControlSettings(false); break;
                case ApplySettings: Game->CloseControlSettings(true); break;
                case ResetSettings: Game->DraftControls=FLunaPlayerControls(); break;
                case SelectKeyboard: Game->DraftControls.bUseDevice=false; break;
                case SelectNano: Game->DraftControls.bUseDevice=true; break;
                case ShowTrajectory: Game->DraftControls.bShowTrajectory=true; break;
                case HideTrajectory: Game->DraftControls.bShowTrajectory=false; break;
            }
        }
        else
        {
            switch(A.Id)
            {
                case 1:Game->StartSelectedRun();break;
                case 3:Game->RequestCenter();break;case 4:Game->TogglePause();break;
                case 5:Game->ShowMenu();break;case 6:Game->StartSelectedRun(0,true);break;
                case 7:Game->StartSelectedRun(1,true);break;case 8:Game->StartSelectedRun(2,true);break;
                case 9:Game->StartFreeFlight();break;
                case 19:Game->ToggleNavigationMap();break;
                case SelectKeyboard:Game->SelectInputMode(false);break;
                case SelectNano:Game->SelectInputMode(true);break;
                case OpenSettings:Game->OpenControlSettings();break;
            }
        }
        return FReply::Handled();
    }return FReply::Unhandled();
}
int32 SLunaHUD::OnPaint(const FPaintArgs&,const FGeometry& Geometry,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const
{
    auto* G=Game.Get();if(!G||!G->Moon)return Layer;
    auto* Moon=G->Moon.Get();auto* Motion=Moon->Movement.Get();FDraw D(Geometry,Out,Layer);Actions.Reset();
    if (G->bMapOpen)
    {
        DrawNavigationMap(D,*G);
        return D.L;
    }
    auto Button=[&](float X,float Y,float W,const FString& Label,int32 Id,bool Primary=false)
    {
        const FSlateRect R(X,Y,X+W,Y+40);const bool Hover=R.ContainsPoint(Pointer);
        if(Primary)D.Rect(X,Y,W,40,Alpha(White,Hover?1.f:.91f));
        else
        {
            if(Hover)D.Rect(X,Y,W,40,Alpha(White,.07f));
            D.Line(X,Y+40,X+W,Y+40,Hover?White:Rule);
        }
        D.Text(Label,X+16,Y+10,15,Primary?C(TEXT("162027")):Hover?White:Muted);
        Actions.Add({R,Id});
    };
    auto Choice=[&](float X,float Y,float W,const TCHAR* Label,int32 Id,bool Selected)
    {
        const FSlateRect R(X,Y,X+W,Y+36);
        const bool Hover=R.ContainsPoint(Pointer);
        D.Rect(X,Y,W,36,Alpha(Blue,Selected?.16f:Hover?.09f:.035f));
        D.Line(X,Y+36,X+W,Y+36,Selected?Blue:Rule,Selected?2.f:1.f);
        D.Center(Label,X+W*.5f,Y+8,14,Selected||Hover?White:Muted);
        Actions.Add({R,Id});
    };
    if (G->bControlSettingsOpen)
    {
        const auto& Draft=G->DraftControls;
        D.Rect(0,0,1280,720,Alpha(C(TEXT("080C10")),.54f));
        D.Panel(368,48,864,624,.98f);
        D.Line(368,48,1232,48,Blue,2);
        D.Text(TEXT("Control settings"),400,66,40,White,true);
        D.Text(TEXT("LUNA"),1146,82,20,Blue,true);
        D.Line(400,124,1200,124,Rule);
        D.Text(TEXT("Control method"),400,144,15,White);
        Choice(650,136,180,TEXT("Keyboard simulation"),SelectKeyboard,!Draft.bUseDevice);
        Choice(842,136,148,TEXT("NANO RP2040"),SelectNano,Draft.bUseDevice);
        D.Text(TEXT("Navigation trail"),400,190,15,White);
        Choice(650,182,84,TEXT("Show"),ShowTrajectory,Draft.bShowTrajectory);
        Choice(746,182,84,TEXT("Hide"),HideTrajectory,!Draft.bShowTrajectory);
        D.Line(400,230,1200,230,Rule);
        if (Draft.bUseDevice)
        {
            const float Values[]={Draft.ForwardGain,Draft.RightGain,Draft.DeadZoneDegrees,Draft.Gesture.ThresholdMps2,
                float(Draft.Gesture.HoldMs),float(Draft.Gesture.QuietMs),float(Draft.Gesture.CooldownMs)};
            D.Text(TEXT("Tilt response"),400,242,15,White);
            const bool PushGestures=G->Device.bGatewayGestureProfile;
            if (PushGestures) D.Text(TEXT("Push recognition"),400,370,15,White);
            else D.Text(G->Device.bHasVerticalStep?TEXT("Push up / down to shift orbit. Q / E also works.")
                :TEXT("Q / E   Raise / lower orbit"),400,370,14,Muted);
            const int32 RowCount=PushGestures?UE_ARRAY_COUNT(ControlRows):3;
            for (int32 Index=0;Index<RowCount;++Index)
            {
                const FControlRow& Row=ControlRows[Index];
                const float Y=Index<3?268.f+Index*30.f:396.f+(Index-3)*33.f;
                const FSlateRect Hit(SliderLeft-8,Y-3,SliderLeft+SliderWidth+8,Y+25);
                const bool Active=Hit.ContainsPoint(Pointer)||DragControl==Index;
                const float Fraction=(Values[Index]-Row.Min)/(Row.Max-Row.Min);
                const float Mark=SliderLeft+Fraction*SliderWidth;
                D.Text(Row.Label,400,Y,14,Muted);
                if (Active) D.Rect(SliderLeft-8,Y-3,SliderWidth+16,28,Alpha(Blue,.08f));
                D.Rect(SliderLeft,Y+12,SliderWidth,2,Rule);
                D.Rect(SliderLeft,Y+12,Mark-SliderLeft,2,Blue);
                D.Rect(Mark-2,Y+7,4,12,Active?White:Blue);
                const FString Value=FString::Printf(TEXT("%.*f%s"),Row.Decimals,Values[Index],Row.Unit);
                const bool InstrumentFont=Index!=3;
                D.Text(Value,930-D.Width(Value,16,InstrumentFont),Y-2,16,White,InstrumentFont);
                D.Line(400,Y+25,930,Y+25,Alpha(White,.12f));
                Actions.Add({Hit,FirstSlider+Index});
            }

            D.Line(962,242,962,541,Rule);
            D.Text(TEXT("Thrust preview"),986,242,15,White);
            const bool Live=G->Device.Healthy(FPlatformTime::Seconds());
            const FVector2D Command=Live?G->ControlCommand(Draft):FVector2D::ZeroVector;
            constexpr float CenterX=1093.f, CenterY=354.f, Range=62.f;
            D.Rect(CenterX-Range,CenterY-Range,Range*2,Range*2,Alpha(Blue,.06f));
            D.Line(CenterX-Range,CenterY,CenterX+Range,CenterY,Rule);
            D.Line(CenterX,CenterY-Range,CenterX,CenterY+Range,Rule);
            D.Line(CenterX,CenterY,CenterX+Command.Y*Range,CenterY-Command.X*Range,Blue,2);
            D.Diamond(CenterX+Command.Y*Range,CenterY-Command.X*Range,5,White);
            D.Center(TEXT("Forward"),CenterX,270,12,Muted);
            D.Text(TEXT("Right"),1165,CenterY-9,11,Muted);
            D.Center(Live?FString::Printf(TEXT("F  %+.0f%%     R  %+.0f%%"),100*Command.X,100*Command.Y):TEXT("Awaiting controller"),
                CenterX,426,15,White,true);
            if (Live&&!G->Device.bHasTiltDegrees)
                D.Text(TEXT("Bridge dead zone: 3° minimum"),986,455,12,Muted);
            if (PushGestures)
            {
                D.Line(986,485,1200,485,Rule);
                D.Text(G->Device.GestureProfileStatus,986,502,12,Muted);
            }
        }
        else
        {
            D.Text(TEXT("W / S"),400,255,18,Blue,true);
            D.Text(TEXT("Forward thrust / reverse thrust"),500,255,14,White);
            D.Text(TEXT("A / D"),400,293,18,Blue,true);
            D.Text(TEXT("Slide left / right"),500,293,14,White);
            D.Text(TEXT("Q / E"),400,331,18,Blue,true);
            D.Text(TEXT("Raise / lower orbit in polar training"),500,331,14,White);
        }

        D.Line(400,576,1200,576,Rule);
        Button(400,602,136,TEXT("Defaults"),ResetSettings);
        Button(938,602,112,TEXT("Back"),BackSettings);
        Button(1064,602,136,TEXT("Apply"),ApplySettings,true);
        if (!G->ControlSettingsError.IsEmpty()) D.Text(G->ControlSettingsError,558,613,12,Warning);
        return D.L;
    }
    const bool Menu=G->Phase==ELunaPhase::Menu,Complete=G->Phase==ELunaPhase::Complete;
    const FString Guidance=G->Phase==ELunaPhase::Playing&&!Moon->IsDeparting()?G->Task():FString();
    auto Caption=[&]()
    {
        if(G->SubtitleRemaining<=0||G->Subtitle.IsEmpty()||G->Subtitle==Guidance)return;
        const float A=FMath::Min(1.f,G->SubtitleRemaining/.4f);
        const float W=D.Width(G->Subtitle,15)+40;
        D.Rect(640-W*.5,548,W,36,FLinearColor(.005f,.007f,.009f,.7f*A));
        D.Center(G->Subtitle,640,556,15,Alpha(White,A));
    };
    if(Menu)
    {
        bMapInitialized=false;
        D.Panel(32,64,426,526,.78f);
        D.Text(TEXT("The Wandering Luna"),54,109,48,White,true);
        D.Line(54,176,435,176,Rule);
        D.Text(TEXT("Control method"),54,198,13,Muted);
        Choice(54,224,190,TEXT("Keyboard simulation"),SelectKeyboard,!G->bDeviceMode);
        Choice(254,224,181,TEXT("NANO RP2040"),SelectNano,G->bDeviceMode);
        Button(54,288,192,TEXT("Training   /   Enter"),1,true);
        Button(254,288,181,TEXT("Free flight   /   G"),9);
        D.Text(TEXT("Practice sector"),54,359,13,Muted);
        Button(54,385,117,TEXT("1  Brake"),6);
        Button(182,385,117,TEXT("2  Lateral"),7);
        Button(310,385,125,TEXT("3  Polar"),8);
        D.Line(54,462,435,462,Rule);
        Button(54,488,226,TEXT("Control settings   /   O"),OpenSettings);
        if (!G->ControlSettingsError.IsEmpty()) D.Text(G->ControlSettingsError,54,549,12,Warning);
        return D.L;
    }
    if(Moon->IsDeparting()&&G->Phase!=ELunaPhase::Paused&&G->Phase!=ELunaPhase::DeviceSetup)
    {
        D.Panel(520,24,240,70,.68f);
        D.Center(TEXT("ORBITAL TRANSFER"),640,36,20,White,true);
        D.Gauge(551,72,178,4,Moon->DepartureProgress());
        Caption();return D.L;
    }
    D.Panel(32,25,226,46,.66f);
    D.Text(TEXT("LUNA"),46,34,22,White,true);
    D.Line(108,37,108,60,Rule);
    D.Text(G->StageName(),121,36,16,White,true);

    D.Panel(531,24,218,69,.72f);
    if (G->bFreeFlight)
    {
        D.Center(G->bCruise?TEXT("CRUISE DRIVE   /   SPACE"):TEXT("LOCAL DRIVE   /   SPACE"),640,34,17,White,true);
    }
    else
    {
        for (int32 I=0;I<3;++I)
        {
            const float X=592+I*48;
            const FLinearColor Tint=I==G->Stage?Blue:I<G->Stage?White:Muted;
            D.Diamond(X,49,14,Tint);
            D.Center(FString::FromInt(I+1),X,40,16,Tint,true);
        }
        D.Gauge(552,85,176,2,float(G->Progress()));
    }
    const int32 Time=FMath::Max(0,FMath::FloorToInt(G->RunTime));
    D.Center(FString::Printf(TEXT("%02d:%02d"),Time/60,Time%60),640,68,14,Muted,true);

    D.Panel(1006,25,185,46,.72f);
    const FLinearColor LinkTint=G->bSafetyLocked?Warning:Blue;
    D.Diamond(1022,47,4,LinkTint);
    D.Text(G->bSafetyLocked?TEXT("CONTROL HOLD"):G->bDeviceMode?TEXT("NANO ONLINE"):TEXT("KEYBOARD"),
        1035,35,17,LinkTint,true);
    if(G->Phase==ELunaPhase::Playing) Button(1200,28,48,TEXT("II"),4);
    if(!Guidance.IsEmpty())
    {
        const float Width=D.Width(Guidance,14)+30;
        D.Rect(640-Width*.5f,103,Width,26,Alpha(Dark,.72f));
        D.Center(Guidance,640,107,14,White);
    }
    const FVector2D Applied=Motion->bLocked?FVector2D::ZeroVector:Motion->Command.GetClampedToMaxSize(1.);
    const float Thrust=FMath::Max(float(Applied.Size()),FMath::Max(Moon->NorthThrust,Moon->SouthThrust));
    const double Speed=LunaScale::SpeedKmS(Motion->Velocity.Size());
    D.Panel(978,547,270,140,.74f);
    D.Text(TEXT("VELOCITY"),995,557,14,Muted,true);
    const FString SpeedText=FString::Printf(TEXT("%.2f"),Speed);
    D.Text(SpeedText,995,571,41,White,true);
    D.Text(TEXT("km/s"),1004+D.Width(SpeedText,41,true),595,14,Muted,true);
    D.Line(995,623,1231,623,Rule);
    D.Text(TEXT("THRUST"),995,629,15,Muted,true);
    const FString ThrustText=FString::Printf(TEXT("%.0f%%"),100*Thrust);
    D.Text(ThrustText,1230-D.Width(ThrustText,16,true),629,16,Blue,true);
    D.Gauge(995,656,236,10,Thrust);
    if(G->Stage==2||G->bFreeFlight)
    {
        D.Panel(1080,309,168,211,.70f);
        D.Text(TEXT("ORBIT ALTITUDE"),1094,319,16,Muted,true);
        constexpr float X=1159,Y=357,H=94;
        D.Line(X,Y,X,Y+H,Rule);
        for(int32 I=-1;I<=1;++I)
        {
            const float YY=Y+H*(1-I)*.5;const bool Current=Motion->TargetLayer==I;
            D.Line(X-(Current?14:7),YY,X+7,YY,Current?Blue:Rule);
            D.Text(FString::Printf(TEXT("%+d"),I),X+14,YY-9,14,Current?White:Muted,true);
        }
        const float Actual=FMath::Clamp(float((Moon->FlightPosition().Z-100)/400.),0.f,1.f);
        D.Diamond(X,Y+H*(1-Actual),4,White);
        const FString Alt=FString::Printf(TEXT("%+.0f km"),LunaScale::DistanceKm(Moon->FlightPosition().Z-300));
        D.Center(Alt,1164,465,18,White,true);
        D.Center(Motion->Stable()?TEXT("ORBIT HOLD"):TEXT("ORBIT SHIFT"),1164,491,14,
            Motion->Stable()?Blue:Warning,true);
    }
    if (G->Phase==ELunaPhase::Playing)
    {
        DrawMinimap(D,*G);
    }
    if(auto* PC=G->GetWorld()->GetFirstPlayerController(); PC&&G->HasNavigationTarget())
    {
        int32 W=1,H=1;
        PC->GetViewportSize(W,H);
        auto Project=[&](FVector P,FVector2D& Result)
        {
            FVector2D Screen;
            if(!PC->ProjectWorldLocationToScreen(P,Screen)) return false;
            const FVector2D Local(Screen.X/FMath::Max(1,W)*Geometry.GetLocalSize().X,Screen.Y/FMath::Max(1,H)*Geometry.GetLocalSize().Y);
            Result=(Local-FVector2D(D.Offset))/D.S;
            return true;
        };
        const FVector Target=G->NavigationTarget();
        FVector2D Raw;
        const bool bTargetVisible=Project(Target,Raw);
        if (!bTargetVisible)
        {
            const FRotationMatrix View(PC->PlayerCameraManager->GetCameraRotation());
            const FVector ToTarget=Target-PC->PlayerCameraManager->GetCameraLocation();
            const FVector2D Side(FVector::DotProduct(ToTarget,View.GetScaledAxis(EAxis::Y)),
                -FVector::DotProduct(ToTarget,View.GetScaledAxis(EAxis::Z)));
            Raw=FVector2D(640,326)+Side.GetSafeNormal()*1000.;
        }
        const FVector2D Way(FMath::Clamp(Raw.X,308.,992.),FMath::Clamp(Raw.Y,150.,504.));
        FVector2D Origin;
        const bool bOriginVisible=Project(Moon->GetActorLocation(),Origin);
        if (bOriginVisible&&bTargetVisible&&G->Phase==ELunaPhase::Playing&&G->ShowsTrajectory())
        {
            // Compress the screen projection toward the same clipped marker the player sees.
            FVector2D Compression(1.,1.);
            if (FMath::Abs(Raw.X-Origin.X)>1.) Compression.X=(Way.X-Origin.X)/(Raw.X-Origin.X);
            if (FMath::Abs(Raw.Y-Origin.Y)>1.) Compression.Y=(Way.Y-Origin.Y)/(Raw.Y-Origin.Y);
            const FSlateRect TrailBounds(30,129,1244,531);
            TArray<FVector2f> Points;
            TArray<FLinearColor> Colors;
            auto DrawTrail=[&]()
            {
                if (Points.Num()>1)
                    FSlateDrawElement::MakeLines(Out,D.L++,Geometry.ToPaintGeometry(),MoveTemp(Points),
                        MoveTemp(Colors),ESlateDrawEffect::None,FLinearColor::White,true,1.5f*D.S);
                Points.Reset();
                Colors.Reset();
            };
            constexpr int32 Samples=32;
            for (int32 I=1;I<=Samples;++I)
            {
                const double T=FMath::Square(double(I)/Samples);
                FVector2D Point;
                if (!Project(G->NavigationPathPoint(T),Point))
                {
                    DrawTrail();
                    continue;
                }
                Point=Origin+(Point-Origin)*Compression;
                if (!TrailBounds.ContainsPoint(Point))
                {
                    DrawTrail();
                    continue;
                }
                const double Fade=FMath::SmoothStep(38.,105.,FVector2D::Distance(Origin,Point));
                Points.Add(FVector2f(Point)*D.S+D.Offset);
                Colors.Add(Alpha(Blue,float(Fade*(.32+.26*T))));
            }
            DrawTrail();
        }
        D.Diamond(Way.X,Way.Y,8,Blue);
        if(!Way.Equals(Raw,1.))
        {const auto V=(Raw-Way).GetSafeNormal(),Side=FVector2D(-V.Y,V.X);const auto Tip=Way+V*18;D.Line(Tip.X,Tip.Y,Tip.X-V.X*5+Side.X*4,Tip.Y-V.Y*5+Side.Y*4,Blue);D.Line(Tip.X,Tip.Y,Tip.X-V.X*5-Side.X*4,Tip.Y-V.Y*5-Side.Y*4,Blue);}
        const double Km=LunaScale::DistanceKm(FVector::Dist(Moon->GetActorLocation(),Target));
        const FString Distance=FString::Printf(TEXT("%.0f km"),Km);
        D.Rect(Way.X+14,Way.Y-10,D.Width(Distance,15,true)+12,20,Alpha(Dark,.68f));
        D.Text(Distance,Way.X+20,Way.Y-9,15,White,true);
        if(bOriginVisible)
        {
            FVector2D End;
            if(Motion->Velocity.Size()>2&&Project(Moon->GetActorLocation()+Motion->Velocity.GetSafeNormal()*70,End))
            {
                D.Line(Origin.X,Origin.Y,End.X,End.Y,Alpha(White,.3f));D.Diamond(End.X,End.Y,3,White);
            }
        }
    }
    Caption();
    if(G->Stage==0&&G->DockHold>0){D.Center(TEXT("Hold"),640,464,13,White);D.Line(608,488,608+64*FMath::Min(G->DockHold,1.f),488,Blue,2);}
    if(G->bDiagnostics)
    {
        D.Panel(32,278,340,118,.86f);
        D.Text(G->Device.SourceSeq<0?FString::Printf(TEXT("UDP %s   No samples"),G->Device.bBound?TEXT("bound"):TEXT("off")):FString::Printf(TEXT("UDP %s   seq %lld   age %.0f ms"),G->Device.bBound?TEXT("bound"):TEXT("off"),G->Device.SourceSeq,G->Device.Age(FPlatformTime::Seconds())*1000),46,289,15,Blue,true);
        D.Text(G->Device.Status,46,317,13,Muted);
        const TCHAR* Altitude=G->Device.bHasVerticalStep?TEXT("Tilt thrust   /   Push altitude   /   C center")
            :TEXT("Tilt thrust   /   Q E altitude   /   C center");
        D.Text(G->bDeviceMode?Altitude:TEXT("WASD thrust   /   Q E altitude"),46,344,12,White);
        D.Text(TEXT("F2  Input    R  Reset    V  Voice    M  Menu"),46,370,12,Muted);
    }
    if(G->Phase==ELunaPhase::Paused||G->Phase==ELunaPhase::DeviceSetup||Complete)
    {
        const bool Setup=G->Phase==ELunaPhase::DeviceSetup;
        D.Rect(0,0,1280,720,Alpha(Dark,.62f));
        D.Panel(414,199,452,425,.90f);
        D.Text(Complete?TEXT("Flight complete"):Setup?TEXT("NANO RP2040"):TEXT("Flight paused"),440,228,40,White,true);
        D.Line(440,292,840,292,Rule);
        D.Text(Complete?FString::Printf(TEXT("%02d:%02d   /   %d resets"),Time/60,Time%60,G->Resets)
            :Setup?G->Device.Status:TEXT("Release flight controls before resuming"),440,312,14,Muted);
        Button(440,376,300,Complete?TEXT("Fly again     /     Enter"):Setup?TEXT("Center     /     C"):TEXT("Resume     /     Esc"),Complete?1:Setup?3:4,true);
        Button(440,432,200,TEXT("Main menu     /     M"),5);
        if (!Complete) Button(440,484,224,TEXT("Control settings     /     O"),OpenSettings);
        D.Text(G->bMuted?TEXT("V   Voice off"):TEXT("V   Voice on"),440,556,12,Muted);
        if(!Setup&&!Complete)D.Text(TEXT("F1   Flight controls"),440,584,12,Muted);
    }
    return D.L;
}
