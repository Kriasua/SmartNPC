
#include "AIBrainComponent.h"
#include "HttpModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "NPCActionRegistry.h"

namespace
{
FString NormalizeBaseUrl(const FString& InUrl)
{
	FString Url = InUrl.TrimStartAndEnd();
	while (Url.Len() > 0 && Url.EndsWith(TEXT("/")))
	{
		Url.LeftChopInline(1, EAllowShrinking::No);
	}
	return Url;
}

FString BuildChatCompletionsUrl(const FString& BaseUrl)
{
	return NormalizeBaseUrl(BaseUrl) + TEXT("/chat/completions");
}

/** Environment variables checked, in order, for the LLM API key.
 *  SMARTNPC_LLM_API_KEY is provider-neutral and wins. DEEPSEEK_API_KEY is the name the
 *  DeepSeek tooling uses. DASHSCOPE_API_KEY stays last only so an existing Qwen setup still
 *  resolves — against DeepSeek a Qwen key yields HTTP 401, which is a visible, safe failure. */
const TCHAR* GAPIKeyEnvVars[] = { TEXT("SMARTNPC_LLM_API_KEY"), TEXT("DEEPSEEK_API_KEY"), TEXT("DASHSCOPE_API_KEY") };

/** Gitignored local file that may carry the key on a dev machine: <Project>/Config/LocalSecrets.ini */
const TCHAR* GLocalSecretsSection = TEXT("SmartNPC");
const TCHAR* GLocalSecretsKeyName = TEXT("LLM_API_Key");

FString GetLocalSecretsFilePath()
{
	return FPaths::ProjectConfigDir() / TEXT("LocalSecrets.ini");
}

/** 拼 system prompt：角色设定 + 由工具注册表生成的能力清单 + 输出协议 + few-shot。
 *  每次请求现拼，所以改 NPC_Persona 立刻生效；工具清单由 NPCActionRegistry 生成。 */
FString BuildSystemPrompt(const FString& Persona)
{
	return FString::Printf(
		TEXT("%s\n\n")
		TEXT("你可以通过工具行动。每轮只输出一个 JSON 对象，二选一：\n")
		TEXT("1) {\"type\":\"tool_call\",\"tool\":\"<工具 id>\",\"args\":{...}}\n")
		TEXT("2) {\"type\":\"final_answer\",\"text\":\"你的台词\"}\n\n")
		TEXT("可用工具：\n")
		TEXT("%s\n")
		TEXT("规则：\n")
		TEXT("1. 只输出一个 JSON 对象，不要 Markdown 代码块，不要任何解释文字。\n")
		TEXT("2. 只能用上面列出的工具 id；args 的字段见工具说明。\n")
		TEXT("3. 以 [观察] 开头的消息是引擎返回的工具结果，不是玩家说的话。收到后必须继续输出一个 JSON。\n")
		TEXT("4. 需要知道周围有什么时先调用 query_world，再从返回的 nearby 里挑目标 —— 只能使用出现过的 id。\n")
		TEXT("5. 工具失败时看 error.hint 调整，不要重复完全相同的调用。\n")
		TEXT("6. 想说话或结束时用 final_answer；它表示本轮结束。\n")
		TEXT("7. 玩家要求你做不到的事（吃饭、睡觉之类），直接用 final_answer 说明做不到，不要调用工具。\n")
		TEXT("8. 即使看不懂玩家的话，也必须输出一个 JSON 对象，不能返回空内容。\n")
		TEXT("9. 玩家让你停下、别打了、住手、等着时，用 idle 工具原地停住。"
			"只有当\"当前状态\"已经是 idle 时，才可以直接用 final_answer 回应；否则必须调工具。\n\n")
		TEXT("示例一（直接回答）：\n")
		TEXT("玩家：你好呀\n")
		TEXT("{\"type\":\"final_answer\",\"text\":\"喵，你好。齿轮今天转得不错。\"}\n\n")
		TEXT("示例二（先观察、再行动、再回答）：\n")
		TEXT("玩家：去那个球那边\n")
		TEXT("{\"type\":\"tool_call\",\"tool\":\"query_world\"}\n")
		TEXT("[观察] {\"tool\":\"query_world\",\"ok\":true,\"status\":\"Succeeded\",\"step\":1,\"elapsed_ms\":9,"
			"\"result\":{\"nearby\":[{\"id\":\"actor:3\",\"name\":\"红球\",\"distance_m\":8.0,\"direction\":\"front\"}]}}\n")
		TEXT("{\"type\":\"tool_call\",\"tool\":\"move_to\",\"args\":{\"target\":\"actor:3\"}}\n")
		TEXT("[观察] {\"tool\":\"move_to\",\"ok\":true,\"status\":\"Succeeded\",\"step\":2,\"elapsed_ms\":4200,"
			"\"result\":{\"target\":\"actor:3\",\"final_distance_m\":1.2}}\n")
		TEXT("{\"type\":\"final_answer\",\"text\":\"喵，到球边上了。\"}\n"),
		*Persona, *BuildActionPromptLines());
}

/** 把模型的一段输出解析成 FNPCDecision（final_answer 或 tool_call）。
 *  只做"结构 + 必填字段"，工具是否存在由行为层校验；失败时 OutError 给出可读原因。 */
bool ParseDecision(const FString& RawContent, FNPCDecision& OutDecision, FString& OutError)
{
	FString Json = RawContent.TrimStartAndEnd();

	// 模型偶尔会套一层 Markdown 围栏，先剥掉。
	if (Json.StartsWith(TEXT("```")))
	{
		int32 FirstNewline = INDEX_NONE;
		if (Json.FindChar(TEXT('\n'), FirstNewline))
		{
			Json = Json.RightChop(FirstNewline + 1);
		}
		int32 FenceEnd = INDEX_NONE;
		if (Json.FindLastChar(TEXT('`'), FenceEnd))
		{
			Json = Json.Left(FenceEnd);
		}
		Json.TrimStartAndEndInline();
	}

	if (Json.IsEmpty())
	{
		OutError = TEXT("empty-content");
		return false;
	}

	TSharedPtr<FJsonObject> Root;

	const auto TryDeserialize = [](const FString& Text, TSharedPtr<FJsonObject>& Out) -> bool
	{
		const TSharedRef<TJsonReader<>> CandidateReader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(CandidateReader, Out) && Out.IsValid();
	};

	if (!TryDeserialize(Json, Root))
	{
		// 关掉 JSON Output 后，模型可能输出"好的，这是结果：{...}"这类包裹文本。
		// 退一步：截取第一个 '{' 到最后一个 '}' 再试一次。
		int32 FirstBrace = INDEX_NONE;
		int32 LastBrace = INDEX_NONE;
		const bool bHasBraces = Json.FindChar(TEXT('{'), FirstBrace)
			&& Json.FindLastChar(TEXT('}'), LastBrace)
			&& LastBrace > FirstBrace;

		if (!bHasBraces || !TryDeserialize(Json.Mid(FirstBrace, LastBrace - FirstBrace + 1), Root))
		{
			OutError = TEXT("not-json");
			return false;
		}
	}

	FString Type;
	Root->TryGetStringField(TEXT("type"), Type);
	Type.TrimStartAndEndInline();

	// ---- final_answer ----
	if (Type.Equals(TEXT("final_answer"), ESearchCase::IgnoreCase))
	{
		FString Text;
		if (!Root->TryGetStringField(TEXT("text"), Text))
		{
			OutError = TEXT("final_answer-missing-text");
			return false;
		}

		OutDecision = FNPCDecision();
		OutDecision.bFinalAnswer = true;
		OutDecision.Speech = Text.TrimStartAndEnd();
		OutError.Reset();
		return true;
	}

	// ---- tool_call ----
	if (Type.Equals(TEXT("tool_call"), ESearchCase::IgnoreCase))
	{
		FString ToolId;
		if (!Root->TryGetStringField(TEXT("tool"), ToolId) || ToolId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("tool_call-missing-tool");
			return false;
		}

		OutDecision = FNPCDecision();
		OutDecision.bFinalAnswer = false;
		OutDecision.ToolId = ToolId.TrimStartAndEnd();

		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("args"), ArgsPtr) && ArgsPtr && (*ArgsPtr).IsValid())
		{
			const TSharedPtr<FJsonObject>& Args = *ArgsPtr;

			FString Target;
			if (Args->TryGetStringField(TEXT("target"), Target))
			{
				OutDecision.Target = Target.TrimStartAndEnd();
			}
		}

