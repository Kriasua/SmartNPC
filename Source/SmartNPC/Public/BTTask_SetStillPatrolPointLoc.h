// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_SetStillPatrolPointLoc.generated.h"

/**
 * 
 */
UCLASS()
class SMARTNPC_API UBTTask_SetStillPatrolPointLoc : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_SetStillPatrolPointLoc();
	UPROPERTY(EditAnywhere, Category = Blackboard)
	FBlackboardKeySelector TargetLocationKey;

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};
