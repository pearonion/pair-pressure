#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UMaterialInterface;
class UPostProcessComponent;
class UTextBlock;
class UUserWidget;
class UWidget;
class UVNHAlienLocomotionComponent;
class AVNHLobbyPlayButton;
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

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	FString GetRoundStatusText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	FString GetDebugDeckInteractionText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Debug")
	FString GetRevealStatusText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	AVNHShopperCharacter* GetFocusedShopper() const { return FocusedShopper.Get(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	AVNHShopperCharacter* GetTargetedShopper() const { return TargetedShopper.Get(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetInteractionPromptText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetLastInteractionText() const { return LastInteractionText; }

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	FString GetMarkedSuspectsText() const;

	FString GetMarkedSuspectsPanelText() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Interaction")
	bool IsAssignedHunter() const;

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void CancelTargetSelection();

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

	UFUNCTION(BlueprintCallable, Category = "VNH|Quick Chat")
	void RequestQuickChat(EVNHQuickChatLine Line);

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void RequestStartRoundFromLobby();

	UFUNCTION(BlueprintCallable, Category = "VNH|Debug")
	void RequestDebugPossessShopper(int32 ShopperIndex, EVNHPlayerRole ForcedRole);

	UFUNCTION(Server, Reliable)
	void ServerRequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(Server, Reliable)
	void ServerRequestQuestion(AVNHShopperCharacter* QuestionedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestDirectQuestion(AVNHShopperCharacter* QuestionedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestQuickChat(EVNHQuickChatLine Line);

	UFUNCTION(Server, Reliable)
	void ServerRequestStartRoundFromLobby();

	UFUNCTION(Server, Reliable)
	void ServerDebugPossessShopper(int32 ShopperIndex, EVNHPlayerRole ForcedRole);

	UFUNCTION(Server, Reliable)
	void ServerRequestActNatural();

	UFUNCTION(Client, Reliable)
	void ClientReceiveInteractionText(const FString& InteractionText);

	UFUNCTION(Client, Reliable)
	void ClientShowLobbyMenu();

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
	void HandleTargetFocusPressed();
	void HandleInteractPressed();
	void HandleQuickChatPressed();
	void HandleQuickChatLookingForShirtPressed();
	void HandleQuickChatWaitingForFriendPressed();
	void HandleQuickChatNoThanksPressed();
	void HandleQuickChatFoundWrongSizePressed();
	void ToggleDebugHud();
	void ApplyDebugHudInputMode(bool bDebugHudVisible);
	void ShowLobbyMenu();
	void EnsureTargetOutlinePostProcess();
	void EnsureMarkedSuspectsWidget();
	void UpdateDebugDeckRuntimeLabels(float DeltaTime);
	void UpdateMarkedSuspectsWidgetRuntimeLabels(float DeltaTime);
	void UpdateMarkedSuspectsForRound();
	void RegisterGameplayHardwareCursors();
	void UpdateGameplayCursor();
	void UpdateRoleCameraMode();
	void PushLegacyAlienMoveInput();
	void PollAlienKeyboardInput();
	void PollInteractionInput();
	void UpdateFocusedShopper();
	AVNHShopperCharacter* GetInteractionShopper() const;
	void SetTargetedShopper(AVNHShopperCharacter* NewTarget);
	void ClearTargetedShopper();
	bool IsShopperMarked(const AVNHShopperCharacter* Shopper) const;
	void RefreshShopperOutline(AVNHShopperCharacter* Shopper, bool bHovered) const;
	void ClearMarkedSuspectsForNewRound();

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
	bool bWasPolledTargetFocusDown = false;
	bool bWasPolledAccuseDown = false;
	bool bWasPolledCancelTargetDown = false;
	bool bWasPolledMarkDown = false;
	bool bWasPolledFakeAccuseDown = false;
	bool bHardwareCursorsRegistered = false;
	float HardwareCursorRegistrationRetrySeconds = 0.0f;
	UPROPERTY(Transient)
	TObjectPtr<UPostProcessComponent> TargetOutlinePostProcessComponent;

	UPROPERTY(EditDefaultsOnly, Category = "VNH|Interaction")
	TObjectPtr<UMaterialInterface> TargetOutlinePostProcessMaterial;

	TWeakObjectPtr<AVNHShopperCharacter> FocusedShopper;
	TWeakObjectPtr<AVNHLobbyPlayButton> FocusedLobbyPlayButton;
	TWeakObjectPtr<AVNHShopperCharacter> TargetedShopper;
	TWeakObjectPtr<UTextBlock> RoundStatusTextBlock;
	TWeakObjectPtr<UTextBlock> InteractionTextBlock;
	TWeakObjectPtr<UTextBlock> RoleStatusTextBlock;
	TWeakObjectPtr<UTextBlock> LocomotionStatusTextBlock;
	TWeakObjectPtr<UTextBlock> RevealStatusTextBlock;
	TWeakObjectPtr<UWidget> RevealStatusBoxWidget;
	TWeakObjectPtr<UWidget> RevealStatusShadowWidget;
	TWeakObjectPtr<UWidget> RevealRailWidget;
	TArray<TWeakObjectPtr<AVNHShopperCharacter>> MarkedSuspects;
	TWeakObjectPtr<UUserWidget> MarkedSuspectsWidget;
	TWeakObjectPtr<UTextBlock> MarkedSuspectsListTextBlock;
	TWeakObjectPtr<UWidget> MarkedSuspectsPanelWidget;
	FString LastInteractionText;
	float LastInteractionTimeSeconds = -100.0f;
	float TimeUntilDebugDeckLabelLookup = 0.0f;
	float TimeUntilMarkedWidgetLookup = 0.0f;
	int32 LastMarkedRoundNumber = INDEX_NONE;
	TWeakObjectPtr<UUserWidget> LobbyMenuWidget;
};
