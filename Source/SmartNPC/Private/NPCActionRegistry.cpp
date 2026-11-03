// Fill out your copyright notice in the Description page of Project Settings.

#include "NPCActionRegistry.h"
#include "SteampuckCatAIController.h"
#include "BehaviorTree/BlackboardComponent.h"

namespace
{
	ASteampuckCatAIController* AsCat(const AAIController& Controller)
	{
		return Cast<ASteampuckCatAIController>(&const_cast<AAIController&>(Controller));
	}
}

const TArray<FActionRuntime>& GetActionRuntimes()
{
	// 加动作：在下面加一行，并在 ASteampuckCatAIController 里实现对应的 Apply/IsComplete。
	static const TArray<FActionRuntime> Table =
	{
		{
			ENPCAction::None,
			TEXT("none"),
			TEXT("不改变行为。闲聊、问好、或你做不到的请求都用这个，并在台词里说明。"),
			false,
			[](AAIController& Controller, UBlackboardComponent& Blackboard, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyNoneAction(Blackboard, Decision) : false;
			},
			[](const AAIController&, const FActiveGoal&)
			{
				return true;    // 瞬间完成
			}
		},
		{
			ENPCAction::MoveTo,
			TEXT("move_to"),
			TEXT("走向玩家。玩家让你靠近、过来时使用。"),
			false,
			[](AAIController& Controller, UBlackboardComponent& Blackboard, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyMoveToAction(Blackboard, Decision) : false;
			},
			[](const AAIController& Controller, const FActiveGoal& Goal)
			{
				const ASteampuckCatAIController* Cat = AsCat(Controller);
				return Cat ? Cat->IsMoveToComplete(Goal) : false;
			}
		},
		{
			ENPCAction::Patrol,
			TEXT("patrol"),
			TEXT("恢复自主巡逻。玩家让你自己逛、别跟着时使用。"),
			true,   // mode：没有终点
			[](AAIController& Controller, UBlackboardComponent& Blackboard, const FNPCDecision& Decision)
			{
				ASteampuckCatAIController* Cat = Cast<ASteampuckCatAIController>(&Controller);
				return Cat ? Cat->ApplyPatrolAction(Blackboard, Decision) : false;
			},
			nullptr
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
