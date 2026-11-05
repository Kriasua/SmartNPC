// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIBrainComponent.h"   // ENPCAction / FNPCDecision

class AAIController;
class UBlackboardComponent;

/**
 * 动作的运行期定义。
 *
 * 这里的"动作"其实是**状态切换**：follow / patrol / combat 都是行为树顶层的
 * NPCBehaviour 枚举值，LLM 只决定"进入哪个状态"，具体怎么执行由行为树负责。
 *
 * 提示词、白名单校验、执行共用这一张表 —— 加一个状态 = 在这里加一行
 * （再在控制器里实现它的 Apply）。这样可以避免"模型会说、引擎不认"的漂移。
 */
struct FActionRuntime
{
	ENPCAction Action = ENPCAction::None;

	/** 给模型看的 id，例如 "follow"。 */
	const TCHAR* Id = TEXT("");

	/** 写进 system prompt 的用法说明。 */
	const TCHAR* PromptHint = TEXT("");

	/** 执行：写黑板。返回 false 表示这次执行失败（例如找不到玩家）。 */
	TFunction<bool(AAIController&, UBlackboardComponent&, const FNPCDecision&)> Apply;
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
