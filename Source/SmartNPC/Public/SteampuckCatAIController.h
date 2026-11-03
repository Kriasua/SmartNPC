// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "AIBrainComponent.h"
#include "NPCActionRegistry.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AISenseConfig_Sight.h"
#include "SteampuckCatAIController.generated.h"

class UBlackboardComponent;

/**
 * 友方机械猫的 AI 控制器。
 *
 * 三层结构：
 *   1. 默认状态机   —— 按玩家距离决定"跟随 / 自主巡逻"（含滞回）
 *   2. LLM 意图     —— 覆盖默认状态；有终点的 goal 由条件判定完成，无终点的 mode 持续生效
 *   3. 感知         —— 只写事实（IsPlayerInSight / LastKnownLocation），不写行为状态
 *
 * 切换与仲裁：`SmartNPC.Mode 0|1` 决定是否接受 LLM 决策。同一时刻只有一个写入者，
 * 所以两套逻辑不会互相覆盖。
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

	// ---- 供动作注册表（NPCActionRegistry）调用的执行与判定 ----
	// 参数名刻意避开 "Blackboard"：AAIController 有同名成员，局部同名会触发 C4458（警告即错误）。
	bool ApplyNoneAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyMoveToAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool IsMoveToComplete(const FActiveGoal& Goal) const;

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

	// ---- 行为参数（蓝图可调，不用改代码） ----

	/** 目标动作的到达半径：走进这个距离就算 move_to 完成。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Behaviour", meta = (ClampMin = "10"))
	float ArriveRadius = 250.f;

	/** 玩家进入这个距离 -> 开始跟随（默认状态机）。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Behaviour", meta = (ClampMin = "10"))
	float FollowEnterRadius = 600.f;

	/** 玩家离开这个距离 -> 停止跟随。与 Enter 拉开距离形成滞回，避免抖动。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Behaviour", meta = (ClampMin = "10"))
	float FollowExitRadius = 900.f;

	/** 兜底：goal 超过这个时间仍未完成就放弃（异常路径，不是"意图有效期"）。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Behaviour", meta = (ClampMin = "1"))
	float WatchdogSeconds = 60.f;

	UFUNCTION()
	void onTargetPerceptionUpdated(AActor* UpdatedActor, FAIStimulus Stimulus);

	/** M1-4：接收 LLM 决策（绑定在 Pawn 的 UAIBrainComponent 上）。 */
	UFUNCTION()
	void HandleLLMDecision(FNPCDecision Decision);

	/** 从黑板键的枚举定义里按名字取数值，避免把 0/1 硬编码进代码。 */
	static bool ResolveBehaviourValue(UBlackboardComponent* BlackboardComp, const TCHAR* ValueName, uint8& OutValue);

private:
	/** 当前谁在驾驶：无（默认状态机）/ 目标 / 模式。 */
	enum class EHold : uint8
	{
		None,
		Goal,
		Mode,
	};

	void UpdateDefaultState();
	void FinishGoal(const TCHAR* Reason);
	void ResetBlackboardToNative();

	APawn* GetPlayerPawn() const;
	float DistanceToPlayer() const;

	TWeakObjectPtr<UAIBrainComponent> Brain;

	EHold Hold = EHold::None;
	FActiveGoal ActiveGoal;

	/** 上一次看到的模式值，用于检测切换。 */
	bool bLastLLMMode = false;
};
