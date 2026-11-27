#pragma once
#include "CoreMinimal.h"
class FJsonObject;
class FSocket;
class ULunaSettings;

struct FLunaGestureProfile
{
    float ThresholdMps2 = 3.5f;
    int32 HoldMs = 40;
    int32 QuietMs = 250;
    int32 CooldownMs = 500;
};

// Owned and polled only on the game thread. No UObject is touched by a receiver thread.
class FLunaDevice
{
public:
    ~FLunaDevice();
    bool Open(const ULunaSettings& Settings);
    void Close();
    void Poll(double Now);
    bool Consume(const FString& Json, double Now);
    bool Healthy(double Now) const;
    double Age(double Now) const;
    bool CenterReady(double Now) const;
    void RequestCenter(double Now);
    void ConsumeGesture(bool& bUp, bool& bDown);
    int32 ConsumeGesture();
    void SetGestureProfile(const FLunaGestureProfile& Profile);
    void ClearEvents();
    void InvalidateCenter();

    bool bBound=false, bValid=false, bCalibrated=false, bCentered=false;
    FString Status=TEXT("Waiting for controller"), BridgeId, BootId, PendingRequest;
    float Pitch=0, Roll=0;
    FVector2D Move = FVector2D::ZeroVector;
    bool bSchema2 = false;
    bool bHasTiltDegrees = false;
    bool bHasVerticalStep = false;
    bool bGatewayGestureProfile = true;
    FString GestureProfileStatus = TEXT("Waiting for gateway");
    int64 SourceSeq=-1, GestureSeq=-1;
    int32 Accepted=0, Rejected=0, Duplicate=0;
    double LastSourceTime=-1e9, SourceAge=1e9, Stale=.25, RequestTime=0;
private:
    FSocket* Socket=nullptr;
    FString ControlAddress=TEXT("127.0.0.1");
    int32 ControlPort=7002;
    int64 AckSource=-1, DatagramSeq=-1, LinkEpoch=-1;
    bool bAcked=false, bPendingUp=false, bPendingDown=false;
    FString DeviceId;
    FString ExpectedDeviceId;
    TArray<FString> RetiredSessions;

    FLunaGestureProfile GestureProfile;
    bool bHasGestureProfile = false, bGestureProfileDirty = false;
    FString GestureProfileRequest;
    double GestureProfileRequestTime = 0;

    bool ConsumeSchema2(const TSharedPtr<FJsonObject>& Json, const FString& Type, double Now);
    bool ConsumeControlAck(const TSharedPtr<FJsonObject>& Json);
    bool SendControl(const TSharedRef<FJsonObject>& Json);
    TSharedRef<FJsonObject> SessionMessage(const TCHAR* Type) const;
    void TrySendGestureProfile(double Now);
    void ResetGestureProfileRequest();
};