		OutError.Reset();
		return true;
	}

	// ---- 兼容旧协议：{"action":"follow","speech":"..."} ----
	FString LegacyAction;
	if (Root->TryGetStringField(TEXT("action"), LegacyAction))
	{
		OutDecision = FNPCDecision();

		if (LegacyAction.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			FString Speech;
			Root->TryGetStringField(TEXT("speech"), Speech);
			OutDecision.bFinalAnswer = true;
			OutDecision.Speech = Speech.TrimStartAndEnd();
		}
		else
		{
			OutDecision.bFinalAnswer = false;
			OutDecision.ToolId = LegacyAction.TrimStartAndEnd();
		}

		OutError.Reset();
		return true;
	}

	OutError = TEXT("missing-type");
	return false;
}
}

UAIBrainComponent::UAIBrainComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UAIBrainComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Red, TEXT("C++ Activating!"));
	}
}

void UAIBrainComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PendingLLMRequest.IsValid())
	{
		PendingLLMRequest->OnProcessRequestComplete().Unbind();
		PendingLLMRequest->CancelRequest();
		PendingLLMRequest.Reset();
	}
	bLLMRequestInFlight = false;

	Super::EndPlay(EndPlayReason);
}

void UAIBrainComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UAIBrainComponent::TestAILogic()
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Cyan, FString::Printf(TEXT("NPC %s is thinking..."), *NPC_Name));
	}
}

