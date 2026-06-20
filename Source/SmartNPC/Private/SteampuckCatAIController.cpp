// Fill out your copyright notice in the Description page of Project Settings.


#include "SteampuckCatAIController.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AIPerceptionComponent.h"
#include "BehaviorTree/BlackboardComponent.h"

ASteampuckCatAIController::ASteampuckCatAIController()
{
	// 创建感知组件
	AIPerceptionComp = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerceptionComp"));
	SetPerceptionComponent(*AIPerceptionComp);

	// 创建并配置视觉感知
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = 1500.f;                  // 进入此距离算"看到"
	SightConfig->LoseSightRadius = 2000.f;              // 超出此距离才算"丢失"
	SightConfig->PeripheralVisionAngleDegrees = 90.f;   // 视野半角(左右各90°)
	SightConfig->SetMaxAge(5.f);
	// 关键:三种阵营都感知,否则看不到没设阵营的玩家
	SightConfig->DetectionByAffiliation.bDetectEnemies    = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals   = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = true;
	
	// 创建并配置预测(Prediction)感知
	PredictionConfig = CreateDefaultSubobject<UAISenseConfig_Prediction>(TEXT("PredictionConfig"));

	AIPerceptionComp->ConfigureSense(*SightConfig);
	AIPerceptionComp->ConfigureSense(*PredictionConfig);
	AIPerceptionComp->SetDominantSense(SightConfig->GetSenseImplementation());
	
	UBlackboardComponent* blackboardComponent = GetBlackboardComponent();
	if (!blackboardComponent) return;
	blackboardComponent->SetValueAsEnum(TEXT("NPCBehaviour"),1);
	
}

void ASteampuckCatAIController::BeginPlay()
{
	Super::BeginPlay();

	// 绑定感知更新事件到回调
	if (AIPerceptionComp)
	{
		AIPerceptionComp->OnTargetPerceptionUpdated.AddDynamic(
			this, &ASteampuckCatAIController::onTargetPerceptionUpdated);
	}
}

void ASteampuckCatAIController::onTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor) return;
	UBlackboardComponent* blackboardComponent = GetBlackboardComponent();
	// 只处理带 "Player" 标签的目标
	if (Actor->ActorHasTag(FName("Player")))
	{
		if (!blackboardComponent) return;
		
		// 看到玩家→true,丢失→false。对应 BT 里 isPlayerInSight 的 Is Set / Is Not Set
		bool playerInSight = Stimulus.WasSuccessfullySensed();
		if (playerInSight)
		{
			blackboardComponent->SetValueAsBool(TEXT("IsPlayerInSight"), true);
			blackboardComponent->SetValueAsObject(TEXT("ChasingTarget"), Actor);
		}
		else
		{
			blackboardComponent->SetValueAsBool(TEXT("IsPlayerInSight"), false);
			blackboardComponent->ClearValue(TEXT("ChasingTarget"));

			// 丢失瞬间:把"最后感知到的位置"写入 LastKnownLocation(给 Find Search Point 当圆心)
			blackboardComponent->SetValueAsVector(TEXT("LastKnownLocation"), Stimulus.StimulusLocation);
			blackboardComponent->SetValueAsEnum(TEXT("NPCBehaviour"), 0);
		}
	}
}
