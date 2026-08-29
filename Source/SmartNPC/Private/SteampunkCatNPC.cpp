// Fill out your copyright notice in the Description page of Project Settings.


#include "SteampunkCatNPC.h"
#include "Components/SkeletalMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"

// Sets default values
ASteampunkCatNPC::ASteampunkCatNPC()
{
 	// Set this character to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
	StillPatrolPoint.Reserve(3);
}

// Called when the game starts or when spawned
void ASteampunkCatNPC::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void ASteampunkCatNPC::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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

