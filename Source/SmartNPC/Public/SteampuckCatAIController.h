// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "AIBrainComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AISenseConfig_Sight.h"
#include "SteampuckCatAIController.generated.h"

class UBlackboardComponent;

/**
 * 友方机械猫的 AI 控制器。
 *
 * 职责划分（刻意保持简单）：
 *   - LLM 只负责**切换行为树顶层的 NPCBehaviour 状态**：patrol / follow / combat。
 *   - 行为树负责"怎么执行"：追谁、开几枪、走哪条巡逻路线。
 *   - 感知只写事实（IsPlayerInSight / LastKnownLocation），不写行为状态。
 *   - 初始状态为 Patrol；没有任何"距离自动切换"的逻辑——所有状态变化都来自 LLM。
 */
UCLASS()
class SMARTNPC_API ASteampuckCatAIController : public AAIController
{
	GENERATED_BODY()

public:
	ASteampuckCatAIController();

	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- 供动作注册表（NPCActionRegistry）调用的状态切换 ----
	// 参数名刻意避开 "Blackboard"：AAIController 有同名成员，局部同名会触发 C4458（警告即错误）。
	bool ApplyNoneAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyFollowAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyCombatAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);

protected:
	virtual void BeginPlay() override;

	// 感知组件本体
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAIPerceptionComponent* AIPerceptionComp;

	// 视觉感知配置
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAISenseConfig_Sight* SightConfig;

	// 预感感知配置
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAISenseConfig_Prediction* PredictionConfig;

	UFUNCTION()
	void onTargetPerceptionUpdated(AActor* UpdatedActor, FAIStimulus Stimulus);

	/** M1-4：接收 LLM 决策（绑定在 Pawn 的 UAIBrainComponent 上）。 */
	UFUNCTION()
	void HandleLLMDecision(FNPCDecision Decision);

	/** 从黑板键的枚举定义里按名字取数值，避免把 0/1 硬编码进代码。 */
	static bool ResolveBehaviourValue(UBlackboardComponent* BlackboardComp, const TCHAR* ValueName, uint8& OutValue);

private:
	/** 把 NPCBehaviour 写成指定名字对应的枚举值。 */
	bool WriteBehaviour(UBlackboardComponent& BlackboardComp, const TCHAR* ValueName);

	APawn* GetPlayerPawn() const;

	TWeakObjectPtr<UAIBrainComponent> Brain;

	/** 初始状态（Patrol）只在黑板创建之后写一次。 */
	bool bInitialStateApplied = false;
};
