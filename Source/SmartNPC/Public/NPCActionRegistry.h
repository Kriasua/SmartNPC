// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIBrainComponent.h"   // FNPCDecision

class AAIController;
class UBlackboardComponent;

/** 工具的执行方式。 */
enum class EToolKind : uint8
{
	/** 立即完成：只写黑板（follow / patrol / combat）。 */
	InstantState,

	/** 立即完成：只读世界，返回一段 JSON（query_world）。 */
	Query,

	/** 交给行为树执行，完成后由 BT 上报（move_to）。 */
	BlockingBT,
};

/**
 * 一个工具的运行期定义。
 *
 * 提示词、白名单、执行共用这一张表 —— 加一个工具 = 在这里加一行
 * （再在控制器里实现它的 Apply）。这样不会出现"模型会说、引擎不认"的漂移。
 */
struct FActionRuntime
{
	/** 给模型看的工具 id，例如 "move_to"。 */
	FString Id;

	/** 一句话说明，写进 system prompt。 */
	FString PromptHint;

	/** args 的字段说明，写进 system prompt（例如 "{target: 目标 id}"）。 */
	FString ArgsHint;

	EToolKind Kind = EToolKind::InstantState;

	/** 执行：写黑板 / 读世界 / 启动 BT 动作。返回 false 表示这次执行失败。 */
	TFunction<bool(AAIController&, UBlackboardComponent&, const FNPCDecision&)> Apply;
};

/** 唯一的工具注册表。函数内静态，避免跨编译单元的初始化顺序问题。 */
const TArray<FActionRuntime>& GetActionRuntimes();

/** 按 id 查工具（大小写不敏感），找不到返回 nullptr。 */
const FActionRuntime* FindActionRuntimeById(const FString& Id);

/** 由注册表生成 system prompt 里的工具清单。 */
FString BuildActionPromptLines();

/** 所有可用工具 id，用于错误提示（"可用工具：..."）。 */
FString BuildAvailableToolsHint();
