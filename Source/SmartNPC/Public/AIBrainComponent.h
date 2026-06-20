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

	/** DashScope OpenAI-compatible base URL (no trailing /chat/completions). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString LLM_BaseUrl = TEXT("https://dashscope.aliyuncs.com/compatible-mode/v1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString LLM_API_Key;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM")
	FString LLM_Model = TEXT("qwen-plus");

	/** Max completed user+assistant turns kept after each reply (matches Python MAX_TURNS). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AI_System|LLM", meta = (ClampMin = "1"))
	int32 MaxTurns = 8;

	UPROPERTY(BlueprintAssignable, Category = "AI_System|LLM")
	FOnLLMChatFinished OnLLMChatFinished;

	UFUNCTION(BlueprintCallable, Category = "AI_System")
	void TestAILogic();

	/** Appends user message, POSTs to Qwen compatible chat/completions; on failure pops user. */
	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void SendRequestToLLM(const FString& UserMessage);

	UFUNCTION(BlueprintCallable, Category = "AI_System|LLM")
	void ClearLLMConversation();

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
