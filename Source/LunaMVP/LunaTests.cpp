#include "LunaMotion.h"
#include "LunaScale.h"
#include "LunaPropulsion.h"
#include "LunaDevice.h"
#include "LunaSolarSystem.h"
#include "LunaFlightPlan.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "GameFramework/PlayerInput.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString Packet(int64 Seq,int64 Gesture=0,FString Action=TEXT("NONE"),FString Boot=TEXT("board-1"),double Pitch=0,double Age=20)
{
    return FString::Printf(TEXT("{\"schema\":1,\"type\":\"input\",\"bridge_id\":\"bridge-1\",\"boot_id\":\"%s\",\"source_seq\":%lld,\"source_age_ms\":%.1f,\"valid\":true,\"calibrated\":true,\"pitch_deg\":%.1f,\"roll_deg\":0,\"gesture_seq\":%lld,\"gesture\":\"%s\"}"),*Boot,Seq,Age,Pitch,Gesture,*Action);
}
FString Ack(const FLunaDevice& D,int64 Seq)
{
    return FString::Printf(TEXT("{\"schema\":1,\"type\":\"recenter_ack\",\"bridge_id\":\"bridge-1\",\"boot_id\":\"board-1\",\"request_id\":\"%s\",\"ok\":true,\"code\":\"CENTERED\",\"source_seq\":%lld}"),*D.PendingRequest,Seq);
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaMotionTest,"LunaMVP.Motion.FixedStepAndLimits",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaMotionTest::RunTest(const FString&)
{
    TestTrue(TEXT("Moon diameter is 3474.8 km"),FMath::Abs(LunaScale::DistanceKm(56)-3474.8)<1.e-6);
    TestTrue(TEXT("Simulation speed is dimensionally consistent"),FMath::Abs(LunaScale::SpeedKmS(500)*3600-LunaScale::DistanceKm(500))<1.e-6);
    const auto& S=*GetDefault<ULunaSettings>();
    TestEqual(TEXT("dead zone"),LunaMotion::TiltAxis(3,S),0.f);
    TestEqual(TEXT("positive saturation"),LunaMotion::TiltAxis(25,S),1.f);
    TestEqual(TEXT("negative saturation"),LunaMotion::TiltAxis(-25,S),-1.f);
    TestTrue(TEXT("diagonal input magnitude"),LunaMotion::Tilt(25,25,S).Size()<=1.00001);
    TArray<FVector> Positions;
    for(int32 Rate:{30,60,144})
    {
        FVector V=FVector::ZeroVector,P(0,0,300);double Acc=0;
        for(int32 Frame=0;Frame<Rate*6;++Frame)
        {
            Acc+=1./Rate;
            while(Acc+1e-8>=1./120){P+=LunaMotion::Step(V,{1,0},P.Z,0,1.f/120,S);Acc-=1./120;}
        }
        Positions.Add(P);TestTrue(TEXT("speed ceiling"),V.Size2D()<=S.MaxSpeed+.01);
        const float Before=V.X;LunaMotion::Step(V,{-1,0},P.Z,0,1.f/120,S);TestTrue(TEXT("reverse input brakes"),V.X<Before);
        for(int32 I=0;I<1200;++I)LunaMotion::Step(V,{0,0},300,0,1.f/120,S);
        TestTrue(TEXT("neutral drag decays velocity"),V.Size2D()<.01);
    }
    TestTrue(TEXT("30/60 fps deterministic displacement"),FVector::Dist(Positions[0],Positions[1])<.1);
    TestTrue(TEXT("60/144 fps deterministic displacement"),FVector::Dist(Positions[1],Positions[2])<.1);
    for(int32 Layer:{-1,1})
    {
        FVector V=FVector::ZeroVector,P(0,0,300);
        for(int32 I=0;I<1200;++I){P+=LunaMotion::Step(V,{0,0},P.Z,Layer,1.f/120,S);TestTrue(TEXT("vertical speed ceiling"),FMath::Abs(V.Z)<=S.MaxVerticalSpeed+.01);}
        TestTrue(TEXT("PD reaches layer without gravity drift"),LunaMotion::HeightStable(P.Z,V.Z,Layer));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaProtocolTest,"LunaMVP.Input.AgeSequenceAndValidation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaProtocolTest::RunTest(const FString&)
{
    FLunaDevice D;
    TestTrue(TEXT("valid input accepted"),D.Consume(Packet(1),10));
    TestTrue(TEXT("source is initially healthy"),D.Healthy(10.2));
    TestFalse(TEXT("duplicate rejected"),D.Consume(Packet(1),10.22));
    TestFalse(TEXT("duplicate cannot refresh source age"),D.Healthy(10.24));
    TestFalse(TEXT("invalid JSON rejected"),D.Consume(TEXT("broken"),11));
    TestFalse(TEXT("non-finite/outside tilt rejected"),D.Consume(Packet(2,0,TEXT("NONE"),TEXT("board-1"),46),11));
    TestFalse(TEXT("negative age rejected"),D.Consume(Packet(2,0,TEXT("NONE"),TEXT("board-1"),0,-1),11));
    TestTrue(TEXT("new source accepted"),D.Consume(Packet(2),11));
    TestFalse(TEXT("old sequence rejected"),D.Consume(Packet(1),11.01));
    TestTrue(TEXT("new boot accepts reset sequence"),D.Consume(Packet(0,0,TEXT("NONE"),TEXT("board-2")),11.02));
    TestFalse(TEXT("new boot requires recenter"),D.bCentered);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaCenterTest,"LunaMVP.Input.CenterCausalityAndGestures",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaCenterTest::RunTest(const FString&)
{
    FLunaDevice D;D.Consume(Packet(1),10);D.RequestCenter(10.01);
    TestTrue(TEXT("center request is correlated"),!D.PendingRequest.IsEmpty());
    TestTrue(TEXT("matching acknowledgement accepted"),D.Consume(Ack(D,1),10.02));
    TestFalse(TEXT("ACK alone does not unlock"),D.bCentered);
    D.Consume(Packet(1),10.03);TestFalse(TEXT("duplicate after ACK does not unlock"),D.bCentered);
    D.Consume(Packet(2),10.04);TestTrue(TEXT("causal new input unlocks"),D.bCentered);
    D.Consume(Packet(3,1,TEXT("UP")),10.05);D.Consume(Packet(4,1),10.06);
    TestEqual(TEXT("gesture survives later non-event packets"),D.ConsumeGesture(),1);
    TestEqual(TEXT("gesture consumed only once"),D.ConsumeGesture(),0);
    D.InvalidateCenter();D.Consume(Packet(5),10.07);TestFalse(TEXT("pause invalidation cannot reuse earlier ACK"),D.bCentered);
    D.RequestCenter(11);D.Poll(13.1);TestTrue(TEXT("recenter request times out"),D.PendingRequest.IsEmpty());
    TestFalse(TEXT("timed out request cannot unlock"),D.bCentered);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaTiltPlayTest,"LunaMVP.Input.TiltPlayHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FLunaTiltPlayTest::RunTest(const FString&)
{
    FLunaDevice Device;
    const FString Session = TEXT(R"("schema":2,"bridge_id":"b-live","device_id":"nano-a1b2c3d4e5f6","boot_id":"replay-boot","link_epoch":1)");
    const auto Input = [&](int32 Seq, int32 Sample, const TCHAR* Age, bool bValid)
    {
        return FString::Printf(TEXT(R"({%s,"type":"input","seq":%d,"source_seq":%d,"source_age_ms":%s,"source_bound_ms":%s,"valid":%s,"move":[0.5,-0.25],"angles_deg":[-14,0,-8.5]})"),
            *Session, Seq, Sample, Age, Age, bValid ? TEXT("true") : TEXT("false"));
    };

    TestFalse(TEXT("valid input must carry an age"), Device.Consume(Input(1, 1, TEXT("null"), true), 1.0));
    TestTrue(TEXT("startup input can report an unknown age"), Device.Consume(Input(1, 1, TEXT("null"), false), 1.0));
    TestFalse(TEXT("startup input cannot release flight"), Device.Healthy(1.0));
    TestTrue(TEXT("fresh sample is accepted"), Device.Consume(Input(2, 2, TEXT("25"), true), 1.02));
    Device.RequestCenter(1.03);
    const FString Ack = FString::Printf(TEXT(R"({%s,"type":"control_ack","request_id":"%s","ok":true,"code":"CENTERED"})"),
        *Session, *Device.PendingRequest);
    TestTrue(TEXT("gateway ack without op or source sequence is accepted"), Device.Consume(Ack, 1.04));
    Device.Consume(Input(3, 2, TEXT("40"), true), 1.05);
    TestFalse(TEXT("repeated source sample cannot release flight"), Device.bCentered);
    Device.Consume(Input(4, 3, TEXT("25"), true), 1.06);
    TestTrue(TEXT("fresh sample after ack releases flight"), Device.bCentered);
    TestTrue(TEXT("gateway move retains its forward and left signs"), Device.Move.Equals(FVector2D(.5, -.25), .001));
    TestFalse(TEXT("diagnostic angles do not override gateway move"), Device.bHasTiltDegrees);
    TestFalse(TEXT("a legacy frame leaves vertical-step capability off"), Device.bHasVerticalStep);
    auto WithStep = [&](int32 Seq, int32 Sample, const TCHAR* Step)
    {
        return Input(Seq, Sample, TEXT("25"), true).LeftChop(1)
            + FString::Printf(TEXT(",\"vertical_step\":%s}"), Step);
    };
    Device.Consume(WithStep(5, 4, TEXT("null")), 1.07);
    TestTrue(TEXT("a null event advertises vertical-step capability"), Device.bHasVerticalStep);
    const TCHAR* Up = TEXT(R"({"token":1,"direction":1,"age_bound_ms":20})");
    Device.Consume(WithStep(6, 5, Up), 1.08);
    TestEqual(TEXT("a fresh upward push shifts once"), Device.ConsumeGesture(), 1);
    Device.Consume(WithStep(7, 6, Up), 1.09);
    TestEqual(TEXT("a repeated token does not shift again"), Device.ConsumeGesture(), 0);
    Device.Consume(WithStep(8, 7, TEXT(R"({"token":2,"direction":-1,"age_bound_ms":20})")), 1.10);
    TestEqual(TEXT("a fresh downward push shifts once"), Device.ConsumeGesture(), -1);
    TestTrue(TEXT("invalid repeat without an age is accepted"), Device.Consume(Input(9, 7, TEXT("null"), false), 1.11));
    TestFalse(TEXT("invalid repeat immediately stops flight"), Device.bCentered || Device.Healthy(1.11));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaPolarTest,"LunaMVP.Appearance.PolarForceDirection",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaPolarTest::RunTest(const FString&)
{
    const auto& S=*GetDefault<ULunaSettings>();
    auto Power=[&](float Z,float V,int32 Layer,bool Locked=false){return LunaPropulsion::PolarPower(LunaPropulsion::VerticalAcceleration(Z,V,Layer,S),S.MaxVerticalAcceleration,Locked);};
    TestTrue(TEXT("raising orbit fires south, not north"),Power(300,0,1).Equals(FVector2D(0,1),.001));
    TestTrue(TEXT("lowering orbit fires north, not south"),Power(300,0,-1).Equals(FVector2D(1,0),.001));
    TestTrue(TEXT("upward approach brakes using north exhaust"),Power(490,150,1).X>0&&Power(490,150,1).Y==0);
    TestTrue(TEXT("downward approach brakes using south exhaust"),Power(110,-150,-1).Y>0&&Power(110,-150,-1).X==0);
    TestTrue(TEXT("settled orbit has no artificial full-power plume"),Power(500,0,1).IsNearlyZero());
    TestTrue(TEXT("pause and failsafe suppress both polar engines"),Power(300,0,1,true).IsNearlyZero());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaPlanarExhaustTest,"LunaMVP.Appearance.PlanarExhaustDirection",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaPlanarExhaustTest::RunTest(const FString&)
{
    using LunaPropulsion::PlanarPower;
    const FVector Front(1,0,0),Rear(-1,0,0),Port(0,-1,0),Starboard(0,1,0);
    TestEqual(TEXT("forward drive fires aft"),PlanarPower(Rear,{1,0}),1.);
    TestEqual(TEXT("forward drive leaves the front dark"),PlanarPower(Front,{1,0}),0.);
    TestEqual(TEXT("reverse drive fires forward"),PlanarPower(Front,{-1,0}),1.);
    TestEqual(TEXT("right drive fires port thrusters"),PlanarPower(Port,{0,1}),1.);
    TestEqual(TEXT("left drive fires starboard thrusters"),PlanarPower(Starboard,{0,-1}),1.);
    TestEqual(TEXT("diagonal drive applies its lateral share"),PlanarPower(Port,{.8,.6}),.6);
    TestEqual(TEXT("neutral drive has zero directional plume boost"),PlanarPower(Rear,FVector2D::ZeroVector),0.);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaBindingsTest,"LunaMVP.Input.DiagnosticsPreserveRendering",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaBindingsTest::RunTest(const FString&)
{
    for(const auto& Binding:GetDefault<UPlayerInput>()->DebugExecBindings)
        TestFalse(TEXT("F1/F2 cannot execute engine view-mode shortcuts"),Binding.Key==EKeys::F1||Binding.Key==EKeys::F2);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaUDPTest,"LunaMVP.Input.RealUDPLoopbackAndRecenter",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaUDPTest::RunTest(const FString&)
{
    const auto& S=*GetDefault<ULunaSettings>();FLunaDevice D;
    if(!TestTrue(TEXT("UE binds configured input port"),D.Open(S)))return false;
    FSocket* Peer=FUdpSocketBuilder(TEXT("LunaTransportTest")).AsNonBlocking().BoundToEndpoint(FIPv4Endpoint(FIPv4Address::InternalLoopback,S.ControlPort));
    if(!TestNotNull(TEXT("Python-side control endpoint binds"),Peer)){D.Close();return false;}
    auto Destination=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();bool Valid=false;Destination->SetIp(*S.ListenAddress,Valid);Destination->SetPort(S.InputPort);
    auto Send=[&](const FString& Data){FTCHARToUTF8 Bytes(*Data);int32 Sent=0;Peer->SendTo(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length(),Sent,*Destination);FPlatformProcess::Sleep(.015f);D.Poll(FPlatformTime::Seconds());};
    Send(Packet(1));TestTrue(TEXT("actual UDP sensor packet reaches UE"),D.Accepted==1&&D.Healthy(FPlatformTime::Seconds()));
    D.RequestCenter(FPlatformTime::Seconds());FPlatformProcess::Sleep(.015f);
    uint8 Buffer[4097]={};int32 Read=0;const bool Received=Peer->Recv(Buffer,4096,Read)&&Read>0;
    TestTrue(TEXT("UE recenter request reaches configured control port"),Received);
    if(Received){TSharedPtr<FJsonObject> Request;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(reinterpret_cast<char*>(Buffer)))),Request);TestTrue(TEXT("request is correlated"),Request&&Request->GetStringField(TEXT("request_id"))==D.PendingRequest);}
    Send(Ack(D,1));TestFalse(TEXT("UDP ACK alone cannot unlock"),D.bCentered);
    Send(Packet(2));TestTrue(TEXT("new healthy UDP sample after ACK unlocks"),D.bCentered);
    Send(Packet(3,1,TEXT("UP")));Send(Packet(4,1));TestEqual(TEXT("UDP gesture delivered once"),D.ConsumeGesture(),1);TestEqual(TEXT("UDP gesture not repeated"),D.ConsumeGesture(),0);
    D.Poll(FPlatformTime::Seconds()+.5);TestFalse(TEXT("lost UDP source becomes unhealthy"),D.Healthy(FPlatformTime::Seconds()+.5));TestFalse(TEXT("lost source requires fresh recenter"),D.bCentered);
    D.Close();Peer->Close();ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Peer);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaRouteTest,"LunaMVP.Navigation.EclipticKeepOut",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FLunaRouteTest::RunTest(const FString&)
{
    const double Plane=LunaFlight::PlaneZ;
    FLunaSolarSystem System;
    System.Bodies.Add({TEXT("Planet"),{},FVector(900.,0.,Plane),170.,0.});
    System.Bodies.Add({TEXT("Elevated moon"),TEXT("Planet"),FVector(2200.,0.,Plane+800.),100.,0.});
    const FVector Start(0.,0.,Plane), Goal(2500.,0.,Plane);
    TestTrue(TEXT("a direct sweep stops before penetrating a planet"),System.TravelFraction(Start,Goal)<.3);
    TArray<FVector> Route;
    if (!TestTrue(TEXT("the planner detours a same-plane body"),System.PlanRoute(Start,Goal,Route))) return false;
    TestEqual(TEXT("two detour points and the requested endpoint"),Route.Num(),3);
    FVector Previous=Start;
    for (const FVector& Point:Route)
    {
        TestEqual(TEXT("waypoint altitude stays on the ecliptic"),Point.Z,Plane);
        TestEqual(TEXT("planned leg clears bodies and rings"),System.TravelFraction(Previous,Point),1.);
        Previous=Point;
    }
    TestFalse(TEXT("points inside keep-out zones are rejected"),System.PlanRoute(Start,System.Bodies[0].Position,Route));
    return true;
}
#endif
