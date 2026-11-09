// Fill out your copyright notice in the Description page of Project Settings.

#include "NPCActionRegistry.h"
#include "SteampuckCatAIController.h"
#include "BehaviorTree/BlackboardComponent.h"

const TArray<FActionRuntime>& GetActionRuntimes()
{
	// 加工具：在下面加一行，并在 ASteampuckCatAIController 里实现对应的 Apply。
	static const TArray<FActionRuntime> Table =
	{
		{
			TEXT("query_world"),
			TEXT("查看整个地图：所有可交互物、玩家位置、自己状态。需要知道环境时先调它。"),
			TEXT("无参数"),
			EToolKind::Query,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyQueryWorldAction(BlackboardComp, Decision) : false;
			}
		},
		{
			TEXT("move_to"),
			TEXT("走向一个目标，走路期间会阻塞，到达或失败后返回结果。target 只能用 query_world 返回的 id。"),
			TEXT("{\"target\": \"目标 id，例如 player:0 或 actor:3\"}"),
			EToolKind::BlockingBT,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyMoveToAction(BlackboardComp, Decision) : false;
			}
		},
		{
			TEXT("idle"),
			TEXT("停下、原地站住，什么都不做。玩家让你停手、别打了、等着时用它。"),
			TEXT("无参数"),
			EToolKind::InstantState,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyIdleAction(BlackboardComp, Decision) : false;
			}
		},
		{
			TEXT("follow"),
			TEXT("进入跟随状态：一直跟着玩家，不需要目标参数。"),
			TEXT("无参数"),
			EToolKind::InstantState,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyFollowAction(BlackboardComp, Decision) : false;
			}
		},
		{
			TEXT("patrol"),
			TEXT("进入巡逻状态：自己在巡逻点之间走动，不需要目标参数。"),
			TEXT("无参数"),
			EToolKind::InstantState,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyPatrolAction(BlackboardComp, Decision) : false;
			}
		},
		{
			TEXT("combat"),
			TEXT("进入战斗状态：追击并开火，不需要目标参数。"),
			TEXT("无参数"),
			EToolKind::InstantState,
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyCombatAction(BlackboardComp, Decision) : false;
			}
		},
	};

	return Table;
}

const FActionRuntime* FindActionRuntimeById(const FString& Id)
{
	const FString Wanted = Id.TrimStartAndEnd();
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		if (Wanted.Equals(Spec.Id, ESearchCase::IgnoreCase))
		{
			return &Spec;
		}
	}
	return nullptr;
}

FString BuildActionPromptLines()
{
	FString Lines;
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		Lines += FString::Printf(TEXT("- %s：%s  args：%s\n"), *Spec.Id, *Spec.PromptHint, *Spec.ArgsHint);
	}
	return Lines;
}

FString BuildAvailableToolsHint()
{
	TArray<FString> Ids;
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		Ids.Add(Spec.Id);
	}
	return FString::Printf(TEXT("可用工具：%s，也可以直接用 final_answer"), *FString::Join(Ids, TEXT(", ")));
}
