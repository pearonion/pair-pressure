#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UTextBlock;
class UVNHAlienLocomotionComponent;
class AVNHShopperCharacter;

UCLASS()
class VNHSIMULATOR_API AVNHPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void AcknowledgePossession(APawn* PossessedPawn) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	FString DescribeAlienInputDebugState() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	FString GetRoleStatusText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	FString GetLocomotionStatusText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	AVNHShopperCharacter* GetFocusedShopper() const { return FocusedShopper.Get(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetInteractionPromptText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetLastInteractionText() const { return LastInteractionText; }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetMarkedSuspectsText() const;

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(BlueprintCallable, Category = "VNH|Alien")
	void RequestActNatural();

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void RequestInteract();

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void MarkFocusedShopper();

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void FakeAccuseFocusedShopper();

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void DebugAccuseFocusedShopper();

	UFUNCTION(Server, Reliable)
	void ServerRequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(Server, Reliable)
	void ServerRequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestActNatural();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	TObjectPtr<UInputMappingContext> AlienInputMappingContext;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	int32 AlienInputMappingPriority = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	TObjectPtr<UInputAction> AlienMoveAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	TObjectPtr<UInputAction> AlienFastWalkAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	TObjectPtr<UInputAction> AlienActNaturalAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Input|Alien")
	bool bEnablePolledAlienKeyboardInput = true;

private:
	UVNHAlienLocomotionComponent* GetAlienLocomotionComponent() const;

	void UpdateAlienInputMapping();
	void BindAlienInputActions();
	void HandleAlienMoveInput(const struct FInputActionValue& Value);
	void HandleAlienMoveStopped();
	void HandleAlienFastWalkStarted();
	void HandleAlienFastWalkStopped();
	void HandleAlienMoveForwardAxis(float Value);
	void HandleAlienMoveRightAxis(float Value);
	void HandleTurnAxis(float Value);
	void HandleLookUpAxis(float Value);
	void HandleInteractPressed();
	void ToggleDebugHud();
	void ApplyDebugHudInputMode(bool bDebugHudVisible);
	void UpdateDebugDeckRuntimeLabels(float DeltaTime);
	void PushLegacyAlienMoveInput();
	void PollAlienKeyboardInput();
	void UpdateFocusedShopper();

	bool bAlienInputMappingApplied = false;
	FVector2D LegacyAlienMoveInput = FVector2D::ZeroVector;
	FVector2D LastPushedLegacyAlienMoveInput = FVector2D::ZeroVector;
	FVector2D LastEnhancedAlienMoveInput = FVector2D::ZeroVector;
	FVector2D LastPolledAlienMoveInput = FVector2D::ZeroVector;
	int32 LegacyForwardAxisSamples = 0;
	int32 LegacyRightAxisSamples = 0;
	int32 LegacyMovePushes = 0;
	int32 EnhancedMoveSamples = 0;
	int32 PolledMoveSamples = 0;
	int32 FastWalkStartedSamples = 0;
	int32 FastWalkStoppedSamples = 0;
	int32 PolledActNaturalSamples = 0;
	bool bLastLegacyPushHadLocomotion = false;
	bool bLastPolledFastWalkRequested = false;
	bool bWasPolledActNaturalDown = false;
	bool bWasPolledInteractDown = false;
	TWeakObjectPtr<AVNHShopperCharacter> FocusedShopper;
	TWeakObjectPtr<UTextBlock> RoleStatusTextBlock;
	TWeakObjectPtr<UTextBlock> LocomotionStatusTextBlock;
	TArray<TWeakObjectPtr<AVNHShopperCharacter>> MarkedSuspects;
	FString LastInteractionText;
	float LastInteractionTimeSeconds = -100.0f;
	float TimeUntilDebugDeckLabelLookup = 0.0f;
};
