// TiltPlay — C 侧 UDP 输入服务组件实现

#include "InputServiceSubsystem.h"
#include "CoreMinimal.h"
#include "Tickable.h"

#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "HAL/IConsoleManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

// 日志类别在本文件内自带，不依赖工程的 TiltPlay.h：
// 这样这两个文件可以直接放进任意模块（模块名不影响），搬走时不用改 include。
DEFINE_LOG_CATEGORY_STATIC(LogTiltPlayInput, Log, All);

// 控制台命令 TiltDump：空场景里敲一条就把最新一帧打出来。
// 子系统挂在 GameInstance 上，跟关卡里有没有东西无关，所以纯空场景也能测。
namespace
{
	void TiltDumpCommand(UWorld* World)
	{
		UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		UInputServiceSubsystem* Svc = GI ? GI->GetSubsystem<UInputServiceSubsystem>() : nullptr;
		if (!Svc)
		{
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] TiltDump：拿不到输入服务子系统（GameInstance 还没起？）"));
			return;
		}

		const FTiltInputFrame Frame = Svc->GetLatestFrame();
		UE_LOG(LogTiltPlayInput, Log,
			TEXT("[TiltPlay] TiltDump device=%s seq=%d move=(%.3f,%.3f) angles=(%.2f,%.2f,%.2f) valid=%d stale=%d age=%.1fms recv=%d reject=%d"),
			*Frame.DeviceId, Frame.Seq, Frame.Move.X, Frame.Move.Y,
			Frame.AnglesDeg.X, Frame.AnglesDeg.Y, Frame.AnglesDeg.Z,
			Frame.bValid ? 1 : 0, Frame.bStale ? 1 : 0, Svc->GetPacketAgeMs(),
			Svc->GetReceivedFrames(), Svc->GetRejectedFrames());

		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.f, Frame.bValid ? FColor::Green : FColor::Red,
				FString::Printf(TEXT("TiltDump  seq %d  move[%.2f, %.2f]  angle[%.1f, %.1f]  %s"),
					Frame.Seq, Frame.Move.X, Frame.Move.Y, Frame.AnglesDeg.X, Frame.AnglesDeg.Z,
					Frame.bValid ? TEXT("VALID") : TEXT("INVALID")));
		}
	}

	static FAutoConsoleCommandWithWorld GTiltDumpCommand(
		TEXT("TiltDump"),
		TEXT("打印最新一帧输入：device/seq/move/angles/valid/stale"),
		FConsoleCommandWithWorldDelegate::CreateStatic(&TiltDumpCommand));

	constexpr int32 kReceiveBufferBytes = 2048;

	/** 独立接收线程：非阻塞 recv，每 1 ms 兜一圈，绝不阻塞游戏线程 */
	class FTiltReceiverRunnable : public FRunnable
	{
	public:
		FTiltReceiverRunnable(FSocket* InSocket, UInputServiceSubsystem* InOwner)
			: Socket(InSocket), Owner(InOwner)
		{
		}

		virtual uint32 Run() override
		{
			TArray<uint8> Buffer;
			Buffer.SetNumUninitialized(kReceiveBufferBytes);

			while (!bStop)
			{
				int32 BytesRead = 0;
				const bool bReceived = Socket && Socket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead, ESocketReceiveFlags::None);
				if (bReceived && BytesRead > 0)
				{
					TArray<uint8> Copy;
					Copy.Append(Buffer.GetData(), static_cast<int32>(BytesRead));
					if (Owner)
					{
						Owner->HandlePacket(Copy);
					}
					continue;
				}

				FPlatformProcess::Sleep(0.001f);
			}

			return 0;
		}

		virtual void Stop() override { bStop = true; }
		virtual void Exit() override {}

	private:
		FSocket* Socket;
		UInputServiceSubsystem* Owner;
		FThreadSafeBool bStop{ false };
	};
}

void UInputServiceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!StartReceiver())
	{
		UE_LOG(LogTiltPlayInput, Error, TEXT("[TiltPlay] 7001 绑定失败：端口被占用？先关掉 UE PIE、udp_probe 或另一个网关。"));
	}

	// 子系统没有组件 Tick，用 core ticker 顶替（游戏线程，同原 TickComponent 时机）
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UInputServiceSubsystem::OnTick));
}

void UInputServiceSubsystem::Deinitialize()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	StopReceiver();

	if (ControlSocket)
	{
		if (ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSub->DestroySocket(ControlSocket);
		}
		ControlSocket = nullptr;
	}

	Super::Deinitialize();
}

