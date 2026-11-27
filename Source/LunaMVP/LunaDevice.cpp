#include "LunaDevice.h"
#include "LunaSettings.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/UdpSocketBuilder.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool Number(const TSharedPtr<FJsonObject>& J,const TCHAR* Key,double& V)
{ return J->TryGetNumberField(Key,V) && FMath::IsFinite(V); }
bool SampleAge(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, bool bValid, double& Value)
{
    const TSharedPtr<FJsonValue> Field = Json->TryGetField(Key);
    if (!bValid && Field && Field->Type == EJson::Null)
    {
        Value = 1e9;
        return true;
    }
    return Number(Json, Key, Value) && Value >= 0.;
}
bool Counter(const TSharedPtr<FJsonObject>& J,const TCHAR* Key,int64& V)
{
    double N;
    if(!Number(J,Key,N)||N<0||N>9007199254740991.||FMath::FloorToDouble(N)!=N) return false;
    V=static_cast<int64>(N); return true;
}

bool Serial(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, int64& Value)
{
    return Counter(Json, Key, Value) && Value <= MAX_uint32;
}

bool IsNewer(int64 Value, int64 Previous)
{
    if (Previous < 0) return true;
    const uint32 Delta = static_cast<uint32>(Value) - static_cast<uint32>(Previous);
    return Delta != 0 && Delta < 0x80000000u;
}

bool Identifier(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, FString& Value)
{
    return Json->TryGetStringField(Key, Value) && !Value.IsEmpty() && Value.Len() <= 128;
}

bool Pair(const TSharedPtr<FJsonObject>& Json, const TCHAR* Key, double Limit, FVector2D& Value)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Json->TryGetArrayField(Key, Values) || !Values || Values->Num() != 2) return false;
    for (int32 Axis = 0; Axis < 2; ++Axis)
    {
        const TSharedPtr<FJsonValue>& Entry = (*Values)[Axis];
        if (!Entry || Entry->Type != EJson::Number || !Entry->TryGetNumber(Value[Axis]) ||
            !FMath::IsFinite(Value[Axis]) || FMath::Abs(Value[Axis]) > Limit) return false;
    }
    return true;
}

