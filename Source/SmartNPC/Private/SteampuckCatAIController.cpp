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
#include "NPCActionRegistry.h"

ASteampuckCatAIController::ASteampuckCatAIController()
{
	// 需要 Tick：黑板要等 RunBehaviorTree 之后才存在，所以要在这里补写一次初始状态。
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
}

void ASteampuckCatAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	bInitialStateApplied = false;

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

	// 初始状态：Patrol。黑板要等 BP 里的 RunBehaviorTree 才有，所以在这里补一次。
	if (!bInitialStateApplied)
	{
		UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
		if (BlackboardComp)
		{
			WriteBehaviour(*BlackboardComp, TEXT("Patrol"));
			BlackboardComp->ClearValue(TEXT("ChasingTarget"));
			bInitialStateApplied = true;

			UE_LOG(LogTemp, Warning, TEXT("[LLM] initial state = patrol"));
		}
	}
}

void ASteampuckCatAIController::onTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor) return;

	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp) return;

	// 只处理带 "Player" 标签的目标
	if (!Actor->ActorHasTag(FName("Player"))) return;

	// 感知只写"事实"：看不看得见 + 最后位置。行为状态由 LLM 决定（NPCBehaviour），
	// 这样感知和 LLM 就不会抢同一把方向盘。
	const bool bSensed = Stimulus.WasSuccessfullySensed();
	BlackboardComp->SetValueAsBool(TEXT("IsPlayerInSight"), bSensed);

	if (!bSensed)
	{
		// 丢失瞬间记下位置，留给"搜索"分支当圆心。
		BlackboardComp->SetValueAsVector(TEXT("LastKnownLocation"), Stimulus.StimulusLocation);
	}
}

// ---------------------------------------------------------------------------
// LLM 决策入口
// ---------------------------------------------------------------------------

void ASteampuckCatAIController::HandleLLMDecision(FNPCDecision Decision)
{
	const FActionRuntime* Spec = FindActionRuntime(Decision.Action);
	if (!Spec)
	{
		UE_LOG(LogTemp, Error, TEXT("[LLM] no action runtime registered for this decision"));
		return;
	}

	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] no blackboard component; decision dropped"));
		return;
	}

	if (!Spec->Apply || !Spec->Apply(*this, *BlackboardComp, Decision))
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=%s FAILED"), Spec->Id);
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[LLM] state -> %s"), Spec->Id);

	// 让模型知道当前处于什么状态（下一次请求的 system prompt 会带上）。
	if (Brain.IsValid() && Decision.Action != ENPCAction::None)
	{
		Brain->SetLastOutcome(FString::Printf(TEXT("当前状态：%s"), Spec->Id));
	}
}

// ---------------------------------------------------------------------------
// 状态切换（由 NPCActionRegistry 的表调用）
// ---------------------------------------------------------------------------

bool ASteampuckCatAIController::ApplyNoneAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	return true;    // 不改变状态：闲聊不该打断巡逻/跟随/战斗
}

bool ASteampuckCatAIController::ApplyFollowAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		return false;
	}

	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
	return WriteBehaviour(BlackboardComp, TEXT("Follow"));
}

bool ASteampuckCatAIController::ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	BlackboardComp.ClearValue(TEXT("ChasingTarget"));
	return WriteBehaviour(BlackboardComp, TEXT("Patrol"));
}

bool ASteampuckCatAIController::ApplyCombatAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		return false;
	}

	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
	return WriteBehaviour(BlackboardComp, TEXT("Combat"));
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

bool ASteampuckCatAIController::WriteBehaviour(UBlackboardComponent& BlackboardComp, const TCHAR* ValueName)
{
	uint8 Value = 0;
	if (!ResolveBehaviourValue(&BlackboardComp, ValueName, Value))
	{
		return false;
	}

	BlackboardComp.SetValueAsEnum(TEXT("NPCBehaviour"), Value);
	return true;
}

APawn* ASteampuckCatAIController::GetPlayerPawn() const
{
	return GetWorld() ? UGameplayStatics::GetPlayerPawn(GetWorld(), 0) : nullptr;
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
