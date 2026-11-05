// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "HealthWidgetBase.generated.h"

class UHealthComponent;

/**
 * 血条界面的 C++ 基类。
 *
 * 自己在 Construct 时找到"拥有这个界面的玩家 Pawn"身上的 UHealthComponent 并订阅，
 * 所以蓝图里**不需要连任何绑定线**；蓝图只负责实现 OnHealthUpdated 去动 ProgressBar / 文本。
 * 换皮肤、改布局都不用碰 C++。
 */
UCLASS(Abstract)
class SMARTNPC_API UHealthWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 血量变化时调用（含首次显示）—— 蓝图里实现它。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnHealthUpdated(float CurrentHealth, float MaxHealth);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

private:
	UFUNCTION()
	void HandleHealthChanged(float CurrentHealth, float MaxHealth);

	TWeakObjectPtr<UHealthComponent> BoundHealth;
};
