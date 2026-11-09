// Fill out your copyright notice in the Description page of Project Settings.


#include "SteampuckCatAIController.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Prediction.h"
#include "Perception/AIPerceptionComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "Navigation/PathFollowingComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HealthComponent.h"
#include "NPCActionRegistry.h"

namespace
{
	/** 保留一位小数，避免把 7.999999 这种数字塞给模型。 */
	double RoundToTenth(double Value)
	{
		return FMath::RoundToDouble(Value * 10.0) / 10.0;
	}
}

ASteampuckCatAIController::ASteampuckCatAIController()
{
	// 需要 Tick：黑板要等 RunBehaviorTree 之后才存在，所以要在这里补写一次初始状态；
	// 另外负责 move_to 的兜底超时。
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
	bHasPendingTool = false;

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
			WriteBehaviour(*BlackboardComp, TEXT("Default"));
			BlackboardComp->ClearValue(TEXT("ChasingTarget"));
			bInitialStateApplied = true;

			UE_LOG(LogTemp, Warning, TEXT("[LLM] initial state = patrol"));
		}
		return;
	}

	// move_to 兜底：行为树一直没上报（例如根节点没跑到那条分支）就强制超时。
	if (bHasPendingTool && MoveToTimeoutSeconds > 0.f)
	{
		const double Elapsed = FPlatformTime::Seconds() - PendingMove.StartedAt;
		if (Elapsed > MoveToTimeoutSeconds)
		{
			UE_LOG(LogTemp, Warning, TEXT("[LLM] move_to watchdog fired after %.1fs"), Elapsed);
			bHasPendingTool = false;
			PendingMove = FPendingMove();
			SendToolResult(TEXT("move_to"), false, TEXT("TimedOut"), FString(),
				TEXT("timeout"), TEXT("超时还没走到，可能被挡住了；先 query_world 换个目标"));
		}
	}
}

void ASteampuckCatAIController::OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result)
{
	// 引擎对每次移动结束都给出原因码：
	//   Success / Blocked / OffPath / Aborted / Invalid / InvalidPath
	// 其中 Aborted 通常意味着"被行为树掐断"，其余多半是导航本身的问题。
	const UEnum* ResultEnum = StaticEnum<EPathFollowingResult::Type>();
	const FString CodeName = ResultEnum
		? ResultEnum->GetNameStringByValue(static_cast<int64>(Result.Code))
		: TEXT("<unknown>");

	APawn* Cat = GetPawn();
	const double DistanceM = (Cat && bHasPendingTool)
		? FVector::Dist(Cat->GetActorLocation(), PendingMove.GoalLocation) / 100.0
		: -1.0;

	// 只在"有工具在等结果"时用 Warning，否则降到 Verbose：
	// GoTo 分支每次移动完成后会重跑，逐帧打 Warning 会把日志刷爆。
	// 注意：UE_LOG 的 verbosity 必须是编译期常量，所以这里分成两段写，不能传变量。
	if (bHasPendingTool)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AI] OnMoveCompleted: code=%s | distance_to_goal=%.1fm | pending_tool=yes"),
			*CodeName, DistanceM);
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("[AI] OnMoveCompleted: code=%s | distance_to_goal=%.1fm | pending_tool=no"),
			*CodeName, DistanceM);
	}

	Super::OnMoveCompleted(RequestID, Result);
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
// 工具分派
// ---------------------------------------------------------------------------

void ASteampuckCatAIController::HandleLLMDecision(FNPCDecision Decision)
{
	const FActionRuntime* Spec = FindActionRuntimeById(Decision.ToolId);
	if (!Spec)
	{
		SendToolResult(Decision.ToolId, false, TEXT("Rejected"), FString(),
			TEXT("unknown_tool"), BuildAvailableToolsHint());
		return;
	}

	UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	if (!BlackboardComp)
	{
		SendToolResult(Decision.ToolId, false, TEXT("Failed"), FString(),
			TEXT("no_blackboard"), TEXT("行为树还没启动（RunBehaviorTree 没调用），黑板不存在"));
		return;
	}

	// Apply 内部会自己回填结果（成功或失败）；返回 false 表示连执行都没能开始。
	if (!Spec->Apply || !Spec->Apply(*this, *BlackboardComp, Decision))
	{
		SendToolResult(Decision.ToolId, false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("工具执行失败，换个方式试试"));
	}
}

