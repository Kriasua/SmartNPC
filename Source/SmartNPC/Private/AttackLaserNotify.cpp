// Fill out your copyright notice in the Description page of Project Settings.


#include "AttackLaserNotify.h"
#include "SteampunkCatNPC.h"
#include "Components/SkeletalMeshComponent.h"

void UAttackLaserNotify::Notify(USkeletalMeshComponent* MeshComp,
	UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (!MeshComp) return;

	// Mesh 的主人就是那个角色;转成我们的猫 NPC 才能调 FireLaser
	if (ASteampunkCatNPC* NPC = Cast<ASteampunkCatNPC>(MeshComp->GetOwner()))
	{
		NPC->FireLaser();
	}
}
