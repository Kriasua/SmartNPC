
#include "AIBrainComponent.h"
#include "HttpModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Async/Async.h"

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

void UAIBrainComponent::SendRequestToLLM(const FString& UserMessage)
{
	const FString Trimmed = UserMessage.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		BroadcastOnGameThread(false, TEXT("Input cannot be empty."));
		return;
	}

	if (LLM_API_Key.TrimStartAndEnd().IsEmpty())
	{
		BroadcastOnGameThread(false, TEXT("LLM_API_Key is not set."));
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
	Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *LLM_API_Key.TrimStartAndEnd()));
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