bool UInputServiceSubsystem::StartReceiver()
{
	if (ListenSocket)
	{
		return bBoundOk;
	}

	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSub)
	{
		return false;
	}

	ListenSocket = SocketSub->CreateSocket(NAME_DGram, TEXT("TiltPlayInputUDP"), false);
	if (!ListenSocket)
	{
		return false;
	}

	// 按 B 的提醒：不要用 SO_REUSEADDR 掩盖"7001 被占"，绑定失败就明确报错
	ListenSocket->SetReuseAddr(false);
	ListenSocket->SetNonBlocking(true);
	ListenSocket->SetRecvErr(false);

	int32 ActualBuffer = 0;
	ListenSocket->SetReceiveBufferSize(1 << 20, ActualBuffer);

	TSharedRef<FInternetAddr> BindAddr = SocketSub->CreateInternetAddr();
	bool bIsValid = false;
	BindAddr->SetIp(TEXT("127.0.0.1"), bIsValid);
	BindAddr->SetPort(ListenPort);

	if (!bIsValid || !ListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogTiltPlayInput, Error, TEXT("[TiltPlay] Bind 127.0.0.1:%d 失败"), ListenPort);
		SocketSub->DestroySocket(ListenSocket);
		ListenSocket = nullptr;
		bBoundOk = false;
		return false;
	}

	ReceiverRunnable = new FTiltReceiverRunnable(ListenSocket, this);
	ReceiverThread = FRunnableThread::Create(ReceiverRunnable, TEXT("TiltPlayInputReceiver"), 0, TPri_AboveNormal);
	bBoundOk = (ReceiverThread != nullptr);

	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] 输入服务已监听 127.0.0.1:%d（控制口 %d，失效阈值 %.0f ms）"),
		ListenPort, ControlPort, StaleSeconds * 1000.f);
	return bBoundOk;
}

void UInputServiceSubsystem::StopReceiver()
{
	if (ReceiverRunnable)
	{
		ReceiverRunnable->Stop();
	}

	// 顶住正在 recv 的线程，最多等 200 ms
	for (int32 i = 0; i < 40 && ReceiverThread == nullptr; ++i)
	{
		FPlatformProcess::Sleep(0.005f);
	}

	if (ReceiverThread)
	{
		ReceiverThread->WaitForCompletion();
		ReceiverThread->Kill(true);
		delete ReceiverThread;
		ReceiverThread = nullptr;
	}

	if (ReceiverRunnable)
	{
		delete ReceiverRunnable;
		ReceiverRunnable = nullptr;
	}

	if (ListenSocket)
	{
		if (ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSub->DestroySocket(ListenSocket);
		}
		ListenSocket = nullptr;
	}

	bBoundOk = false;
	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] 输入服务已停止，7001 已释放"));
}

// ---------------------------------------------------------------- 收帧

