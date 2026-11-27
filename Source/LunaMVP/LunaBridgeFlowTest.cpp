#include "LunaDevice.h"
#include "LunaMotion.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLunaBridgeFlowTest, "LunaBCD.Schema2BridgeFlow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLunaBridgeFlowTest::RunTest(const FString&)
{
    const ULunaSettings& Settings = *GetDefault<ULunaSettings>();
    FLunaDevice Device;
    if (!TestTrue(TEXT("C binds the configured input port"), Device.Open(Settings)))
    {
        return false;
    }

    ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    FUniqueSocket PhaseSocket = Sockets->CreateUniqueSocket(NAME_DGram, TEXT("LunaBCDFixturePhase"));
    if (!TestTrue(TEXT("fixture phase socket opened"), PhaseSocket.IsValid()))
    {
        return false;
    }
    const auto PhaseAddress = FIPv4Endpoint(FIPv4Address::InternalLoopback, 7003).ToInternetAddr();
    auto Phase = [&](const ANSICHAR* Name)
    {
        const int32 Length = FCStringAnsi::Strlen(Name);
        int32 Sent = 0;
        return PhaseSocket->SendTo(reinterpret_cast<const uint8*>(Name), Length, Sent, *PhaseAddress)
            && Sent == Length;
    };
    auto Until = [&](double Seconds, TFunctionRef<bool()> Condition)
    {
        const double Deadline = FPlatformTime::Seconds() + Seconds;
        while (FPlatformTime::Seconds() < Deadline)
        {
            Device.Poll(FPlatformTime::Seconds());
            if (Condition())
            {
                return true;
            }
            FPlatformProcess::Sleep(.008f);
        }
        return false;
    };

    if (!TestTrue(TEXT("Python B delivers live schema 2 telemetry"), Until(4., [&]
        { return Device.bSchema2 && Device.SourceSeq >= 0; })))
    {
        return false;
    }
    const int64 FirstSource = Device.SourceSeq;
    if (!TestTrue(TEXT("stationary raw samples are advancing"), Until(3., [&]
        { return Device.SourceSeq >= FirstSource + 65; })))
    {
        return false;
    }
    Device.RequestCenter(FPlatformTime::Seconds());
    if (!TestTrue(TEXT("B ACK and a fresh centered sample unlock C"), Until(2.5, [&]
        { return Device.bCentered; })))
    {
        AddError(FString::Printf(TEXT("Center status: %s"), *Device.Status));
        return false;
    }

    FLunaGestureProfile Profile;
    Profile.ThresholdMps2 = 4.2f;
    Profile.HoldMs = 55;
    Profile.QuietMs = 230;
    Profile.CooldownMs = 450;
    Device.SetGestureProfile(Profile);
    if (!TestTrue(TEXT("B applied the settings transaction"), Until(2.5, [&]
        { return Device.GestureProfileStatus == TEXT("Gateway settings applied"); })))
    {
        AddError(FString::Printf(TEXT("Settings status: %s"), *Device.GestureProfileStatus));
        return false;
    }

    const auto Acceleration = [&](double ForwardGain = 1., float DeadZone = 3.f)
    {
        const FVector2D Input = Device.bHasTiltDegrees ? FVector2D(Device.Pitch, Device.Roll) : Device.Move;
        const FVector2D Command = LunaMotion::DeviceTilt(Input, Device.bHasTiltDegrees, DeadZone, Settings.FullTiltDegrees, FVector2D(ForwardGain, 1.));
        constexpr float StepSeconds = 1.f / 120.f;
        FVector Velocity = FVector::ZeroVector;
        LunaMotion::Step(Velocity, Command, 300.f, 0, StepSeconds, Settings);
        return FVector2D(Velocity.X, Velocity.Y) / StepSeconds;
    };
    const auto WaitForAxis = [&](const ANSICHAR* Name, double Forward, double Right)
    {
        const FVector2D Expected(Forward, Right);
        return Phase(Name) && Until(3., [&]
        {
            return Device.Move.Equals(Expected, .06)
                && Device.bCentered && Device.Healthy(FPlatformTime::Seconds());
        });
    };

    if (!TestTrue(TEXT("B small forward tilt reaches the C forward axis"), WaitForAxis("pitch_8", 5. / 22., 0.)))
    {
        return false;
    }
    const FVector2D SmallForward = Acceleration();
    if (!TestTrue(TEXT("B large forward tilt reaches the C forward axis"), WaitForAxis("pitch_18", 15. / 22., 0.)))
    {
        return false;
    }
    const FVector2D LargeForward = Acceleration();
    TestTrue(TEXT("player sensitivity changes forward acceleration"), Acceleration(1.25).X > LargeForward.X);
    TestTrue(TEXT("player dead zone reduces forward acceleration"), Acceleration(1., 12.f).X < LargeForward.X);
    if (!TestTrue(TEXT("B rearward tilt produces negative Luna acceleration"),
        WaitForAxis("pitch_-18", -15. / 22., 0.) && Acceleration().X < 0.))
    {
        return false;
    }
    if (!TestTrue(TEXT("B small left tilt reaches the C right axis with a negative sign"), WaitForAxis("roll_-8", 0., -5. / 22.)))
    {
        return false;
    }
    const FVector2D SmallLeft = Acceleration();
    if (!TestTrue(TEXT("B large left tilt reaches the C right axis with a negative sign"), WaitForAxis("roll_-18", 0., -15. / 22.)))
    {
        return false;
    }
    const FVector2D LargeLeft = Acceleration();
    if (!TestTrue(TEXT("B right tilt produces positive Luna acceleration"),
        WaitForAxis("roll_18", 0., 15. / 22.) && Acceleration().Y > 0.))
    {
        return false;
    }
    TestTrue(TEXT("Luna accelerates more strongly as forward tilt increases"), SmallForward.X > 0. && LargeForward.X > SmallForward.X * 1.6);
    TestTrue(TEXT("Luna accelerates more strongly as left tilt increases"), SmallLeft.Y < 0. && LargeLeft.Y < SmallLeft.Y * 1.6);
    TestTrue(TEXT("isolated tilt does not accelerate the other axis"),
        FMath::Abs(LargeForward.Y) < 0.1 * LargeForward.X && FMath::Abs(LargeLeft.X) < 0.1 * -LargeLeft.Y);
    AddInfo(FString::Printf(TEXT("Measured commanded acceleration, cm/s^2: forward %.1f -> %.1f; left %.1f -> %.1f"),
        SmallForward.X, LargeForward.X, SmallLeft.Y, LargeLeft.Y));

    const auto CountGesture = [&](const ANSICHAR* Name, double Seconds)
    {
        FIntPoint Count = FIntPoint::ZeroValue;
        TestTrue(TEXT("gesture fixture phase sent"), Phase(Name));
        Until(Seconds, [&]
        {
            bool bUp = false;
            bool bDown = false;
            Device.ConsumeGesture(bUp, bDown);
            Count.X += bUp;
            Count.Y += bDown;
            return false;
        });
        return Count;
    };
    if (!TestTrue(TEXT("controller returns to neutral"), WaitForAxis("neutral", 0., 0.)))
    {
        return false;
    }
    Until(.8, [] { return false; });
    TestEqual(TEXT("3.8 m/s^2 is below the applied 4.2 m/s^2 trigger"), CountGesture("below", .8), FIntPoint(0, 0));
    TestEqual(TEXT("one upward physical pulse produces one C up boolean"), CountGesture("up", .9), FIntPoint(1, 0));
    TestEqual(TEXT("one downward physical pulse produces one C down boolean"), CountGesture("down", .9), FIntPoint(0, 1));

    if (!TestTrue(TEXT("fresh moving input is available before the sample freezes"), WaitForAxis("pitch_18", 15. / 22., 0.)))
    {
        return false;
    }
    TestTrue(TEXT("freeze fixture phase sent"), Phase("repeat_source"));
    TestTrue(TEXT("retransmitted raw sample expires and locks control"), Until(1.2, [&]
        { return !Device.bCentered && !Device.Healthy(FPlatformTime::Seconds()); }));
    TestTrue(TEXT("fresh source sequence resumes after freeze"), Phase("neutral"));
    const int64 StoppedSource = Device.SourceSeq;
    TestTrue(TEXT("healthy telemetry alone cannot clear the recenter lock"), Until(2., [&]
        { return Device.SourceSeq > StoppedSource && !Device.bCentered; }));
    Until(.9, [] { return false; });
    Device.RequestCenter(FPlatformTime::Seconds());
    TestTrue(TEXT("recovery accepts the new recenter transaction"), Until(2.5, [&] { return Device.bCentered; }));
    return true;
}
#endif
