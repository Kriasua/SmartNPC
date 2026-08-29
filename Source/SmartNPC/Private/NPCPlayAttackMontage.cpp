// Fill out your copyright notice in the Description page of Project Settings.


#include "NPCPlayAttackMontage.h"
#include "AIController.h"
#include "SteampunkCatNPC.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"

// 每个 NPC 跑这个节点时的私有状态。普通 struct（不是 USTRUCT），
// 行为树组件会按 sizeof 给每个 NPC 各分配一块。
struct FNPCPlayAttackMemory
{
	// 缓存当前 NPC 的 AnimInstance，TickTask 里用它查 Montage 还在不在播
	TObjectPtr<UAnimInstance> AnimInst = nullptr;
};

UNPCPlayAttackMontage::UNPCPlayAttackMontage()
{
	NodeName = TEXT("Play Attack Montage");

	// 不开这个，TickTask 不会被调用
	bNotifyTick = true;
}

uint16 UNPCPlayAttackMontage::GetInstanceMemorySize() const
{
	return sizeof(FNPCPlayAttackMemory);
}

EBTNodeResult::Type UNPCPlayAttackMontage::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	// 把传进来的裸内存还原成我们的 struct，并兜底初始化（刚分配的内存内容是脏的）
	FNPCPlayAttackMemory* Mem = CastInstanceNodeMemory<FNPCPlayAttackMemory>(NodeMemory);
	Mem->AnimInst = nullptr;

	if (!AttackMontage)
	{
		return EBTNodeResult::Failed;
	}

	AAIController* AICtrller = OwnerComp.GetAIOwner();
	if (!AICtrller)
	{
		return EBTNodeResult::Failed;
	}

	APawn* Pawn = AICtrller->GetPawn();
	if (!Pawn)
	{
		return EBTNodeResult::Failed;
	}

	ASteampunkCatNPC* NPC = Cast<ASteampunkCatNPC>(Pawn);
	if (!NPC)
	{
		return EBTNodeResult::Failed;
	}

	// PlayAnimMontage 返回动画时长；<= 0 说明没播起来
	const float Duration = NPC->PlayAnimMontage(AttackMontage);
	if (Duration <= 0.0f)
	{
		return EBTNodeResult::Failed;
	}

	USkeletalMeshComponent* Mesh = NPC->GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return EBTNodeResult::Failed;
	}

	// 存进这个 NPC 的私有内存，给 TickTask / AbortTask 用
	Mem->AnimInst = AnimInstance;

	// 播放已启动，交给 TickTask 等它放完
	return EBTNodeResult::InProgress;
}

void UNPCPlayAttackMontage::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FNPCPlayAttackMemory* Mem = CastInstanceNodeMemory<FNPCPlayAttackMemory>(NodeMemory);

	// AnimInstance 没了（比如 NPC 被销毁），直接判失败收尾
	if (!Mem->AnimInst)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	// 攻击 Montage 不在播了 → 任务完成。注意一定要传 AttackMontage 这个具体资产，
	// 否则别的动画也会被当成播完。
	if (!Mem->AnimInst->Montage_IsPlaying(AttackMontage))
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

EBTNodeResult::Type UNPCPlayAttackMontage::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	FNPCPlayAttackMemory* Mem = CastInstanceNodeMemory<FNPCPlayAttackMemory>(NodeMemory);

	// 被打断时停掉正在播的攻击动画，0.2s 混出过渡更自然
	if (Mem->AnimInst)
	{
		Mem->AnimInst->Montage_Stop(0.2f, AttackMontage);
	}

	return EBTNodeResult::Aborted;
}
