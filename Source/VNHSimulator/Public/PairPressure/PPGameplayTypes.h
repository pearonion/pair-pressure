#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "PPGameplayTypes.generated.h"

class UAnimInstance;
class UAnimSequence;
class UPrimitiveComponent;
class USkeletalMesh;
class UTexture2D;

UENUM(BlueprintType)
enum class EPPGameMode : uint8
{
	BringYourIdiotHome,
	AttachedAtTheHip
};

UENUM(BlueprintType)
enum class EPPLobbyCourseType : uint8
{
	PresetMaps,
	BuildForOtherTeam,
	OneCustomObstacle
};

UENUM(BlueprintType)
enum class EPPPresetMap : uint8
{
	FactoryFiasco,
	SkyScramble,
	JungleJam,
	PipePanic
};

USTRUCT(BlueprintType)
struct FPPMascotAnimationRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot")
	TSoftObjectPtr<USkeletalMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot")
	TSoftObjectPtr<UTexture2D> Portrait;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot")
	TSoftClassPtr<UAnimInstance> AnimationBlueprint;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Locomotion")
	TSoftObjectPtr<UAnimSequence> Idle;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Locomotion")
	TSoftObjectPtr<UAnimSequence> Run;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Locomotion")
	TSoftObjectPtr<UAnimSequence> Jump;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Grab;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> OverheadThrow;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Throw;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Hanging;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Crouch;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Dive;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Reactions")
	TSoftObjectPtr<UAnimSequence> HitFront;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Reactions")
	TSoftObjectPtr<UAnimSequence> HitLeft;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Reactions")
	TSoftObjectPtr<UAnimSequence> HitRight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Recovery")
	TSoftObjectPtr<UAnimSequence> GetUpFront;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Recovery")
	TSoftObjectPtr<UAnimSequence> GetUpLeft;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Recovery")
	TSoftObjectPtr<UAnimSequence> GetUpRight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mascot|Actions")
	TSoftObjectPtr<UAnimSequence> Punch;
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

UENUM(BlueprintType)
enum class EPPGrabTargetType : uint8
{
	None,
	GameplayItem,
	Player,
	LedgeOrHandle,
	LargePushable
};

UENUM(BlueprintType)
enum class EPPGrabState : uint8
{
	None,
	Reaching,
	HoldingItem,
	GrabbingPlayer,
	MutualGrab,
	PushingObject,
	HangingFromLedge,
	Releasing,
	GrabCooldown
};

USTRUCT(BlueprintType)
struct FPPGrabProfile
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	EPPGrabTargetType TargetType = EPPGrabTargetType::GameplayItem;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "-1", ClampMax = "1000"))
	int32 PriorityOverride = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "25.0"))
	float MaximumRange = 230.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	float MaximumAngleDegrees = 55.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "20.0"))
	float CarryDistance = 105.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Spring", meta = (ClampMin = "0.0"))
	float LinearStiffness = 750.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Spring", meta = (ClampMin = "0.0"))
	float LinearDamping = 110.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Spring", meta = (ClampMin = "0.0"))
	float AngularStiffness = 450.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab|Spring", meta = (ClampMin = "0.0"))
	float AngularDamping = 85.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "0.0"))
	float BreakForce = 90000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "0.0"))
	float MaximumMass = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float MovementSpeedMultiplier = 0.88f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	bool bMaintainRotation = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pair Pressure|Grab")
	bool bRequiresTwoHands = false;
};

USTRUCT(BlueprintType)
struct FPPGrabTargetData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|Grab")
	TObjectPtr<AActor> TargetActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|Grab")
	TObjectPtr<UPrimitiveComponent> TargetPrimitive = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|Grab")
	FVector_NetQuantize GrabPoint = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|Grab")
	FPPGrabProfile Profile;

	UPROPERTY(BlueprintReadOnly, Category = "Pair Pressure|Grab")
	float Score = 0.0f;

	bool IsValid() const { return TargetActor != nullptr && TargetPrimitive != nullptr; }
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