// ---------------------------------------------------------------------------
// 各工具执行
// ---------------------------------------------------------------------------

bool ASteampuckCatAIController::ApplyQueryWorldAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	const FString Snapshot = BuildWorldSnapshot();
	if (Snapshot.IsEmpty())
	{
		SendToolResult(TEXT("query_world"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("采集环境失败"));
		return true;
	}

	SendToolResult(TEXT("query_world"), true, TEXT("Succeeded"), Snapshot, FString(), FString());
	return true;
}

bool ASteampuckCatAIController::ApplyMoveToAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	AActor* Target = nullptr;
	if (!ResolveActorId(Decision.Target, Target) || !Target)
	{
		SendToolResult(TEXT("move_to"), false, TEXT("Rejected"), FString(),
			TEXT("target_not_found"),
			FString::Printf(TEXT("没有 id \"%s\"；先 query_world，从 nearby 里挑"), *Decision.Target));
		return true;
	}

	if (!GetPawn())
	{
		SendToolResult(TEXT("move_to"), false, TEXT("Failed"), FString(),
			TEXT("no_pawn"), TEXT("没有控制任何 Pawn"));
		return true;
	}

	// 先诊断再动手：把"走不过去"拆成四种可分辨的病因，模型和人都能据此处理。
	FVector GoalLocation = FVector::ZeroVector;
	{
		FString RejectCode;
		FString RejectHint;
		if (!ValidateReachability(Target, RejectCode, RejectHint, GoalLocation))
		{
			UE_LOG(LogTemp, Warning, TEXT("[LLM] move_to rejected: %s"), *RejectCode);
			SendToolResult(TEXT("move_to"), false, TEXT("Rejected"), FString(), RejectCode, RejectHint);
			return true;
		}
	}

	// 用「坐标」代替「追 Actor」：实测同一个目标点，MoveToActor 被引擎拒绝，
	// MoveToLocation 却能成功（与接受半径、Stop on Overlap 无关）。
	// 所以 move_to 一律走 TargetLocation，由行为树里的 GoTo 分支执行。
	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), nullptr);
	BlackboardComp.SetValueAsVector(TEXT("TargetLocation"), GoalLocation);

	if (!WriteBehaviour(BlackboardComp, TEXT("GoTo")))
	{
		SendToolResult(TEXT("move_to"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("黑板写不进去（或枚举里还没有 GoTo）"));
		return true;
	}

	PendingMove.ToolId = TEXT("move_to");
	PendingMove.TargetId = Decision.Target;
	PendingMove.Target = Target;
	PendingMove.GoalLocation = GoalLocation;
	PendingMove.StartedAt = FPlatformTime::Seconds();
	bHasPendingTool = true;

	UE_LOG(LogTemp, Warning, TEXT("[LLM] move_to started: target=%s goal=%s"),
		*Decision.Target, *GoalLocation.ToCompactString());
	return true;    // 结果稍后由行为树上报
}

bool ASteampuckCatAIController::ApplyIdleAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	// 停止一切行为，原地站住。枚举里的显示名是 "Default"，但对模型统一叫 "idle"。
	BlackboardComp.ClearValue(TEXT("ChasingTarget"));
	BlackboardComp.ClearValue(TEXT("TargetLocation"));

	if (!WriteBehaviour(BlackboardComp, TEXT("Default")))
	{
		SendToolResult(TEXT("idle"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("黑板写不进去（或枚举里没有 Default）"));
		return true;
	}

	SendToolResult(TEXT("idle"), true, TEXT("Succeeded"), TEXT("{\"state\":\"idle\"}"), FString(), FString());
	return true;
}

bool ASteampuckCatAIController::ApplyFollowAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		SendToolResult(TEXT("follow"), false, TEXT("Rejected"), FString(),
			TEXT("no_player"), TEXT("找不到玩家"));
		return true;
	}

	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
	if (!WriteBehaviour(BlackboardComp, TEXT("Follow")))
	{
		SendToolResult(TEXT("follow"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("黑板写不进去"));
		return true;
	}

	SendToolResult(TEXT("follow"), true, TEXT("Succeeded"), TEXT("{\"state\":\"follow\"}"), FString(), FString());
	return true;
}

