// TiltPlay — C 侧 UDP 输入服务（UE 5.8）
// 契约来源：指南 §4.1（UGameInstanceSubsystem + GetTiltInput / OnInputUpdated /
// OnDeviceStateChanged / RequestRecenter / ConsumeJump），B 的 HANDOFF.md §4。

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "InputServiceSubsystem.generated.h"

/** 最近一帧输入（只保留最新，不排队） */
USTRUCT(BlueprintType)
struct FTiltInputFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") FVector2D Move = FVector2D(0.f, 0.f);
	/** 原始角度（度）：X = 前后倾角（pitch），Z = 左右倾角（roll），Y 恒 0。
	   只有网关 mode=head3 才非零；tilt2 配置下恒 (0,0,0)。 */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") FVector AnglesDeg = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") int32 Seq = 0;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") float AgeMs = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") bool bHasJumpToken = false;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") int32 JumpToken = 0;
	/** 可选扩展 vertical_step：未启用时字段不存在，整块保持 false/0 */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") bool bHasVerticalStep = false;
	/** 设备会话内独立 uint32 事件计数，0 也是有效 token */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") int32 StepToken = 0;
	/** 只允许 +1（上推=Q）或 -1（下推=E），其余一律丢弃 */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") int32 StepDirection = 0;
	/** 网关侧事件年龄（从首次连续超阈采样时刻起算，含识别等待） */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") float StepAgeBoundMs = 0.f;
	/** 收到该帧的本机时刻，用来加"收帧后经过时间"再复核一次 200 ms */
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") double StepRecvSeconds = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") FString DeviceId;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") FString BridgeId;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") FString BootId;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") int32 LinkEpoch = 0;
	UPROPERTY(BlueprintReadOnly, Category = "TiltPlay") bool bStale = true;
};

/** 每来一帧广播一次（蓝图 HUD 用） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTiltInputUpdated, const FString&, DeviceId, FVector2D, Move, bool, bValid);

/** 设备有效性翻转时广播一次（valid ↔ invalid） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTiltDeviceStateChanged, const FString&, DeviceId, bool, bValid, const FString&, Reason);

/** 回中结果（code 原样透传 B 的取值：CENTERED / NOT_STILL / STALE / SESSION_MISMATCH / EPOCH_MISMATCH） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTiltRecenterResult, bool, bOk, const FString&, Code);

/** 上/下单步手势被消费一次（Direction 只可能是 +1 / -1；Token 用于日志与去重） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTiltVerticalStep, const FString&, DeviceId, int32, Direction, int32, Token);

/**
 * 输入服务子系统：整个 GameInstance 一个，非阻塞收 7001，控制口 7002。
 * 生命周期：Initialize 起线程 → 每帧 OnTick 收控制口应答并广播 → Deinitialize 停线程、销毁 Socket。
 */
