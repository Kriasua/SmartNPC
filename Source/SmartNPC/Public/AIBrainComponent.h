// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "AIBrainComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLLMChatFinished, bool, bSuccess, FString, Message);

USTRUCT(BlueprintType)
struct FLLMChatMessage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System")
	FString Role;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System")
	FString Content;
};

/** M1-2: 引擎侧真正能执行的意图。LLM 只能从这三个里选，其它一律按非法输出处理。 */
UENUM(BlueprintType)
enum class ENPCAction : uint8
{
	None    UMETA(DisplayName = "None"),
	MoveTo  UMETA(DisplayName = "MoveTo"),
	Patrol  UMETA(DisplayName = "Patrol"),
};

/** M1-2: 一次决策的解析结果。M1-4 会让它随 OnNPCDecision 广播进蓝图。 */
USTRUCT(BlueprintType)
struct FNPCDecision
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	ENPCAction Action = ENPCAction::None;

	/** 猫要说的话。任何 action 下都可能是空串（只执行、不说话）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString Speech;
};

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SMARTNPC_API UAIBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAIBrainComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System")
	FString NPC_Name = TEXT("NPC_01");

	/** OpenAI-compatible base URL (no trailing /chat/completions).
	 *  DeepSeek default; the previous Qwen value was
	 *  https://dashscope.aliyuncs.com/compatible-mode/v1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString LLM_BaseUrl = TEXT("https://api.deepseek.com");

	/** LEGACY, and Transient on purpose: a key typed into the Details panel is no longer serialized,
	 *  so it can never be saved into a level (Content/__ExternalActors__/**) or a Blueprint again.
	 *  The real key is resolved per request by GetLLMAPIKey(). */
	UPROPERTY(BlueprintReadWrite, Transient, Category = "AI_System|LLM", meta = (DeprecatedProperty, DeprecationMessage = "The key is resolved at request time: set the SMARTNPC_LLM_API_KEY environment variable, fill Config/LocalSecrets.ini, or call SetLLMAPIKey()."))
	FString LLM_API_Key;

	/** Model name. `deepseek-flash` (DeepSeek-V4.1-Flash) is the cheap default;
	 *  `deepseek-v4-pro` is the stronger one. The old qwen-plus is gone.
	 *  NOTE: `deepseek-chat` / `deepseek-reasoner` were retired on 2026-07-24 and now fail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString LLM_Model = TEXT("deepseek-flash");

	/** Max completed user+assistant turns kept after each reply (matches Python MAX_TURNS). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM", meta = (ClampMin = "1"))
	int32 MaxTurns = 8;

	/** 角色的自我设定。只影响语气/口吻，不影响能力清单（能力由代码里的动作表决定）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString NPC_Persona = TEXT("你是一只蒸汽朋克机械猫，住在老工厂里。用第一人称、简短口语回答，最多 40 字。");

	/** 单次回复的输出上限。JSON 输出被 token 截断就会解析失败，别设太小。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM", meta = (ClampMin = "64", ClampMax = "4096"))
	int32 MaxOutputTokens = 512;

	/** 空内容 / 解析失败时自动重发的次数上限（0 = 不重试）。HTTP 层错误不重试，重试也没用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM", meta = (ClampMin = "0", ClampMax = "3"))
	int32 MaxRetries = 1;

	/** 是否向 API 请求 JSON Output（response_format=json_object）。
	 *  A/B 开关：关掉时完全靠 system prompt 约束 + ParseDecision 的容错解析。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	bool bUseJSONResponseFormat = true;

	UPROPERTY(BlueprintAssignable, Category = "AI_System|LLM")
	FOnLLMChatFinished OnLLMChatFinished;

	UFUNCTION(BlueprintCallable, Category = "AI_System")
	void TestAILogic();

	/** Appends the user message, POSTs to an OpenAI-compatible chat/completions (DeepSeek by
	 *  default); on failure the user turn is popped again. */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void SendRequestToLLM(const FString& UserMessage);

	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void ClearLLMConversation();

	/** Resolves the key for each request, in order: SMARTNPC_LLM_API_KEY / DEEPSEEK_API_KEY
	 *  (legacy DASHSCOPE_API_KEY last) environment variable -> gitignored
	 *  Config/LocalSecrets.ini -> SetLLMAPIKey() override. */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	FString GetLLMAPIKey() const;

	/** Runtime-only override. Never serialized, so it dies with the session. */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void SetLLMAPIKey(const FString& InKey);

	/** True when GetLLMAPIKey() would return something. Useful for gating calls from Blueprint. */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	bool HasLLMAPIKey() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void OnLLMHttpComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully);
	void TrimRecentTurns();
	void BroadcastOnGameThread(bool bSuccess, const FString& Message);

	/** 用当前 ConversationMessages 组包并发请求（不含"添加 user 轮"）。重试复用它。
	 *  bIsRetry 为 true 时会在 system prompt 末尾追加一句提醒，让重试成为"不同的请求"。 */
	bool IssueRequest(FString& OutError, bool bIsRetry = false);

	/** 决策失败的唯一出口：先重试，重试用尽才弹回 user 轮、广播失败、计数。 */
	void HandleDecisionFailure(const FString& Reason, const FString& RawContent, const FString& FinishReason);

	TArray<FLLMChatMessage> ConversationMessages;
	TSharedPtr<IHttpRequest> PendingLLMRequest;
	bool bLLMRequestInFlight = false;

	/** 当前这次请求已经重试过几次；成功或发起新请求时归零。 */
	int32 RetryAttempts = 0;

	/** 会话级统计（先只进日志，M2 再做导出）。 */
	int32 StatOk = 0;
	int32 StatRetry = 0;
	int32 StatFail = 0;
};