FString SessionKey(const FString& Bridge, const FString& Boot, int64 Epoch)
{
    return FString::Printf(TEXT("%d:%s%d:%s:%lld"), Bridge.Len(), *Bridge, Boot.Len(), *Boot, Epoch);
}
}
FLunaDevice::~FLunaDevice(){ Close(); }
bool FLunaDevice::Open(const ULunaSettings& S)
{
    Close(); Stale=FMath::Max(.05f,S.StaleSeconds); ControlAddress=S.BridgeAddress; ControlPort=S.ControlPort;
    ExpectedDeviceId = S.ExpectedDeviceId;
    bGatewayGestureProfile = S.bGatewayGestureProfile;
    FIPv4Address Address;
    if(!FIPv4Address::Parse(S.ListenAddress,Address)||S.InputPort<1||S.InputPort>65535){Status=TEXT("Invalid UDP endpoint");return false;}
    Socket=FUdpSocketBuilder(TEXT("LunaMVP_Input")).AsNonBlocking().BoundToEndpoint(FIPv4Endpoint(Address,S.InputPort)).WithReceiveBufferSize(65536);
    bBound=Socket!=nullptr; Status=bBound?TEXT("Waiting for controller"):TEXT("UDP port unavailable");
    return bBound;
}
void FLunaDevice::Close()
{
    if(Socket){Socket->Close();ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);Socket=nullptr;}
    bBound=false; bValid=false; bCalibrated=false; bCentered=false; bAcked=false;
    SourceSeq=-1; GestureSeq=-1; LastSourceTime=-1e9; SourceAge=1e9; PendingRequest.Reset(); BridgeId.Reset(); BootId.Reset(); ClearEvents();
    DatagramSeq = LinkEpoch = -1;
    DeviceId.Reset();
    RetiredSessions.Reset();
    Move = FVector2D::ZeroVector;
    bSchema2 = bHasTiltDegrees = bHasVerticalStep = false;
    ResetGestureProfileRequest();
}
void FLunaDevice::Poll(double Now)
{
    if(Socket)
    {
        uint32 Size=0;
        for(int32 I=0;I<64&&Socket->HasPendingData(Size);++I)
        {
            TArray<uint8> Bytes; Bytes.SetNumUninitialized(FMath::Min<uint32>(Size,65507)+1);
            int32 Read=0;
            if(!Socket->Recv(Bytes.GetData(),Bytes.Num()-1,Read)) break;
            if(Read<=0||Read>4096){++Rejected;continue;}
            Bytes[Read]=0;
            Consume(FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()))),Now);
        }
    }
    if (!GestureProfileRequest.IsEmpty() && Now - GestureProfileRequestTime > 2.)
    {
        GestureProfileRequest.Reset();
        GestureProfileStatus = TEXT("Gateway settings timed out");
    }
    TrySendGestureProfile(Now);
    if(!PendingRequest.IsEmpty()&&Now-RequestTime>2.){PendingRequest.Reset();bAcked=false;Status=TEXT("Centering timed out. Try again");}
    if (bCentered && !Healthy(Now))
    {
        InvalidateCenter();
        Status = TEXT("Controller lost. Flight on hold");
        UE_LOG(LogTemp, Display, TEXT("LUNA_CONTROLLER_LOST source_seq=%lld"), SourceSeq);
    }
}
double FLunaDevice::Age(double Now) const { return SourceAge+FMath::Max(0.,Now-LastSourceTime); }
bool FLunaDevice::Healthy(double Now) const {return bValid&&bCalibrated&&Age(Now)<=Stale;}
bool FLunaDevice::CenterReady(double Now) const
{
    return bAcked && Healthy(Now) && (bSchema2 ? IsNewer(SourceSeq, AckSource) : SourceSeq > AckSource);
}
void FLunaDevice::ClearEvents(){ bPendingUp = bPendingDown = false; }
void FLunaDevice::InvalidateCenter(){bCentered=false;bAcked=false;PendingRequest.Reset();ClearEvents();}
void FLunaDevice::ConsumeGesture(bool& bUp, bool& bDown)
{
    bUp = bPendingUp;
    bDown = bPendingDown;
    ClearEvents();
}
int32 FLunaDevice::ConsumeGesture()
{
    bool bUp, bDown;
    ConsumeGesture(bUp, bDown);
    return static_cast<int32>(bUp) - static_cast<int32>(bDown);
}
bool FLunaDevice::Consume(const FString& Text,double Now)
{
    TSharedPtr<FJsonObject> J; auto Reader=TJsonReaderFactory<>::Create(Text);
    auto Fail=[this](){++Rejected;return false;};
    if(Text.Len()>4096||!FJsonSerializer::Deserialize(Reader,J)||!J.IsValid()) return Fail();
    double Schema; FString Type;
    if(!Number(J,TEXT("schema"),Schema)||!J->TryGetStringField(TEXT("type"),Type)) return Fail();
    if (Schema == 2) return ConsumeSchema2(J, Type, Now);
    if (Schema != 1 || bSchema2) return Fail();
    if(Type==TEXT("recenter_ack"))
    {
        FString Request,Code,Bridge,Boot; bool Ok=false; int64 Seq;
        if(!J->TryGetStringField(TEXT("request_id"),Request)||Request!=PendingRequest||Request.IsEmpty()||
           !J->TryGetStringField(TEXT("bridge_id"),Bridge)||Bridge!=BridgeId||!J->TryGetStringField(TEXT("boot_id"),Boot)||Boot!=BootId||
           !J->TryGetBoolField(TEXT("ok"),Ok)||!J->TryGetStringField(TEXT("code"),Code)||!Counter(J,TEXT("source_seq"),Seq))return Fail();
        if(!Ok||Code!=TEXT("CENTERED")){Status=TEXT("Centering incomplete. Hold still and retry");PendingRequest.Reset();return Fail();}
        bAcked=true; AckSource=Seq; Status=TEXT("Waiting for centered input"); return true;
    }
    if(Type!=TEXT("input"))return Fail();
    FString Bridge,Boot,Gesture; double P,R,AgeMs,BoundMs=0; int64 Seq,GSeq; bool Valid,Calibrated;
    if(!J->TryGetStringField(TEXT("bridge_id"),Bridge)||Bridge.IsEmpty()||Bridge.Len()>128||
       !J->TryGetStringField(TEXT("boot_id"),Boot)||Boot.IsEmpty()||Boot.Len()>128||
       !Counter(J,TEXT("source_seq"),Seq)||!Counter(J,TEXT("gesture_seq"),GSeq)||
       !Number(J,TEXT("source_age_ms"),AgeMs)||AgeMs<0||AgeMs>10000||
       !Number(J,TEXT("pitch_deg"),P)||FMath::Abs(P)>45||!Number(J,TEXT("roll_deg"),R)||FMath::Abs(R)>45||
       !J->TryGetBoolField(TEXT("valid"),Valid)||!J->TryGetBoolField(TEXT("calibrated"),Calibrated)||
       !J->TryGetStringField(TEXT("gesture"),Gesture)||(Gesture!=TEXT("NONE")&&Gesture!=TEXT("UP")&&Gesture!=TEXT("DOWN")))return Fail();
    if(J->HasField(TEXT("source_bound_ms"))&&(!Number(J,TEXT("source_bound_ms"),BoundMs)||BoundMs<0||BoundMs>10000))return Fail();
    if(Bridge!=BridgeId||Boot!=BootId)
    {
        BridgeId=Bridge; BootId=Boot; SourceSeq=-1; GestureSeq=-1;
        bCentered=false;bAcked=false;PendingRequest.Reset();ClearEvents();Status=TEXT("Controller reconnected. Recenter required");
    }
    if(Seq<=SourceSeq){++Duplicate;return false;}
    const bool bNewGesture=GSeq>GestureSeq&&GestureSeq>=0;
    SourceSeq=Seq; LastSourceTime=Now; SourceAge=(AgeMs+BoundMs)/1000.;
    bValid=Valid;bCalibrated=Calibrated;Pitch=P;Roll=R;++Accepted;
    bHasTiltDegrees = true;
    if (bHasGestureProfile) GestureProfileStatus = TEXT("Gateway settings unavailable");
    if (bNewGesture && bCentered && Healthy(Now))
    {
        bPendingUp |= Gesture == TEXT("UP");
        bPendingDown |= Gesture == TEXT("DOWN");
    }
    GestureSeq=FMath::Max(GestureSeq,GSeq);
    if(!bCentered&&CenterReady(Now)){bCentered=true;PendingRequest.Reset();ClearEvents();Status=TEXT("Controller centered");}
    return true;
}
bool FLunaDevice::ConsumeSchema2(const TSharedPtr<FJsonObject>& Json, const FString& Type, double Now)
{
    auto Reject = [this]() { ++Rejected; return false; };
    if (Type == TEXT("control_ack")) return ConsumeControlAck(Json) || Reject();
    if (Type != TEXT("input")) return Reject();

    FString Device, Bridge, Boot;
    int64 Epoch, Seq, Sample;
    double SourceAgeMs, SourceBoundMs;
    bool bInputValid;
    FVector2D Axes;
    if (!Identifier(Json, TEXT("device_id"), Device) || !Identifier(Json, TEXT("bridge_id"), Bridge) ||
        !Identifier(Json, TEXT("boot_id"), Boot) ||
        !Serial(Json, TEXT("link_epoch"), Epoch) || !Serial(Json, TEXT("seq"), Seq) ||
        !Serial(Json, TEXT("source_seq"), Sample) || !Json->TryGetBoolField(TEXT("valid"), bInputValid) ||
        !SampleAge(Json, TEXT("source_age_ms"), bInputValid, SourceAgeMs) ||
        !SampleAge(Json, TEXT("source_bound_ms"), bInputValid, SourceBoundMs) ||
        !Pair(Json, TEXT("move"), 1., Axes)) return Reject();

    FVector2D Degrees = FVector2D::ZeroVector;
    const bool bAnglesSupplied = Json->HasField(TEXT("tilt_deg"));
    if (bAnglesSupplied && !Pair(Json, TEXT("tilt_deg"), 180., Degrees)) return Reject();
    if ((!ExpectedDeviceId.IsEmpty() && Device != ExpectedDeviceId) || (bSchema2 && Device != DeviceId)) return Reject();

    if (!bSchema2 || Bridge != BridgeId || Boot != BootId || Epoch != LinkEpoch)
    {
        if (RetiredSessions.Contains(SessionKey(Bridge, Boot, Epoch))) return Reject();
        if (bSchema2)
        {
            if (Bridge == BridgeId && Boot == BootId && !IsNewer(Epoch, LinkEpoch)) return Reject();
            if (RetiredSessions.Num() == 8) RetiredSessions.RemoveAt(0);
            RetiredSessions.Add(SessionKey(BridgeId, BootId, LinkEpoch));
        }
        DeviceId = Device;
        BridgeId = Bridge;
        BootId = Boot;
        LinkEpoch = Epoch;
        SourceSeq = DatagramSeq = GestureSeq = -1;
        bSchema2 = true;
        InvalidateCenter();
        ResetGestureProfileRequest();
        Status = TEXT("Controller connected. Recenter required");
    }

    const bool bNewSample = IsNewer(Sample, SourceSeq);
    if (!IsNewer(Seq, DatagramSeq) || (!bNewSample && Sample != SourceSeq))
    {
        ++Duplicate;
        return false;
    }
    DatagramSeq = Seq;
    bHasVerticalStep = Json->HasField(TEXT("vertical_step"));
    if (bNewSample)
    {
        SourceSeq = Sample;
        LastSourceTime = Now;
        SourceAge = FMath::Max(SourceAgeMs, SourceBoundMs) / 1000.;
        bValid = bInputValid;
        bCalibrated = bInputValid;
        Move = Axes;
        bHasTiltDegrees = bAnglesSupplied;
        Pitch = Degrees.X;
        Roll = Degrees.Y;
        ++Accepted;
    }
    else
    {
        // UDP retransmission can invalidate a sample, but cannot make it newer or valid again.
        bValid &= bInputValid;
        ++Duplicate;
    }

    const bool bAcceptStep = bCentered && Healthy(Now);
    if (bCentered && !bAcceptStep)
    {
        InvalidateCenter();
        Status = TEXT("Controller lost. Flight on hold");
        UE_LOG(LogTemp, Display, TEXT("LUNA_CONTROLLER_LOST source_seq=%lld"), SourceSeq);
    }
    else if (!bCentered && CenterReady(Now))
    {
        bCentered = true;
        PendingRequest.Reset();
        Status = TEXT("Controller centered");
    }

    const TSharedPtr<FJsonObject>* Step = nullptr;
    int64 Token;
    double Direction, StepAge;
    if (Json->TryGetObjectField(TEXT("vertical_step"), Step) && Step && Step->IsValid() &&
        Serial(*Step, TEXT("token"), Token) &&
        Number(*Step, TEXT("direction"), Direction) && (Direction == -1 || Direction == 1) &&
        Number(*Step, TEXT("age_bound_ms"), StepAge) && StepAge >= 0)
    {
        if (IsNewer(Token, GestureSeq))
        {
            GestureSeq = Token;
            if (bAcceptStep && StepAge <= 200.)
            {
                bPendingUp |= Direction > 0;
                bPendingDown |= Direction < 0;
            }
        }
        const auto Ack = SessionMessage(TEXT("vertical_step_ack"));
        Ack->SetNumberField(TEXT("token"), Token);
        SendControl(Ack);
    }
    return true;
}

