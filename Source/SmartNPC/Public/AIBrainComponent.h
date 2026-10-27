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

	TArray<FLLMChatMessage> ConversationMessages;
	TSharedPtr<IHttpRequest> PendingLLMRequest;
	bool bLLMRequestInFlight = false;
};