void UAIBrainComponent::ClearLLMConversation()
{
	ConversationMessages.Reset();
}

void UAIBrainComponent::ResetToolLoop()
{
	LoopObservations.Reset();
	ToolStepCount = 0;
	bToolLoopActive = false;
	bAwaitingToolResult = false;
	LastToolSignature.Reset();
	ToolStartedAt = 0.0;
}

void UAIBrainComponent::AbortToolLoop(const TCHAR* Reason)
{
	UE_LOG(LogTemp, Warning, TEXT("[LLM] loop aborted: %s (steps=%d)"), Reason, ToolStepCount);

	// 保持历史自洽：本轮没有产生 assistant 轮，就把这次玩家指令弹回。
	if (ConversationMessages.Num() > 0 && ConversationMessages.Last().Role == TEXT("user"))
	{
		ConversationMessages.Pop(EAllowShrinking::No);
	}

	ResetToolLoop();
	BroadcastOnGameThread(false, TEXT("（喵……想太久，先这样吧）"));
}

bool UAIBrainComponent::IsToolLoopActive() const
{
	return bToolLoopActive;
}

void UAIBrainComponent::SetCurrentStateHint(const FString& InState)
{
	CurrentStateHint = InState;
}

void UAIBrainComponent::ReportToolResult(const FToolResult& Result)
{
	if (!bToolLoopActive || !bAwaitingToolResult)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[LLM] tool result ignored (no pending tool)"));
		return;
	}

	bAwaitingToolResult = false;
	PushObservationAndContinue(Result);
}

