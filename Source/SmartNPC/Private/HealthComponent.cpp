// Fill out your copyright notice in the Description page of Project Settings.

#include "HealthComponent.h"

UHealthComponent::UHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	CurrentHealth = MaxHealth;

	// 订阅者（血条 widget）会在自己的 Construct 里主动来取初始值，
	// 所以这里广播不广播都不影响正确性；广播一次方便已经在监听的对象。
	BroadcastHealth();
}

void UHealthComponent::ApplyDamage(float Amount)
{
	if (Amount <= 0.f || IsDead())
	{
		return;
	}

	CurrentHealth = FMath::Clamp(CurrentHealth - Amount, 0.f, MaxHealth);

	UE_LOG(LogTemp, Warning, TEXT("[Health] %s took %.1f damage -> %.1f / %.1f"),
		*GetNameSafe(GetOwner()), Amount, CurrentHealth, MaxHealth);

	BroadcastHealth();
}

void UHealthComponent::Heal(float Amount)
{
	if (Amount <= 0.f)
	{
		return;
	}

	CurrentHealth = FMath::Clamp(CurrentHealth + Amount, 0.f, MaxHealth);
	BroadcastHealth();
}

float UHealthComponent::GetHealthPercent() const
{
	return MaxHealth > 0.f ? CurrentHealth / MaxHealth : 0.f;
}

bool UHealthComponent::IsDead() const
{
	return CurrentHealth <= 0.f;
}

void UHealthComponent::BroadcastHealth()
{
	OnHealthChanged.Broadcast(CurrentHealth, MaxHealth);
}
