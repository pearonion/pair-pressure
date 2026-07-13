#pragma once

#include "CoreMinimal.h"
#include "PPGameplayTypes.generated.h"

UENUM(BlueprintType)
enum class EPPGameMode : uint8
{
	BringYourIdiotHome,
	AttachedAtTheHip
};

UENUM(BlueprintType)
enum class EPPPhysicalState : uint8
{
	Grounded,
	Reactive,
	Stumbling,
	Falling,
	Ragdolled,
	Unconscious,
	Piggybacked
};

USTRUCT(BlueprintType)
struct FPPImpactData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	float Severity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	FVector_NetQuantize ImpactPoint = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	FVector_NetQuantizeNormal ImpactDirection = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	FName BodyRegion = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	TObjectPtr<AActor> InstigatorActor = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Impact")
	bool bHeavyObstacle = false;
};

USTRUCT(BlueprintType)
struct FPPHUDSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	FText PartnerName;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	FText PartnerState;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	float PartnerDirectionDegrees = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	float DazeNormalized = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	bool bLocalPlayerHome = false;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	bool bPartnerHome = false;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	bool bTeamFinished = false;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	FText HeldItemLabel;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|UI")
	FText InteractionPrompt;
};