void UAIBrainComponent::PushObservationAndContinue(const FToolResult& Result)
{
	const double ElapsedMs = (ToolStartedAt > 0.0)
		? (FPlatformTime::Seconds() - ToolStartedAt) * 1000.0
		: 0.0;

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("tool"), Result.ToolId);
	Envelope->SetBoolField(TEXT("ok"), Result.bOk);
	Envelope->SetStringField(TEXT("status"), Result.Status);
	Envelope->SetNumberField(TEXT("step"), ToolStepCount);
	Envelope->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToInt(ElapsedMs));

	if (!Result.bOk)
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"),
			Result.ErrorCode.IsEmpty() ? TEXT("failed") : Result.ErrorCode);
		if (!Result.ErrorHint.IsEmpty())
		{
			Error->SetStringField(TEXT("hint"), Result.ErrorHint);
		}
		Envelope->SetObjectField(TEXT("error"), Error);
	}

	if (!Result.ResultJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> ParsedResult;
		const TSharedRef<TJsonReader<>> ResultReader = TJsonReaderFactory<>::Create(Result.ResultJson);
		if (FJsonSerializer::Deserialize(ResultReader, ParsedResult) && ParsedResult.IsValid())
		{
			Envelope->SetObjectField(TEXT("result"), ParsedResult);
		}
	}

	FString Line;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Line);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	LoopObservations.Add(FLLMChatMessage{TEXT("user"), FString::Printf(TEXT("[观察] %s"), *Line)});
	while (LoopObservations.Num() > 4)
	{
		LoopObservations.RemoveAt(0);
	}

	UE_LOG(LogTemp, Warning, TEXT("[LLM] observation step=%d %s"), ToolStepCount, *Line);

	// 延后一拍再请求：避免在行为层的 Broadcast 调用栈里递归发 HTTP。
	TWeakObjectPtr<UAIBrainComponent> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (!WeakThis.IsValid())
		{
			return;
		}

		UAIBrainComponent* Self = WeakThis.Get();
		if (!Self->bToolLoopActive)
		{
			return;
		}

		FString IssueError;
		if (!Self->IssueRequest(IssueError))
		{
			UE_LOG(LogTemp, Error, TEXT("[LLM] continue failed: %s"), *IssueError);
			Self->AbortToolLoop(TEXT("issue_failed"));
		}
	});
}

void UAIBrainComponent::TrimRecentTurns()
{
	const int32 MaxMessages = MaxTurns * 2;
	if (ConversationMessages.Num() <= MaxMessages)
	{
		return;
	}
	const int32 RemoveCount = ConversationMessages.Num() - MaxMessages;
	ConversationMessages.RemoveAt(0, RemoveCount);
}

void UAIBrainComponent::BroadcastOnGameThread(bool bSuccess, const FString& Message)
{
	TWeakObjectPtr<UAIBrainComponent> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, bSuccess, Message]()
	{
		if (!WeakThis.IsValid())
		{
			return;
		}
		UAIBrainComponent* Self = WeakThis.Get();
		Self->OnLLMChatFinished.Broadcast(bSuccess, Message);
	});
}

FString UAIBrainComponent::GetLLMAPIKey() const
{
	// 1) Environment variable: the recommended source, and the only one that cannot be committed.
	for (const TCHAR* VarName : GAPIKeyEnvVars)
	{
		const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(VarName).TrimStartAndEnd();
		if (!FromEnv.IsEmpty())
		{
			return FromEnv;
		}
	}

	// 2) Config/LocalSecrets.ini — gitignored, so a key kept here never reaches the repository.
	const FString SecretsFile = GetLocalSecretsFilePath();
	if (FPaths::FileExists(SecretsFile))
	{
		// LoadFile returns void (see ConfigCacheIni.h): it merges the file into the config cache.
		GConfig->LoadFile(SecretsFile);
		FString FromIni;
		if (GConfig->GetString(GLocalSecretsSection, GLocalSecretsKeyName, FromIni, SecretsFile))
		{
			FromIni.TrimStartAndEndInline();
			if (!FromIni.IsEmpty())
			{
				return FromIni;
			}
		}
	}

	// 3) Transient runtime override (SetLLMAPIKey, or the legacy Blueprint pin).
	return LLM_API_Key.TrimStartAndEnd();
}

