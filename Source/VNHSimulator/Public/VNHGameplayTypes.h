#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
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

UENUM(BlueprintType)
enum class EVNHComposureState : uint8
{
	Calm,
	Stable,
	Nervous,
	Cracking,
	Panic
};

UENUM(BlueprintType)
enum class EVNHUniversalAction : uint8
{
	None,
	Inspect,
	Point,
	Wave,
	Laugh,
	Fart,
	PlaceDecoy,
	PickUp,
	Drop
};

UENUM(BlueprintType)
enum class EVNHHumanDrillAction : uint8
{
	None,
	Wave,
	Point,
	Laugh,
	Jump,
	Crouch,
	PickUpNearestItem
};

UENUM(BlueprintType)
enum class EVNHHunterCommandPromptType : uint8
{
	None,
	HumanDrill,
	FakeDrill,
	EveryonePoint
};

UENUM(BlueprintType)
enum class EVNHCustomizationSlot : uint8
{
	Body,
	Hair,
	Face,
	Hat,
	Mustache,
	Outfit,
	Outwear,
	Pants,
	Shoes,
	Accessory
};

UENUM(BlueprintType)
enum class EVNHPropType : uint8
{
	Box,
	Bag,
	Cup,
	Tool,
	SuspiciousObject
};

USTRUCT(BlueprintType)
struct FVNHPhaseTiming
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Round")
	float PreRoundCustomizationSeconds = 30.0f;

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

USTRUCT(BlueprintType)
struct FVNHHumanDrillPrompt
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Hunter Command")
	EVNHHunterCommandPromptType PromptType = EVNHHunterCommandPromptType::None;

	UPROPERTY(BlueprintReadOnly, Category = "Human Drill")
	EVNHHumanDrillAction Action = EVNHHumanDrillAction::None;

	UPROPERTY(BlueprintReadOnly, Category = "Human Drill")
	float PromptEndsAtServerTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Human Drill")
	float CooldownEndsAtServerTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Hunter Command")
	float EveryonePointCooldownEndsAtServerTime = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Hunter Command")
	int32 EveryonePointUsesThisRound = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Human Drill")
	int32 Serial = 0;
};

USTRUCT(BlueprintType)
struct FVNHCustomizationItem : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	EVNHCustomizationSlot Category = EVNHCustomizationSlot::Outfit;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	TSoftObjectPtr<UTexture2D> Icon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	int32 SortOrder = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Customization")
	bool bEnabled = true;
};

USTRUCT(BlueprintType)
struct FVNHCharacterCustomization
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	int32 PresetIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> BodyMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> HairMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> FaceMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> HatMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> MustacheMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> OutfitMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> OutwearMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> PantsMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> ShoesMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	TSoftObjectPtr<USkeletalMesh> AccessoryMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	FLinearColor BodyColor = FLinearColor(0.95f, 0.58f, 0.36f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	FLinearColor HairColor = FLinearColor(0.12f, 0.07f, 0.04f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	FLinearColor OutfitColor = FLinearColor(0.05f, 0.58f, 0.82f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	bool bNoFace = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Customization")
	FName Nickname = TEXT("Greg Adjacent");
};
