// Fill out your copyright notice in the Description page of Project Settings.


#include "BTTask_SetPatrolLocation.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "NavigationSystem.h"

UBTTask_SetPatrolLocation::UBTTask_SetPatrolLocation()
{
	NodeName = TEXT("Set Randomly Patrol Location");
}

EBTNodeResult::Type UBTTask_SetPatrolLocation::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController) return EBTNodeResult::Failed;

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn) return EBTNodeResult::Failed;

	UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(GetWorld());
	if (!NavSys) return EBTNodeResult::Failed;

	FNavLocation ResultLocation;
	if (NavSys->GetRandomReachablePointInRadius(Pawn->GetActorLocation(), PatrolRadius, ResultLocation))
	{
		OwnerComp.GetBlackboardComponent()->SetValueAsVector(
			TargetLocationKey.SelectedKeyName, ResultLocation.Location);
		return EBTNodeResult::Succeeded;
	}

	return EBTNodeResult::Failed;
}
