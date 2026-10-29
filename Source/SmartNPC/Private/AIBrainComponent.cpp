
#include "AIBrainComponent.h"
#include "HttpModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

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

/** 一个可执行动作的元数据：引擎枚举 + 给模型看的 id + 给模型看的用法说明。
 *  这张表是**唯一事实来源**：system prompt 由它生成，白名单校验也查它。加动作 = 改这里。 */
struct FNPCActionSpec
{
	ENPCAction Action;
	const TCHAR* Id;
	const TCHAR* PromptHint;
};

const TArray<FNPCActionSpec>& GetActionSpecs()
{
	static const TArray<FNPCActionSpec> Specs =
	{
		{ ENPCAction::None,   TEXT("none"),    TEXT("不改变行为。闲聊、问好、或你做不到的请求都用这个，并在台词里说明。") },
		{ ENPCAction::MoveTo, TEXT("move_to"), TEXT("走向玩家。玩家让你靠近、过来时使用。") },
		{ ENPCAction::Patrol, TEXT("patrol"),  TEXT("恢复自主巡逻。玩家让你自己逛、别跟着时使用。") },
	};
	return Specs;
}

/** 把模型返回的字符串 id 解析成引擎枚举。大小写不敏感；不在表里就返回 false。 */
bool TryParseAction(const FString& Id, ENPCAction& OutAction)
{
	const FString Trimmed = Id.TrimStartAndEnd();
	for (const FNPCActionSpec& Spec : GetActionSpecs())
	{
		if (Trimmed.Equals(Spec.Id, ESearchCase::IgnoreCase))
		{
			OutAction = Spec.Action;
			return true;
		}
	}
	return false;
}

/** 枚举 -> 字符串 id，只用于日志。 */
FString ActionToId(ENPCAction Action)
{
	for (const FNPCActionSpec& Spec : GetActionSpecs())
	{
		if (Spec.Action == Action)
		{
			return Spec.Id;
		}
	}
	return TEXT("?");
}

/** 拼 system prompt：角色设定 + 由动作表生成的能力清单 + 输出契约 + few-shot。
 *  每次请求现拼，所以改 NPC_Persona 立刻生效，不用重编译。 */
FString BuildSystemPrompt(const FString& Persona)
{
	FString ActionLines;
	for (const FNPCActionSpec& Spec : GetActionSpecs())
	{
		ActionLines += FString::Printf(TEXT("- %s：%s\n"), Spec.Id, Spec.PromptHint);
	}

	return FString::Printf(
		TEXT("%s\n\n")
		TEXT("你能做的行为只有下面三种，没有别的：\n")
		TEXT("%s\n")
		TEXT("规则：\n")
		TEXT("1. 只输出一个 JSON 对象，不要 Markdown 代码块，不要任何解释文字。\n")
		TEXT("2. 格式：{\"action\":\"none|move_to|patrol\",\"speech\":\"你的台词\"}\n")
		TEXT("3. action 必须是上面三个值之一；speech 是字符串，可以为空。\n")
		TEXT("4. 玩家要求你做不存在的事（吃饭、睡觉之类），用 none，并在 speech 里说明做不到。\n")
		TEXT("5. 无论玩家说什么（闲聊、抱怨、无关话题、看不懂），都必须输出一个 JSON 对象，不能返回空内容。\n\n")
		TEXT("示例：\n")
		TEXT("玩家：你好呀\n{\"action\":\"none\",\"speech\":\"喵，你好。齿轮今天转得不错。\"}\n")
		TEXT("玩家：过来\n{\"action\":\"move_to\",\"speech\":\"来了。\"}\n")
		TEXT("玩家：去吃饭\n{\"action\":\"none\",\"speech\":\"我是机械猫，不吃东西。\"}\n"),
		*Persona, *ActionLines);
}

/** 把模型的一段输出解析成 FNPCDecision。
 *  只做"结构 + 白名单"，不做行为映射（那是 M1-4）；失败时 OutError 给出可读原因。 */
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

	FString ActionId;
	if (!Root->TryGetStringField(TEXT("action"), ActionId))
	{
		OutError = TEXT("missing-action");
		return false;
	}

	ENPCAction ParsedAction = ENPCAction::None;
	if (!TryParseAction(ActionId, ParsedAction))
	{
		OutError = FString::Printf(TEXT("unknown-action: %s"), *ActionId);
		return false;
	}

	FString Speech;
	Root->TryGetStringField(TEXT("speech"), Speech);

	OutDecision.Action = ParsedAction;
	OutDecision.Speech = Speech.TrimStartAndEnd();
	OutError.Reset();
	return true;
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

	UE_LOG(LogTemp, Warning, TEXT("[LLM] decision ok    action=%s speech=\"%s\" (finish=%s) ok=%d retry=%d fail=%d"),
		*ActionToId(Decision.Action), *Decision.Speech, *FinishReason, StatOk, StatRetry, StatFail);

	// 历史里存**原始 JSON**，不存台词。实测（Tools/deepseek_probe.py，四组各 20 条）：
	//   assistant 存台词  -> 17/20 只返回空白字符（JSON 模式卡死）
	//   assistant 存 JSON -> 0/20 异常
	// 原因：system 要求"只输出 JSON"，历史里却是纯文本，冲突会让模型退化。
	// 屏幕上显示的仍然是 Decision.Speech，两者分离。
	ConversationMessages.Add(FLLMChatMessage{TEXT("assistant"), Content});
	TrimRecentTurns();

	BroadcastOnGameThread(true, Decision.Speech);
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