void UInputServiceSubsystem::HandlePacket(const TArray<uint8>& Data)
{
	// UTF-8 → FString（补一个 0 结尾）
	TArray<uint8> NullTerminated;
	NullTerminated.Reserve(Data.Num() + 1);
	NullTerminated.Append(Data);
	NullTerminated.Add(0);

	const FString Payload(UTF8_TO_TCHAR(NullTerminated.GetData()));

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		RejectedFrames.Increment();
		UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 丢弃非 JSON 数据报（%d 字节）"), Data.Num());
		return;
	}

	double SchemaNum = 0.0;
	if (!Root->TryGetNumberField(TEXT("schema"), SchemaNum) || FMath::RoundToInt(SchemaNum) != 2)
	{
		RejectedFrames.Increment();
		return;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type) || Type != TEXT("input"))
	{
		RejectedFrames.Increment();
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* MoveArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("move"), MoveArray) || MoveArray == nullptr || MoveArray->Num() != 2)
	{
		RejectedFrames.Increment();
		return;
	}

	// 数值范围与非有限值：指南 4.1.3「检查类型、长度及数值范围」，MVP 7.2「非有限值、超范围不驱动角色」
	const double MoveX = (*MoveArray)[0]->AsNumber();
	const double MoveY = (*MoveArray)[1]->AsNumber();
	if (!FMath::IsFinite(MoveX) || !FMath::IsFinite(MoveY) || FMath::Abs(MoveX) > 1.0 || FMath::Abs(MoveY) > 1.0)
	{
		RejectedFrames.Increment();
		UE_LOG(LogTiltPlayInput, Verbose, TEXT("[TiltPlay] 丢弃非法 move（%.3f, %.3f）"), MoveX, MoveY);
		return;
	}

	FTiltInputFrame Frame;
	Frame.Move.X = static_cast<float>(MoveX);
	Frame.Move.Y = static_cast<float>(MoveY);

	// angles_deg：网关给的原始角度 [前后倾角, 0, 左右倾角]（B 的 mode=head3 才填，tilt2 下恒 0）。
	// 字段缺失按 0 处理；存在就必须是 3 个数字且 |v|<=180，否则整帧拒掉（手册 4.1.3 检查类型、长度及数值范围）。
	const TArray<TSharedPtr<FJsonValue>>* AnglesArray = nullptr;
	if (Root->TryGetArrayField(TEXT("angles_deg"), AnglesArray) && AnglesArray != nullptr)
	{
		if (AnglesArray->Num() != 3)
		{
			RejectedFrames.Increment();
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 丢弃 angles_deg 长度非法帧（%d 项）"), AnglesArray->Num());
			return;
		}
		if ((*AnglesArray)[0]->Type != EJson::Number || (*AnglesArray)[1]->Type != EJson::Number ||
			(*AnglesArray)[2]->Type != EJson::Number)
		{
			RejectedFrames.Increment();
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 丢弃 angles_deg 类型非法帧（非数字）"));
			return;
		}
		const double A0 = (*AnglesArray)[0]->AsNumber();
		const double A1 = (*AnglesArray)[1]->AsNumber();
		const double A2 = (*AnglesArray)[2]->AsNumber();
		if (!FMath::IsFinite(A0) || !FMath::IsFinite(A1) || !FMath::IsFinite(A2) ||
			FMath::Abs(A0) > 180.0 || FMath::Abs(A1) > 180.0 || FMath::Abs(A2) > 180.0)
		{
			RejectedFrames.Increment();
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 丢弃非法 angles_deg（%.2f, %.2f, %.2f）"), A0, A1, A2);
			return;
		}
		Frame.AnglesDeg = FVector(static_cast<float>(A0), static_cast<float>(A1), static_cast<float>(A2));
	}

	Root->TryGetBoolField(TEXT("valid"), Frame.bValid);

	double SeqNum = 0.0;
	Root->TryGetNumberField(TEXT("seq"), SeqNum);
	Frame.Seq = static_cast<int32>(SeqNum);

	double AgeNum = 0.0;
	Root->TryGetNumberField(TEXT("source_age_ms"), AgeNum);
	Frame.AgeMs = static_cast<float>(AgeNum);

	Root->TryGetStringField(TEXT("device_id"), Frame.DeviceId);
	Root->TryGetStringField(TEXT("bridge_id"), Frame.BridgeId);
	Root->TryGetStringField(TEXT("boot_id"), Frame.BootId);

	double EpochNum = 0.0;
	Root->TryGetNumberField(TEXT("link_epoch"), EpochNum);
	Frame.LinkEpoch = static_cast<int32>(EpochNum);

	// device_id 白名单：MVP 7.2「错 ID 不驱动角色」
	if (Frame.DeviceId != FallbackDeviceId)
	{
		RejectedFrames.Increment();
		UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 丢弃非本设备帧 device_id=%s"), *Frame.DeviceId);
		return;
	}

	// 会话门控：MVP 7.2「旧会话不驱动角色」。
	// 会话 = boot_id + link_epoch。只有 epoch 更小才判旧会话整帧丢弃（真回退，例如抓到上一次会话的滞留帧）；
	// epoch 相同但 boot_id 变了、或 epoch 更大，都按新会话接管——网关重启正是这个形态（boot_id 每次新生成，
	// epoch 不一定加一），用字符串比 boot_id 判新旧会在每次重启后把数据全丢掉。
	if (AcceptedLinkEpoch >= 0 && Frame.LinkEpoch < AcceptedLinkEpoch)
	{
		RejectedFrames.Increment();
		UE_LOG(LogTiltPlayInput, Warning,
			TEXT("[TiltPlay] 丢弃旧会话帧 boot=%s epoch=%d（当前 boot=%s epoch=%d）"),
			*Frame.BootId, Frame.LinkEpoch, *AcceptedBootId, AcceptedLinkEpoch);
		return;
	}

	if (AcceptedLinkEpoch >= 0 &&
		(Frame.LinkEpoch > AcceptedLinkEpoch || Frame.BootId != AcceptedBootId))
	{
		UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 新会话接管 boot=%s epoch=%d，序号从 %d 重新计"),
			*Frame.BootId, Frame.LinkEpoch, Frame.Seq);
		AcceptedSeq = -1;
	}

	// 序号单调：MVP 7.2「旧序号不驱动角色」，重复序号同样丢弃
	if (Frame.Seq <= AcceptedSeq)
	{
		RejectedFrames.Increment();
		UE_LOG(LogTiltPlayInput, Verbose, TEXT("[TiltPlay] 丢弃旧序号帧 seq=%d（已接受 %d）"), Frame.Seq, AcceptedSeq);
		return;
	}
	AcceptedBootId = Frame.BootId;
	AcceptedLinkEpoch = Frame.LinkEpoch;
	AcceptedBridgeId = Frame.BridgeId;
	AcceptedSeq = Frame.Seq;

	// vertical_step（可选扩展）：只有启用时网关才发这个字段，未启用时整个字段不存在。
	// 字段非法只丢字段、不丢整帧——MVP 基线里本来就没有它，不能因为扩展出错把基础输入打掉。
	if (const TSharedPtr<FJsonValue>* StepValue = Root->Values.Find(TEXT("vertical_step")))
	{
		if (StepValue && (*StepValue)->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Step = (*StepValue)->AsObject();
			if (Step.IsValid())
			{
				double TokenNum = -1.0;
				double DirectionNum = 0.0;
				double AgeNum2 = -1.0;
				const bool bHasToken = Step->TryGetNumberField(TEXT("token"), TokenNum);
				const bool bHasDirection = Step->TryGetNumberField(TEXT("direction"), DirectionNum);
				const bool bHasAge = Step->TryGetNumberField(TEXT("age_bound_ms"), AgeNum2);

				const bool bTokenOk = bHasToken && TokenNum >= 0.0 && TokenNum <= 4294967295.0
					&& FMath::FloorToDouble(TokenNum) == TokenNum;
				const bool bDirectionOk = bHasDirection && FMath::RoundToDouble(DirectionNum) == DirectionNum
					&& (FMath::RoundToInt(DirectionNum) == 1 || FMath::RoundToInt(DirectionNum) == -1);
				const bool bAgeOk = bHasAge && FMath::IsFinite(AgeNum2) && AgeNum2 >= 0.0 && AgeNum2 <= StepAgeLimitMs;

				if (bTokenOk && bDirectionOk && bAgeOk)
				{
					Frame.bHasVerticalStep = true;
					Frame.StepToken = static_cast<int32>(static_cast<uint32>(TokenNum));
					Frame.StepDirection = FMath::RoundToInt(DirectionNum);
					Frame.StepAgeBoundMs = static_cast<float>(AgeNum2);
				}
				else
				{
					UE_LOG(LogTiltPlayInput, Warning,
						TEXT("[TiltPlay] 丢弃非法 vertical_step（token=%.0f dir=%.0f age=%.2f）"),
						TokenNum, DirectionNum, AgeNum2);
				}
			}
		}
		else
		{
			UE_LOG(LogTiltPlayInput, Verbose, TEXT("[TiltPlay] vertical_step 不是对象，忽略该字段"));
		}
	}

	// jump_token：null 表示无动作；数值 0 也是有效 token
	if (const TSharedPtr<FJsonValue>* TokenValue = Root->Values.Find(TEXT("jump_token")))
	{
		if (TokenValue && TokenValue->IsValid() && !(*TokenValue)->IsNull())
		{
			Frame.bHasJumpToken = true;
			Frame.JumpToken = static_cast<int32>(FMath::RoundToInt((*TokenValue)->AsNumber()));
		}
	}

	const double NowSeconds = Now();
	Frame.StepRecvSeconds = NowSeconds;
	{
		FScopeLock Lock(&FrameLock);
		if (LastRecvSeconds > 0.0)
		{
			MaxGapSeconds = FMath::Max(MaxGapSeconds, NowSeconds - LastRecvSeconds);
		}
		LastRecvSeconds = NowSeconds;
		Frame.bStale = false;
		LatestFrame = Frame;
	}

	ReceivedFrames.Increment();
}

