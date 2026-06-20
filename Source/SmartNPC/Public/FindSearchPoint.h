// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "FindSearchPoint.generated.h"

/**
 * 以"圆心键"(如 LastKnownLocation)为中心,在半径 SearchRadius 内取一个随机可达点,
 * 写入"输出键"(如 SearchPoint)。用于丢失玩家后的区域搜索。
 */
UCLASS()
class SMARTNPC_API UFindSearchPoint : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UFindSearchPoint();

	// 搜索圆心(在 BT 里指向 LastKnownLocation)
	UPROPERTY(EditAnywhere, Category = Blackboard)
	FBlackboardKeySelector CenterKey;

	// 输出:随机搜索点(在 BT 里指向 SearchPoint)
	UPROPERTY(EditAnywhere, Category = Blackboard)
	FBlackboardKeySelector SearchPointKey;

	// 搜索半径 r
	UPROPERTY(EditAnywhere, Category = Search, meta = (ClampMin = "0.0"))
	float SearchRadius = 1500.f;

protected:
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};
