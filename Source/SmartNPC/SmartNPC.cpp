// Copyright Epic Games, Inc. All Rights Reserved.

#include "SmartNPC.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Pawn.h"
#include "SteampunkCatNPC.h"
#include "AIBrainComponent.h"
#include "TimerManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, SmartNPC, "SmartNPC" );

/**
 * M1-1 · 控制台输入通道（方案 C：先控制台跑通链路，中文输入问题留到 UMG 阶段解决）
 *
 * 用法：游戏内按 `~` 打开控制台，输入
 *     AIChat hello
 *     AIChat "write a bubble sort"     ← 多词必须加引号，否则只有第一个词会被收进来
 *
 * 行为：
 *   1) 把参数拼回一句话；
 *   2) 在场景里找离玩家最近、且身上挂着 UAIBrainComponent 的 ASteampunkCatNPC
 *      （场景里有两只猫时，这条规则保证你命令的是"你面前那只"，而不是看迭代顺序碰运气）；
 *   3) 调用该组件的 SendRequestToLLM()；
 *   4) 若没有任何对象绑定 OnLLMChatFinished，打印一条提示——否则回复会"悄无声息"，
 *      容易被误判成网络或 key 的问题。
 *
 * 回复怎么显示：OnLLMChatFinished 是动态多播委托，**只能绑 UFUNCTION，不能绑 lambda**
 *   （AddLambda/AddWeakLambda 只存在于非动态委托上）。所以显示交给绑定方：
 *   在猫的蓝图里 "Bind Event to On LLM Chat Finished" → Print String。
 *
 * 为什么用静态控制台命令，而不是 UFUNCTION(Exec)：
 *   Exec 只会被「本地玩家 Possess 的那个 Pawn」收到，所以它必须挂在玩家 Pawn 的类上。
 *   本项目的 GlobalDefaultGameMode 是第三方蓝图 GM_SteampunkCat_C，玩家的 Pawn 类型存在
 *   蓝图资产里，无法确认它是否派生自 AMyCharacter —— 挂错类就完全收不到输入，还很难查。
 *   静态命令与 Pawn 类型无关。
 */
namespace SmartNPCChatCommand
{
	void PrintToScreen(const FString& Line, const FColor& Color)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 10.f, Color, Line);
		}
		UE_LOG(LogTemp, Warning, TEXT("[AIChat] %s"), *Line);
	}

	/** 控制台命令没有 World 参数，从引擎的世界上下文里取当前运行的 Game/PIE 世界。 */
	UWorld* FindGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
			{
				if (UWorld* World = Context.World())
				{
					return World;
				}
			}
		}

		return nullptr;
	}
}

static void SmartNPCAIChatCommand(const TArray<FString>& Args)
{
	using namespace SmartNPCChatCommand;

	const FString Text = FString::Join(Args, TEXT(" ")).TrimStartAndEnd();
	if (Text.IsEmpty())
	{
		PrintToScreen(TEXT("usage: AIChat <text>   (multi-word text needs quotes)"), FColor::Yellow);
		return;
	}

	UWorld* World = FindGameWorld();
	if (!World)
	{
		PrintToScreen(TEXT("no game world — start PIE or run the game first"), FColor::Yellow);
		return;
	}

	const APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0);

	ASteampunkCatNPC* BestCat = nullptr;
	UAIBrainComponent* BestBrain = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (TActorIterator<ASteampunkCatNPC> It(World); It; ++It)
	{
		ASteampunkCatNPC* Cat = *It;
		if (!Cat)
		{
			continue;
		}

		// 先确认有大脑，再比距离：没有组件的猫不能被选中（命令发过去也没人接）。
		UAIBrainComponent* Brain = Cat->FindComponentByClass<UAIBrainComponent>();
		if (!Brain)
		{
			continue;
		}

		const float DistSq = PlayerPawn
			? FVector::DistSquared(PlayerPawn->GetActorLocation(), Cat->GetActorLocation())
			: 0.f;

		if (!BestCat || DistSq < BestDistSq)
		{
			BestCat = Cat;
			BestBrain = Brain;
			BestDistSq = DistSq;
		}
	}

	if (!BestCat || !BestBrain)
	{
		PrintToScreen(TEXT("no ASteampunkCatNPC with a UAIBrainComponent in this world "
			"(add the component to the cat Blueprint first)"), FColor::Yellow);
		return;
	}

	PrintToScreen(FString::Printf(TEXT("-> %s: %s"), *BestCat->GetName(), *Text), FColor::Cyan);

	if (!BestBrain->OnLLMChatFinished.IsBound())
	{
		PrintToScreen(TEXT("note: nothing is bound to OnLLMChatFinished — the reply will be "
			"stored but never shown. Bind it to a Print String in the cat Blueprint."), FColor::Yellow);
	}

	BestBrain->SendRequestToLLM(Text);
}

