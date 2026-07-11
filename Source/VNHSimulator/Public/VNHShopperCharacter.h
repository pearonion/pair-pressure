#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "TimerManager.h"
#include "VNHRoutineComponent.h"
#include "VNHShopperCharacter.generated.h"

class UVNHAlienLocomotionComponent;
class UAnimMontage;
class UCameraComponent;
class UDataTable;
class USoundBase;
class USpringArmComponent;
class UStaticMeshComponent;
class USkeletalMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHActNaturalUsed, EVNHActNaturalRecovery, Recovery);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVNHPublicTestReceived, EVNHPublicTestType, TestType);

UCLASS()
class VNHSIMULATOR_API AVNHShopperCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AVNHShopperCharacter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UVNHRoutineComponent* GetRoutineComponent() const { return RoutineComponent; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UVNHAlienLocomotionComponent* GetAlienLocomotionComponent() const { return AlienLocomotionComponent; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	USpringArmComponent* GetFollowCameraBoom() const { return FollowCameraBoom; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UCameraComponent* GetFollowCamera() const { return FollowCamera; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	UCameraComponent* GetFirstPersonCamera() const { return FirstPersonCamera; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper|Camera")
	void SetFirstPersonViewEnabled(bool bEnabled);

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool IsPossessedByAlien() const { return bPossessedByAlien; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool HasActNaturalAvailable() const { return bActNaturalAvailable; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	bool IsFrozenByPublicTest() const { return bFrozenByPublicTest; }

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper")
	TSoftObjectPtr<UObject> GetShopperStateTree() const { return ShopperStateTree; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void SetPossessedByAlien(bool bNewPossessedByAlien);

	void PrepareForPlayerPossession();

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	bool UseActNatural();

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void ApplyPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper")
	void StartRoutineMovement();

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper|Interaction")
	FString BuildQuestionResponse() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	float GetComposure() const { return Composure; }

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	EVNHComposureState GetComposureState() const { return ComposureState; }

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	FText GetComposureStateText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	float GetInactivitySeconds() const { return InactivitySeconds; }

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	float GetCurrentFartThreshold() const { return CurrentFartThreshold; }

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	float GetFartCooldownRemaining() const { return FartCooldownRemaining; }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	EVNHUniversalAction GetLastUniversalAction() const { return LastUniversalAction; }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	AActor* GetHeldProp() const { return HeldProps.IsEmpty() ? nullptr : HeldProps.Last(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	int32 GetHeldPropCount() const { return HeldProps.Num(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	bool IsHoldingProp(const AActor* Prop) const { return HeldProps.Contains(Prop); }

	UFUNCTION(BlueprintPure, Category = "VNH|Composure")
	bool IsBeingWatchedByHunter() const { return bWasWatchedByHunter; }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	bool WasActionRepeatedRecently(EVNHUniversalAction Action, float WithinSeconds = 2.0f) const;

	UFUNCTION(BlueprintCallable, Category = "VNH|Composure")
	void ApplyComposureDelta(float Delta, FName Reason);

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void RegisterMeaningfulAction(EVNHUniversalAction Action, AActor* Target);

	UFUNCTION(BlueprintCallable, Category = "VNH|Composure")
	bool TriggerFartFromAction();

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void PlayUniversalActionMontage(UAnimMontage* Montage);

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void SetHeldProp(AActor* NewHeldProp);

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void PlayDirectionalKnockdown(const FVector& ImpactOrigin);

	UFUNCTION(BlueprintPure, Category = "VNH|Shopper|Debug")
	FString DescribeAnimationDebugState() const;

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void SetCharacterCustomization(const FVNHCharacterCustomization& NewCustomization);

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	FVNHCharacterCustomization GetCharacterCustomization() const { return CharacterCustomization; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper|Interaction")
	void SetInteractionHighlighted(bool bHighlighted);

	UFUNCTION(BlueprintCallable, Category = "VNH|Shopper|Interaction")
	void SetInteractionHighlightStencil(int32 StencilValue);

	UPROPERTY(BlueprintAssignable, Category = "VNH|Shopper")
	FVNHActNaturalUsed OnActNaturalUsed;

	UPROPERTY(BlueprintAssignable, Category = "VNH|Shopper")
	FVNHPublicTestReceived OnPublicTestReceived;

private:
	void UpdateAdaptiveFollowCamera(float DeltaSeconds);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVNHRoutineComponent> RoutineComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVNHAlienLocomotionComponent> AlienLocomotionComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> FollowCameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FirstPersonCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UStaticMeshComponent> DebugBodyMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> HairMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> FaceMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> HatMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> MustacheMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> OutfitMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> OutwearMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> PantsMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> ShoesMeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USkeletalMeshComponent> AccessoryMeshComponent;

	UPROPERTY(ReplicatedUsing = OnRep_PossessedByAlien, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bPossessedByAlien = false;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Shopper", meta = (AllowPrivateAccess = "true"))
	bool bActNaturalAvailable = true;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Shopper|Public Tests", meta = (AllowPrivateAccess = "true", ClampMin = "0.0"))
	float FreezeTestHoldSeconds = 4.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USoundBase> FartSound;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Actions|Animations", meta = (AllowPrivateAccess = "true"))
	TSoftObjectPtr<UDataTable> HumanActionAnimationTable;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Actions|Animations", meta = (AllowPrivateAccess = "true"))
	TSoftObjectPtr<UDataTable> AlienActionAnimationTable;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VNH|Shopper|AI", meta = (AllowPrivateAccess = "true", AllowedClasses = "/Script/StateTreeModule.StateTree"))
	TSoftObjectPtr<UObject> ShopperStateTree = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/AI/ST_Shoppers.ST_Shoppers")));

	UPROPERTY(ReplicatedUsing = OnRep_FrozenByPublicTest, BlueprintReadOnly, Category = "VNH|Shopper|Public Tests", meta = (AllowPrivateAccess = "true"))
	bool bFrozenByPublicTest = false;

	UPROPERTY(ReplicatedUsing = OnRep_Composure, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	float Composure = 100.0f;

	UPROPERTY(ReplicatedUsing = OnRep_Composure, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	EVNHComposureState ComposureState = EVNHComposureState::Calm;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	float InactivitySeconds = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	float CurrentFartThreshold = 13.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Composure", meta = (AllowPrivateAccess = "true"))
	float FartCooldownRemaining = 0.0f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Interaction", meta = (AllowPrivateAccess = "true"))
	EVNHUniversalAction LastUniversalAction = EVNHUniversalAction::None;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "VNH|Interaction", meta = (AllowPrivateAccess = "true"))
	TArray<TObjectPtr<AActor>> HeldProps;

	UPROPERTY(ReplicatedUsing = OnRep_CharacterCustomization, BlueprintReadOnly, Category = "VNH|Customization", meta = (AllowPrivateAccess = "true"))
	FVNHCharacterCustomization CharacterCustomization;

	FTimerHandle FreezeTestTimerHandle;
	FTimerHandle PublicTestReactionTimerHandle;
	FTimerHandle UniversalActionMovementLockTimerHandle;
	bool bFirstPersonViewEnabled = false;
	bool bStandingStillPenaltyApplied = false;
	bool bWasWatchedByHunter = false;
	float TimeSinceHunterWatch = 0.0f;
	float LastSuspiciousEventTime = -100.0f;
	float LastUniversalActionTime = -100.0f;
	float LastInspectTime = -100.0f;
	float LastManualFartActionTime = -100.0f;
	float LastManualFartCooldownSeconds = 0.0f;
	int32 ManualFartSpamStreak = 0;
	FVector LastMeaningfulLocation = FVector::ZeroVector;
	FRotator LastMeaningfulControlRotation = FRotator::ZeroRotator;
	TWeakObjectPtr<AActor> LastInspectedActor;

	UFUNCTION()
	void OnRep_PossessedByAlien();

	UFUNCTION()
	void OnRep_FrozenByPublicTest();

	UFUNCTION()
	void OnRep_Composure();

	UFUNCTION()
	void OnRep_CharacterCustomization();

	UFUNCTION(NetMulticast, Reliable)
	void MulticastTriggerFart();

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayUniversalActionMontage(UAnimMontage* Montage);

	void SetFrozenByPublicTest(bool bNewFrozen);
	void UpdateComposureSystem(float DeltaSeconds);
	void UpdateComposureState();
	void ResetInactivity();
	bool TriggerFart();
	bool IsWatchedByHunter(bool& bOutHunterVeryClose) const;
	bool IsNearSuspiciousObject() const;
	void ApplyComposureVisualState();
	void ClearPublicTestFreeze();
	void ApplyFrozenVisualState();
	void StartUniversalActionMovementLock(float DurationSeconds);
	void ClearUniversalActionMovementLock();
	void ApplyLookToEntranceReaction();
	void ApplyClearAisleReaction();
	void ResumeRoutineMovement();
	void ConfigureCreativeCharacterVisuals();
	void ApplyCharacterCustomization();
	void ApplySlotMesh(USkeletalMeshComponent* SlotComponent, const TSoftObjectPtr<USkeletalMesh>& MeshAsset, bool bHideSlot = false);
	void ApplyColorToMesh(USkeletalMeshComponent* MeshComponent, const FLinearColor& Color);
	UAnimMontage* ResolveFartKnockdownMontage(EVNHPlayerRole PlayerRole, FName RowName) const;
	FName GetFartKnockdownRowName(const AVNHShopperCharacter* HitShopper, const FVector& CloudCenter) const;
};
