// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIBrainComponent.h"   // ENPCAction / FNPCDecision

class AAIController;
class UBlackboardComponent;

/** 一个"有终点"的活跃目标。mode 类型的动作（如 patrol）不会产生它。 */
struct FActiveGoal
{
	ENPCAction Action = ENPCAction::None;
	TWeakObjectPtr<AActor> Target;
	double StartedAt = 0.0;
};

/**
 * 动作的运行期定义。
 *
 * 提示词、白名单校验、执行、完成判定**共用这一张表** —— 加一个动作 = 在这里加一行
 * （再在控制器里实现它的 Apply / IsComplete）。这样可以避免"模型会说、引擎不认"的漂移。
 */
struct FActionRuntime
{
	ENPCAction Action = ENPCAction::None;

	/** 给模型看的 id，例如 "move_to"。 */
	const TCHAR* Id = TEXT("");

	/** 写进 system prompt 的用法说明。 */
	const TCHAR* PromptHint = TEXT("");

	/** true = 无终点的状态（patrol，只能被新意图覆盖）；false = 有终点的目标（move_to）。 */
	bool bIsMode = false;

	/** 执行：写黑板。返回 false 表示这次执行失败（例如找不到玩家）。 */
	TFunction<bool(AAIController&, UBlackboardComponent&, const FNPCDecision&)> Apply;

	/** 完成判定；mode 类型留空。 */
	TFunction<bool(const AAIController&, const FActiveGoal&)> IsComplete;
};

/** 唯一的动作注册表。函数内静态，避免跨编译单元的初始化顺序问题。 */
const TArray<FActionRuntime>& GetActionRuntimes();

const FActionRuntime* FindActionRuntime(ENPCAction Action);

/** 模型返回的字符串 id -> 枚举（大小写不敏感）。失败返回 false。 */
bool TryParseActionId(const FString& Id, ENPCAction& OutAction);

/** 枚举 -> 字符串 id，用于日志。 */
FString ActionToId(ENPCAction Action);

/** 由注册表生成 system prompt 里的能力清单。 */
FString BuildActionPromptLines();
