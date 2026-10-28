// Copyright Epic Games, Inc. All Rights Reserved.

#include "SmartNPC.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, SmartNPC, "SmartNPC" );

/**
 * M1-1 · 控制台输入通道（方案 C：先控制台跑通链路，中文输入问题留到 UMG 阶段解决）
 *
 * 用法：游戏内按 `~` 打开控制台，输入
 *     AIChat hello
 *     AIChat "write a bubble sort"     ← 多词要用引号，否则只会收进第一个词
 *
 * 为什么用静态控制台命令，而不是 UFUNCTION(Exec)：
 *   Exec 只会被「本地玩家 Possess 的那个 Pawn」收到，所以它必须挂在玩家 Pawn 的类上。
 *   本项目的 GlobalDefaultGameMode 是第三方蓝图 GM_SteampunkCat_C，玩家的 Pawn 类型存在
 *   蓝图资产里，无法确认它是否派生自 AMyCharacter —— 挂错类就完全收不到输入，还很难查。
 *   静态命令与 Pawn 类型无关，先把链路跑通；之后要"按键说话"再补 UMG。
 *
 * TODO(M1-1) —— 由你实现，建议分四步：
 *   1) FString Text = FString::Join(Args, TEXT(" "));   // 把控制台参数拼回一句话
 *   2) 找到场景里的猫：TActorIterator<ASteampunkCatNPC>（#include "EngineUtils.h"）
 *      或 UGameplayStatics::GetAllActorsOfClass
 *   3) UAIBrainComponent* Brain = Cat->FindComponentByClass<UAIBrainComponent>();
 *      if (!Brain) { return; }   // 猫身上还没挂组件时，这里是第一个要验证的点
 *   4) Brain->SendRequestToLLM(Text);
 *
 *   想立刻看到回复：回复是通过 OnLLMChatFinished 广播出来的，需要有对象绑了处理函数。
 *   最省事的临时做法：在命令里绑一次（AddUObject / AddWeakLambda），把 Message 打到屏幕上：
 *     UE_LOG(LogTemp, Warning, TEXT("%s"), *Message);
 *     GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Green, Message);
 */
static void SmartNPCAIChatCommand(const TArray<FString>& Args)
{
	// TODO(M1-1): 按上面四步实现。当前是空实现——命令存在，但什么也不做。
}

static FAutoConsoleCommand GSmartNPCAIChatCommand(
	TEXT("AIChat"),
	TEXT("Send a line of text to the cat NPC's LLM brain. Scaffold only — TODO(M1-1)."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&SmartNPCAIChatCommand));
