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

/**
 * 模型这一轮要说的话 / 要做的工具调用。
 *
 * 协议（每轮一个 JSON）：
 *   {"type":"final_answer","text":"喵，到了。"}
 *   {"type":"tool_call","tool":"query_world"}
 *   {"type":"tool_call","tool":"move_to","args":{"target":"actor:3"}}
 *   {"type":"tool_call","tool":"follow"}
 */
USTRUCT(BlueprintType)
struct FNPCDecision
{
	GENERATED_BODY()

	/** true = 说完就结束本轮；false = 要执行一个工具，执行完再来一轮。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	bool bFinalAnswer = false;

	/** 工具 id（follow / patrol / combat / query_world / move_to）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString ToolId;

	/** move_to 的目标 id（player:0 或 actor:N）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString Target;

	/** 台词（final_answer 的 text）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString Speech;
};

/** 工具执行结果。由行为层填写，组件负责补上 step / elapsed_ms 并组装成观察 JSON。 */
USTRUCT(BlueprintType)
struct FToolResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString ToolId;

	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	bool bOk = true;

	/** Succeeded / Failed / Rejected / TimedOut */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString Status = TEXT("Succeeded");

	/** 成功时的载荷（一段 JSON 对象文本，可为空）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString ResultJson;

	/** 失败时的封闭错误码：unknown_tool / bad_args / no_target / target_not_found / ... */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString ErrorCode;

	/** 失败时给模型的一句中文提示，告诉它下一步怎么改。 */
	UPROPERTY(BlueprintReadOnly, Category = "AI_System|LLM")
	FString ErrorHint;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnNPCDecision, FNPCDecision, Decision);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SMARTNPC_API UAIBrainComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAIBrainComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System")
	FString NPC_Name = TEXT("NPC_01");

	/** OpenAI-compatible base URL (no trailing /chat/completions).
	 *  DeepSeek default; local servers (Ollama / llama.cpp) work the same way. */
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

	/** 一条玩家指令最多允许执行几个工具（ReAct 循环上限）。3 = 查询 → 行动 → 回答。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM", meta = (ClampMin = "1", ClampMax = "6"))
	int32 MaxToolSteps = 3;

	/** 角色的自我设定。只影响语气/口吻，不影响能力清单（能力由动作注册表决定）。 */
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

	/** 结构化决策出口：控制器订阅它去执行工具。 */
	UPROPERTY(BlueprintAssignable, Category = "AI_System|LLM")
	FOnNPCDecision OnNPCDecision;

	UFUNCTION(BlueprintCallable, Category = "AI_System")
	void TestAILogic();

	/** 玩家说一句话 -> 开始一轮（可能包含多次工具调用的）循环。 */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void SendRequestToLLM(const FString& UserMessage);

	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void ClearLLMConversation();

	/** 行为层回填工具执行结果；组件会把它变成一条 [观察] 消息并继续下一轮。
	 *  这是 ReAct 循环的"反馈"入口。 */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void ReportToolResult(const FToolResult& Result);

	/** 行为层回填"当前处于什么状态"（patrol / follow / combat / idle）。
	 *  会拼进**每次**请求的 system prompt —— 否则模型只能靠猜，容易出现
	 *  "嘴上说停了、实际还在巡逻"的错位。 */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void SetCurrentStateHint(const FString& InState);

	/** 当前是否有循环在进行（行为层用于判断要不要上报）。 */
	UFUNCTION(BlueprintPure, Category = "AI_System|LLM")
	bool IsToolLoopActive() const;

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

	/** 用当前 ConversationMessages + 本轮观察组包并发请求（不含"添加 user 轮"）。 */
	bool IssueRequest(FString& OutError, bool bIsRetry = false);

	/** 请求失败（传输层 / 解析失败）的唯一出口：先重试，用完才弹回 user 轮、广播失败。 */
	void HandleDecisionFailure(const FString& Reason, const FString& RawContent, const FString& FinishReason);

	/** 结束本轮循环：清空观察与计数。 */
	void ResetToolLoop();

	/** 循环异常终止（步数用尽 / 重复调用），给玩家一句兜底台词。 */
	void AbortToolLoop(const TCHAR* Reason);

	/** 把一次工具结果包装成 [观察] 消息，然后继续下一轮。 */
	void PushObservationAndContinue(const FToolResult& Result);

	TArray<FLLMChatMessage> ConversationMessages;

	/** 本轮循环内产生的 [观察] 消息；**不写入** ConversationMessages，循环结束即丢弃。 */
	TArray<FLLMChatMessage> LoopObservations;

	TSharedPtr<IHttpRequest> PendingLLMRequest;
	bool bLLMRequestInFlight = false;

	/** 当前这次请求已经重试过几次；成功或发起新请求时归零。 */
	int32 RetryAttempts = 0;

	/** 本轮已经执行了几个工具。 */
	int32 ToolStepCount = 0;

	/** 是否有循环在进行。 */
	bool bToolLoopActive = false;

	/** 已广播工具、正在等行为层回填结果（防止重复上报）。 */
	bool bAwaitingToolResult = false;

	/** 上一次工具调用签名（tool + 参数），用于检测原地打转。 */
	FString LastToolSignature;

	/** 当前行为状态（由行为层回填），每次请求都会拼进 system prompt。 */
	FString CurrentStateHint;

	/** 本次工具开始时间（秒），用于 elapsed_ms。 */
	double ToolStartedAt = 0.0;

	/** 会话级统计（先只进日志，M2 再做导出）。 */
	int32 StatOk = 0;
	int32 StatRetry = 0;
	int32 StatFail = 0;
};