bool ASteampuckCatAIController::ApplyPatrolAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	BlackboardComp.ClearValue(TEXT("ChasingTarget"));
	if (!WriteBehaviour(BlackboardComp, TEXT("Patrol")))
	{
		SendToolResult(TEXT("patrol"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("黑板写不进去"));
		return true;
	}

	SendToolResult(TEXT("patrol"), true, TEXT("Succeeded"), TEXT("{\"state\":\"patrol\"}"), FString(), FString());
	return true;
}

bool ASteampuckCatAIController::ApplyCombatAction(UBlackboardComponent& BlackboardComp, const FNPCDecision& Decision)
{
	APawn* PlayerPawn = GetPlayerPawn();
	if (!PlayerPawn)
	{
		SendToolResult(TEXT("combat"), false, TEXT("Rejected"), FString(),
			TEXT("no_player"), TEXT("找不到玩家"));
		return true;
	}

	BlackboardComp.SetValueAsObject(TEXT("ChasingTarget"), PlayerPawn);
	if (!WriteBehaviour(BlackboardComp, TEXT("Combat")))
	{
		SendToolResult(TEXT("combat"), false, TEXT("Failed"), FString(),
			TEXT("apply_failed"), TEXT("黑板写不进去"));
		return true;
	}

	SendToolResult(TEXT("combat"), true, TEXT("Succeeded"), TEXT("{\"state\":\"combat\"}"), FString(), FString());
	return true;
}

// ---------------------------------------------------------------------------
// 行为树上报入口
// ---------------------------------------------------------------------------

void ASteampuckCatAIController::ReportPendingToolResult(bool bSucceeded)
{
	// 同一条 Follow 分支也服务"状态跟随"，那时没有待处理工具，直接忽略。
	if (!bHasPendingTool)
	{
		return;
	}

	bHasPendingTool = false;

	// 先把要用的信息取出来，再清空 PendingMove。
	const FString PendingTargetId = PendingMove.TargetId;
	AActor* Target = PendingMove.Target.Get();
	APawn* Cat = GetPawn();
	const double DistanceM = (Target && Cat)
		? FVector::Dist(Cat->GetActorLocation(), Target->GetActorLocation()) / 100.0
		: -1.0;

	PendingMove = FPendingMove();

	// 现场快照：BT 上报时跟随组件处于什么状态、当前是什么行为状态、离目标还有多远。
	{
		const UEnum* StatusEnum = StaticEnum<EPathFollowingStatus::Type>();
		const EPathFollowingStatus::Type Status = GetPathFollowingComponent()
			? GetPathFollowingComponent()->GetStatus()
			: EPathFollowingStatus::Idle;

		UE_LOG(LogTemp, Warning,
			TEXT("[LLM] report pending tool: succeeded=%s | state=%s | pf_status=%s | distance=%.1fm"),
			bSucceeded ? TEXT("true") : TEXT("false"),
			*CurrentStateId,
			StatusEnum ? *StatusEnum->GetNameStringByValue(static_cast<int64>(Status)) : TEXT("<unknown>"),
			DistanceM);
	}

	if (bSucceeded)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("target"), PendingTargetId);
		Result->SetStringField(TEXT("target_name"), Target ? Target->GetActorNameOrLabel() : TEXT("?"));
		Result->SetNumberField(TEXT("final_distance_m"), DistanceM >= 0.0 ? RoundToTenth(DistanceM) : 0.0);

		FString Json;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);

		SendToolResult(TEXT("move_to"), true, TEXT("Succeeded"), Json, FString(), FString());
	}
	else
	{
		// 失败后清掉目标：否则行为树会每帧重试同一条走不通的路线（寻路开销 + 噪声），
		// 而模型已经拿到 Failed，接下来该由它决定要不要换目标。
		if (UBlackboardComponent* BlackboardComp = GetBlackboardComponent())
		{
			BlackboardComp->ClearValue(TEXT("ChasingTarget"));
		}

		SendToolResult(TEXT("move_to"), false, TEXT("Failed"), FString(),
			TEXT("unreachable"),
			TEXT("走不过去（可能被挡住或目标不可达）；先 query_world 换个目标"));
	}
}

