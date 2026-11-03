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
	// 需要 Tick：检测模式切换 + 判定 goal 完成 + 默认状态机。
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

	Hold = EHold::None;

	Super::OnUnPossess();
}

void ASteampuckCatAIController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 1) 模式切换：清空一切驱动状态，避免上一模式留下脏值。
	const bool bNowLLM = IsLLMModeEnabled();
	if (bNowLLM != bLastLLMMode)
	{
		bLastLLMMode = bNowLLM;
		Hold = EHold::None;
		ResetBlackboardToNative();

		// 感知静音期间刺激可能已过期，这里主动唤醒一次，让猫立刻重新"看见"玩家。
		if (AIPerceptionComp)
		{
			AIPerceptionComp->RequestStimuliListenerUpdate();
		}

		UE_LOG(LogTemp, Warning, TEXT("[LLM] mode -> %s (state reset)"),
			bNowLLM ? TEXT("LLM") : TEXT("native"));
	}

	// 2) 有终点的目标：判定完成 / 兜底超时。
	if (Hold == EHold::Goal && GetWorld())
	{
		const FActionRuntime* Spec = FindActionRuntime(ActiveGoal.Action);
		const double Elapsed = GetWorld()->GetTimeSeconds() - ActiveGoal.StartedAt;

		if (Spec && Spec->IsComplete && Spec->IsComplete(*this, ActiveGoal))
		{
			FinishGoal(TEXT("完成"));
		}
		else if (Elapsed >= WatchdogSeconds)
		{
			FinishGoal(TEXT("超时放弃"));
		}
	}

	// 3) 没有 LLM 在驾驶时，跑默认状态机（距离跟随 / 自主巡逻）。
	if (Hold == EHold::None)
	{
		UpdateDefaultState();
	}
}

void ASteampuckCatAIController::onTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor) return;

	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp) return;

	// 只处理带 "Player" 标签的目标
	if (!Actor->ActorHasTag(FName("Player"))) return;

	// 感知只写"事实"：看不看得见 + 最后位置。行为状态由状态机 / LLM 决定，
	// 这样感知和 LLM 就不会抢同一把方向盘（M1-5 的核心）。
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
	if (!IsLLMModeEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] decision ignored: native mode is active"));
		return;
	}

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

	// none：不改变行为，也不占用方向盘。
	if (Decision.Action == ENPCAction::None)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply action=none (no-op)"));
		return;
	}

	if (Spec->bIsMode)
	{
		// mode：没有终点，持续到下一次意图或模式切换。
		Hold = EHold::Mode;
		UE_LOG(LogTemp, Warning, TEXT("[LLM] apply mode=%s (persists until replaced)"), Spec->Id);
		return;
	}

	// goal：记录起点，之后每帧判定完成。
	Hold = EHold::Goal;
	ActiveGoal.Action = Decision.Action;
	ActiveGoal.Target = GetPlayerPawn();
	ActiveGoal.StartedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;

	UE_LOG(LogTemp, Warning, TEXT("[LLM] apply goal=%s (arrive radius %.0f)"), Spec->Id, ArriveRadius);
}

void ASteampuckCatAIController::FinishGoal(const TCHAR* Reason)
{
	const double Elapsed = (GetWorld() && ActiveGoal.StartedAt > 0.0)
		? GetWorld()->GetTimeSeconds() - ActiveGoal.StartedAt
		: 0.0;

	const FString ActionId = ActionToId(ActiveGoal.Action);
	UE_LOG(LogTemp, Warning, TEXT("[LLM] goal %s %s in %.1fs"), *ActionId, Reason, Elapsed);

	// 反馈给模型：下一次请求的 system prompt 里会带上这句。
	if (Brain.IsValid())
	{
		Brain->SetLastOutcome(FString::Printf(TEXT("%s %s（用时 %.1f 秒）"), *ActionId, Reason, Elapsed));
	}

	Hold = EHold::None;
	ActiveGoal = FActiveGoal();

	// 立刻回到默认状态机（不等下一帧）。
	UpdateDefaultState();
}

// ---------------------------------------------------------------------------
// 动作执行（由 NPCActionRegistry 的表调用）
// ---------------------------------------------------------------------------

