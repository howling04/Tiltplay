#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
class ALunaGame;
namespace LunaHUD { struct FDraw; }

class SLunaHUD:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLunaHUD){} SLATE_ARGUMENT(ALunaGame*,Game) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool)const override;
    virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&)override;
    virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&)override;
    virtual void OnMouseCaptureLost(const FCaptureLostEvent&)override;
    virtual FReply OnMouseMove(const FGeometry&,const FPointerEvent&)override;
    virtual FReply OnMouseWheel(const FGeometry&,const FPointerEvent&)override;
    virtual FVector2D ComputeDesiredSize(float)const override {return {1280,720};}
private:
    TWeakObjectPtr<ALunaGame> Game;
    struct FAction{FSlateRect Rect;int32 Id;};
    mutable TArray<FAction> Actions;
    FVector2D Pointer=FVector2D(-100,-100);
    int32 DragControl=INDEX_NONE;
    void MoveControlSlider();
    enum class EMapView : uint8 { Course, Inner, Solar, System };
    mutable EMapView MapView=EMapView::Course;
    mutable bool bMapInitialized=false;
    mutable int32 MapSelection=INDEX_NONE;
    int32 MapPrimary=INDEX_NONE;
    FVector2D MapPan=FVector2D::ZeroVector;
    double MapZoom=1.;
    mutable FVector2D PaintedCenter=FVector2D::ZeroVector;
    mutable double PaintedUnitsPerPixel=1.;
    bool bDraggingMap=false;
    void DrawMinimap(LunaHUD::FDraw& Draw,const ALunaGame& Game) const;
    void DrawNavigationMap(LunaHUD::FDraw& Draw,const ALunaGame& Game) const;
    FReply MapMouseDown(const FPointerEvent& Event);
    void SetMapView(EMapView View);
};