FTiltInputFrame UInputServiceSubsystem::ReadFrame() const
{
	FTiltInputFrame Out;
	double LastRecv = -1.0;
	{
		FScopeLock Lock(&FrameLock);
		Out = LatestFrame;
		LastRecv = LastRecvSeconds;
	}

	// 完全收不到 UDP：不是等下一帧 valid=false，而是本地按时间判失联
	const double AgeSeconds = (LastRecv > 0.0) ? (Now() - LastRecv) : 1000.0;
	Out.bStale = (AgeSeconds > StaleSeconds);
	return Out;
}

float UInputServiceSubsystem::GetPacketAgeMs() const
{
	double LastRecv = -1.0;
	{
		FScopeLock Lock(&FrameLock);
		LastRecv = LastRecvSeconds;
	}
	return (LastRecv > 0.0) ? static_cast<float>((Now() - LastRecv) * 1000.0) : -1.f;
}

float UInputServiceSubsystem::GetMaxGapMs() const
{
	double Gap = 0.0;
	{
		FScopeLock Lock(&FrameLock);
		Gap = MaxGapSeconds;
	}
	return static_cast<float>(Gap * 1000.0);
}

bool UInputServiceSubsystem::IsInputLost() const
{
	return ReadFrame().bStale;
}

bool UInputServiceSubsystem::GetTiltInput(const FString& InDeviceId, FVector2D& OutMove)
{
	const FTiltInputFrame Frame = ReadFrame();

	// 设备对不上就当没有输入（多设备时不会串台）
	if (!InDeviceId.IsEmpty() && !Frame.DeviceId.IsEmpty() &&
		!Frame.DeviceId.Equals(InDeviceId, ESearchCase::IgnoreCase))
	{
		OutMove = FVector2D::ZeroVector;
		return false;
	}

	OutMove = Frame.Move;
	return Frame.bValid && !Frame.bStale;
}