UCLASS(ClassGroup = (TiltPlay), DisplayName = "Tilt Input Service")
class TILTPLAYINPUT_API UInputServiceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// -- 监听参数（与 B 冻结的参数对齐） --
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net", meta = (ConfigRestartRequired = true))
	int32 ListenPort = 7001;

	/** vertical_step 事件年龄上限：网关 age_bound_ms + 收帧后经过时间，超过就不消费 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	float StepAgeLimitMs = 200.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	int32 ControlPort = 7002;

	/** 超过该时间没有新帧即判定失联（B 的失效阈值 250 ms） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	float StaleSeconds = 0.25f;

	/** 回中单次等待上限（B 要求 300 ms） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	float RecenterTimeoutSeconds = 0.3f;

	/** 回中超时后同 request_id 再重试的次数（B 要求 2 次） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	int32 RecenterRetries = 2;

	/** 本机唯一 device_id：既用于没收到帧时回中，也作为收帧白名单（MVP 7.2 错 ID 不驱动） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TiltPlay|Net")
	FString FallbackDeviceId = TEXT("nano-a1b2c3d4e5f6");

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "TiltPlay|Debug")
	bool bShowDebugOverlay = true;

	/** 每秒往 Saved/Logs/TiltPlay.log 打一行统计（实测取证用） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "TiltPlay|Debug")
	bool bLogSummary = true;

	/** 自动消费上/下单步事件并广播 OnVerticalStep；关掉后由玩法自己调 ConsumeVerticalStep（默认开着，否则没人消费就没有确认） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "TiltPlay")
	bool bAutoConsumeVerticalStep = true;

	UPROPERTY(BlueprintAssignable, Category = "TiltPlay")
	FOnTiltInputUpdated OnInputUpdated;

	UPROPERTY(BlueprintAssignable, Category = "TiltPlay")
	FOnTiltDeviceStateChanged OnDeviceStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "TiltPlay")
	FOnTiltRecenterResult OnRecenterResult;

	/** 上/下单步手势：每消费一个事件广播一次（重复帧只回确认，不重复广播） */
	UPROPERTY(BlueprintAssignable, Category = "TiltPlay")
	FOnTiltVerticalStep OnVerticalStep;

	// -- UGameInstanceSubsystem 生命周期 --
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** FTSTicker 驱动：每帧收控制口应答 + 每秒统计行 + 广播 */
	bool OnTick(float DeltaSeconds);

	// -- 蓝图接口（手册 §4.1.3） --
	/** 取指定设备的最新输入；返回 false 表示这一帧不能驱动角色 */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	bool GetTiltInput(const FString& DeviceId, FVector2D& OutMove);

	/** 蓝图用：不填 device_id，直接取本地设备（FallbackDeviceId）的输入 */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	bool GetLocalTiltInput(FVector2D& OutMove);

	/** 当前帧原始角度（度）：X=pitch，Z=roll，Y 恒 0（见 FTiltInputFrame::AnglesDeg 的说明） */
	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	FVector GetAnglesDeg() const;

	/** 这一帧到底能不能驱动：bValid 且未 stale。蓝图里用它当分支条件最省事 */
	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	bool IsDriving() const;

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	FTiltInputFrame GetLatestFrame() const;

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	float GetPacketAgeMs() const;

	/** 完全收不到 UDP 也算失联：返回 true 时角色必须停控 */
	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	bool IsInputLost() const;

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	int32 GetReceivedFrames() const { return ReceivedFrames.GetValue(); }

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	int32 GetRejectedFrames() const { return RejectedFrames.GetValue(); }

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	float GetMaxGapMs() const;

	/** 发起回中；结果通过 OnRecenterResult 或 GetLastRecenterCode 给出 */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	void RequestRecenter(const FString& DeviceId = TEXT(""));

	/** 取出并确认一个跳跃令牌（返回 true 时已回 input_ack；0 也是有效 token） */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	bool ConsumeJump(const FString& DeviceId, int32& OutToken);

	/** 只回 input_ack，不负责取令牌（ConsumeJump 内部也用这个） */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	void AckJumpToken(int32 Token);

	/** 取出并确认一个上/下单步事件：返回 true 时 OutDirection 为 +1（上推=Q）或 -1（下推=E）。
	    同一 token 的重复帧只回 vertical_step_ack，不会重复返回 true；过期事件回确认但返回 false。 */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	bool ConsumeVerticalStep(const FString& DeviceId, int32& OutDirection, int32& OutToken);

	/** 只回 vertical_step_ack，不消费 */
	UFUNCTION(BlueprintCallable, Category = "TiltPlay")
	void AckVerticalStep(int32 Token);

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	bool IsRecenterPending() const { return bRecenterPending; }

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	FString GetLastRecenterCode() const { return LastRecenterCode; }

	UFUNCTION(BlueprintPure, Category = "TiltPlay")
	FString GetDeviceId() const;

	// -- 给接收线程 / 内部用 --
	void HandlePacket(const TArray<uint8>& Data);
	void HandleAckPacket(const TArray<uint8>& Data);
	FTiltInputFrame ReadFrame() const;
	FString MakeRequestId();

private:
	bool StartReceiver();
	void StopReceiver();
	bool EnsureControlSocket();
	void PollControlSocket();
	bool SendControlPayload(const FString& PayloadJson);
	void SendRecenterPayload();
	double Now() const { return FPlatformTime::Seconds(); }

	class FSocket* ListenSocket = nullptr;
	class FSocket* ControlSocket = nullptr;
	class FRunnable* ReceiverRunnable = nullptr;
	class FRunnableThread* ReceiverThread = nullptr;

	mutable FCriticalSection FrameLock;
	FTiltInputFrame LatestFrame;
	mutable double LastRecvSeconds = -1.0;
	mutable double MaxGapSeconds = 0.0;

	FThreadSafeCounter ReceivedFrames;
	FThreadSafeCounter RejectedFrames;

	bool bRecenterPending = false;
	FString PendingRequestId;
	FString PendingDeviceId;
	double PendingSendTime = 0.0;
	int32 PendingAttempts = 0;
	FString LastRecenterCode;

	int32 RequestCounter = 0;

	// 会话/序号门控（只在接收线程内读写；Tick 侧不读，故不加锁）
	int32 AcceptedSeq = -1;
	FString AcceptedBootId;
	int32 AcceptedLinkEpoch = -1;
	FString AcceptedBridgeId;

	// 手势去重：token 按会话独立递增，换会话要清空（只在游戏线程用）
	bool bHasLastStepToken = false;
	int32 LastStepToken = 0;
	FString LastStepBootId;
	int32 LastStepEpoch = -1;

	double LastDebugSeconds = 0.0;
	int32 LastSecondFrames = 0;
	bool bBoundOk = false;

	// 广播去重状态（只在游戏线程用）
	int32 LastBroadcastSeq = -1;
	int32 LastBroadcastValid = -1;
	bool bLoggedFirstValid = false;
	FTSTicker::FDelegateHandle TickHandle;
};