void UAIBrainComponent::SetLLMAPIKey(const FString& InKey)
{
	LLM_API_Key = InKey.TrimStartAndEnd();
}

bool UAIBrainComponent::HasLLMAPIKey() const
{
	return !GetLLMAPIKey().IsEmpty();
}

void UAIBrainComponent::SendRequestToLLM(const FString& UserMessage)
{
	const FString Trimmed = UserMessage.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		BroadcastOnGameThread(false, TEXT("Input cannot be empty."));
		return;
	}

	const FString APIKey = GetLLMAPIKey();
	if (APIKey.IsEmpty())
	{
		BroadcastOnGameThread(false, TEXT("No LLM API key. Set the SMARTNPC_LLM_API_KEY environment variable, fill Config/LocalSecrets.ini, or call SetLLMAPIKey()."));
		return;
	}

	if (bLLMRequestInFlight)
	{
		BroadcastOnGameThread(false, TEXT("A request is already in progress."));
		return;
	}

	ResetToolLoop();
	bToolLoopActive = true;

	ConversationMessages.Add(FLLMChatMessage{TEXT("user"), Trimmed});
	RetryAttempts = 0;

	FString IssueError;
	if (!IssueRequest(IssueError))
	{
		ConversationMessages.Pop(EAllowShrinking::No);
		BroadcastOnGameThread(false, IssueError);
	}
}

bool UAIBrainComponent::IssueRequest(FString& OutError, bool bIsRetry)
{
	const FString APIKey = GetLLMAPIKey();
	if (APIKey.IsEmpty())
	{
		OutError = TEXT("No LLM API key. Set the SMARTNPC_LLM_API_KEY environment variable, fill Config/LocalSecrets.ini, or call SetLLMAPIKey().");
		return false;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), LLM_Model);
	Root->SetBoolField(TEXT("stream"), false);

	// JSON Output 只保证"语法合法"，语义仍由 ParseDecision 校验；可用开关做 A/B。
	if (bUseJSONResponseFormat)
	{
		TSharedPtr<FJsonObject> ResponseFormat = MakeShared<FJsonObject>();
		ResponseFormat->SetStringField(TEXT("type"), TEXT("json_object"));
		Root->SetObjectField(TEXT("response_format"), ResponseFormat);
	}
	Root->SetNumberField(TEXT("max_tokens"), MaxOutputTokens);

	// NPC 对话不需要思考过程：关掉更快也更省，同时是以后升级到 strict tool calls 的前提。
	{
		TSharedPtr<FJsonObject> Thinking = MakeShared<FJsonObject>();
		Thinking->SetStringField(TEXT("type"), TEXT("disabled"));
		Root->SetObjectField(TEXT("thinking"), Thinking);
	}

	TArray<TSharedPtr<FJsonValue>> MsgArray;
	MsgArray.Reserve(ConversationMessages.Num() + 1);

	// system 只进请求体，不写入 ConversationMessages —— 历史里只留真正的对话。
	{
		FString SystemPrompt = BuildSystemPrompt(NPC_Persona);

		// 当前状态：模型必须知道"我现在在做什么"，否则会出现"嘴上说停了、实际还在巡逻"。
		if (!CurrentStateHint.IsEmpty())
		{
			SystemPrompt += FString::Printf(
				TEXT("\n当前状态：%s（玩家要求停止时，如果这里不是 idle，你必须调用 idle 工具）\n"),
				*CurrentStateHint);
		}

		if (bIsRetry)
		{
			// 让重试成为"不同的请求"：原样重发等于重复同一个失败。
			SystemPrompt += TEXT("\n注意：上一次你返回了空内容。这次必须只输出一个 JSON 对象，不能返回空内容。\n");
		}

		TSharedPtr<FJsonObject> SystemMsg = MakeShared<FJsonObject>();
		SystemMsg->SetStringField(TEXT("role"), TEXT("system"));
		SystemMsg->SetStringField(TEXT("content"), SystemPrompt);
		MsgArray.Add(MakeShared<FJsonValueObject>(SystemMsg));
	}

	for (const FLLMChatMessage& M : ConversationMessages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("role"), M.Role);
		MsgObj->SetStringField(TEXT("content"), M.Content);
		MsgArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	// 本轮的 [观察] 追加在最后（不写入历史），利于 prompt 前缀缓存命中。
	for (const FLLMChatMessage& M : LoopObservations)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("role"), M.Role);
		MsgObj->SetStringField(TEXT("content"), M.Content);
		MsgArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	Root->SetArrayField(TEXT("messages"), MsgArray);

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		OutError = TEXT("Failed to serialize request JSON.");
		return false;
	}

	FHttpModule& HttpModule = FHttpModule::Get();
	if (!HttpModule.IsHttpEnabled())
	{
		OutError = TEXT("HTTP is disabled in project settings.");
		return false;
	}

	TSharedRef<IHttpRequest> Req = HttpModule.CreateRequest();
	PendingLLMRequest = Req;
	Req->SetVerb(TEXT("POST"));
	Req->SetURL(BuildChatCompletionsUrl(LLM_BaseUrl));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *APIKey));
	Req->SetContentAsString(Body);
	Req->OnProcessRequestComplete().BindUObject(this, &UAIBrainComponent::OnLLMHttpComplete);

	bLLMRequestInFlight = true;
	Req->ProcessRequest();
	return true;
}