bool ASteampuckCatAIController::HasPendingTool() const
{
	return bHasPendingTool;
}

// ---------------------------------------------------------------------------
// 世界快照 / id 解析
// ---------------------------------------------------------------------------

FString ASteampuckCatAIController::BuildWorldSnapshot()
{
	APawn* Cat = GetPawn();
	if (!Cat)
	{
		return FString();
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// self
	{
		TSharedPtr<FJsonObject> Self = MakeShared<FJsonObject>();
		Self->SetStringField(TEXT("state"), ReadBehaviourName());

		if (const UHealthComponent* Health = Cat->FindComponentByClass<UHealthComponent>())
		{
			Self->SetNumberField(TEXT("health"), RoundToTenth(Health->CurrentHealth));
			Self->SetNumberField(TEXT("max_health"), RoundToTenth(Health->MaxHealth));
		}

		Root->SetObjectField(TEXT("self"), Self);
	}

	// player
	APawn* PlayerPawn = GetPlayerPawn();
	if (PlayerPawn)
	{
		TSharedPtr<FJsonObject> PlayerObj = MakeShared<FJsonObject>();
		const double DistanceM = FVector::Dist(Cat->GetActorLocation(), PlayerPawn->GetActorLocation()) / 100.0;
		PlayerObj->SetNumberField(TEXT("distance_m"), RoundToTenth(DistanceM));
		PlayerObj->SetStringField(TEXT("direction"), DirectionWord(Cat, PlayerPawn->GetActorLocation()));

		bool bVisible = false;
		if (const UBlackboardComponent* BlackboardComp = GetBlackboardComponent())
		{
			bVisible = BlackboardComp->GetValueAsBool(TEXT("IsPlayerInSight"));
		}
		PlayerObj->SetBoolField(TEXT("visible"), bVisible);

		Root->SetObjectField(TEXT("player"), PlayerObj);
	}

	// nearby：带 Interactable 标签的 Actor
	struct FNearbyEntry
	{
		AActor* Actor = nullptr;
		double DistanceCm = 0.0;
	};

	TArray<AActor*> Tagged;
	UGameplayStatics::GetAllActorsWithTag(this, InteractableTag, Tagged);

	TArray<FNearbyEntry> Nearby;

	for (AActor* Candidate : Tagged)
	{
		if (!Candidate || Candidate == Cat)
		{
			continue;
		}

		// 活物不算"可交互物"：玩家已经单独作为 player:0 给出了，其他 NPC 也不该混进道具列表。
		if (Candidate->IsA<APawn>())
		{
			continue;
		}

		const double DistanceCm = FVector::Dist(Cat->GetActorLocation(), Candidate->GetActorLocation());
		Nearby.Add({Candidate, DistanceCm});
	}

	Nearby.Sort([](const FNearbyEntry& Left, const FNearbyEntry& Right)
	{
		return Left.DistanceCm < Right.DistanceCm;
	});

	TArray<TSharedPtr<FJsonValue>> NearbyJson;
	const int32 Count = FMath::Min(Nearby.Num(), MaxNearbyActors);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		AActor* Actor = Nearby[Index].Actor;

		FString Name = Actor->GetActorNameOrLabel();
		if (Name.Len() > 12)
		{
			Name = Name.Left(12);
		}

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), RegisterActor(Actor));
		Item->SetStringField(TEXT("name"), Name);
		Item->SetStringField(TEXT("category"), TEXT("interactable"));
		Item->SetNumberField(TEXT("distance_m"), RoundToTenth(Nearby[Index].DistanceCm / 100.0));
		Item->SetStringField(TEXT("direction"), DirectionWord(Cat, Actor->GetActorLocation()));

		NearbyJson.Add(MakeShared<FJsonValueObject>(Item));
	}

	Root->SetArrayField(TEXT("nearby"), NearbyJson);
	Root->SetBoolField(TEXT("truncated"), Nearby.Num() > Count);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		return FString();
	}

	return Json;
}

