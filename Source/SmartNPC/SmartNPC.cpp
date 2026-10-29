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

static FAutoConsoleCommand GSmartNPCAIChatCommand(
	TEXT("AIChat"),
	TEXT("Send one line of text to the nearest cat NPC's LLM brain. "
		"e.g. AIChat \"write a bubble sort\""),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SmartNPCAIChatCommand));
