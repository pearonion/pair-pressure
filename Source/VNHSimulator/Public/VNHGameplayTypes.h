#pragma once

#include "CoreMinimal.h"
#include "VNHGameplayTypes.generated.h"

class APlayerState;

UENUM(BlueprintType)
enum class EVNHRoundPhase : uint8
{
	WaitingForPlayers,
	AssigningRoles,
	AlienSetup,
	AlienHeadStart,
	Investigation,
	Accusation,
	Reveal,
	Resetting
};

UENUM(BlueprintType)
enum class EVNHPlayerRole : uint8
{
	Unassigned,
	Human,
	Alien,
	Hunter
};

UENUM(BlueprintType)
enum class EVNHPublicTestType : uint8
{
	Freeze,
	LookToEntrance,
	ClearAisle,
	CheckoutOpen
};

UENUM(BlueprintType)
enum class EVNHQuickChatLine : uint8
{
	JustBrowsing,
	LookingForShirt,
	WaitingForFriend,
	NoThanks,
	FoundWrongSize
};

USTRUCT(BlueprintType)
struct FVNHPhaseTiming
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float AlienSetupSeconds = 7.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float AlienHeadStartSeconds = 8.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float InvestigationSeconds = 120.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float AccusationSeconds = 10.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float RevealSeconds = 10.0f;
};

USTRUCT(BlueprintType)
struct FVNHAccusationResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Round")
	TObjectPtr<AActor> AccusedActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Round")
	bool bCorrect = false;

	UPROPERTY(BlueprintReadOnly, Category = "Round")
	bool bResolved = false;
};

USTRUCT(BlueprintType)
struct FVNHQuickChatMessage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Quick Chat")
	TObjectPtr<APlayerState> Speaker = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Quick Chat")
	EVNHQuickChatLine Line = EVNHQuickChatLine::JustBrowsing;

	UPROPERTY(BlueprintReadOnly, Category = "Quick Chat")
	FText Text;

	UPROPERTY(BlueprintReadOnly, Category = "Quick Chat")
	int32 Serial = 0;
};