/**
 * M2 之前的调试命令：让"离玩家最近的那只猫"转身朝玩家开一枪。
 *
 * 用途：猫的攻击蒙太奇已经从行为树里摘掉（友方化之后不再自动攻击），
 * 但伤害链路（射线 → ApplyPointDamage → Event AnyDamage → HealthComponent → 血条）
 * 仍然需要一条触发路径来验证。
 */
static void SmartNPCAIFireCommand(const TArray<FString>& Args)
{
	using namespace SmartNPCChatCommand;

	UWorld* World = FindGameWorld();
	if (!World)
	{
		PrintToScreen(TEXT("no game world — start PIE first"), FColor::Yellow);
		return;
	}

	APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0);

	ASteampunkCatNPC* BestCat = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (TActorIterator<ASteampunkCatNPC> It(World); It; ++It)
	{
		ASteampunkCatNPC* Cat = *It;
		if (!Cat)
		{
			continue;
		}

		const float DistSq = PlayerPawn
			? FVector::DistSquared(PlayerPawn->GetActorLocation(), Cat->GetActorLocation())
			: 0.f;

		if (!BestCat || DistSq < BestDistSq)
		{
			BestCat = Cat;
			BestDistSq = DistSq;
		}
	}

	if (!BestCat)
	{
		PrintToScreen(TEXT("no ASteampunkCatNPC in this world"), FColor::Yellow);
		return;
	}

	// FireLaser() 沿 Actor 前向发射，所以先让猫面向玩家，否则大概率打不中。
	if (PlayerPawn)
	{
		const FVector ToPlayer = PlayerPawn->GetActorLocation() - BestCat->GetActorLocation();
		BestCat->SetActorRotation(FRotator(0.f, ToPlayer.Rotation().Yaw, 0.f));
	}

	PrintToScreen(FString::Printf(TEXT("%s fires at %s"),
		*BestCat->GetName(), PlayerPawn ? *PlayerPawn->GetName() : TEXT("<no player>")), FColor::Orange);

	BestCat->FireLaser();
}

static FAutoConsoleCommand GSmartNPCAIFireCommand(
	TEXT("AIFire"),
	TEXT("Debug: the nearest cat turns to the player and fires its laser once."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SmartNPCAIFireCommand));

// ---------------------------------------------------------------------------
// AIBench：固定题库 + 事件驱动（等上一轮真正结束再发下一条）+ CSV 导出
//
// 为什么不用固定间隔：复合任务要走几十米，"每 2.5 秒一条"会互相打断。
// 结束信号 = UAIBrainComponent::IsToolLoopActive() 变回 false（final_answer / 失败 / 中止都会置回）。
// ---------------------------------------------------------------------------
namespace SmartNPCBench
{
	/** 单条命令的超时（秒）。超时视为"卡死"，整轮 bench 中止。 */
	static constexpr double CommandTimeoutSeconds = 90.0;
	/** 轮询周期（秒）。只需秒级精度，0.25 s 足够。 */
	static constexpr float PollIntervalSeconds = 0.25f;
	/** 两条命令之间静置，避免上下文串味与触发限速。 */
	static constexpr double SettleSeconds = 1.0;

	struct FRow
	{
		FString Command;
		bool bFinished = false;
		double Seconds = 0.0;
		int32 LlmCalls = 0;
		int32 Tokens = 0;
		double CostYuan = 0.0;
	};

	struct FState
	{
		TArray<FString> Queue;
		int32 Index = 0;
		TArray<FRow> Rows;
		TWeakObjectPtr<UAIBrainComponent> Brain;
		TWeakObjectPtr<UWorld> World;
		FTimerHandle Timer;

		double StartedAt = 0.0;
		int32 CallsAtStart = 0;
		int32 TokensAtStart = 0;
		double CostAtStart = 0.0;

		/** 两条命令之间的静置状态（由同一个轮询定时器驱动）。 */
		bool bSettling = false;
		double SettleStart = 0.0;
	};

	static TSharedPtr<FState> GState;

