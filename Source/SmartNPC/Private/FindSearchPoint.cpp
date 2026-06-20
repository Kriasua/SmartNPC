// Fill out your copyright notice in the Description page of Project Settings.


#include "FindSearchPoint.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "NavigationSystem.h"

UFindSearchPoint::UFindSearchPoint()
{
	NodeName = TEXT("Find Search Point");

	// 限制下拉只显示 Vector 类型的键,防止选错类型
	CenterKey.AddVectorFilter(this, GET_MEMBER_NAME_CHECKED(UFindSearchPoint, CenterKey));
	SearchPointKey.AddVectorFilter(this, GET_MEMBER_NAME_CHECKED(UFindSearchPoint, SearchPointKey));
}

EBTNodeResult::Type UFindSearchPoint::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return EBTNodeResult::Failed;

	// 读取圆心(最后已知位置)
	const FVector Center = BB->GetValueAsVector(CenterKey.SelectedKeyName);

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(GetWorld());
	if (!NavSys) return EBTNodeResult::Failed;

	// 以圆心为中心,在半径内取一个随机可达点
	FNavLocation ResultLocation;
	if (NavSys->GetRandomReachablePointInRadius(Center, SearchRadius, ResultLocation))
	{
		BB->SetValueAsVector(SearchPointKey.SelectedKeyName, ResultLocation.Location);
		return EBTNodeResult::Succeeded;
	}

	return EBTNodeResult::Failed;
}
