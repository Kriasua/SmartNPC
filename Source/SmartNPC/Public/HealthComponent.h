// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHealthChanged, float, CurrentHealth, float, MaxHealth);

/**
 * 通用血量组件。挂在任何 Actor（通常是 Pawn）上。
 *
 * 只做数据，不碰 UI：UI 由 UHealthWidgetBase 的派生 widget 自己订阅 OnHealthChanged。
 *
 * 伤害链路：
 *   UGameplayStatics::ApplyPointDamage / ApplyDamage
 *     → 目标 Actor 的 TakeDamage → 蓝图 Event AnyDamage
 *       → HealthComponent::ApplyDamage
 *         → OnHealthChanged 广播（血条等订阅者自己更新）
 *
 * 组件不会自动收伤害：引擎的伤害是发给 Actor 的，所以需要蓝图把 Event AnyDamage 接到 ApplyDamage。
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SMARTNPC_API UHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHealthComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health", meta = (ClampMin = "1"))
	float MaxHealth = 100.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	float CurrentHealth = 0.f;

	UPROPERTY(BlueprintAssignable, Category = "Health")
	FOnHealthChanged OnHealthChanged;

	/** 扣血（负数会被忽略；已经死了也忽略）。 */
	UFUNCTION(BlueprintCallable, Category = "Health")
	void ApplyDamage(float Amount);

	UFUNCTION(BlueprintCallable, Category = "Health")
	void Heal(float Amount);

	UFUNCTION(BlueprintPure, Category = "Health")
	float GetHealthPercent() const;

	UFUNCTION(BlueprintPure, Category = "Health")
	bool IsDead() const;

protected:
	virtual void BeginPlay() override;

private:
	void BroadcastHealth();
};