	const TArray<FString>& GetCommands()
	{
		static const TArray<FString> Commands =
		{
			// 闲聊（应 1 次调用）
			TEXT("hello there"),
			TEXT("how are you today"),
			TEXT("what can you do"),
			// 单步移动（query + move）
			TEXT("go to the cube"),
			TEXT("go to the sphere"),
			TEXT("go to actor2"),
			TEXT("come to me"),
			TEXT("walk over to the cube"),
			// 复合任务（多步）
			TEXT("go to the cube then the sphere"),
			TEXT("walk to the sphere and then come back to me"),
			TEXT("check the cube and then report back"),
			// 状态切换
			TEXT("follow me"),
			TEXT("patrol"),
			TEXT("stop"),
			TEXT("fight me"),
			TEXT("stop fighting"),
			// 做不到 / 幻觉输入
			TEXT("eat something"),
			TEXT("fly to the moon"),
			TEXT("go to the dragon"),
			// 环境感知
			TEXT("what is around you"),
			// 重复目标（考重复检测）
			TEXT("go to the cube again"),
		};
		return Commands;
	}

	static void WriteCsv(const TArray<FRow>& Rows)
	{
		FString Csv = TEXT("index,command,finished,seconds,llm_calls,tokens,cost_yuan\n");
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FRow& Row = Rows[Index];
			FString Escaped = Row.Command.Replace(TEXT("\""), TEXT("\"\""));
			Csv += FString::Printf(TEXT("%d,\"%s\",%d,%.3f,%d,%d,%.6f\n"),
				Index + 1, *Escaped, Row.bFinished ? 1 : 0,
				Row.Seconds, Row.LlmCalls, Row.Tokens, Row.CostYuan);
		}

		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Bench");
		IFileManager::Get().MakeDirectory(*Dir, true);

		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString Path = Dir / FString::Printf(TEXT("bench_%s.csv"), *Stamp);

		if (FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Bench] CSV written: %s"), *Path);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[Bench] failed to write %s"), *Path);
		}
	}

	static void Finish()
	{
		if (!GState.IsValid())
		{
			return;
		}

		TSharedPtr<FState> State = GState;
		GState.Reset();

		if (State->World.IsValid())
		{
			State->World->GetTimerManager().ClearTimer(State->Timer);
		}

		// 统计
		TArray<double> Seconds;
		int32 Finished = 0;
		int32 TotalCalls = 0;
		int32 TotalTokens = 0;
		double TotalCost = 0.0;

		for (const FRow& Row : State->Rows)
		{
			Seconds.Add(Row.Seconds);
			Finished += Row.bFinished ? 1 : 0;
			TotalCalls += Row.LlmCalls;
			TotalTokens += Row.Tokens;
			TotalCost += Row.CostYuan;
		}

		Seconds.Sort();

		const auto Percentile = [&Seconds](double P) -> double
		{
			if (Seconds.Num() == 0) { return 0.0; }
			const int32 Index = FMath::Clamp(
				FMath::CeilToInt(P * Seconds.Num()) - 1, 0, Seconds.Num() - 1);
			return Seconds[Index];
		};

		const int32 Count = State->Rows.Num();
		UE_LOG(LogTemp, Warning,
			TEXT("[Bench] DONE rows=%d finished=%d | p50=%.2fs p95=%.2fs p99=%.2fs max=%.2fs"),
			Count, Finished, Percentile(0.50), Percentile(0.95), Percentile(0.99),
			Seconds.Num() > 0 ? Seconds.Last() : 0.0);

		UE_LOG(LogTemp, Warning,
			TEXT("[Bench] 调用=%d (%.2f 次/条) tokens=%d (%.0f/条) 费用=%.6f 元 (%.6f 元/条)"),
			TotalCalls, Count > 0 ? static_cast<double>(TotalCalls) / Count : 0.0,
			TotalTokens, Count > 0 ? static_cast<double>(TotalTokens) / Count : 0.0,
			TotalCost, Count > 0 ? TotalCost / Count : 0.0);

		if (State->Brain.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[Bench] session: %s"),
				*State->Brain->GetTokenStatsSummary());
		}

		WriteCsv(State->Rows);
	}

	static void SendCurrent(TSharedPtr<FState> State);
	static void Poll(TSharedPtr<FState> State);

	static void SendCurrent(TSharedPtr<FState> State)
	{
		if (!State.IsValid() || !State->Brain.IsValid() || !State->World.IsValid())
		{
			Finish();
			return;
		}

		if (State->Index >= State->Queue.Num())
		{
			Finish();
			return;
		}

		UAIBrainComponent* Brain = State->Brain.Get();
		State->StartedAt = FPlatformTime::Seconds();
		State->CallsAtStart = Brain->GetSessionRequestCount();
		State->TokensAtStart = Brain->GetSessionTotalTokens();
		State->CostAtStart = Brain->GetSessionCostYuan();

		UE_LOG(LogTemp, Warning, TEXT("[Bench] %d/%d -> \"%s\""),
			State->Index + 1, State->Queue.Num(), *State->Queue[State->Index]);

		Brain->SendRequestToLLM(State->Queue[State->Index]);
	}

	static void Poll(TSharedPtr<FState> State)
	{
		if (!GState.IsValid() || !State.IsValid())
		{
			return;
		}

		UAIBrainComponent* Brain = State->Brain.Get();
		if (!Brain)
		{
			Finish();
			return;
		}

		const double Now = FPlatformTime::Seconds();

		// 静置阶段：时间到了就发下一条。
		if (State->bSettling)
		{
			if (Now - State->SettleStart >= SettleSeconds)
			{
				State->bSettling = false;
				SendCurrent(State);
			}
			return;
		}

		const double Elapsed = Now - State->StartedAt;

		// 还在跑：只有超时才处理。
		if (Brain->IsToolLoopActive())
		{
			if (Elapsed > CommandTimeoutSeconds)
			{
				UE_LOG(LogTemp, Error, TEXT("[Bench] TIMEOUT after %.1fs on \"%s\" — aborting bench"),
					Elapsed, *State->Queue[State->Index]);
				Finish();
			}
			return;
		}

		// 本轮结束（final_answer / 失败 / 中止都算）。
		FRow Row;
		Row.Command = State->Queue[State->Index];
		Row.bFinished = true;   // "本轮是否正常收尾"；工具层面的成败在日志与 error.code 里
		Row.Seconds = Elapsed;
		Row.LlmCalls = Brain->GetSessionRequestCount() - State->CallsAtStart;
		Row.Tokens = Brain->GetSessionTotalTokens() - State->TokensAtStart;
		Row.CostYuan = Brain->GetSessionCostYuan() - State->CostAtStart;
		State->Rows.Add(Row);

		UE_LOG(LogTemp, Warning,
			TEXT("[Bench]   done in %.2fs | %d 次调用 | %d tokens | %.6f 元"),
			Row.Seconds, Row.LlmCalls, Row.Tokens, Row.CostYuan);

		++State->Index;

		if (State->Index >= State->Queue.Num())
		{
			Finish();
			return;
		}

		State->bSettling = true;
		State->SettleStart = Now;
	}

	static void Start(UWorld* World, UAIBrainComponent* Brain)
	{
		if (!World || !Brain)
		{
			UE_LOG(LogTemp, Error, TEXT("[Bench] need a cat with UAIBrainComponent"));
			return;
		}

		Finish();   // 清理上一次

		TSharedPtr<FState> State = MakeShared<FState>();
		State->Queue = GetCommands();
		State->Brain = Brain;
		State->World = World;
		GState = State;

		UE_LOG(LogTemp, Warning, TEXT("[Bench] start: %d commands, timeout %.0fs each"),
			State->Queue.Num(), CommandTimeoutSeconds);

		SendCurrent(State);

		World->GetTimerManager().SetTimer(State->Timer,
			FTimerDelegate::CreateLambda([State]()
			{
				Poll(State);
			}),
			PollIntervalSeconds, true);
	}
}