FString ASteampuckCatAIController::RegisterActor(AActor* Actor)
{
	if (!Actor)
	{
		return FString();
	}

	for (const TPair<int32, TWeakObjectPtr<AActor>>& Pair : KnownActors)
	{
		if (Pair.Value.Get() == Actor)
		{
			return FString::Printf(TEXT("actor:%d"), Pair.Key);
		}
	}

	const int32 NewId = NextActorId++;
	KnownActors.Add(NewId, Actor);

	if (KnownActors.Num() > 32)
	{
		for (auto It = KnownActors.CreateIterator(); It; ++It)
		{
			if (!It->Value.IsValid())
			{
				It.RemoveCurrent();
			}
		}

		while (KnownActors.Num() > 32)
		{
			int32 MinKey = MAX_int32;
			for (const TPair<int32, TWeakObjectPtr<AActor>>& Pair : KnownActors)
			{
				MinKey = FMath::Min(MinKey, Pair.Key);
			}
			KnownActors.Remove(MinKey);
		}
	}

	return FString::Printf(TEXT("actor:%d"), NewId);
}

bool ASteampuckCatAIController::ResolveActorId(const FString& Id, AActor*& OutActor)
{
	const FString Trimmed = Id.TrimStartAndEnd();

	if (Trimmed.Equals(TEXT("player:0"), ESearchCase::IgnoreCase))
	{
		OutActor = GetPlayerPawn();
		return OutActor != nullptr;
	}

	if (Trimmed.StartsWith(TEXT("actor:"), ESearchCase::IgnoreCase))
	{
		const int32 Key = FCString::Atoi(*Trimmed.RightChop(6));
		if (const TWeakObjectPtr<AActor>* Found = KnownActors.Find(Key))
		{
			OutActor = Found->Get();
			return OutActor != nullptr;
		}
	}

	OutActor = nullptr;
	return false;
}

