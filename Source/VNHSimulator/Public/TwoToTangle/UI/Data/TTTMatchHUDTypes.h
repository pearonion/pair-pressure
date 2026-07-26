#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "GameplayTagContainer.h"
#include "TTTMatchHUDTypes.generated.h"

UENUM(BlueprintType)
enum class ETTTRacePhase : uint8
{
	Waiting,
	Countdown,
	Racing,
	LocalPlayerFinished,
	TeamFinished,
	Eliminated,
	ResultsTransition
};
UENUM(BlueprintType)
enum class ETTTPhysicalState : uint8
{
	Upright,
	Stumbling,
	Ragdolled,
	FullyDazed,
	BeingCarried,
	Recovering,
	Unconscious
};

UENUM(BlueprintType)
enum class ETTTHUDMessageType : uint8
{
	None,
	GetReady,
	Go,
	DazedNeedsTeammate,
	BeingCarried,
	Recovered,
	CourseCleared,
	Eliminated
};

UENUM(BlueprintType)
enum class ETTTFinishResult : uint8
{
	None,
	First,
	Second,
	Third,
	Finished,
	DidNotFinish
};

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTHeldItemPresentation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bHasItem = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") FText DisplayName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") TSoftObjectPtr<UTexture2D> Icon;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bCanThrow = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bSupportsCharge = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") FLinearColor AccentColor = FLinearColor(1.0f, 0.45f, 0.08f, 1.0f);
};

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTFinishPresentation
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") ETTTFinishResult Result = ETTTFinishResult::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") int32 Placement = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") int32 TeamCount = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float FinalTeamTime = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bWaitingForResults = false;
};

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTDazeEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") float Amount = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") FGameplayTag SourceTag;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") TWeakObjectPtr<AActor> SourceActor;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") TWeakObjectPtr<AActor> InstigatorActor;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") FVector ImpactLocation = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|Daze") float ImpactSeverity = 0.0f;
};

USTRUCT(BlueprintType)
struct VNHSIMULATOR_API FTTTMatchHUDSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") ETTTRacePhase RacePhase = ETTTRacePhase::Waiting;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float RaceElapsedSeconds = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") int32 Placement = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") int32 TeamCount = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bHasHeldItem = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") TSoftObjectPtr<UTexture2D> HeldItemIcon;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") FText HeldItemName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bIsChargingThrow = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float ThrowChargeNormalized = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float DazeNormalized = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float DazeRecoveryThresholdNormalized = 0.3f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") ETTTPhysicalState PhysicalState = ETTTPhysicalState::Upright;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bCanMove = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bCanDismount = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") ETTTHUDMessageType ActiveMessage = ETTTHUDMessageType::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") ETTTFinishResult FinishResult = ETTTFinishResult::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") float FinalTeamTime = 0.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") bool bWaitingForResults = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") FLinearColor TeamColor = FLinearColor::White;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Two to Tangle|HUD") int32 TeamId = INDEX_NONE;
};
