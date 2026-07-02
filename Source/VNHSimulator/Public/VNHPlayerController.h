#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VNHGameplayTypes.h"
#include "VNHPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UAnimMontage;
class UDataTable;
class UMaterialInterface;
class UButton;
class UPostProcessComponent;
class UProgressBar;
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
	AVNHPlayerController();

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

	UFUNCTION(BlueprintCallable, Category = "VNH|Interaction")
	void RequestUniversalAction(EVNHUniversalAction Action);

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestHumanDrill();

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestFakeDrill();

	UFUNCTION(BlueprintCallable, Category = "VNH|Hunter")
	void RequestEveryonePoint();

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

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void ApplySavedCharacterCustomization();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void OpenCharacterCustomizerFromMainMenu();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void RequestPreRoundCustomizationReady();

	UFUNCTION(BlueprintCallable, Category = "VNH|Debug")
	void RequestDebugPossessShopper(int32 ShopperIndex, EVNHPlayerRole ForcedRole);

	UFUNCTION(Server, Reliable)
	void ServerRequestPublicTest(EVNHPublicTestType TestType);

	UFUNCTION(Server, Reliable)
	void ServerRequestQuestion(AVNHShopperCharacter* QuestionedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestAccusation(AVNHShopperCharacter* AccusedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestHumanDrill();

	UFUNCTION(Server, Reliable)
	void ServerRequestFakeDrill();

	UFUNCTION(Server, Reliable)
	void ServerRequestEveryonePoint();

	UFUNCTION(Server, Reliable)
	void ServerRequestDirectQuestion(AVNHShopperCharacter* QuestionedShopper);

	UFUNCTION(Server, Reliable)
	void ServerRequestQuickChat(EVNHQuickChatLine Line);

	UFUNCTION(Server, Reliable)
	void ServerRequestStartRoundFromLobby();

	UFUNCTION(Server, Reliable)
	void ServerSetCharacterCustomization(const FVNHCharacterCustomization& Customization);

	UFUNCTION(Server, Reliable)
	void ServerSetPreRoundCustomizationReady();

	UFUNCTION(Server, Reliable)
	void ServerDebugPossessShopper(int32 ShopperIndex, EVNHPlayerRole ForcedRole);

	UFUNCTION(Server, Reliable)
	void ServerRequestActNatural();

	UFUNCTION(Server, Reliable)
	void ServerPerformUniversalAction(EVNHUniversalAction Action, AActor* Target);

	UFUNCTION(Client, Reliable)
	void ClientReceiveInteractionText(const FString& InteractionText);

	UFUNCTION(Client, Reliable)
	void ClientShowLobbyMenu();

	UFUNCTION()
	void HandleLobbyCustomizeClicked();

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

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Actions|Animations")
	TSoftObjectPtr<UDataTable> HumanActionAnimationTable;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Actions|Animations")
	TSoftObjectPtr<UDataTable> AlienActionAnimationTable;

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
	void HandleInspectPressed();
	void HandlePointPressed();
	void HandleWavePressed();
	void HandleLaughPressed();
	void HandleFartPressed();
	void HandlePlaceDecoyPressed();
	void HandlePickUpPressed();
	void HandleDropPressed();
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
	void EnsureRoleHudWidget();
	void EnsureMarkedSuspectsWidget();
	void EnsureComposureWidget();
	void RemoveComposureWidget();
	void BindRoleHudActionButtons();
	void BindRoleHudActionButton(FName ButtonName, FName HandlerName);
	void BindComposureWidgetButtons();
	UFUNCTION()
	void HandleHudCustomizeClicked();
	UFUNCTION()
	void HandleHudMarkClicked();
	UFUNCTION()
	void HandleHudAccuseClicked();
	UFUNCTION()
	void HandleHudPressureClicked();
	UFUNCTION()
	void HandleHudHumanDrillClicked();
	UFUNCTION()
	void HandleHudFakeDrillClicked();
	UFUNCTION()
	void HandleHudEveryonePointClicked();
	UFUNCTION()
	void HandleHudInspectClicked();
	UFUNCTION()
	void HandleHudWaveClicked();
	UFUNCTION()
	void HandleHudPointClicked();
	UFUNCTION()
	void HandleHudLaughClicked();
	UFUNCTION()
	void HandleHudFartClicked();
	UFUNCTION()
	void HandleHudPlaceDecoyClicked();
	FString GetRoundTimerText() const;
	void UpdateRoleHudWidgetRuntimeLabels(float DeltaTime);
	void UpdateDebugDeckRuntimeLabels(float DeltaTime);
	void UpdateMarkedSuspectsWidgetRuntimeLabels(float DeltaTime);
	void UpdateComposureWidgetRuntimeLabels(float DeltaTime);
	void DismissActiveHunterCommandPrompt();
	void UpdatePreRoundCustomizationFlow();
	void RestoreGameplayInputMode();
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
	AActor* GetUniversalActionTarget(EVNHUniversalAction Action) const;
	UAnimMontage* ResolveRoleActionMontage(EVNHPlayerRole PlayerRole, const AVNHShopperCharacter* Shopper, EVNHUniversalAction Action) const;
	void PlayRoleActionAnimation(EVNHPlayerRole PlayerRole, AVNHShopperCharacter* Shopper, EVNHUniversalAction Action) const;
	void SetInteractableOutline(AActor* Actor, bool bEnabled) const;
	void PerformPickUp(AVNHShopperCharacter* Shopper, AActor* Prop);
	void PerformDrop(AVNHShopperCharacter* Shopper);

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
	TWeakObjectPtr<AActor> FocusedInteractable;
	TWeakObjectPtr<AVNHShopperCharacter> TargetedShopper;
	TWeakObjectPtr<UTextBlock> RoundStatusTextBlock;
	TWeakObjectPtr<UTextBlock> InteractionTextBlock;
	TWeakObjectPtr<UTextBlock> RoleStatusTextBlock;
	TWeakObjectPtr<UTextBlock> LocomotionStatusTextBlock;
	TWeakObjectPtr<UTextBlock> RevealStatusTextBlock;
	TWeakObjectPtr<UWidget> RevealStatusBoxWidget;
	TWeakObjectPtr<UWidget> RevealStatusShadowWidget;
	TWeakObjectPtr<UWidget> RevealRailWidget;
	TWeakObjectPtr<UUserWidget> RoleHudWidget;
	TWeakObjectPtr<UTextBlock> RoleHudRoundTimerTextBlock;
	TWeakObjectPtr<UTextBlock> RoleHudComposureStateTextBlock;
	TWeakObjectPtr<UTextBlock> RoleHudComposureValueTextBlock;
	TWeakObjectPtr<UProgressBar> RoleHudComposureProgressBar;
	TWeakObjectPtr<UWidget> RoleHudDrillPromptPanelWidget;
	TWeakObjectPtr<UTextBlock> RoleHudDrillPromptTextBlock;
	TWeakObjectPtr<UWidget> RoleHudHumanDrillCooldownPanelWidget;
	TWeakObjectPtr<UTextBlock> RoleHudHumanDrillCooldownTextBlock;
	TWeakObjectPtr<UWidget> RoleHudEveryonePointCooldownPanelWidget;
	TWeakObjectPtr<UTextBlock> RoleHudEveryonePointCooldownTextBlock;
	TArray<TWeakObjectPtr<AVNHShopperCharacter>> MarkedSuspects;
	TWeakObjectPtr<UUserWidget> MarkedSuspectsWidget;
	TWeakObjectPtr<UTextBlock> MarkedSuspectsListTextBlock;
	TWeakObjectPtr<UWidget> MarkedSuspectsPanelWidget;
	TWeakObjectPtr<UUserWidget> ComposureWidget;
	TWeakObjectPtr<UWidget> ComposurePanelWidget;
	TWeakObjectPtr<UTextBlock> ComposureStateTextBlock;
	TWeakObjectPtr<UTextBlock> ComposureValueTextBlock;
	TWeakObjectPtr<UTextBlock> FartRiskTextBlock;
	TWeakObjectPtr<UTextBlock> UniversalActionTextBlock;
	TWeakObjectPtr<UProgressBar> ComposureProgressBar;
	TWeakObjectPtr<UButton> HudCustomizeButton;
	FString LastInteractionText;
	float LastInteractionTimeSeconds = -100.0f;
	float TimeUntilRoleHudWidgetLookup = 0.0f;
	float TimeUntilDebugDeckLabelLookup = 0.0f;
	float TimeUntilMarkedWidgetLookup = 0.0f;
	float TimeUntilComposureWidgetLookup = 0.0f;
	EVNHPlayerRole ActiveRoleHudRole = EVNHPlayerRole::Unassigned;
	int32 DismissedHunterCommandPromptSerial = INDEX_NONE;
	int32 LastMarkedRoundNumber = INDEX_NONE;
	int32 LastPreRoundCustomizerRound = INDEX_NONE;
	EVNHRoundPhase LastObservedRoundPhase = EVNHRoundPhase::WaitingForPlayers;
	bool bObservedRoundPhaseInitialized = false;
	TWeakObjectPtr<UUserWidget> LobbyMenuWidget;
};