bool UInputServiceSubsystem::GetLocalTiltInput(FVector2D& OutMove)
{
	return GetTiltInput(GetDeviceId(), OutMove);
}

FVector UInputServiceSubsystem::GetAnglesDeg() const
{
	return ReadFrame().AnglesDeg;
}

bool UInputServiceSubsystem::IsDriving() const
{
	const FTiltInputFrame Frame = ReadFrame();
	return Frame.bValid && !Frame.bStale;
}

FTiltInputFrame UInputServiceSubsystem::GetLatestFrame() const
{
	return ReadFrame();
}

FString UInputServiceSubsystem::GetDeviceId() const
{
	FString Id;
	{
		FScopeLock Lock(&FrameLock);
		Id = LatestFrame.DeviceId;
	}
	return Id.IsEmpty() ? FallbackDeviceId : Id;
}

// ---------------------------------------------------------------- 控制口

bool UInputServiceSubsystem::EnsureControlSocket()
{
	if (ControlSocket)
	{
		return true;
	}

	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSub)
	{
		return false;
	}

	ControlSocket = SocketSub->CreateSocket(NAME_DGram, TEXT("TiltPlayControlUDP"), false);
	if (!ControlSocket)
	{
		return false;
	}

	ControlSocket->SetNonBlocking(true);

	// 绑到本地临时端口，用来收回中应答
	TSharedRef<FInternetAddr> LocalAddr = SocketSub->CreateInternetAddr();
	bool bIsValid = false;
	LocalAddr->SetIp(TEXT("127.0.0.1"), bIsValid);
	LocalAddr->SetPort(0);

	if (!bIsValid || !ControlSocket->Bind(*LocalAddr))
	{
		UE_LOG(LogTiltPlayInput, Error, TEXT("[TiltPlay] 控制口本地绑定失败"));
		SocketSub->DestroySocket(ControlSocket);
		ControlSocket = nullptr;
		return false;
	}

	return true;
}

bool UInputServiceSubsystem::SendControlPayload(const FString& PayloadJson)
{
	if (!EnsureControlSocket())
	{
		return false;
	}

	ISocketSubsystem* SocketSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Target = SocketSub->CreateInternetAddr();
	bool bIsValid = false;
	Target->SetIp(TEXT("127.0.0.1"), bIsValid);
	Target->SetPort(ControlPort);
	if (!bIsValid)
	{
		return false;
	}

	// PayloadJson 是 TCHAR（Windows 上 2 字节/字符），必须转成 UTF-8 字节再发
	FTCHARToUTF8 Utf8Payload(*PayloadJson);

	int32 BytesSent = 0;
	// 裸 UDP 没有 Connect，Windows 上 FSocket::Send 会直接失败（WSAENOTCONN），必须走 SendTo
	const bool bSent = ControlSocket->SendTo(reinterpret_cast<const uint8*>(Utf8Payload.Get()),
		Utf8Payload.Length(), BytesSent, *Target);
	return bSent && BytesSent > 0;
}

