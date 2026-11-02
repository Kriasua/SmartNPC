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
 * 猫的 AI 控制器。
 *
 * 两种驱动模式（控制台 `SmartNPC.Mode 0|1` 切换）：
 *   0 = 原生：感知回调把事实（看得见 / 丢失位置）写进黑板，行为树据此决策。
 *   1 = LLM ：感知回调静音，改由 Pawn 上 UAIBrainComponent 的 OnNPCDecision 驱动黑板。
 * 同一时刻只有一个写入者，所以两套逻辑不会互相覆盖。
 *
 * 技术债（M1-4）：LLM 模式下 IsPlayerInSight 被当作"要追吗"的控制位使用，而行为树第一层
 * 门正是它。M1-5 会引入衍生键（如 bShouldChase）并把仲裁收敛到 ApplyDecisionToBlackboard 一处。
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

	/** 把决策翻译成黑板键。M1-5 的优先级仲裁也将加在这里。 */
	void ApplyDecisionToBlackboard(const FNPCDecision& Decision);

	/** 从黑板键自己的枚举定义里按名字取数值，避免把 0/1 硬编码进代码。 */
	static bool ResolveBehaviourValue(UBlackboardComponent* Blackboard, const TCHAR* ValueName, uint8& OutValue);

	/** 模式切换时清理黑板，避免上一模式留下的脏值卡住行为树。 */
	void ResetBlackboardForModeSwitch();

private:
	TWeakObjectPtr<UAIBrainComponent> Brain;

	/** 上一次看到的模式值，用于检测切换。 */
	bool bLastLLMMode = false;
};
