// Fill out your copyright notice in the Description page of Project Settings.


#include "BTTask_SetStillPatrolPointLoc.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "SteampunkCatNPC.h"


UBTTask_SetStillPatrolPointLoc::UBTTask_SetStillPatrolPointLoc()
{
	NodeName = TEXT("Set Still Patrol Points");
}

EBTNodeResult::Type UBTTask_SetStillPatrolPointLoc::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AICtrller = OwnerComp.GetAIOwner();
	if (!AICtrller) return EBTNodeResult::Failed;

	APawn* Pawn = AICtrller->GetPawn();
	if (!Pawn) return EBTNodeResult::Failed;

	ASteampunkCatNPC* NPC = Cast<ASteampunkCatNPC>(Pawn);
	if (!NPC) return EBTNodeResult::Failed;

	TArray<AActor*>& Stillpoints = NPC->StillPatrolPoint;
	if (Stillpoints.Num() == 0) return EBTNodeResult::Failed;

	int32& Idx = NPC->curPointIdx;
	if (!Stillpoints.IsValidIndex(Idx)) Idx = 0;

	AActor* TargetPoint = Stillpoints[Idx];
	if (!TargetPoint) return EBTNodeResult::Failed;

	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return EBTNodeResult::Failed;

	bool& reverse = NPC->reverse;

	BB->SetValueAsVector(TargetLocationKey.SelectedKeyName, TargetPoint->GetActorLocation());

	if (Idx == 0)
	{
		reverse = false;
	}
	if (Idx == Stillpoints.Num() - 1)
	{
		reverse = true;
	}

	if (reverse)
	{
		Idx--;
	}
	else
	{
		Idx++;
	}

	return EBTNodeResult::Succeeded;
}
