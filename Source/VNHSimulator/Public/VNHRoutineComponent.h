#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VNHGameplayTypes.h"
#include "VNHRoutineComponent.generated.h"

UENUM(BlueprintType)
enum class EVNHShopperContext : uint8
{
	Browsing,
	Mirror,
	Checkout,
	Walking,
	Idle,
	Reacting
};

UENUM(BlueprintType)
enum class EVNHActNaturalRecovery : uint8
{
	NeutralIdle,
	InspectTag,
	CheckPhone,
	TakeSip,
	AdjustClothing,
	CheckAppearance
};

USTRUCT(BlueprintType)
struct FVNHShopperRoutineSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routine")
	EVNHShopperContext Context = EVNHShopperContext::Browsing;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routine")
	FName HeldProp = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routine")
	FName SuggestedNextActivity = TEXT("Mirror");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Routine")
	bool bPausedForPossession = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHShopperRoutineChanged, const FVNHShopperRoutineSnapshot&, Snapshot);

UCLASS(ClassGroup = (VNH), meta = (BlueprintSpawnableComponent))
class VNHSIMULATOR_API UVNHRoutineComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVNHRoutineComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(BlueprintAssignable, Category = "VNH|Routine")
	FVNHShopperRoutineChanged OnRoutineChanged;

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	FVNHShopperRoutineSnapshot GetSnapshot() const { return Snapshot; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Routine")
	void SetContext(EVNHShopperContext NewContext, FName NewSuggestedNextActivity);

	UFUNCTION(BlueprintCallable, Category = "VNH|Routine")
	void SetHeldProp(FName NewHeldProp);

	UFUNCTION(BlueprintCallable, Category = "VNH|Routine")
	void PauseRoutineForPossession();

	UFUNCTION(BlueprintCallable, Category = "VNH|Routine")
	void ResumeRoutineAfterPossession();

	UFUNCTION(BlueprintPure, Category = "VNH|Routine")
	EVNHActNaturalRecovery ChooseActNaturalRecovery() const;

private:
	UPROPERTY(ReplicatedUsing = OnRep_Snapshot, EditAnywhere, BlueprintReadOnly, Category = "VNH|Routine", meta = (AllowPrivateAccess = "true"))
	FVNHShopperRoutineSnapshot Snapshot;

	UFUNCTION()
	void OnRep_Snapshot();

	void BroadcastChanged();
};
