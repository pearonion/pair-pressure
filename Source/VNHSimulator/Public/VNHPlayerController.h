#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "PairPressure/PPGameplayTypes.h"
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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
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

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	bool IsLocalLobbyHost() const;

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	bool IsLobbyStartFocused() const { return FocusedLobbyPlayButton.IsValid(); }

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	float GetLobbyStartHoldProgress() const;

	bool GetLobbyStartPromptScreenPosition(FVector2D& OutScreenPosition) const;

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

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void RequestLobbyMatchSetup(EPPGameMode RequestedGameMode, EPPLobbyCourseType RequestedCourseType, EPPPresetMap RequestedPresetMap);

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void RequestLobbyTeam(int32 RequestedTeamId);

	UFUNCTION(BlueprintCallable, Category = "Pair Pressure|Mascot")
	void RequestMascotSelection(FName RequestedMascotRowName);

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
	void ServerRequestPressure(AVNHShopperCharacter* PressuredShopper);

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
	void ServerSetLobbyMatchSetup(EPPGameMode RequestedGameMode, EPPLobbyCourseType RequestedCourseType, EPPPresetMap RequestedPresetMap);

	UFUNCTION(Server, Reliable)
	void ServerSetLobbyTeam(int32 RequestedTeamId);

	UFUNCTION(Server, Reliable)
	void ServerSetMascotSelection(FName RequestedMascotRowName);

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

	UFUNCTION(Server, Reliable)
	void ServerThrowHeldProp(float ChargeAlpha, FVector_NetQuantizeNormal ThrowDirection);

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

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pair Pressure|Input")
	TObjectPtr<UInputAction> GrabAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> GrabInputMappingContext;

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
	void InitializePairPressureGrabInput();
	void UpdatePairPressureGrabInputMapping();
	void BindPairPressureGrabInputAction();
	void HandleAlienMoveInput(const struct FInputActionValue& Value);
	void HandleAlienMoveStopped();
	void HandleAlienFastWalkStarted();
	void HandleAlienFastWalkStopped();
	void HandleAlienMoveForwardAxis(float Value);
	void HandleAlienMoveRightAxis(float Value);
	void HandleTurnAxis(float Value);
	void HandleLookUpAxis(float Value);
	void HandleCursorXAxis(float Value);
	void HandleCursorYAxis(float Value);
	void HandleCameraOrbitStarted();
	void HandleCameraOrbitStopped();
	void HandleCameraOrbitReset();
	void HandlePairPressureDebugRagdollPressed();
	void HandlePairPressureDebugRecoveryPressed();
	void HandleTargetFocusPressed();
	void HandleInspectPressed();
	void HandlePointPressed();
	void HandleWavePressed();
	void HandleLaughPressed();
	void HandleFartPressed();
	void HandlePlaceDecoyPressed();
	void HandleJumpPressed();
	void HandleJumpReleased();
	void HandleCrouchPressed();
	void HandleCrouchReleased();
	void HandleRoleHudActionSlot1Pressed();
	void HandleRoleHudActionSlot2Pressed();
	void HandleRoleHudActionSlot3Pressed();
	void HandleRoleHudActionSlot4Pressed();
	void HandleRoleHudActionSlot5Pressed();
	void HandleRoleHudActionSlot6Pressed();
	void HandlePickUpPressed();
	void HandleDropPressed();
	void HandlePairPressureDivePressed();
	void HandlePairPressureAssistPressed();
	void HandlePairPressureAssistReleased();
	void HandlePairPressureGrabPressed();
	void HandlePairPressureGrabReleased();
	void HandleThrowChargePressed();
	void HandleThrowChargeReleased();
	void UpdateThrowChargeIndicator();
	void RemoveThrowChargeIndicator();
	void HandleInteractPressed();
	void UpdateLobbyStartHold(float DeltaTime);
	void ResetLobbyStartHold();
	void HandleQuickChatPressed();
	void HandleQuickChatLookingForShirtPressed();
	void HandleQuickChatWaitingForFriendPressed();
	void HandleQuickChatNoThanksPressed();
	void HandleQuickChatFoundWrongSizePressed();
	void ToggleDebugHud();
	void ApplyDebugHudInputMode(bool bDebugHudVisible);
	void RemoveBlockingMenuWidgets();
	void ShowLobbyMenu();
	void RemoveLobbyMenu();
	void EnsureTargetOutlinePostProcess();
	void EnsureRoleHudWidget();
	void EnsureMarkedSuspectsWidget();
	void EnsureComposureWidget();
	void EnsurePairPressureHUD();
	void RemoveComposureWidget();
	void BindRoleHudActionButtons();
	void BindRoleHudActionButton(FName ButtonName, FName HandlerName);
	void BindComposureWidgetButtons();
	void ExecuteRoleHudActionSlot(int32 SlotIndex);
	void UpdateRoleHudActionHotkeyLabels();
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
	UFUNCTION()
	void HandleHudThrowClicked();
	UFUNCTION()
	void HandleThrownPropHit(AActor* ThrownActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);
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
	void UpdateCursorReticleAndHeadLook(float DeltaTime);
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
	bool bGrabInputMappingApplied = false;
	bool bGrabInputActionBound = false;
	bool bPairPressureGrabInputDown = false;
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
	bool bThrowInputDown = false;
	bool bCameraOrbitActive = false;
	bool bThrowChargeActive = false;
	float ThrowChargeStartedAtSeconds = 0.0f;
	bool bLobbyStartHoldActive = false;
	bool bLobbyStartRequestSent = false;
	float LobbyStartHoldSeconds = 0.0f;
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
	TWeakObjectPtr<UTextBlock> RoleHudActionHotkeyTextBlocks[6];
	TWeakObjectPtr<UWidget> RoleHudFartCooldownPanelWidget;
	TWeakObjectPtr<UTextBlock> RoleHudFartCooldownTextBlock;
	TArray<TWeakObjectPtr<AVNHShopperCharacter>> MarkedSuspects;
	TWeakObjectPtr<UUserWidget> MarkedSuspectsWidget;
	TWeakObjectPtr<UTextBlock> MarkedSuspectsListTextBlock;
	TWeakObjectPtr<UWidget> MarkedSuspectsPanelWidget;
	TWeakObjectPtr<UUserWidget> ComposureWidget;
	TWeakObjectPtr<UUserWidget> PairPressureHUDWidget;
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
	bool bDebugDeckVisible = false;
	bool bGameplayInputModeApplied = false;
	float TimeUntilMarkedWidgetLookup = 0.0f;
	float TimeUntilComposureWidgetLookup = 0.0f;
	float RoleHudDisplayedFartCooldownRemaining = 0.0f;
	float RoleHudLastReplicatedFartCooldownRemaining = 0.0f;
	EVNHPlayerRole ActiveRoleHudRole = EVNHPlayerRole::Unassigned;
	float SmoothedHeadLookYaw = 0.0f;
	float SmoothedHeadLookPitch = 0.0f;
	FVector2D VirtualCursorPosition = FVector2D::ZeroVector;
	bool bVirtualCursorInitialized = false;
	int32 DismissedHunterCommandPromptSerial = INDEX_NONE;
	int32 LastMarkedRoundNumber = INDEX_NONE;
	int32 LastPreRoundCustomizerRound = INDEX_NONE;
	EVNHRoundPhase LastObservedRoundPhase = EVNHRoundPhase::WaitingForPlayers;
	bool bObservedRoundPhaseInitialized = false;
	TWeakObjectPtr<UUserWidget> LobbyMenuWidget;
};