static void SmartNPCAIBenchCommand(const TArray<FString>& Args)
{
	using namespace SmartNPCChatCommand;

	UWorld* World = FindGameWorld();
	if (!World)
	{
		PrintToScreen(TEXT("no game world — start PIE first"), FColor::Yellow);
		return;
	}

	APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0);

	ASteampunkCatNPC* BestCat = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();

	for (TActorIterator<ASteampunkCatNPC> It(World); It; ++It)
	{
		ASteampunkCatNPC* Cat = *It;
		if (!Cat) { continue; }

		const float DistSq = PlayerPawn
			? FVector::DistSquared(PlayerPawn->GetActorLocation(), Cat->GetActorLocation())
			: 0.f;

		if (!BestCat || DistSq < BestDistSq)
		{
			BestCat = Cat;
			BestDistSq = DistSq;
		}
	}

	UAIBrainComponent* Brain = BestCat ? BestCat->FindComponentByClass<UAIBrainComponent>() : nullptr;
	if (!Brain)
	{
		PrintToScreen(TEXT("nearest cat has no UAIBrainComponent"), FColor::Yellow);
		return;
	}

	SmartNPCBench::Start(World, Brain);
}

static FAutoConsoleCommand GSmartNPCAIBenchCommand(
	TEXT("AIBench"),
	TEXT("Run the 20-command benchmark (event-driven, writes Saved/Bench/bench_*.csv)."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SmartNPCAIBenchCommand));

static FAutoConsoleCommand GSmartNPCAIChatCommand(
	TEXT("AIChat"),
	TEXT("Send one line of text to the nearest cat NPC's LLM brain. "
		"e.g. AIChat \"write a bubble sort\""),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SmartNPCAIChatCommand));