void UInputServiceSubsystem::SendRecenterPayload()
{
	const FString Device = PendingDeviceId.IsEmpty() ? GetDeviceId() : PendingDeviceId;
	const FString Payload = FString::Printf(
		TEXT("{\"schema\":2,\"type\":\"control\",\"op\":\"recenter\",\"request_id\":\"%s\",\"device_id\":\"%s\"}"),
		*PendingRequestId, *Device);

	if (!SendControlPayload(Payload))
	{
		bRecenterPending = false;
		LastRecenterCode = TEXT("SEND_FAILED");
		OnRecenterResult.Broadcast(false, LastRecenterCode);
	}
}

FString UInputServiceSubsystem::MakeRequestId()
{
	++RequestCounter;
	return FString::Printf(TEXT("ue-%d-%d"), RequestCounter, FMath::RandRange(1000, 9999));
}

void UInputServiceSubsystem::RequestRecenter(const FString& InDeviceId)
{
	if (bRecenterPending)
	{
		return;
	}

	PendingDeviceId = InDeviceId.IsEmpty() ? GetDeviceId() : InDeviceId;
	PendingRequestId = MakeRequestId();
	PendingAttempts = 1;
	PendingSendTime = Now();
	bRecenterPending = true;
	SendRecenterPayload();

	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] 回中请求 %s（device=%s）"), *PendingRequestId, *PendingDeviceId);
}

bool UInputServiceSubsystem::ConsumeJump(const FString& InDeviceId, int32& OutToken)
{
	const FTiltInputFrame Frame = GetLatestFrame();
	if (!Frame.bHasJumpToken)
	{
		return false;
	}
	if (!InDeviceId.IsEmpty() && !Frame.DeviceId.Equals(InDeviceId, ESearchCase::IgnoreCase))
	{
		return false;
	}

	OutToken = Frame.JumpToken;
	AckJumpToken(OutToken);
	return true;
}

	void UInputServiceSubsystem::AckJumpToken(int32 Token)
{
	FTiltInputFrame Frame;
	{
		FScopeLock Lock(&FrameLock);
		Frame = LatestFrame;
	}

	const FString Payload = FString::Printf(
		TEXT("{\"schema\":2,\"type\":\"input_ack\",\"bridge_id\":\"%s\",\"device_id\":\"%s\",\"boot_id\":\"%s\",\"link_epoch\":%d,\"jump_token\":%d}"),
		*Frame.BridgeId, *Frame.DeviceId, *Frame.BootId, Frame.LinkEpoch, Token);

	SendControlPayload(Payload);
	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] input_ack jump_token=%d"), Token);
}

bool UInputServiceSubsystem::ConsumeVerticalStep(const FString& InDeviceId, int32& OutDirection, int32& OutToken)
{
	OutDirection = 0;
	OutToken = 0;

	const FTiltInputFrame Frame = GetLatestFrame();
	// valid=false / stale 一律禁止消费（B 的扩展口径）
	if (!Frame.bValid || Frame.bStale || !Frame.bHasVerticalStep || Frame.StepDirection == 0)
	{
		return false;
	}
	if (!InDeviceId.IsEmpty() && !Frame.DeviceId.Equals(InDeviceId, ESearchCase::IgnoreCase))
	{
		return false;
	}

	// 事件年龄 = 网关侧 age_bound_ms + 收帧后经过的本机时间，超过 200 ms 不消费（只回确认让网关清待发事件）
	const float ElapsedMs = static_cast<float>((Now() - Frame.StepRecvSeconds) * 1000.0) + Frame.StepAgeBoundMs;
	if (ElapsedMs > StepAgeLimitMs)
	{
		UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] vertical_step 过期（%.1f ms > %.0f），只回确认 token=%u"),
			ElapsedMs, StepAgeLimitMs, static_cast<uint32>(Frame.StepToken));
		AckVerticalStep(Frame.StepToken);
		return false;
	}

	// 同一 token 的重复帧：只重发确认，不能再执行一次 Q/E
	// 换会话（boot_id 或 epoch 变了）时 token 从 0 重新计，去重记录必须清空
	if (!LastStepBootId.Equals(Frame.BootId) || LastStepEpoch != Frame.LinkEpoch)
	{
		bHasLastStepToken = false;
		LastStepBootId = Frame.BootId;
		LastStepEpoch = Frame.LinkEpoch;
	}
	if (bHasLastStepToken && Frame.StepToken == LastStepToken)
	{
		AckVerticalStep(Frame.StepToken);
		return false;
	}
	// 模比较挡掉明显倒退的 token（uint32 回绕时差值为小负数，仍算新事件）
	if (bHasLastStepToken)
	{
		const uint32 Delta = static_cast<uint32>(Frame.StepToken) - static_cast<uint32>(LastStepToken);
		if (Delta > 0x7fffffffu)
		{
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] vertical_step token 倒退（%u < %u），丢弃"),
				static_cast<uint32>(Frame.StepToken), static_cast<uint32>(LastStepToken));
			AckVerticalStep(Frame.StepToken);
			return false;
		}
	}

	bHasLastStepToken = true;
	LastStepToken = Frame.StepToken;
	OutDirection = Frame.StepDirection;
	OutToken = Frame.StepToken;

	AckVerticalStep(OutToken);
	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] vertical_step 消费 dir=%d token=%u age=%.1fms+%.1fms"),
		OutDirection, static_cast<uint32>(OutToken), Frame.StepAgeBoundMs, ElapsedMs - Frame.StepAgeBoundMs);
	OnVerticalStep.Broadcast(Frame.DeviceId, OutDirection, OutToken);
	return true;
}

