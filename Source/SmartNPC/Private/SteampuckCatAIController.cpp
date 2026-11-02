// Fill out your copyright notice in the Description page of Project Settings.


#include "SteampuckCatAIController.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AIPerceptionComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"

ASteampuckCatAIController::ASteampuckCatAIController()
{
	// 需要 Tick 来检测 SmartNPC.Mode 的切换（切换时要清理黑板）。
	PrimaryActorTick.bCanEverTick = true;

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

	// 注意：这里不能写黑板。BlackboardComponent 要等 RunBehaviorTree() 在 BP 的 BeginPlay
	// 里调用后才存在，构造函数阶段 GetBlackboardComponent() 一定是 nullptr。
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

	bLastLLMMode = IsLLMModeEnabled();
}

void ASteampuckCatAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// 决策出口挂在 Pawn 的组件上；OnPossess 是"控制器已拿到 Pawn"的最早时机。
	if (UAIBrainComponent* Component = InPawn ? InPawn->FindComponentByClass<UAIBrainComponent>() : nullptr)
	{
		Component->OnNPCDecision.AddDynamic(this, &ASteampuckCatAIController::HandleLLMDecision);
		Brain = Component;
		UE_LOG(LogTemp, Log, TEXT("[LLM] controller bound to %s"), *Component->GetName());
	}
}

void ASteampuckCatAIController::OnUnPossess()
{
	if (Brain.IsValid())
	{
		Brain->OnNPCDecision.RemoveDynamic(this, &ASteampuckCatAIController::HandleLLMDecision);
		Brain.Reset();
	}

	Super::OnUnPossess();
}

void ASteampuckCatAIController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// cvar 可能在 PIE 中随时被改：检测到变化就清理黑板。
	const bool bNowLLM = IsLLMModeEnabled();
	if (bNowLLM != bLastLLMMode)
	{
		bLastLLMMode = bNowLLM;
		ResetBlackboardForModeSwitch();
		UE_LOG(LogTemp, Warning, TEXT("[LLM] mode -> %s (blackboard reset)"),
			bNowLLM ? TEXT("LLM") : TEXT("native"));
	}
}

void ASteampuckCatAIController::onTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor) return;

	// LLM 模式下感知不参与：否则它会在下一帧覆盖掉 LLM 写进黑板的值。
	if (IsLLMModeEnabled()) return;

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

void ASteampuckCatAIController::HandleLLMDecision(FNPCDecision Decision)
{
	if (!IsLLMModeEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] decision ignored: native mode is active"));
		return;
	}

	ApplyDecisionToBlackboard(Decision);
}

void ASteampuckCatAIController::ApplyDecisionToBlackboard(const FNPCDecision& Decision)
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp)
	{
		// 只有 BP 里没调 RunBehaviorTree（或黑板资产不兼容）才会到这里。
		UE_LOG(LogTemp, Warning, TEXT("[LLM] no blackboard component; decision dropped"));
		return;
	}

	switch (Decision.Action)
	{
	case ENPCAction::Patrol:
	{
		uint8 PatrolValue = 0;
		if (!ResolveBehaviourValue(BlackboardComp, TEXT("Patrol"), PatrolValue))
		{
			UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=patrol FAILED: enum value 'Patrol' not found"));
			return;
		}

		// 先退出追击分支（行为树第一层门是 IsPlayerInSight），再切到巡逻。
		BlackboardComp->SetValueAsBool(TEXT("IsPlayerInSight"), false);
		BlackboardComp->ClearValue(TEXT("ChasingTarget"));
		BlackboardComp->SetValueAsEnum(TEXT("NPCBehaviour"), PatrolValue);

		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=patrol -> IsPlayerInSight=false NPCBehaviour=%d"),
			static_cast<int32>(PatrolValue));
		return;
	}

	case ENPCAction::MoveTo:
	{
		APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
		if (!PlayerPawn)
		{
			UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=move_to FAILED: no player pawn"));
			return;
		}

		// 追击分支由 IsPlayerInSight 门控，所以这里"借"这个键当"要追吗"用（见头文件的技术债说明）。
		BlackboardComp->SetValueAsBool(TEXT("IsPlayerInSight"), true);
		BlackboardComp->SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);

		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=move_to -> chasing %s"), *PlayerPawn->GetName());
		return;
	}

	case ENPCAction::None:
	default:
		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=none (no-op)"));
		return;
	}
}

bool ASteampuckCatAIController::ResolveBehaviourValue(UBlackboardComponent* BlackboardComp, const TCHAR* ValueName, uint8& OutValue)
{
	if (!BlackboardComp)
	{
		return false;
	}

	const UBlackboardData* Data = BlackboardComp->GetBlackboardAsset();
	if (!Data)
	{
		UE_LOG(LogTemp, Error, TEXT("[LLM] blackboard component has no BlackboardData asset"));
		return false;
	}

	for (const FBlackboardEntry& Entry : Data->Keys)
	{
		if (Entry.EntryName != TEXT("NPCBehaviour") || !Entry.KeyType)
		{
			continue;
		}

		const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType);
		if (!EnumKey)
		{
			UE_LOG(LogTemp, Error, TEXT("[LLM] blackboard key 'NPCBehaviour' is not an enum key"));
			return false;
		}

		UEnum* Enum = EnumKey->EnumType;
		if (!Enum)
		{
			UE_LOG(LogTemp, Error, TEXT("[LLM] blackboard key 'NPCBehaviour' has no enum type assigned"));
			return false;
		}

		// 蓝图枚举（UserDefinedEnum）的内部名通常是 NewEnumerator0 之类的自动名，
		// "Patrol" 只存在于显示名里 —— 所以两者都要比，并且把可用名字打出来方便排查。
		const FString Wanted(ValueName);
		FString Available;

		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			const FString InternalName = Enum->GetNameStringByIndex(Index);
			if (InternalName.EndsWith(TEXT("_MAX")))
			{
				continue;
			}

			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			Available += FString::Printf(TEXT("'%s'(%s) "), *DisplayName, *InternalName);

			const bool bMatches = DisplayName.Equals(Wanted, ESearchCase::IgnoreCase)
				|| InternalName.Equals(Wanted, ESearchCase::IgnoreCase)
				|| InternalName.EndsWith(FString(TEXT("::")) + Wanted, ESearchCase::IgnoreCase);

			if (bMatches)
			{
				OutValue = static_cast<uint8>(Enum->GetValueByIndex(Index));
				return true;
			}
		}

		UE_LOG(LogTemp, Error, TEXT("[LLM] enum '%s' has no value '%s'. Available: %s"),
			*Enum->GetName(), ValueName, *Available);
		return false;
	}

	UE_LOG(LogTemp, Error, TEXT("[LLM] blackboard has no key named 'NPCBehaviour'"));
	return false;
}

void ASteampuckCatAIController::ResetBlackboardForModeSwitch()
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp)
	{
		return;
	}

	// 两个方向都清成"没看见玩家 + 巡逻"，让新的一方从干净状态开始。
	BlackboardComp->SetValueAsBool(TEXT("IsPlayerInSight"), false);
	BlackboardComp->ClearValue(TEXT("ChasingTarget"));

	uint8 PatrolValue = 0;
	if (ResolveBehaviourValue(BlackboardComp, TEXT("Patrol"), PatrolValue))
	{
		BlackboardComp->SetValueAsEnum(TEXT("NPCBehaviour"), PatrolValue);
	}
}
