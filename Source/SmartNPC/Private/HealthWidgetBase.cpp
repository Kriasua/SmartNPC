// Fill out your copyright notice in the Description page of Project Settings.

#include "HealthWidgetBase.h"
#include "HealthComponent.h"
#include "GameFramework/Pawn.h"

void UHealthWidgetBase::NativeConstruct()
{
	Super::NativeConstruct();

	// 在"拥有这个界面的玩家 Pawn"身上找血量组件并订阅 —— 蓝图里不用连绑定线。
	APawn* OwnerPawn = GetOwningPlayerPawn();
	UHealthComponent* HealthComp = OwnerPawn ? OwnerPawn->FindComponentByClass<UHealthComponent>() : nullptr;

	if (!HealthComp)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Health] %s: 拥有者 Pawn 上没有 UHealthComponent，血条不会更新"), *GetName());
		return;
	}

	HealthComp->OnHealthChanged.AddDynamic(this, &UHealthWidgetBase::HandleHealthChanged);
	BoundHealth = HealthComp;

	// 推一次初始值，避免界面在第一次扣血前是空的。
	OnHealthUpdated(HealthComp->CurrentHealth, HealthComp->MaxHealth);
}

void UHealthWidgetBase::NativeDestruct()
{
	if (BoundHealth.IsValid())
	{
		BoundHealth->OnHealthChanged.RemoveDynamic(this, &UHealthWidgetBase::HandleHealthChanged);
		BoundHealth.Reset();
	}

	Super::NativeDestruct();
}

void UHealthWidgetBase::HandleHealthChanged(float CurrentHealth, float MaxHealth)
{
	OnHealthUpdated(CurrentHealth, MaxHealth);
}