bool ASteampuckCatAIController::ApplyNoneAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	return true;    // 什么都不写：闲聊不该打断正在执行的行为
}

bool ASteampuckCatAIController::ApplyMoveToAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		return false;
	}

	// 追击/跟随分支的门是 bShouldFollow（行为树顶层），不是 IsPlayerInSight。
	BlackboardComp.SetValueAsBool(TEXT("bShouldFollow"), true);
	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
	return true;
}

bool ASteampuckCatAIController::ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	uint8 PatrolValue = 0;
	if (!ResolveBehaviourValue(&BlackboardComp, TEXT("Patrol"), PatrolValue))
	{
		return false;
	}

	BlackboardComp.SetValueAsBool(TEXT("bShouldFollow"), false);
	BlackboardComp.ClearValue(TEXT("ChasingTarget"));
	BlackboardComp.SetValueAsEnum(TEXT("NPCBehaviour"), PatrolValue);
	return true;
}

bool ASteampuckCatAIController::IsMoveToComplete(const FActiveGoal& Goal) const
{
	// 优先用 goal 自己的目标：以后加 move_to_actor 之类时，完成判定不会退化成"到玩家的距离"。
	const APawn* Cat = GetPawn();
	const AActor* Target = Goal.Target.IsValid() ? Goal.Target.Get() : GetPlayerPawn();
	if (!Cat || !Target)
	{
		return false;
	}

	return FVector::Dist(Cat->GetActorLocation(), Target->GetActorLocation()) <= ArriveRadius;
}

// ---------------------------------------------------------------------------
// 默认状态机
// ---------------------------------------------------------------------------

void ASteampuckCatAIController::UpdateDefaultState()
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	APawn* Cat = GetPawn();
	if (!BlackboardComp || !Cat)
	{
		return;
	}

	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		BlackboardComp->SetValueAsBool(TEXT("bShouldFollow"), false);
		return;
	}

	const float Distance = FVector::Dist(Cat->GetActorLocation(), PlayerPawn->GetActorLocation());

	// 滞回：进入用 Enter，离开用 Exit，避免在边界上抖动。
	bool bFollow = BlackboardComp->GetValueAsBool(TEXT("bShouldFollow"));
	if (!bFollow && Distance <= FollowEnterRadius)
	{
		bFollow = true;
	}
	else if (bFollow && Distance >= FollowExitRadius)
	{
		bFollow = false;
	}

	BlackboardComp->SetValueAsBool(TEXT("bShouldFollow"), bFollow);

	if (bFollow)
	{
		BlackboardComp->SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
		return;
	}

	BlackboardComp->ClearValue(TEXT("ChasingTarget"));

	// 不跟随时回落到巡逻（覆盖 LLM 模式切换后可能残留的 Search 状态）。
	uint8 PatrolValue = 0;
	if (ResolveBehaviourValue(BlackboardComp, TEXT("Patrol"), PatrolValue))
	{
		BlackboardComp->SetValueAsEnum(TEXT("NPCBehaviour"), PatrolValue);
	}
}

void ASteampuckCatAIController::ResetBlackboardToNative()
{
	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp)
	{
		return;
	}

	BlackboardComp->SetValueAsBool(TEXT("IsPlayerInSight"), false);
	BlackboardComp->SetValueAsBool(TEXT("bShouldFollow"), false);
	BlackboardComp->ClearValue(TEXT("ChasingTarget"));

	uint8 PatrolValue = 0;
	if (ResolveBehaviourValue(BlackboardComp, TEXT("Patrol"), PatrolValue))
	{
		BlackboardComp->SetValueAsEnum(TEXT("NPCBehaviour"), PatrolValue);
	}
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

APawn* ASteampuckCatAIController::GetPlayerPawn() const
{
	return GetWorld() ? UGameplayStatics::GetPlayerPawn(GetWorld(), 0) : nullptr;
}

float ASteampuckCatAIController::DistanceToPlayer() const
{
	const APawn* Cat = GetPawn();
	const APawn* PlayerPawn = GetPlayerPawn();
	if (!Cat || !PlayerPawn)
	{
		return TNumericLimits<float>::Max();
	}
	return FVector::Dist(Cat->GetActorLocation(), PlayerPawn->GetActorLocation());
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
