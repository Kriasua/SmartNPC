// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SteampunkCatNPC.generated.h"

UCLASS()
class SMARTNPC_API ASteampunkCatNPC : public ACharacter
{
	GENERATED_BODY()

public:
	// Sets default values for this character's properties
	ASteampunkCatNPC();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	TArray<AActor*> StillPatrolPoint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	int32 curPointIdx = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Patrol")
	bool reverse = false;

	// 激光从哪个骨骼插槽射出(在 Mesh 上建一个 Socket,比如猫眼/炮口)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	FName MuzzleSocketName = TEXT("Muzzle");

	// 激光最大射程
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	float LaserRange = 2000.f;

	// 命中一次造成的伤害
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack|Laser")
	float LaserDamage = 20.f;

	// 命中检测(给 notify 调用)。
	UFUNCTION(BlueprintCallable, Category = "Attack|Laser")
	void FireLaser();


};