void UAIBrainComponent::OnLLMHttpComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
	PendingLLMRequest.Reset();
	bLLMRequestInFlight = false;

	auto PopLastUserIfNeeded = [this]()
	{
		if (ConversationMessages.Num() > 0 && ConversationMessages.Last().Role == TEXT("user"))
		{
			ConversationMessages.Pop(EAllowShrinking::No);
		}
	};

	if (!bConnectedSuccessfully || !Response.IsValid())
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Network error or invalid response."));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	const FString Raw = Response->GetContentAsString();

	if (Code < 200 || Code >= 300)
	{
		PopLastUserIfNeeded();

		FString ErrMsg = FString::Printf(TEXT("HTTP %d"), Code);
		TSharedPtr<FJsonObject> JsonParsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (FJsonSerializer::Deserialize(Reader, JsonParsed) && JsonParsed.IsValid())
		{
			const TSharedPtr<FJsonObject>* ErrObj = nullptr;
			if (JsonParsed->TryGetObjectField(TEXT("error"), ErrObj) && ErrObj && (*ErrObj).IsValid())
			{
				FString ApiMsg;
				if ((*ErrObj)->TryGetStringField(TEXT("message"), ApiMsg))
				{
					ErrMsg = ApiMsg;
				}
			}
		}
		else if (Raw.Len() > 0 && Raw.Len() < 512)
		{
			ErrMsg = Raw;
		}

		BroadcastOnGameThread(false, ErrMsg);
		return;
	}

	TSharedPtr<FJsonObject> JsonParsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, JsonParsed) || !JsonParsed.IsValid())
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Failed to parse response JSON."));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!JsonParsed->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->Num() == 0)
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Response has no choices."));
		return;
	}

	const TSharedPtr<FJsonObject> Choice0 = (*Choices)[0]->AsObject();
	if (!Choice0.IsValid())
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("choices[0] is invalid."));
		return;
	}

	const TSharedPtr<FJsonObject>* MessagePtr = nullptr;
	if (!Choice0->TryGetObjectField(TEXT("message"), MessagePtr) || !MessagePtr || !(*MessagePtr).IsValid())
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Response has no message."));
		return;
	}

	FString FinishReason;
	Choice0->TryGetStringField(TEXT("finish_reason"), FinishReason);

	FString Content;
	if (!(*MessagePtr)->TryGetStringField(TEXT("content"), Content))
	{
		HandleDecisionFailure(TEXT("missing-content"), Raw, FinishReason);
		return;
	}

	Content.TrimStartAndEndInline();
	if (Content.IsEmpty())
	{
		HandleDecisionFailure(TEXT("empty-content"), FString(), FinishReason);
		return;
	}

	FNPCDecision Decision;
	FString ParseError;
	if (!ParseDecision(Content, Decision, ParseError))
	{
		HandleDecisionFailure(ParseError, Content, FinishReason);
		return;
	}

	++StatOk;
	RetryAttempts = 0;

	// ---- final_answer：本轮结束 ----
	if (Decision.bFinalAnswer)
	{
		UE_LOG(LogTemp, Warning, TEXT("[LLM] final_answer steps=%d speech=\"%s\" (finish=%s) ok=%d retry=%d fail=%d"),
			ToolStepCount, *Decision.Speech, *FinishReason, StatOk, StatRetry, StatFail);

		ConversationMessages.Add(FLLMChatMessage{TEXT("assistant"), Content});
		TrimRecentTurns();
		ResetToolLoop();

		BroadcastOnGameThread(true, Decision.Speech);
		return;
	}

	// ---- tool_call：占一步，交给行为层执行 ----
	++ToolStepCount;

	UE_LOG(LogTemp, Warning, TEXT("[LLM] tool_call step=%d tool=%s target=\"%s\""),
		ToolStepCount, *Decision.ToolId, *Decision.Target);

	if (ToolStepCount > MaxToolSteps)
	{
		AbortToolLoop(TEXT("step_limit"));
		return;
	}

	// 原地打转检测：同一工具 + 同一参数连续两次 -> 直接中止。
	const FString Signature = FString::Printf(TEXT("%s|%s"), *Decision.ToolId, *Decision.Target);
	if (!LastToolSignature.IsEmpty() && Signature == LastToolSignature)
	{
		AbortToolLoop(TEXT("repeated_call"));
		return;
	}
	LastToolSignature = Signature;

	// 中间轮的工具调用 JSON 不写进历史：保持"一次玩家指令 = 一对 user/assistant"。
	ToolStartedAt = FPlatformTime::Seconds();
	bAwaitingToolResult = true;

	OnNPCDecision.Broadcast(Decision);
}

