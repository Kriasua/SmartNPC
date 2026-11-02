// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SteampunkCatNPC.generated.h"

class UWidgetComponent;
class UTextBlock;
class UUserWidget;

UCLASS()
class SMARTNPC_API ASteampunkCatNPC : public ACharacter
{
	GENERATED_BODY()

public:
	// Sets default values for this character's properties
	ASteampunkCatNPC();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	TArray<AActor*> StillPatrolPoint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	int32 curPointIdx = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	bool reverse = false;

	// 激光从哪个骨骼插槽射出(在 Mesh 上建一个 Socket,比如猫眼/炮口)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	FName MuzzleSocketName = TEXT("Muzzle");

	// 激光最大射程
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	float LaserRange = 2000.f;

	// 命中一次造成的伤害
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	float LaserDamage = 20.f;

	// 命中检测(给 notify 调用)。
	UFUNCTION(BlueprintCallable, Category = "Attack|Laser")
	void FireLaser();

	// ------------------------------------------------------------------
	// M1-6 台词气泡
	// ------------------------------------------------------------------

	/** 气泡的 widget 类：指向 WBP_BubbleSpeech。留空 = 不显示气泡（只进日志）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	TSubclassOf<UUserWidget> SpeechBubbleClass;

	/** 气泡画布的虚拟像素尺寸（对应 UMG 里的 DrawSize）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	FVector2D SpeechBubbleDrawSize = FVector2D(420.f, 120.f);

	/** 气泡在 3D 世界里的大小（相对 DrawSize 的缩放）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	float SpeechBubbleWorldScale = 0.5f;

	/** 气泡相对 Actor 的偏移（默认在头顶上方）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	FVector SpeechBubbleOffset = FVector(0.f, 0.f, 110.f);

	/** 台词显示几秒后自动隐藏。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	float SpeechSeconds = 6.f;

	/** 超过这个长度就截断并加省略号，避免长句撑破气泡。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Speech")
	int32 SpeechMaxChars = 60;

	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 世界空间的气泡组件（构造时创建；蓝图里只需要填 SpeechBubbleClass）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Speech")
	UWidgetComponent* SpeechBubbleWidget;

	/** 订阅 UAIBrainComponent::OnLLMChatFinished，把台词显示出来。 */
	UFUNCTION()
	void HandleSpeech(bool bSuccess, FString Message);

	void HideSpeech();

private:
	/** 运行时按名字 "SpeechText" 在 widget 里找到的 TextBlock。 */
	TWeakObjectPtr<UTextBlock> SpeechText;

	FTimerHandle SpeechHideTimer;
};
