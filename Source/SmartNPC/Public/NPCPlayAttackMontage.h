// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "NPCPlayAttackMontage.generated.h"

class UAnimMontage;

/**
 * 播放攻击 Montage 的行为树任务。
 * ExecuteTask 启动播放并返回 InProgress；TickTask 每帧轮询 Montage 是否还在播，
 * 播完了就 FinishLatentTask(Succeeded)。被打断时 AbortTask 停掉动画。
 *
 * 本次执行的临时状态存在 NodeMemory（每个 NPC 一份）里，不放成员变量，
 * 因为节点实例是所有跑这棵树的 NPC 共用的。
 */
UCLASS()
class SMARTNPC_API UNPCPlayAttackMontage : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UNPCPlayAttackMontage();

	// 要播放的攻击动画，在行为树节点的 Details 面板里指定
	UPROPERTY(EditAnywhere, Category = Attack)
	TObjectPtr<UAnimMontage> AttackMontage;

protected:
	// 启动播放，返回 InProgress
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	// 每帧轮询：Montage 播完就结束任务
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

	// 任务被中途打断时（玩家脱离视野）停掉动画
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	// 告诉行为树这个节点每个 NPC 要分配多大的私有内存
	virtual uint16 GetInstanceMemorySize() const override;
};
