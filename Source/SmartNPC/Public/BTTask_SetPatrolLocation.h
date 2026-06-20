// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_SetPatrolLocation.generated.h"

/**
 * 
 */
UCLASS()
class SMARTNPC_API UBTTask_SetPatrolLocation : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UBTTask_SetPatrolLocation();

	// ∂‘”¶Õº¿Ô "Target Location: Target"
	UPROPERTY(EditAnywhere, Category = Blackboard)
	FBlackboardKeySelector TargetLocationKey;

	UPROPERTY(EditAnywhere, Category = Patrol)
	float PatrolRadius = 1000.f;

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};