FString ASteampuckCatAIController::DirectionWord(const AActor* From, const FVector& ToLocation)
{
	if (!From)
	{
		return TEXT("unknown");
	}

	const FRotator ToRotation = (ToLocation - From->GetActorLocation()).Rotation();
	const float DeltaYaw = FRotator::NormalizeAxis(ToRotation.Yaw - From->GetActorRotation().Yaw);

	int32 Index = FMath::RoundToInt(DeltaYaw / 45.f);
	Index = ((Index % 8) + 8) % 8;

	static const TCHAR* Words[] =
	{
		TEXT("front"), TEXT("front-right"), TEXT("right"), TEXT("back-right"),
		TEXT("back"), TEXT("back-left"), TEXT("left"), TEXT("front-left")
	};

	return Words[Index];
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

bool ASteampuckCatAIController::ValidateReachability(AActor* Target, FString& OutCode, FString& OutHint, FVector& OutGoalLocation)
{
	OutGoalLocation = FVector::ZeroVector;

	APawn* Cat = GetPawn();
	UWorld* World = GetWorld();
	UNavigationSystemV1* NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;

	if (!NavSys)
	{
		OutCode = TEXT("no_navmesh");
		OutHint = TEXT("场景里没有导航系统：放一个 NavMeshBoundsVolume，等导航重建完再试");
		return false;
	}

	// 1) 自己脚下有没有网格
	FNavLocation SelfLocation;
	if (!Cat || !NavSys->ProjectPointToNavigation(Cat->GetActorLocation(), SelfLocation, FVector(200.f, 200.f, 400.f)))
	{
		OutCode = TEXT("self_off_navmesh");
		OutHint = TEXT("你自己脚下没有导航网格，先扩大 NavMeshBoundsVolume");
		return false;
	}

	// 2) 目标脚下有没有网格
	if (!Target)
	{
		OutCode = TEXT("target_not_found");
		OutHint = TEXT("目标已经不存在了");
		return false;
	}

	FNavLocation TargetLocation;

	// 把目标点朝"自己"的方向推开一段再投影：
	// 目标 Actor 的原点常贴在障碍边缘，直接当终点会让角色卡在边界上、"永远差一点点"。
	const FVector FromTargetToSelf = (SelfLocation.Location - Target->GetActorLocation()).GetSafeNormal();
	const FVector DesiredGoal = Target->GetActorLocation() + FromTargetToSelf * GoalOffsetCm;

	if (!NavSys->ProjectPointToNavigation(DesiredGoal, TargetLocation, FVector(200.f, 200.f, 400.f)))
	{
		// 推开后投不到网格（目标本来就离网格很远）——退回原始位置再试一次。
		if (!NavSys->ProjectPointToNavigation(Target->GetActorLocation(), TargetLocation, FVector(200.f, 200.f, 400.f)))
		{
			OutCode = TEXT("target_off_navmesh");
			OutHint = FString::Printf(TEXT("%s 脚下没有导航网格，先扩大 NavMeshBoundsVolume"),
				*Target->GetActorNameOrLabel());
			return false;
		}
	}

	// 3) 两点之间有没有通路（部分路径也算走不通）
	UNavigationPath* Path = NavSys->FindPathToLocationSynchronously(World, SelfLocation.Location, TargetLocation.Location, Cat);
	if (!Path || !Path->IsValid() || Path->IsPartial())
	{
		OutCode = TEXT("no_path");
		OutHint = TEXT("两点都在网格上，但中间走不通（被挡住，或分属两块不连通的网格）");
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[LLM] reachability ok: %s -> %s"),
		*SelfLocation.Location.ToCompactString(), *TargetLocation.Location.ToCompactString());

	OutGoalLocation = TargetLocation.Location;
	return true;
}

FString ASteampuckCatAIController::ReadBehaviourName() const
{
	const UBlackboardComponent* BlackboardComp = GetBlackboardComponent();
	const UBlackboardData* Data = BlackboardComp ? BlackboardComp->GetBlackboardAsset() : nullptr;
	if (!Data)
	{
		return CurrentStateId;
	}

	for (const FBlackboardEntry& Entry : Data->Keys)
	{
		if (Entry.EntryName != TEXT("NPCBehaviour") || !Entry.KeyType)
		{
			continue;
		}

		const UBlackboardKeyType_Enum* EnumKey = Cast<UBlackboardKeyType_Enum>(Entry.KeyType);
		const UEnum* Enum = EnumKey ? EnumKey->EnumType : nullptr;
		if (!Enum)
		{
			break;
		}

		const int32 Value = static_cast<int32>(BlackboardComp->GetValueAsEnum(TEXT("NPCBehaviour")));
		FString Name = Enum->GetDisplayNameTextByValue(Value).ToString().ToLower();

		// 枚举里这个值叫 "Default"，但对模型统一叫 "idle"，免得它把状态和工具名对不上。
		if (Name == TEXT("default"))
		{
			Name = TEXT("idle");
		}

		return Name;
	}

	return CurrentStateId;
}

bool ASteampuckCatAIController::WriteBehaviour(UBlackboardComponent& BlackboardComp, const TCHAR* ValueName){
	uint8 Value = 0;
	if (!ResolveBehaviourValue(&BlackboardComp, ValueName, Value))
	{
		return false;
	}

	BlackboardComp.SetValueAsEnum(TEXT("NPCBehaviour"), Value);
	CurrentStateId = FString(ValueName).ToLower();

	// 把最新状态同步给 LLM（每次请求的 system prompt 都会带上"当前状态"）。
	if (Brain.IsValid())
	{
		Brain->SetCurrentStateHint(ReadBehaviourName());
	}

	return true;
}

APawn* ASteampuckCatAIController::GetPlayerPawn() const
{
	return GetWorld() ? UGameplayStatics::GetPlayerPawn(GetWorld(), 0) : nullptr;
}

void ASteampuckCatAIController::SendToolResult(const FString& ToolId, bool bOk, const FString& Status,
	const FString& ResultJson, const FString& ErrorCode, const FString& ErrorHint)
{
	if (!Brain.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] tool result dropped (no brain component)"));
		return;
	}

	FToolResult Result;
	Result.ToolId = ToolId;
	Result.bOk = bOk;
	Result.Status = Status;
	Result.ResultJson = ResultJson;
	Result.ErrorCode = ErrorCode;
	Result.ErrorHint = ErrorHint;

	Brain->ReportToolResult(Result);
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