bool FLunaDevice::ConsumeControlAck(const TSharedPtr<FJsonObject>& Json)
{
    FString Device, Bridge, Boot, Request, Code, Op;
    int64 Epoch;
    bool bOk;
    if (!bSchema2 || !Identifier(Json, TEXT("device_id"), Device) || Device != DeviceId ||
        !Identifier(Json, TEXT("bridge_id"), Bridge) || Bridge != BridgeId ||
        !Identifier(Json, TEXT("boot_id"), Boot) || Boot != BootId ||
        !Serial(Json, TEXT("link_epoch"), Epoch) || Epoch != LinkEpoch ||
        !Identifier(Json, TEXT("request_id"), Request) || !Identifier(Json, TEXT("code"), Code) ||
        !Json->TryGetBoolField(TEXT("ok"), bOk)) return false;
    Json->TryGetStringField(TEXT("op"), Op);

    if (Request == GestureProfileRequest && Op == TEXT("set_gesture_profile"))
    {
        GestureProfileRequest.Reset();
        GestureProfileStatus = (bOk && Code == TEXT("PROFILE_APPLIED"))
            ? TEXT("Gateway settings applied") : FString::Printf(TEXT("Gateway rejected settings: %s"), *Code);
        return true;
    }

    if (Request != PendingRequest || (!Op.IsEmpty() && Op != TEXT("recenter"))) return false;
    if (!bOk || Code != TEXT("CENTERED"))
    {
        bAcked = false;
        PendingRequest.Reset();
        if (Code == TEXT("NOT_STILL") || Code == TEXT("NOT_LEVEL"))
        {
            Status = TEXT("Lay the board flat and still, then press C");
        }
        else if (Code == TEXT("STALE"))
        {
            Status = TEXT("Waiting for fresh board samples. Try C again");
        }
        else
        {
            Status = FString::Printf(TEXT("Centering failed (%s). Try C again"), *Code);
        }
        UE_LOG(LogTemp, Display, TEXT("LUNA_CONTROLLER_CENTER result=%s"), *Code);
        return true;
    }
    // Keep the stricter boundary when the gateway supplies the source sequence at recenter.
    int64 AckSample;
    AckSource = SourceSeq;
    if (Serial(Json, TEXT("source_seq"), AckSample) && IsNewer(AckSample, SourceSeq)) AckSource = AckSample;
    bAcked = true;
    Status = TEXT("Waiting for centered input");
    UE_LOG(LogTemp, Display, TEXT("LUNA_CONTROLLER_CENTER result=%s"), *Code);
    return true;
}

