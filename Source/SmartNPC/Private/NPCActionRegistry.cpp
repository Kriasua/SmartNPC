// Fill out your copyright notice in the Description page of Project Settings.

#include "NPCActionRegistry.h"
#include "SteampuckCatAIController.h"
#include "BehaviorTree/BlackboardComponent.h"

const TArray<FActionRuntime>& GetActionRuntimes()
{
	// 加状态：在下面加一行，并在 ASteampuckCatAIController 里实现对应的 Apply。
	static const TArray<FActionRuntime> Table =
	{
		{
			ENPCAction::None,
			TEXT("none"),
			TEXT("不改变当前状态。闲聊、问好、或你做不到的请求都用这个，并在台词里说明。"),
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyNoneAction(BlackboardComp, Decision) : false;
			}
		},
		{
			ENPCAction::Follow,
			TEXT("follow"),
			TEXT("跟着玩家走。玩家让你靠近、过来、跟着他时使用。"),
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyFollowAction(BlackboardComp, Decision) : false;
			}
		},
		{
			ENPCAction::Patrol,
			TEXT("patrol"),
			TEXT("自己去巡逻。玩家让你自己逛逛、别跟着、原地待着时使用。"),
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyPatrolAction(BlackboardComp, Decision) : false;
			}
		},
		{
			ENPCAction::Combat,
			TEXT("combat"),
			TEXT("进入战斗状态，攻击目标。玩家让你打、开火、进入战斗时使用。"),
			[](AAIController& Controller, UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyCombatAction(BlackboardComp, Decision) : false;
			}
		},
	};

	return Table;
}

const FActionRuntime* FindActionRuntime(ENPCAction Action)
{
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		if (Spec.Action == Action)
		{
			return &Spec;
		}
	}
	return nullptr;
}

bool TryParseActionId(const FString& Id, ENPCAction& OutAction)
{
	const FString Trimmed = Id.TrimStartAndEnd();
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		if (Trimmed.Equals(Spec.Id, ESearchCase::IgnoreCase))
		{
			OutAction = Spec.Action;
			return true;
		}
	}
	return false;
}

FString ActionToId(ENPCAction Action)
{
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		if (Spec.Action == Action)
		{
			return Spec.Id;
		}
	}
	return TEXT("?");
}

FString BuildActionPromptLines()
{
	FString Lines;
	for (const FActionRuntime& Spec : GetActionRuntimes())
	{
		Lines += FString::Printf(TEXT("- %s：%s\n"), Spec.Id, Spec.PromptHint);
	}
	return Lines;
}
