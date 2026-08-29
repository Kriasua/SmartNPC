// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AttackLaserNotify.generated.h"

/**
 * 攻击 Montage 上的"发射激光"通知。
 * 放在激光发射的那一帧;触发时找到播动画的 NPC,调它的 FireLaser() 做命中检测。
 */
UCLASS(meta = (DisplayName = "Fire Laser"))
class SMARTNPC_API UAttackLaserNotify : public UAnimNotify
{
	GENERATED_BODY()

public:
	// notify 触发时引擎自动调用。MeshComp = 正在播这个动画的骨骼网格体
	virtual void Notify(USkeletalMeshComponent* MeshComp,
		UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
};