void UInputServiceSubsystem::AckVerticalStep(int32 Token)
{
	FTiltInputFrame Frame;
	{
		FScopeLock Lock(&FrameLock);
		Frame = LatestFrame;
	}

	// 身份字段必须原样带上，网关只认全匹配的确认
	const FString Payload = FString::Printf(
		TEXT("{\"schema\":2,\"type\":\"vertical_step_ack\",\"bridge_id\":\"%s\",\"device_id\":\"%s\",\"boot_id\":\"%s\",\"link_epoch\":%d,\"token\":%u}"),
		*Frame.BridgeId, *Frame.DeviceId, *Frame.BootId, Frame.LinkEpoch, static_cast<uint32>(Token));

	SendControlPayload(Payload);
	UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] vertical_step_ack token=%u"), static_cast<uint32>(Token));
}

void UInputServiceSubsystem::PollControlSocket()
{
	if (!ControlSocket)
	{
		return;
	}

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(1024);

	for (int32 Guard = 0; Guard < 8; ++Guard)
	{
		int32 BytesRead = 0;
		const bool bReceived = ControlSocket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead, ESocketReceiveFlags::None);
		if (!bReceived || BytesRead == 0)
		{
			break;
		}

		TArray<uint8> Copy;
		Copy.Append(Buffer.GetData(), static_cast<int32>(BytesRead));
		HandleAckPacket(Copy);
	}
}

void UInputServiceSubsystem::HandleAckPacket(const TArray<uint8>& Data)
{
	TArray<uint8> NullTerminated;
	NullTerminated.Reserve(Data.Num() + 1);
	NullTerminated.Append(Data);
	NullTerminated.Add(0);

	const FString Payload(UTF8_TO_TCHAR(NullTerminated.GetData()));

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	FString Type;
	Root->TryGetStringField(TEXT("type"), Type);
	if (Type != TEXT("control_ack"))
	{
		return;
	}

	FString RequestId;
	Root->TryGetStringField(TEXT("request_id"), RequestId);

	FString Code;
	Root->TryGetStringField(TEXT("code"), Code);

	bool bOk = false;
	Root->TryGetBoolField(TEXT("ok"), bOk);

	// 只认自己这一轮 request_id，避免旧应答串场
	if (bRecenterPending && RequestId == PendingRequestId)
	{
		bRecenterPending = false;
		LastRecenterCode = Code;
		UE_LOG(LogTiltPlayInput, Log, TEXT("[TiltPlay] 回中应答 %s -> %s（%d 次尝试）"), *RequestId, *Code, PendingAttempts);
		OnRecenterResult.Broadcast(bOk, Code);
	}
}

// GetLastRecenterCode 已在头文件内联定义

// ---------------------------------------------------------------- Tick

