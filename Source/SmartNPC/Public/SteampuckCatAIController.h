// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AISenseConfig_Sight.h"
#include "SteampuckCatAIController.generated.h"

/**
 * 
 */
UCLASS()
class SMARTNPC_API ASteampuckCatAIController : public AAIController
{
	GENERATED_BODY()
	
public:
	ASteampuckCatAIController();
	
protected:
	virtual void BeginPlay() override;
	
	// 感知组件本体
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAIPerceptionComponent* AIPerceptionComp;

	// 视觉感知配置
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAISenseConfig_Sight* SightConfig;
	
	// 预感感知配置
	UPROPERTY(VisibleAnywhere, Category = "AI|Perception")
	UAISenseConfig_Prediction* PredictionConfig;
	
	UFUNCTION()
	void onTargetPerceptionUpdated(AActor* UpdatedActor, FAIStimulus Stimulus);
	
};
