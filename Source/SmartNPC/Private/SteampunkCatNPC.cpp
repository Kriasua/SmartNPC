// Fill out your copyright notice in the Description page of Project Settings.


#include "SteampunkCatNPC.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "TimerManager.h"
#include "AIBrainComponent.h"

// Sets default values
ASteampunkCatNPC::ASteampunkCatNPC()
{
 	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	StillPatrolPoint.Reserve(3);

	// M1-6：台词气泡。WidgetClass 由蓝图（SpeechBubbleClass）在 BeginPlay 里指定，
	// 这样 C++ 不依赖具体资产，换样式只改蓝图。
	SpeechBubbleWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("SpeechBubbleWidget"));
	SpeechBubbleWidget->SetupAttachment(RootComponent);
	SpeechBubbleWidget->SetWidgetSpace(EWidgetSpace::World);
	SpeechBubbleWidget->SetPivot(FVector2D(0.5f, 1.0f));   // 底边中点 → 气泡"站在"头顶
	SpeechBubbleWidget->SetDrawSize(SpeechBubbleDrawSize);
	SpeechBubbleWidget->SetRelativeLocation(SpeechBubbleOffset);
	SpeechBubbleWidget->SetRelativeScale3D(FVector(SpeechBubbleWorldScale));
	SpeechBubbleWidget->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SpeechBubbleWidget->SetGenerateOverlapEvents(false);
	SpeechBubbleWidget->SetTwoSided(true);
	SpeechBubbleWidget->SetVisibility(false);
}

// Called when the game starts or when spawned
void ASteampunkCatNPC::BeginPlay()
{
	Super::BeginPlay();

	if (SpeechBubbleWidget)
	{
		// 允许在蓝图里调，所以下面几项都在运行期再应用一次。
		SpeechBubbleWidget->SetRelativeLocation(SpeechBubbleOffset);
		SpeechBubbleWidget->SetRelativeScale3D(FVector(SpeechBubbleWorldScale));
		SpeechBubbleWidget->SetDrawSize(SpeechBubbleDrawSize);
		SpeechBubbleWidget->SetVisibility(false);

		if (SpeechBubbleClass)
		{
			SpeechBubbleWidget->SetWidgetClass(SpeechBubbleClass);
			if (!SpeechBubbleWidget->GetUserWidgetObject())
			{
				// 运行时换类时，WidgetComponent 不一定会自动初始化。
				SpeechBubbleWidget->InitWidget();
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Speech] %s: SpeechBubbleClass 未设置，台词只会进日志（设为 WBP_BubbleSpeech）"),
				*GetName());
		}
	}

	// 台词由组件的 OnLLMChatFinished 广播过来；控件在 Pawn 身上，所以由 Pawn 订阅。
	if (UAIBrainComponent* Brain = FindComponentByClass<UAIBrainComponent>())
	{
		Brain->OnLLMChatFinished.AddDynamic(this, &ASteampunkCatNPC::HandleSpeech);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Speech] %s: 没有 UAIBrainComponent，气泡不会工作"), *GetName());
	}
}

void ASteampunkCatNPC::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(SpeechHideTimer);

	if (SpeechBubbleWidget)
	{
		SpeechBubbleWidget->SetVisibility(false);
	}

	Super::EndPlay(EndPlayReason);
}

// Called every frame
void ASteampunkCatNPC::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 世界空间的 widget 默认跟着 Actor 朝向；让气泡始终面向摄像机（只绕 Yaw，避免歪头）。
	if (SpeechBubbleWidget && SpeechBubbleWidget->IsVisible())
	{
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC)
		{
			FVector CameraLocation;
			FRotator CameraRotation;
			PC->GetPlayerViewPoint(CameraLocation, CameraRotation);

			const FRotator Facing =
				(CameraLocation - SpeechBubbleWidget->GetComponentLocation()).Rotation();
			SpeechBubbleWidget->SetWorldRotation(FRotator(0.f, Facing.Yaw, 0.f));
		}
	}
}

void ASteampunkCatNPC::HandleSpeech(bool bSuccess, FString Message)
{
	if (!SpeechBubbleWidget)
	{
		return;
	}

	UUserWidget* UserWidget = SpeechBubbleWidget->GetUserWidgetObject();
	if (!UserWidget || !UserWidget->WidgetTree)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Speech] %s: widget 未创建，检查 SpeechBubbleClass"), *GetName());
		return;
	}

	// 按名字找控件，只找一次（控件不会重建）。
	if (!SpeechText.IsValid())
	{
		SpeechText = Cast<UTextBlock>(UserWidget->WidgetTree->FindWidget(TEXT("SpeechText")));
		if (!SpeechText.IsValid())
		{
			UE_LOG(LogTemp, Error,
				TEXT("[Speech] %s: widget 里没有名为 'SpeechText' 的 TextBlock"), *GetName());
			return;
		}
	}

	// 空台词（比如只执行、不说话）就不显示。
	if (Message.IsEmpty())
	{
		HideSpeech();
		return;
	}

	FString Shown = Message;
	if (SpeechMaxChars > 0 && Shown.Len() > SpeechMaxChars)
	{
		Shown = Shown.Left(SpeechMaxChars) + TEXT("…");
	}

	SpeechText->SetText(FText::FromString(Shown));
	SpeechBubbleWidget->SetVisibility(true);

	// 每条新台词都重新计时。
	GetWorldTimerManager().SetTimer(
		SpeechHideTimer, this, &ASteampunkCatNPC::HideSpeech, SpeechSeconds, false);

	UE_LOG(LogTemp, Verbose, TEXT("[Speech] %s: %s"), *GetName(), *Shown);
}

void ASteampunkCatNPC::HideSpeech()
{
	if (SpeechBubbleWidget)
	{
		SpeechBubbleWidget->SetVisibility(false);
	}
}

// Called to bind functionality to input
void ASteampunkCatNPC::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

}

void ASteampunkCatNPC::FireLaser()
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp) return;


	const FVector Start = MeshComp->GetSocketLocation(MuzzleSocketName);
	const FVector End = Start + (GetActorForwardVector() * LaserRange);
	
	FHitResult Hit;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);
	const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
	
	const FVector DrawEnd = bHit ? Hit.ImpactPoint : End;
	DrawDebugLine(GetWorld(), Start, DrawEnd, bHit ? FColor::Red : FColor::Green,
			false, 1.0f, 0, 1.5f);

	
	if (bHit && Hit.GetActor() && Hit.GetActor()->ActorHasTag(FName("Player")))
	{
		UGameplayStatics::ApplyPointDamage(
				Hit.GetActor(), LaserDamage, (End - Start).GetSafeNormal(),
				Hit, GetController(), this, UDamageType::StaticClass());
	}
}