void UAIBrainComponent::HandleDecisionFailure(const FString& Reason, const FString& RawContent, const FString& FinishReason)
{
	FString FinalReason = Reason;
	const FString RawSnippet = RawContent.Len() > 200 ? RawContent.Left(200) : RawContent;

	// 先重试：JSON Output 偶发返回空内容是服务端的已知问题，重发一次通常就好。
	if (RetryAttempts < MaxRetries)
	{
		++RetryAttempts;
		++StatRetry;

		UE_LOG(LogTemp, Warning, TEXT("[LLM] decision retry reason=%s (finish=%s) %d/%d"),
			*FinalReason, *FinishReason, RetryAttempts, MaxRetries);

		FString IssueError;
		if (IssueRequest(IssueError, /*bIsRetry=*/true))
		{
			return;     // 重发成功，结果走下一次回调
		}

		FinalReason = FString::Printf(TEXT("%s; resend failed: %s"), *FinalReason, *IssueError);
	}

	// 重试用尽：弹回 user 轮（保持上下文自洽），计数，广播一条可读的失败信息。
	++StatFail;
	if (ConversationMessages.Num() > 0 && ConversationMessages.Last().Role == TEXT("user"))
	{
		ConversationMessages.Pop(EAllowShrinking::No);
	}

	UE_LOG(LogTemp, Error, TEXT("[LLM] decision FAIL  reason=%s (finish=%s) raw=\"%s\" ok=%d retry=%d fail=%d"),
		*FinalReason, *FinishReason, *RawSnippet, StatOk, StatRetry, StatFail);

	BroadcastOnGameThread(false, FString::Printf(TEXT("（走神了：%s）"), *FinalReason));
}