TSharedRef<FJsonObject> FLunaDevice::SessionMessage(const TCHAR* Type) const
{
    TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetNumberField(TEXT("schema"), bSchema2 ? 2 : 1);
    Message->SetStringField(TEXT("type"), Type);
    Message->SetStringField(TEXT("bridge_id"), BridgeId);
    Message->SetStringField(TEXT("boot_id"), BootId);
    if (bSchema2)
    {
        Message->SetStringField(TEXT("device_id"), DeviceId);
        Message->SetNumberField(TEXT("link_epoch"), LinkEpoch);
    }
    return Message;
}

bool FLunaDevice::SendControl(const TSharedRef<FJsonObject>& Json)
{
    if (!Socket) return false;
    FString Text;
    const auto Writer = TJsonWriterFactory<>::Create(&Text);
    if (!FJsonSerializer::Serialize(Json, Writer)) return false;

    const auto Destination = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
    bool bAddressValid = false;
    Destination->SetIp(*ControlAddress, bAddressValid);
    Destination->SetPort(ControlPort);
    FTCHARToUTF8 Bytes(*Text);
    int32 Sent = 0;
    return bAddressValid && Socket->SendTo(reinterpret_cast<const uint8*>(Bytes.Get()),
        Bytes.Length(), Sent, *Destination) && Sent == Bytes.Length();
}

