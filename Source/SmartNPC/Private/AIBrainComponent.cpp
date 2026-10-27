
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

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), LLM_Model);
	Root->SetBoolField(TEXT("stream"), false);

	TArray<TSharedPtr<FJsonValue>> MsgArray;
	MsgArray.Reserve(ConversationMessages.Num());
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
		ConversationMessages.Pop(EAllowShrinking::No);
		BroadcastOnGameThread(false, TEXT("Failed to serialize request JSON."));
		return;
	}

	FHttpModule& HttpModule = FHttpModule::Get();
	if (!HttpModule.IsHttpEnabled())
	{
		ConversationMessages.Pop(EAllowShrinking::No);
		BroadcastOnGameThread(false, TEXT("HTTP is disabled in project settings."));
		return;
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

	FString Content;
	if (!(*MessagePtr)->TryGetStringField(TEXT("content"), Content))
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Response has no content."));
		return;
	}

	Content.TrimStartAndEndInline();
	if (Content.IsEmpty())
	{
		PopLastUserIfNeeded();
		BroadcastOnGameThread(false, TEXT("Model returned empty text."));
		return;
	}

	ConversationMessages.Add(FLLMChatMessage{TEXT("assistant"), Content});
	TrimRecentTurns();

	BroadcastOnGameThread(true, Content);
}
