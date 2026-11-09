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
 * 职责划分：
 *   - LLM 输出工具调用（query_world / move_to / follow / patrol / combat / final_answer）。
 *   - 控制器负责执行工具、写黑板、把**结构化结果**回填给 UAIBrainComponent（ReAct 循环）。
 *   - 行为树负责"怎么执行"：追谁、走哪条路线、开几枪；move_to 的完成由 BT 上报。
 *   - 感知只写事实（IsPlayerInSight / LastKnownLocation），不写行为状态。
 *   - 初始状态为 Patrol。
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

	/** 每次移动请求结束时引擎都会回调这里，带一个原因码。
	 *  用来区分"导航失败（Blocked / OffPath / Invalid）"与"被行为树掐断（Aborted）"。 */
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;

	// ---- 工具执行（由 NPCActionRegistry 的表调用；成功与否都会回填结果）----
	// 参数名刻意避开 "Blackboard"：AAIController 有同名成员，局部同名会触发 C4458（警告即错误）。
	bool ApplyQueryWorldAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyMoveToAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyIdleAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyFollowAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);
	bool ApplyCombatAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision);

	/** 行为树的上报入口：BTTask_ReportToolResult 在 Move To 成功/失败后调用。
	 *  没有待处理工具时什么都不做（同一条 Follow 分支也服务"状态跟随"）。 */
	UFUNCTION(BlueprintCallable, Category = "AI|Agent")
	void ReportPendingToolResult(bool bSucceeded);

	/** 是否有等待上报的 move_to。 */
	UFUNCTION(BlueprintPure, Category = "AI|Agent")
	bool HasPendingTool() const;

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

	/** move_to 的兜底超时：BT 一直不上报时强制返回 TimedOut（秒）。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Agent", meta = (ClampMin = "1"))
	float MoveToTimeoutSeconds = 30.f;

	/** query_world 最多返回几个附近物体。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Agent", meta = (ClampMin = "1", ClampMax = "10"))
	int32 MaxNearbyActors = 5;

	/** move_to 的目标点会朝"自己"的方向推开这么远（厘米），再投影到网格。
	 *  否则目标点紧贴障碍边缘，角色的胶囊半径会让它永远差一点、判定失败。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Agent", meta = (ClampMin = "0", ClampMax = "500"))
	float GoalOffsetCm = 120.f;

	/** 可交互物必须带的标签。 */
	UPROPERTY(EditDefaultsOnly, Category = "AI|Agent")
	FName InteractableTag = TEXT("Interactable");

	UFUNCTION()
	void onTargetPerceptionUpdated(AActor* UpdatedActor, FAIStimulus Stimulus);

	/** 接收 LLM 的工具调用（绑定在 Pawn 的 UAIBrainComponent 上）。 */
	UFUNCTION()
	void HandleLLMDecision(FNPCDecision Decision);

	/** 从黑板键的枚举定义里按名字取数值，避免把序号硬编码进代码。 */
	static bool ResolveBehaviourValue(UBlackboardComponent* BlackboardComp, const TCHAR* ValueName, uint8& OutValue);

private:
	/** 一个正在等 BT 上报的 move_to。 */
	struct FPendingMove
	{
		FString ToolId;
		FString TargetId;
		TWeakObjectPtr<AActor> Target;
		/** 投影到导航网格后的目标点（行为树按它执行 Move To Location）。 */
		FVector GoalLocation = FVector::ZeroVector;
		double StartedAt = 0.0;
	};

	/** 把 NPCBehaviour 写成指定名字对应的枚举值。 */
	bool WriteBehaviour(UBlackboardComponent& BlackboardComp, const TCHAR* ValueName);

	/** 从黑板上读出当前状态名（小写显示名）。
	 *  必须读黑板而不是读缓存：行为树的 BTTask_SetNPCBehaviour 会直接改黑板、绕过控制器缓存。 */
	FString ReadBehaviourName() const;

	APawn* GetPlayerPawn() const;

	/** 采集 query_world 的载荷（一段 JSON 文本）。扫全图，不设半径。 */
	FString BuildWorldSnapshot();

	/** Actor <-> 稳定 id（actor:1, actor:2 ...），同一会话内不变。 */
	FString RegisterActor(AActor* Actor);
	bool ResolveActorId(const FString& Id, AActor*& OutActor);

	/** 组 FToolResult 并回填给组件（触发下一轮）。 */
	void SendToolResult(const FString& ToolId, bool bOk, const FString& Status,
		const FString& ResultJson, const FString& ErrorCode, const FString& ErrorHint);

	/** 走路前的可诊断检查：导航系统 / 自己是否在网格上 / 目标是否在网格上 / 有没有通路。
	 *  失败时给出精确的 code 与 hint（no_navmesh / self_off_navmesh / target_off_navmesh / no_path）。
	 *  成功时 OutGoalLocation 带回投影到网格后的目标点，供按位置移动的回退使用。 */
	bool ValidateReachability(AActor* Target, FString& OutCode, FString& OutHint, FVector& OutGoalLocation);

	/** 8 向方位词（相对 From 的朝向）。 */
	static FString DirectionWord(const AActor* From, const FVector& ToLocation);

	TWeakObjectPtr<UAIBrainComponent> Brain;

	/** 初始状态（Patrol）只在黑板创建之后写一次。 */
	bool bInitialStateApplied = false;

	/** 当前行为状态 id（patrol / follow / combat），用于 query_world 回传。 */
	FString CurrentStateId = TEXT("patrol");

	FPendingMove PendingMove;
	bool bHasPendingTool = false;

	TMap<int32, TWeakObjectPtr<AActor>> KnownActors;
	int32 NextActorId = 1;
};