void FLunaDevice::RequestCenter(double Now)
{
    InvalidateCenter();
    if (BridgeId.IsEmpty() || BootId.IsEmpty())
    {
        Status = TEXT("Connect controller first");
        return;
    }
    PendingRequest = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    RequestTime = Now;
    const auto Request = SessionMessage(bSchema2 ? TEXT("control") : TEXT("recenter"));
    Request->SetStringField(TEXT("request_id"), PendingRequest);
    if (bSchema2) Request->SetStringField(TEXT("op"), TEXT("recenter"));
    if (Socket && !SendControl(Request))
    {
        PendingRequest.Reset();
        Status = TEXT("Center request failed");
        return;
    }
    Status = TEXT("Hold still. Centering");
}

void FLunaDevice::SetGestureProfile(const FLunaGestureProfile& Profile)
{
    if (!bGatewayGestureProfile) return;
    GestureProfile = Profile;
    bHasGestureProfile = true;
    ResetGestureProfileRequest();
}

void FLunaDevice::ResetGestureProfileRequest()
{
    GestureProfileRequest.Reset();
    bGestureProfileDirty = bHasGestureProfile;
    GestureProfileStatus = !bSchema2 && !BridgeId.IsEmpty()
        ? TEXT("Gateway settings unavailable") : TEXT("Waiting for gateway");
}

void FLunaDevice::TrySendGestureProfile(double Now)
{
    if (!bGestureProfileDirty || !bSchema2 || !Socket) return;
    bGestureProfileDirty = false;
    const auto Profile = MakeShared<FJsonObject>();
    Profile->SetNumberField(TEXT("threshold_mps2"), GestureProfile.ThresholdMps2);
    Profile->SetNumberField(TEXT("hold_ms"), GestureProfile.HoldMs);
    Profile->SetNumberField(TEXT("quiet_ms"), GestureProfile.QuietMs);
    Profile->SetNumberField(TEXT("cooldown_ms"), GestureProfile.CooldownMs);

    const auto Request = SessionMessage(TEXT("control"));
    GestureProfileRequest = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Request->SetStringField(TEXT("request_id"), GestureProfileRequest);
    Request->SetStringField(TEXT("op"), TEXT("set_gesture_profile"));
    Request->SetObjectField(TEXT("profile"), Profile);
    if (SendControl(Request))
    {
        GestureProfileRequestTime = Now;
        GestureProfileStatus = TEXT("Applying gateway settings");
    }
    else
    {
        GestureProfileRequest.Reset();
        GestureProfileStatus = TEXT("Gateway settings send failed");
    }
}