bool UInputServiceSubsystem::OnTick(float DeltaSeconds)
{
	PollControlSocket();

	// 回中超时 → 同 request_id 重发，最多 RecenterRetries 次
	if (bRecenterPending && (Now() - PendingSendTime) > RecenterTimeoutSeconds)
	{
		if (PendingAttempts <= RecenterRetries)
		{
			++PendingAttempts;
			PendingSendTime = Now();
			SendRecenterPayload();
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 回中超时，重试第 %d 次（%s）"), PendingAttempts - 1, *PendingRequestId);
		}
		else
		{
			bRecenterPending = false;
			LastRecenterCode = TEXT("TIMEOUT");
			OnRecenterResult.Broadcast(false, LastRecenterCode);
			UE_LOG(LogTiltPlayInput, Warning, TEXT("[TiltPlay] 回中失败：%s 三次无应答"), *PendingRequestId);
		}
	}

	// 每帧广播：新 seq -> OnInputUpdated；有效性翻转 -> OnDeviceStateChanged
	{
		const FTiltInputFrame TickFrame = ReadFrame();
		const bool bTickValid = TickFrame.bValid && !TickFrame.bStale;

		// 上/下单步：默认这里自动消费并回 vertical_step_ack，玩法只需接 OnVerticalStep
		if (bAutoConsumeVerticalStep)
		{
			int32 StepDirection = 0;
			int32 StepToken = 0;
			ConsumeVerticalStep(TEXT(""), StepDirection, StepToken);
		}

		if (TickFrame.Seq != LastBroadcastSeq)
		{
			LastBroadcastSeq = TickFrame.Seq;
			OnInputUpdated.Broadcast(TickFrame.DeviceId, TickFrame.Move, bTickValid);
		}

		const int32 ValidFlag = bTickValid ? 1 : 0;
		if (ValidFlag != LastBroadcastValid)
		{
			LastBroadcastValid = ValidFlag;
			OnDeviceStateChanged.Broadcast(TickFrame.DeviceId, bTickValid,
				bTickValid ? TEXT("valid") : TEXT("invalid"));
		}

		// 首帧有效数据到达时打一行完整的值：这是"信号真到了 UE"的硬证据，
		// 不依赖编辑器、不依赖屏幕，只要看日志就能判。
		if (!bLoggedFirstValid && bTickValid)
		{
			bLoggedFirstValid = true;
			UE_LOG(LogTiltPlayInput, Log,
				TEXT("[TiltPlay] TiltDump 首帧有效数据 device=%s seq=%d move=(%.3f,%.3f) angles=(%.2f,%.2f,%.2f) age=%.1fms recv=%d reject=%d"),
				*TickFrame.DeviceId, TickFrame.Seq, TickFrame.Move.X, TickFrame.Move.Y,
				TickFrame.AnglesDeg.X, TickFrame.AnglesDeg.Y, TickFrame.AnglesDeg.Z,
				GetPacketAgeMs(), GetReceivedFrames(), GetRejectedFrames());
		}
	}

	// 每秒一行统计：既是屏幕取证，也是写进 TiltPlay.log 的实测证据
	const double NowSeconds = Now();
	if (NowSeconds - LastDebugSeconds >= 1.0)
	{
		// 日志间隔本身会漂移（实测 1.0~1.35 s），所以速率必须用真实间隔算，
		// 不能拿"帧数当 fps"，否则会出现 80 fps 这种假数字。
		const double Interval = FMath::Max(NowSeconds - LastDebugSeconds, 0.001);
		const int32 Delta = ReceivedFrames.GetValue() - LastSecondFrames;
		const int32 Rate = FMath::RoundToInt(static_cast<double>(Delta) / Interval);
		LastSecondFrames = ReceivedFrames.GetValue();
		LastDebugSeconds = NowSeconds;

		const FTiltInputFrame Frame = ReadFrame();

		// 用"驱动角色时真正采用的有效性"来报数：bValid && !bStale。
		// 只报 bValid 会出现"网关已停 150 秒、日志还写 valid=1 绿字"的假象。
		const bool bEffective = Frame.bValid && !Frame.bStale;
		const FColor OverlayColor = bEffective ? FColor::Green : FColor::Red;

		if (bLogSummary)
		{
			UE_LOG(LogTiltPlayInput, Log,
				TEXT("[TiltPlay] fps=%d seq=%d valid=%d stale=%d move=(%.3f,%.3f) age=%.1fms maxgap=%.1fms recv=%d reject=%d"),
				Rate, Frame.Seq, bEffective ? 1 : 0, Frame.bStale ? 1 : 0, Frame.Move.X, Frame.Move.Y,
				GetPacketAgeMs(), GetMaxGapMs(), ReceivedFrames.GetValue(), RejectedFrames.GetValue());
		}

		if (bShowDebugOverlay && GEngine)
		{
			const FString Msg = FString::Printf(
				TEXT("TiltPlay  %d fps  seq %d  valid %d  stale %d  move[%.2f, %.2f]  age %.0f ms  maxgap %.0f ms  reject %d"),
				Rate, Frame.Seq, bEffective ? 1 : 0, Frame.bStale ? 1 : 0, Frame.Move.X, Frame.Move.Y,
				GetPacketAgeMs(), GetMaxGapMs(), RejectedFrames.GetValue());

			GEngine->AddOnScreenDebugMessage(static_cast<int32>(GetUniqueID()), 1.2f, OverlayColor, Msg);
		}
	}

	return true;
}
