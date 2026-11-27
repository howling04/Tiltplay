#pragma once
#include "CoreMinimal.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElementTypes.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace LunaHUD
{
inline FLinearColor C(const TCHAR* S){return FLinearColor::FromSRGBColor(FColor::FromHex(S));}
inline const FLinearColor White=C(TEXT("EDF3F6")),Blue=C(TEXT("AFD8EA")),Muted=C(TEXT("95A3AD")),Warning=C(TEXT("F3CBA5"));
inline const FLinearColor Dark=C(TEXT("111B22"));
inline FLinearColor Alpha(FLinearColor Color,float A){Color.A=A;return Color;}
inline const FLinearColor Rule=Alpha(White,.25f);
struct FDraw
{
    const FGeometry& G;FSlateWindowElementList& O;int32 L;float S;FVector2f Offset;
    FDraw(const FGeometry& Geo,FSlateWindowElementList& Out,int32 Layer):G(Geo),O(Out),L(Layer)
    {S=FMath::Min(G.GetLocalSize().X/1280.,G.GetLocalSize().Y/720.);Offset=FVector2f((G.GetLocalSize()-FVector2D(1280*S,720*S))*.5);}
    FPaintGeometry PG(float X,float Y,float W=1,float H=1)const{return G.ToPaintGeometry(FVector2f(W*S,H*S),FSlateLayoutTransform(Offset+FVector2f(X*S,Y*S)));}
    void Rect(float X,float Y,float W,float H,FLinearColor Color){FSlateDrawElement::MakeBox(O,L++,PG(X,Y,W,H),FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),ESlateDrawEffect::None,Color);}
    void Line(float X,float Y,float X2,float Y2,FLinearColor Color,float Width=1)
    {TArray<FVector2D> P={{X*S+Offset.X,Y*S+Offset.Y},{X2*S+Offset.X,Y2*S+Offset.Y}};FSlateDrawElement::MakeLines(O,L++,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,Width*S);}
    FSlateFontInfo Font(int32 Size,bool Title=false)const
    {
        const FString Path=FPaths::ProjectContentDir()/(Title?TEXT("LunaArt/Fonts/Rajdhani-Medium.ttf"):TEXT("LunaArt/Fonts/NotoSansCJKsc-Regular.otf"));
        static TMap<FString,TSharedPtr<const FCompositeFont>> Fonts;
        if(!Fonts.Contains(Path))Fonts.Add(Path,MakeShared<FCompositeFont>(TEXT("Regular"),Path,EFontHinting::Default,EFontLoadingPolicy::LazyLoad));
        return FSlateFontInfo(Fonts[Path],FMath::Max(6.f,Size*S*.75f),TEXT("Regular"));
    }
    void Text(const FString& T,float X,float Y,int32 Size,FLinearColor Color,bool Title=false)
    {FSlateDrawElement::MakeText(O,L++,PG(X,Y),T,Font(Size,Title),ESlateDrawEffect::None,Color);}
    float Width(const FString& T,int32 Size,bool Title=false)const
    {return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(T,Font(Size,Title)).X/S;}
    void Center(const FString& T,float X,float Y,int32 Size,FLinearColor Color,bool Title=false)
    {Text(T,X-Width(T,Size,Title)*.5,Y,Size,Color,Title);}
    void Diamond(float X,float Y,float R,FLinearColor Color)
    {Line(X-R,Y,X,Y-R,Color);Line(X,Y-R,X+R,Y,Color);Line(X+R,Y,X,Y+R,Color);Line(X,Y+R,X-R,Y,Color);}
    void Panel(float X,float Y,float W,float H,float Opacity=.73f)
    {
        Rect(X,Y,W,H,Alpha(Dark,Opacity));
        Line(X,Y,X+W,Y,Alpha(Blue,.27f));
        Line(X,Y+H,X+W,Y+H,Alpha(White,.14f));
    }
    void Gauge(float X,float Y,float W,float H,float Value)
    {
        Rect(X,Y,W,H,Alpha(Blue,.12f));
        const float Fill=W*FMath::Clamp(Value,0.f,1.f);
        if (Fill>0.f) Rect(X,Y,Fill,H,Alpha(Blue,.86f));
        for (int32 I=1;I<8;++I)
            Line(X+W*I/8.f,Y,X+W*I/8.f,Y+H,Alpha(Dark,.70f));
    }
    void Ellipse(float X,float Y,float RX,float RY,FLinearColor Color)
    {
        TArray<FVector2f> Points;
        constexpr int32 Steps=48;
        Points.Reserve(Steps+1);
        for (int32 I=0;I<=Steps;++I)
        {
            const float Angle=2.f*PI*I/Steps;
            Points.Add(FVector2f(X+RX*FMath::Cos(Angle),Y+RY*FMath::Sin(Angle))*S+Offset);
        }
        FSlateDrawElement::MakeLines(O,L++,G.ToPaintGeometry(),MoveTemp(Points),ESlateDrawEffect::None,Color,true,S);
    }
};
}
