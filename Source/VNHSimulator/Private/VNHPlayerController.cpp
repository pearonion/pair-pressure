#include "VNHPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PostProcessComponent.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Engine/Scene.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInterface.h"
#include "EngineUtils.h"
#include "VNHGameMode.h"
#include "VNHGameState.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHDebugHUD.h"
#include "VNHGameInstance.h"
#include "VNHLobbyPlayButton.h"
#include "VNHLog.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

namespace
{
const TCHAR* ToPhaseText(EVNHRoundPhase Phase)
{
	switch (Phase)
	{
	case EVNHRoundPhase::WaitingForPlayers:
		return TEXT("Waiting");
	case EVNHRoundPhase::AssigningRoles:
		return TEXT("Assigning");
	case EVNHRoundPhase::AlienSetup:
		return TEXT("Alien Setup");
	case EVNHRoundPhase::AlienHeadStart:
		return TEXT("Head Start");
	case EVNHRoundPhase::Investigation:
		return TEXT("Investigation");
	case EVNHRoundPhase::Accusation:
		return TEXT("Accusation");
	case EVNHRoundPhase::Reveal:
		return TEXT("Reveal");
	case EVNHRoundPhase::Resetting:
		return TEXT("Resetting");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToPublicTestText(EVNHPublicTestType TestType)
{
	switch (TestType)
	{
	case EVNHPublicTestType::Freeze:
		return TEXT("Freeze");
	case EVNHPublicTestType::LookToEntrance:
		return TEXT("Look Here");
	case EVNHPublicTestType::ClearAisle:
		return TEXT("Clear Aisle");
	case EVNHPublicTestType::CheckoutOpen:
		return TEXT("Checkout Open");
	default:
		return TEXT("Unknown");
	}
}

constexpr int32 HoveredShopperStencil = 89;
constexpr int32 TargetedShopperStencil = 186;
constexpr int32 MarkedShopperStencil = 200;
}

void AVNHPlayerController::BeginPlay()
{
	Super::BeginPlay();

	EnsureTargetOutlinePostProcess();
	EnsureMarkedSuspectsWidget();
	RegisterGameplayHardwareCursors();

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (IsLocalController() && MapName.Contains(TEXT("MainMenu")))
	{
		if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
		{
			VNHGameInstance->ShowMainMenu();
		}
	}
	else if (MapName.Contains(TEXT("Lobby")))
	{
		if (IsLocalController())
		{
			ShowLobbyMenu();
		}
		else
		{
			ClientShowLobbyMenu();
		}
	}

	UpdateAlienInputMapping();
	ApplyDebugHudInputMode(true);
}

void AVNHPlayerController::EnsureTargetOutlinePostProcess()
{
	if (!IsLocalController() || TargetOutlinePostProcessComponent)
	{
		return;
	}

	UMaterialInterface* OutlineMaterial = TargetOutlinePostProcessMaterial;
	if (!OutlineMaterial)
	{
		OutlineMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/TNG/Materials/TNG_PPM_TargetHoverLock.TNG_PPM_TargetHoverLock"));
	}

	if (!OutlineMaterial)
	{
		UE_LOG(LogVNH, Warning, TEXT("Target outline post-process material is missing."));
		return;
	}

	TargetOutlinePostProcessComponent = NewObject<UPostProcessComponent>(this, TEXT("VNH_TargetOutlinePostProcess"));
	if (!TargetOutlinePostProcessComponent)
	{
		return;
	}

	TargetOutlinePostProcessComponent->bUnbound = true;
	TargetOutlinePostProcessComponent->bEnabled = true;
	TargetOutlinePostProcessComponent->Priority = 1000.0f;
	TargetOutlinePostProcessComponent->BlendWeight = 1.0f;
	TargetOutlinePostProcessComponent->Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, OutlineMaterial));
	AddInstanceComponent(TargetOutlinePostProcessComponent);
	TargetOutlinePostProcessComponent->RegisterComponent();
}

void AVNHPlayerController::EnsureMarkedSuspectsWidget()
{
	if (!IsLocalController() || MarkedSuspectsWidget.IsValid())
	{
		return;
	}

	UClass* MarkedWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_VNHMarkedSuspects.WBP_VNHMarkedSuspects_C"));
	if (!MarkedWidgetClass)
	{
		return;
	}

	UUserWidget* NewMarkedWidget = CreateWidget<UUserWidget>(this, MarkedWidgetClass);
	if (!NewMarkedWidget)
	{
		return;
	}

	NewMarkedWidget->AddToViewport(6400);
	MarkedSuspectsWidget = NewMarkedWidget;
	TimeUntilMarkedWidgetLookup = 0.0f;
	UpdateMarkedSuspectsWidgetRuntimeLabels(0.0f);
}

void AVNHPlayerController::AcknowledgePossession(APawn* PossessedPawn)
{
	Super::AcknowledgePossession(PossessedPawn);
	UpdateAlienInputMapping();
	UpdateRoleCameraMode();
	RegisterGameplayHardwareCursors();
	UE_LOG(LogVNH, Display, TEXT("AlienInput: acknowledged possession. Controller=%s Pawn=%s InputComponent=%s"),
		*GetClass()->GetName(),
		*GetNameSafe(PossessedPawn),
		*GetNameSafe(InputComponent));
}

void AVNHPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	BindAlienInputActions();

	InputComponent->BindAxis(TEXT("VNH_AlienMoveForward"), this, &AVNHPlayerController::HandleAlienMoveForwardAxis);
	InputComponent->BindAxis(TEXT("VNH_AlienMoveRight"), this, &AVNHPlayerController::HandleAlienMoveRightAxis);
	InputComponent->BindAxis(TEXT("Turn Right / Left Mouse"), this, &AVNHPlayerController::HandleTurnAxis);
	InputComponent->BindAxis(TEXT("Turn Right / Left Gamepad"), this, &AVNHPlayerController::HandleTurnAxis);
	InputComponent->BindAxis(TEXT("Look Up / Down Mouse"), this, &AVNHPlayerController::HandleLookUpAxis);
	InputComponent->BindAxis(TEXT("Look Up / Down Gamepad"), this, &AVNHPlayerController::HandleLookUpAxis);
	InputComponent->BindAction(TEXT("VNH_AlienFastWalk"), IE_Pressed, this, &AVNHPlayerController::HandleAlienFastWalkStarted);
	InputComponent->BindAction(TEXT("VNH_AlienFastWalk"), IE_Released, this, &AVNHPlayerController::HandleAlienFastWalkStopped);
	InputComponent->BindAction(TEXT("VNH_AlienActNatural"), IE_Pressed, this, &AVNHPlayerController::RequestActNatural);
	InputComponent->BindAction(TEXT("VNH_TargetFocus"), IE_Pressed, this, &AVNHPlayerController::HandleTargetFocusPressed);
	InputComponent->BindAction(TEXT("VNH_Interact"), IE_Pressed, this, &AVNHPlayerController::HandleInteractPressed);
	InputComponent->BindAction(TEXT("VNH_MarkSuspect"), IE_Pressed, this, &AVNHPlayerController::MarkFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_FakeAccuse"), IE_Pressed, this, &AVNHPlayerController::FakeAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_Accuse"), IE_Pressed, this, &AVNHPlayerController::DebugAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_QuickChat"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_Shirt"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatLookingForShirtPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_Friend"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatWaitingForFriendPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_NoThanks"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatNoThanksPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_WrongSize"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatFoundWrongSizePressed);
	InputComponent->BindAction(TEXT("VNH_ToggleDebugHud"), IE_Pressed, this, &AVNHPlayerController::ToggleDebugHud);

	UE_LOG(LogVNH, Display, TEXT("AlienInput: setup complete. Controller=%s InputComponent=%s EnhancedComponent=%s"),
		*GetClass()->GetName(),
		*GetNameSafe(InputComponent),
		Cast<UEnhancedInputComponent>(InputComponent) ? TEXT("true") : TEXT("false"));
}

void AVNHPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (!bHardwareCursorsRegistered)
	{
		HardwareCursorRegistrationRetrySeconds -= DeltaTime;
		if (HardwareCursorRegistrationRetrySeconds <= 0.0f)
		{
			RegisterGameplayHardwareCursors();
		}
	}
	UpdateMarkedSuspectsForRound();
	UpdateRoleCameraMode();
	UpdateFocusedShopper();
	UpdateGameplayCursor();
	PollAlienKeyboardInput();
	PollInteractionInput();
	UpdateDebugDeckRuntimeLabels(DeltaTime);
	UpdateMarkedSuspectsWidgetRuntimeLabels(DeltaTime);
}

FString AVNHPlayerController::DescribeAlienInputDebugState() const
{
	const APawn* ControlledPawn = GetPawn();
	const UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent();

	return FString::Printf(
		TEXT("Controller=%s Local=%s Pawn=%s PawnClass=%s InputComponent=%s HasLocomotion=%s LegacyInput=(%.2f, %.2f) LastEnhanced=(%.2f, %.2f) LastPolled=(%.2f, %.2f) LegacyForwardSamples=%d LegacyRightSamples=%d LegacyPushes=%d LastLegacyPushHadLocomotion=%s EnhancedMoveSamples=%d PolledMoveSamples=%d FastWalkStarted=%d FastWalkStopped=%d PolledFastWalk=%s PolledActNatural=%d"),
		*GetClass()->GetName(),
		IsLocalController() ? TEXT("true") : TEXT("false"),
		*GetNameSafe(ControlledPawn),
		ControlledPawn ? *ControlledPawn->GetClass()->GetName() : TEXT("None"),
		*GetNameSafe(InputComponent),
		AlienLocomotionComponent ? TEXT("true") : TEXT("false"),
		LegacyAlienMoveInput.X,
		LegacyAlienMoveInput.Y,
		LastEnhancedAlienMoveInput.X,
		LastEnhancedAlienMoveInput.Y,
		LastPolledAlienMoveInput.X,
		LastPolledAlienMoveInput.Y,
		LegacyForwardAxisSamples,
		LegacyRightAxisSamples,
		LegacyMovePushes,
		bLastLegacyPushHadLocomotion ? TEXT("true") : TEXT("false"),
		EnhancedMoveSamples,
		PolledMoveSamples,
		FastWalkStartedSamples,
		FastWalkStoppedSamples,
		bLastPolledFastWalkRequested ? TEXT("true") : TEXT("false"),
		PolledActNaturalSamples);
}

FString AVNHPlayerController::GetRoleStatusText() const
{
	const AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn());
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;

	if (Shopper && Shopper->IsPossessedByAlien())
	{
		return TEXT("ROLE: ALIEN  //  POSSESSED SHOPPER");
	}

	switch (AssignedRole)
	{
	case EVNHPlayerRole::Human:
		return TEXT("ROLE: HUMAN  //  BLEND IN");
	case EVNHPlayerRole::Alien:
		return TEXT("ROLE: ALIEN  //  AWAITING POSSESSION");
	case EVNHPlayerRole::Hunter:
		return TEXT("ROLE: HUNTER  //  WATCH. QUESTION. ACCUSE.");
	default:
		return TEXT("ROLE: UNASSIGNED  //  WAITING FOR ROUND");
	}
}

FString AVNHPlayerController::GetLocomotionStatusText() const
{
	const UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent();
	if (!AlienLocomotionComponent)
	{
		const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
		const AVNHShopperCharacter* PossessedShopper = VNHGameState ? Cast<AVNHShopperCharacter>(VNHGameState->GetPossessedShopper()) : nullptr;
		AlienLocomotionComponent = PossessedShopper ? PossessedShopper->GetAlienLocomotionComponent() : nullptr;
	}

	if (!AlienLocomotionComponent)
	{
		return TEXT("CONTROL: HUNTER VIEW  //  TARGET SHOPPERS WITH RMB");
	}

	const FVNHAlienLocomotionState State = AlienLocomotionComponent->GetLocomotionState();
	return FString::Printf(
		TEXT("LOCO: %.0f/%.0f CM/S  //  FAST %s  //  BRAKE %s  //  TURN %.0f  //  STABLE %.0f%%"),
		State.CurrentSpeed,
		State.DesiredSpeed,
		State.bFastWalkRequested ? TEXT("ON") : TEXT("OFF"),
		State.bManualBrake ? TEXT("ON") : TEXT("OFF"),
		State.BodyYawDeltaDegrees,
		State.Stability * 100.0f);
}

FString AVNHPlayerController::GetRoundStatusText() const
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return TEXT("Round 0 | Waiting | 0s | Tests 2");
	}

	const float PhaseEndsAt = VNHGameState->GetPhaseEndsAtServerTime();
	const float RemainingSeconds = PhaseEndsAt > 0.0f ? FMath::Max(0.0f, PhaseEndsAt - VNHGameState->GetServerWorldTimeSeconds()) : 0.0f;

	return FString::Printf(
		TEXT("Round %d | %s | %.0fs | Tests %d | Questions %d"),
		VNHGameState->GetRoundNumber(),
		ToPhaseText(VNHGameState->GetRoundPhase()),
		RemainingSeconds,
		VNHGameState->GetTestsRemaining(),
		VNHGameState->GetQuestionsRemaining());
}

FString AVNHPlayerController::GetDebugDeckInteractionText() const
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (VNHGameState && VNHGameState->GetRoundPhase() == EVNHRoundPhase::Reveal)
	{
		return TEXT("Reveal active // watch the result panel");
	}

	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!LastInteractionText.IsEmpty() && Now - LastInteractionTimeSeconds <= 7.0f)
	{
		return LastInteractionText;
	}

	const AVNHShopperCharacter* Targeted = TargetedShopper.Get();
	if (Targeted)
	{
		return FString::Printf(TEXT("LOCKED: %s  //  [E] ASK  [R] MARK  [F] FAKE  [X] ACCUSE  [LMB] CANCEL"), *GetNameSafe(Targeted));
	}

	const FString PromptText = GetInteractionPromptText();
	if (!PromptText.IsEmpty())
	{
		return PromptText;
	}

	return TEXT("AIM AT SHOPPER  //  RMB LOCKS ACTION TARGET");
}

FString AVNHPlayerController::GetRevealStatusText() const
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState || VNHGameState->GetRoundPhase() != EVNHRoundPhase::Reveal)
	{
		return FString();
	}

	const FVNHAccusationResult Result = VNHGameState->GetAccusationResult();
	const AActor* ActualAlien = VNHGameState->GetPossessedShopper();
	if (!Result.bResolved)
	{
		return FString::Printf(TEXT("REVEAL // ALIEN WINS BY TIME // Hidden Alien: %s"), *GetNameSafe(ActualAlien));
	}

	if (!Result.bCorrect)
	{
		return FString::Printf(
			TEXT("REVEAL // WRONGFULLY ACCUSED // Spotlight %s // Alien %s"),
			*GetNameSafe(Result.AccusedActor),
			*GetNameSafe(ActualAlien));
	}

	return FString::Printf(
		TEXT("REVEAL // HUNTER WINS // Caught %s // Alien %s"),
		*GetNameSafe(Result.AccusedActor),
		*GetNameSafe(ActualAlien));
}

void AVNHPlayerController::RequestPublicTest(EVNHPublicTestType TestType)
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can trigger public commands.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestPublicTest(TestType);
	LastInteractionText = FString::Printf(TEXT("Public test triggered: %s."), ToPublicTestText(TestType));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::RequestAccusation(AVNHShopperCharacter* AccusedShopper)
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can make the real accusation.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestAccusation(AccusedShopper);
}

void AVNHPlayerController::RequestActNatural()
{
	ServerRequestActNatural();
	LastInteractionText = TEXT("Act Natural requested.");
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::RequestInteract()
{
	if (FocusedLobbyPlayButton.IsValid())
	{
		RequestStartRoundFromLobby();
		return;
	}

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("Lobby")))
	{
		LastInteractionText = TEXT("Look at the PLAY button and press E to start.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can use the direct question.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState || VNHGameState->GetRoundPhase() != EVNHRoundPhase::Investigation)
	{
		LastInteractionText = TEXT("Question unavailable until Investigation.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	if (VNHGameState->GetQuestionsRemaining() <= 0)
	{
		LastInteractionText = TEXT("Direct questions already used this round.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	AVNHShopperCharacter* Shopper = GetInteractionShopper();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No target. Aim at a shopper and right-click to lock.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
		return;
	}

	ServerRequestDirectQuestion(Shopper);
}

void AVNHPlayerController::MarkFocusedShopper()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can mark suspects.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	AVNHShopperCharacter* Shopper = GetInteractionShopper();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No target to mark. Aim and right-click a shopper.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	for (int32 Index = MarkedSuspects.Num() - 1; Index >= 0; --Index)
	{
		if (!MarkedSuspects[Index].IsValid())
		{
			MarkedSuspects.RemoveAt(Index);
		}
	}

	for (const TWeakObjectPtr<AVNHShopperCharacter>& MarkedSuspect : MarkedSuspects)
	{
		if (MarkedSuspect.Get() == Shopper)
		{
			LastInteractionText = FString::Printf(TEXT("Already marked %s."), *GetNameSafe(Shopper));
			LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
			return;
		}
	}

	if (MarkedSuspects.Num() >= 3)
	{
		LastInteractionText = TEXT("Suspect list full. Three marks max.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	MarkedSuspects.Add(Shopper);
	RefreshShopperOutline(Shopper, Shopper == FocusedShopper.Get());
	LastInteractionText = FString::Printf(TEXT("Marked suspect %d: %s."), MarkedSuspects.Num(), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::FakeAccuseFocusedShopper()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can apply accusation pressure.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	AVNHShopperCharacter* Shopper = GetInteractionShopper();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No target to pressure. Aim and right-click a shopper.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	LastInteractionText = FString::Printf(TEXT("Fake accusation pressure on %s."), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
}

void AVNHPlayerController::DebugAccuseFocusedShopper()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can accuse.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	AVNHShopperCharacter* Shopper = GetInteractionShopper();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No target to accuse. Aim and right-click a shopper.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		VNHGameMode->DebugResolveAccusation(Shopper);
		LastInteractionText = FString::Printf(TEXT("Accused %s."), *GetNameSafe(Shopper));
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	}
}

void AVNHPlayerController::CancelTargetSelection()
{
	if (!TargetedShopper.IsValid())
	{
		return;
	}

	ClearTargetedShopper();
	if (FocusedShopper.IsValid())
	{
		RefreshShopperOutline(FocusedShopper.Get(), true);
	}
	LastInteractionText = TEXT("Target selection cancelled.");
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::RequestQuickChat(EVNHQuickChatLine Line)
{
	ServerRequestQuickChat(Line);
}

void AVNHPlayerController::RequestStartRoundFromLobby()
{
	ServerRequestStartRoundFromLobby();
}

void AVNHPlayerController::RequestDebugPossessShopper(int32 ShopperIndex, EVNHPlayerRole ForcedRole)
{
	ServerDebugPossessShopper(ShopperIndex, ForcedRole);
}

void AVNHPlayerController::ServerRequestPublicTest_Implementation(EVNHPublicTestType TestType)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestPublicTest(this, TestType);
	}
}

void AVNHPlayerController::ServerRequestQuestion_Implementation(AVNHShopperCharacter* QuestionedShopper)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestQuestion(this, QuestionedShopper);
	}
}

void AVNHPlayerController::ServerRequestAccusation_Implementation(AVNHShopperCharacter* AccusedShopper)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestAccusation(this, AccusedShopper);
	}
}

void AVNHPlayerController::ServerRequestDirectQuestion_Implementation(AVNHShopperCharacter* QuestionedShopper)
{
	FString ResponseText;
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		if (VNHGameMode->RequestDirectQuestion(this, QuestionedShopper, ResponseText))
		{
			ClientReceiveInteractionText(ResponseText);
			return;
		}
	}

	ClientReceiveInteractionText(TEXT("Direct question unavailable."));
}

void AVNHPlayerController::ServerRequestQuickChat_Implementation(EVNHQuickChatLine Line)
{
	if (AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr)
	{
		VNHGameState->PublishQuickChat(PlayerState, Line);
		ClientReceiveInteractionText(VNHGameState->GetLastQuickChatMessage().Text.ToString());
	}
}

void AVNHPlayerController::ServerRequestStartRoundFromLobby_Implementation()
{
	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		if (!VNHGameMode->StartRoundFromLobby(this))
		{
			ClientReceiveInteractionText(TEXT("Only the host can start when enough players are connected."));
		}
	}
}

void AVNHPlayerController::ServerDebugPossessShopper_Implementation(int32 ShopperIndex, EVNHPlayerRole ForcedRole)
{
	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		VNHGameMode->DebugPossessShopperByIndex(ShopperIndex, ForcedRole);
	}
}

void AVNHPlayerController::ServerRequestActNatural_Implementation()
{
	if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn()))
	{
		const bool bUsed = Shopper->UseActNatural();
		LastInteractionText = bUsed ? TEXT("Act Natural used.") : TEXT("Act Natural unavailable.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	}
}

void AVNHPlayerController::ClientReceiveInteractionText_Implementation(const FString& InteractionText)
{
	LastInteractionText = InteractionText;
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
}

void AVNHPlayerController::ClientShowLobbyMenu_Implementation()
{
	ShowLobbyMenu();
}

UVNHAlienLocomotionComponent* AVNHPlayerController::GetAlienLocomotionComponent() const
{
	const AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn());
	if (!Shopper)
	{
		return nullptr;
	}

	return Shopper->GetAlienLocomotionComponent();
}

void AVNHPlayerController::UpdateAlienInputMapping()
{
	if (!IsLocalController() || !AlienInputMappingContext)
	{
		return;
	}

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(LocalPlayer))
		{
			const bool bShouldApplyAlienMapping = GetAlienLocomotionComponent() != nullptr;
			if (bShouldApplyAlienMapping && !bAlienInputMappingApplied)
			{
				InputSubsystem->AddMappingContext(AlienInputMappingContext, AlienInputMappingPriority);
				bAlienInputMappingApplied = true;
			}
			else if (!bShouldApplyAlienMapping && bAlienInputMappingApplied)
			{
				InputSubsystem->RemoveMappingContext(AlienInputMappingContext);
				bAlienInputMappingApplied = false;
			}
		}
	}
}

void AVNHPlayerController::BindAlienInputActions()
{
	UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInputComponent)
	{
		return;
	}

	if (AlienMoveAction)
	{
		EnhancedInputComponent->BindAction(AlienMoveAction, ETriggerEvent::Triggered, this, &AVNHPlayerController::HandleAlienMoveInput);
		EnhancedInputComponent->BindAction(AlienMoveAction, ETriggerEvent::Completed, this, &AVNHPlayerController::HandleAlienMoveStopped);
		EnhancedInputComponent->BindAction(AlienMoveAction, ETriggerEvent::Canceled, this, &AVNHPlayerController::HandleAlienMoveStopped);
	}

	if (AlienFastWalkAction)
	{
		EnhancedInputComponent->BindAction(AlienFastWalkAction, ETriggerEvent::Started, this, &AVNHPlayerController::HandleAlienFastWalkStarted);
		EnhancedInputComponent->BindAction(AlienFastWalkAction, ETriggerEvent::Completed, this, &AVNHPlayerController::HandleAlienFastWalkStopped);
		EnhancedInputComponent->BindAction(AlienFastWalkAction, ETriggerEvent::Canceled, this, &AVNHPlayerController::HandleAlienFastWalkStopped);
	}

	if (AlienActNaturalAction)
	{
		EnhancedInputComponent->BindAction(AlienActNaturalAction, ETriggerEvent::Triggered, this, &AVNHPlayerController::RequestActNatural);
	}
}

void AVNHPlayerController::HandleAlienMoveInput(const FInputActionValue& Value)
{
	LastEnhancedAlienMoveInput = Value.Get<FVector2D>();
	++EnhancedMoveSamples;
	if (UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent())
	{
		AlienLocomotionComponent->SetMoveInput(LastEnhancedAlienMoveInput);
	}
}

void AVNHPlayerController::HandleAlienMoveStopped()
{
	if (UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent())
	{
		AlienLocomotionComponent->SetMoveInput(FVector2D::ZeroVector);
	}
}

void AVNHPlayerController::HandleAlienFastWalkStarted()
{
	++FastWalkStartedSamples;
	if (UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent())
	{
		AlienLocomotionComponent->SetFastWalkRequested(true);
	}
}

void AVNHPlayerController::HandleAlienFastWalkStopped()
{
	++FastWalkStoppedSamples;
	if (UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent())
	{
		AlienLocomotionComponent->SetFastWalkRequested(false);
	}
}

void AVNHPlayerController::HandleAlienMoveForwardAxis(float Value)
{
	LegacyAlienMoveInput.Y = Value;
	if (!FMath::IsNearlyZero(Value))
	{
		++LegacyForwardAxisSamples;
	}
	PushLegacyAlienMoveInput();
}

void AVNHPlayerController::HandleAlienMoveRightAxis(float Value)
{
	LegacyAlienMoveInput.X = Value;
	if (!FMath::IsNearlyZero(Value))
	{
		++LegacyRightAxisSamples;
	}
	PushLegacyAlienMoveInput();
}

void AVNHPlayerController::HandleTurnAxis(float Value)
{
	if (FMath::IsNearlyZero(Value))
	{
		return;
	}

	AddYawInput(Value);
}

void AVNHPlayerController::HandleLookUpAxis(float Value)
{
	if (FMath::IsNearlyZero(Value))
	{
		return;
	}

	AddPitchInput(Value);
}

void AVNHPlayerController::HandleTargetFocusPressed()
{
	UpdateFocusedShopper();

	AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	if (!Shopper)
	{
		ClearTargetedShopper();
		LastInteractionText = TEXT("No shopper under cursor to target.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	SetTargetedShopper(Shopper);
	LastInteractionText = FString::Printf(TEXT("Target locked: %s // E Ask | R Mark | F Fake | X Accuse | LMB Cancel"), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::HandleInteractPressed()
{
	RequestInteract();
}

void AVNHPlayerController::HandleQuickChatPressed()
{
	RequestQuickChat(EVNHQuickChatLine::JustBrowsing);
}

void AVNHPlayerController::HandleQuickChatLookingForShirtPressed()
{
	RequestQuickChat(EVNHQuickChatLine::LookingForShirt);
}

void AVNHPlayerController::HandleQuickChatWaitingForFriendPressed()
{
	RequestQuickChat(EVNHQuickChatLine::WaitingForFriend);
}

void AVNHPlayerController::HandleQuickChatNoThanksPressed()
{
	RequestQuickChat(EVNHQuickChatLine::NoThanks);
}

void AVNHPlayerController::HandleQuickChatFoundWrongSizePressed()
{
	RequestQuickChat(EVNHQuickChatLine::FoundWrongSize);
}

void AVNHPlayerController::ToggleDebugHud()
{
	AVNHDebugHUD* DebugHUD = GetHUD<AVNHDebugHUD>();
	if (!DebugHUD)
	{
		return;
	}

	DebugHUD->ToggleDebugPanel();
	ApplyDebugHudInputMode(DebugHUD->IsDebugPanelVisible());
}

void AVNHPlayerController::ApplyDebugHudInputMode(bool bDebugHudVisible)
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	DefaultMouseCursor = EMouseCursor::Default;
	UpdateGameplayCursor();

	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
}

void AVNHPlayerController::ShowLobbyMenu()
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	if (LobbyMenuWidget.IsValid())
	{
		return;
	}

	UClass* LobbyWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_LobbyMenu.WBP_LobbyMenu_C"));
	if (!LobbyWidgetClass)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenu: could not load /Game/UI/WBP_LobbyMenu."));
		return;
	}

	UUserWidget* NewLobbyMenu = CreateWidget<UUserWidget>(this, LobbyWidgetClass);
	if (!NewLobbyMenu)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenu: CreateWidget failed."));
		return;
	}

	NewLobbyMenu->AddToViewport(7500);
	NewLobbyMenu->SetVisibility(ESlateVisibility::HitTestInvisible);
	LobbyMenuWidget = NewLobbyMenu;

	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	DefaultMouseCursor = EMouseCursor::Default;
	UpdateGameplayCursor();
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);

	UE_LOG(LogVNH, Display, TEXT("LobbyMenu: shown as non-blocking overlay."));
}

void AVNHPlayerController::UpdateDebugDeckRuntimeLabels(float DeltaTime)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	TimeUntilDebugDeckLabelLookup -= DeltaTime;
	if ((!RoundStatusTextBlock.IsValid()
		|| !InteractionTextBlock.IsValid()
		|| !RoleStatusTextBlock.IsValid()
		|| !LocomotionStatusTextBlock.IsValid()
		|| !RevealStatusTextBlock.IsValid()
		|| !RevealStatusBoxWidget.IsValid()
		|| !RevealStatusShadowWidget.IsValid()
		|| !RevealRailWidget.IsValid()) && TimeUntilDebugDeckLabelLookup <= 0.0f)
	{
		TimeUntilDebugDeckLabelLookup = 0.5f;

		TArray<UUserWidget*> Widgets;
		UWidgetBlueprintLibrary::GetAllWidgetsOfClass(GetWorld(), Widgets, UUserWidget::StaticClass(), false);
		for (UUserWidget* Widget : Widgets)
		{
			if (!Widget || !Widget->GetClass()->GetName().Contains(TEXT("WBP_VNHDebugDeck")))
			{
				continue;
			}

			if (!RoundStatusTextBlock.IsValid())
			{
				RoundStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("RoundStatusText")));
			}

			if (!InteractionTextBlock.IsValid())
			{
				InteractionTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("InteractionText")));
			}

			if (!RoleStatusTextBlock.IsValid())
			{
				RoleStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("RoleStatusText")));
			}

			if (!LocomotionStatusTextBlock.IsValid())
			{
				LocomotionStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("LocomotionStatusText")));
			}

			if (!RevealStatusTextBlock.IsValid())
			{
				RevealStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("RevealStatusText")));
			}

			if (!RevealStatusBoxWidget.IsValid())
			{
				RevealStatusBoxWidget = Widget->GetWidgetFromName(TEXT("RevealStatusBox"));
			}

			if (!RevealStatusShadowWidget.IsValid())
			{
				RevealStatusShadowWidget = Widget->GetWidgetFromName(TEXT("RevealStatusShadow"));
			}

			if (!RevealRailWidget.IsValid())
			{
				RevealRailWidget = Widget->GetWidgetFromName(TEXT("RevealRail"));
			}
		}
	}

	if (UTextBlock* RoundText = RoundStatusTextBlock.Get())
	{
		RoundText->SetText(FText::FromString(GetRoundStatusText()));
	}

	if (UTextBlock* InteractionText = InteractionTextBlock.Get())
	{
		InteractionText->SetText(FText::FromString(GetDebugDeckInteractionText()));
	}

	if (UTextBlock* RoleText = RoleStatusTextBlock.Get())
	{
		RoleText->SetText(FText::FromString(GetRoleStatusText()));
	}

	if (UTextBlock* LocomotionText = LocomotionStatusTextBlock.Get())
	{
		LocomotionText->SetText(FText::FromString(GetLocomotionStatusText()));
	}

	const FString RevealStatusText = GetRevealStatusText();
	const bool bShowReveal = !RevealStatusText.IsEmpty();
	if (UTextBlock* RevealText = RevealStatusTextBlock.Get())
	{
		RevealText->SetText(FText::FromString(RevealStatusText));
		RevealText->SetVisibility(bShowReveal ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		RevealText->SetRenderOpacity(bShowReveal ? 1.0f : 0.0f);
	}

	if (UWidget* RevealBox = RevealStatusBoxWidget.Get())
	{
		RevealBox->SetVisibility(bShowReveal ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		RevealBox->SetRenderOpacity(bShowReveal ? 1.0f : 0.0f);
	}

	if (UWidget* RevealShadow = RevealStatusShadowWidget.Get())
	{
		RevealShadow->SetVisibility(bShowReveal ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		RevealShadow->SetRenderOpacity(bShowReveal ? 1.0f : 0.0f);
	}

	if (UWidget* RevealRail = RevealRailWidget.Get())
	{
		RevealRail->SetVisibility(bShowReveal ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		RevealRail->SetRenderOpacity(bShowReveal ? 1.0f : 0.0f);
	}
}

void AVNHPlayerController::UpdateMarkedSuspectsWidgetRuntimeLabels(float DeltaTime)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	EnsureMarkedSuspectsWidget();
	if (!MarkedSuspectsWidget.IsValid())
	{
		return;
	}

	TimeUntilMarkedWidgetLookup -= DeltaTime;
	if ((!MarkedSuspectsListTextBlock.IsValid() || !MarkedSuspectsPanelWidget.IsValid()) && TimeUntilMarkedWidgetLookup <= 0.0f)
	{
		TimeUntilMarkedWidgetLookup = 0.5f;
		UUserWidget* Widget = MarkedSuspectsWidget.Get();
		MarkedSuspectsListTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("MarkedSuspectsListText"))) : nullptr;
		MarkedSuspectsPanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("MarkedSuspectsPanel")) : nullptr;
	}

	const FString PanelText = GetMarkedSuspectsPanelText();
	const bool bShowPanel = IsAssignedHunter() && !PanelText.IsEmpty();
	if (UTextBlock* ListText = MarkedSuspectsListTextBlock.Get())
	{
		ListText->SetText(FText::FromString(PanelText));
	}

	if (UWidget* Panel = MarkedSuspectsPanelWidget.Get())
	{
		Panel->SetVisibility(bShowPanel ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		Panel->SetRenderOpacity(bShowPanel ? 1.0f : 0.0f);
	}
}

void AVNHPlayerController::UpdateMarkedSuspectsForRound()
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return;
	}

	const int32 CurrentRoundNumber = VNHGameState->GetRoundNumber();
	if (LastMarkedRoundNumber == INDEX_NONE)
	{
		LastMarkedRoundNumber = CurrentRoundNumber;
	}
	else if (LastMarkedRoundNumber != CurrentRoundNumber)
	{
		ClearMarkedSuspectsForNewRound();
		LastMarkedRoundNumber = CurrentRoundNumber;
	}

	if (!IsAssignedHunter() && !MarkedSuspects.IsEmpty())
	{
		ClearMarkedSuspectsForNewRound();
	}
}

void AVNHPlayerController::PushLegacyAlienMoveInput()
{
	const bool bLegacyInputChanged = !LegacyAlienMoveInput.Equals(LastPushedLegacyAlienMoveInput);
	if (LegacyAlienMoveInput.IsNearlyZero() && !bLegacyInputChanged)
	{
		return;
	}

	++LegacyMovePushes;
	if (UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent())
	{
		bLastLegacyPushHadLocomotion = true;
		LastPushedLegacyAlienMoveInput = LegacyAlienMoveInput;
		AlienLocomotionComponent->SetMoveInput(LegacyAlienMoveInput);
		return;
	}

	bLastLegacyPushHadLocomotion = false;
	LastPushedLegacyAlienMoveInput = LegacyAlienMoveInput;
}

void AVNHPlayerController::PollAlienKeyboardInput()
{
	if (!bEnablePolledAlienKeyboardInput || !IsLocalController())
	{
		return;
	}

	UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent();
	if (!AlienLocomotionComponent)
	{
		LastPolledAlienMoveInput = FVector2D::ZeroVector;
		bLastPolledFastWalkRequested = false;
		bWasPolledActNaturalDown = false;
		bWasPolledTargetFocusDown = false;
		bWasPolledCancelTargetDown = false;
		return;
	}

	FVector2D PolledMoveInput = FVector2D::ZeroVector;
	PolledMoveInput.Y += IsInputKeyDown(EKeys::W) ? 1.0f : 0.0f;
	PolledMoveInput.Y -= IsInputKeyDown(EKeys::S) ? 1.0f : 0.0f;
	PolledMoveInput.X += IsInputKeyDown(EKeys::D) ? 1.0f : 0.0f;
	PolledMoveInput.X -= IsInputKeyDown(EKeys::A) ? 1.0f : 0.0f;
	PolledMoveInput = PolledMoveInput.GetClampedToMaxSize(1.0f);

	const bool bFastWalkRequested = IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);
	const bool bActNaturalDown = IsInputKeyDown(EKeys::Q);
	const bool bInteractDown = IsInputKeyDown(EKeys::E);
	const bool bPolledMoveChanged = !PolledMoveInput.Equals(LastPolledAlienMoveInput);
	const bool bPolledFastWalkChanged = bFastWalkRequested != bLastPolledFastWalkRequested;

	if (bPolledMoveChanged || bPolledFastWalkChanged)
	{
		++PolledMoveSamples;
		LastPolledAlienMoveInput = PolledMoveInput;
		bLastPolledFastWalkRequested = bFastWalkRequested;
	}

	if (!PolledMoveInput.IsNearlyZero() || bPolledMoveChanged || bFastWalkRequested || bPolledFastWalkChanged)
	{
		AlienLocomotionComponent->SetMoveInput(PolledMoveInput);
		AlienLocomotionComponent->SetFastWalkRequested(bFastWalkRequested);
	}

	if (bActNaturalDown && !bWasPolledActNaturalDown)
	{
		++PolledActNaturalSamples;
		RequestActNatural();
	}

	if (bInteractDown && !bWasPolledInteractDown)
	{
		RequestInteract();
	}

	bWasPolledActNaturalDown = bActNaturalDown;
	bWasPolledInteractDown = bInteractDown;
}

void AVNHPlayerController::PollInteractionInput()
{
	if (!IsLocalController())
	{
		return;
	}

	const bool bTargetFocusDown = IsInputKeyDown(EKeys::RightMouseButton);
	const bool bInteractDown = IsInputKeyDown(EKeys::E);
	const bool bMarkDown = IsInputKeyDown(EKeys::R);
	const bool bFakeAccuseDown = IsInputKeyDown(EKeys::F);
	const bool bAccuseDown = IsInputKeyDown(EKeys::X);
	const bool bCancelTargetDown = IsInputKeyDown(EKeys::LeftMouseButton);

	if (bTargetFocusDown && !bWasPolledTargetFocusDown)
	{
		HandleTargetFocusPressed();
	}

	if (bInteractDown && !bWasPolledInteractDown)
	{
		RequestInteract();
	}

	if (bMarkDown && !bWasPolledMarkDown)
	{
		MarkFocusedShopper();
	}

	if (bFakeAccuseDown && !bWasPolledFakeAccuseDown)
	{
		FakeAccuseFocusedShopper();
	}

	if (bAccuseDown && !bWasPolledAccuseDown)
	{
		DebugAccuseFocusedShopper();
	}

	if (bCancelTargetDown && !bWasPolledCancelTargetDown)
	{
		CancelTargetSelection();
	}

	bWasPolledTargetFocusDown = bTargetFocusDown;
	bWasPolledInteractDown = bInteractDown;
	bWasPolledMarkDown = bMarkDown;
	bWasPolledFakeAccuseDown = bFakeAccuseDown;
	bWasPolledAccuseDown = bAccuseDown;
	bWasPolledCancelTargetDown = bCancelTargetDown;
}

void AVNHPlayerController::UpdateGameplayCursor()
{
	if (!IsLocalController())
	{
		return;
	}

	EMouseCursor::Type DesiredCursor = EMouseCursor::Default;
	if (IsAssignedHunter() && FocusedShopper.IsValid())
	{
		DesiredCursor = EMouseCursor::Crosshairs;
	}
	else if (FocusedShopper.IsValid() || FocusedLobbyPlayButton.IsValid())
	{
		DesiredCursor = EMouseCursor::Hand;
	}

	CurrentMouseCursor = DesiredCursor;
}

void AVNHPlayerController::RegisterGameplayHardwareCursors()
{
	if (!IsLocalController())
	{
		return;
	}

	const bool bNormalRegistered = UWidgetBlueprintLibrary::SetHardwareCursor(
		this,
		EMouseCursor::Default,
		TEXT("UI/Cursors/T_NormalCursor"),
		FVector2D::ZeroVector);
	const bool bRegularRegistered = UWidgetBlueprintLibrary::SetHardwareCursor(
		this,
		EMouseCursor::Hand,
		TEXT("UI/Cursors/T_RegularInteract"),
		FVector2D::ZeroVector);
	const bool bHunterRegistered = UWidgetBlueprintLibrary::SetHardwareCursor(
		this,
		EMouseCursor::Crosshairs,
		TEXT("UI/Cursors/T_HunterInteract"),
		FVector2D::ZeroVector);

	bHardwareCursorsRegistered = bNormalRegistered && bRegularRegistered && bHunterRegistered;
	HardwareCursorRegistrationRetrySeconds = bHardwareCursorsRegistered ? 0.0f : 1.0f;

	UE_LOG(
		LogVNH,
		bHardwareCursorsRegistered ? Display : Warning,
		TEXT("HardwareCursors: Normal=%s Regular=%s Hunter=%s"),
		bNormalRegistered ? TEXT("true") : TEXT("false"),
		bRegularRegistered ? TEXT("true") : TEXT("false"),
		bHunterRegistered ? TEXT("true") : TEXT("false"));

	DefaultMouseCursor = EMouseCursor::Default;
	UpdateGameplayCursor();
}

void AVNHPlayerController::UpdateRoleCameraMode()
{
	if (!IsLocalController())
	{
		return;
	}

	if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn()))
	{
		Shopper->SetFirstPersonViewEnabled(IsAssignedHunter());
	}
}

FString AVNHPlayerController::GetInteractionPromptText() const
{
	if (FocusedLobbyPlayButton.IsValid())
	{
		return TEXT("E: Start round");
	}

	if (const AVNHShopperCharacter* Shopper = TargetedShopper.Get())
	{
		return FString::Printf(TEXT("LOCKED: %s  //  E ASK  R MARK  F FAKE  X ACCUSE  LMB CANCEL"), *GetNameSafe(Shopper));
	}

	if (!IsAssignedHunter())
	{
		return FString();
	}

	const AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	return Shopper ? FString::Printf(TEXT("RMB LOCK: %s"), *GetNameSafe(Shopper)) : FString();
}

FString AVNHPlayerController::GetMarkedSuspectsText() const
{
	if (MarkedSuspects.IsEmpty())
	{
		return FString();
	}

	FString Result(TEXT("Marked: "));
	int32 VisibleIndex = 0;
	for (const TWeakObjectPtr<AVNHShopperCharacter>& MarkedSuspect : MarkedSuspects)
	{
		const AVNHShopperCharacter* Shopper = MarkedSuspect.Get();
		if (!Shopper)
		{
			continue;
		}

		if (VisibleIndex > 0)
		{
			Result += TEXT(", ");
		}

		Result += GetNameSafe(Shopper);
		++VisibleIndex;
	}

	return VisibleIndex > 0 ? Result : FString();
}

FString AVNHPlayerController::GetMarkedSuspectsPanelText() const
{
	FString Result;
	int32 VisibleIndex = 0;
	for (const TWeakObjectPtr<AVNHShopperCharacter>& MarkedSuspect : MarkedSuspects)
	{
		const AVNHShopperCharacter* Shopper = MarkedSuspect.Get();
		if (!Shopper)
		{
			continue;
		}

		++VisibleIndex;
		if (!Result.IsEmpty())
		{
			Result += LINE_TERMINATOR;
		}

		Result += FString::Printf(TEXT("%d  %s"), VisibleIndex, *GetNameSafe(Shopper));
	}

	return Result;
}

bool AVNHPlayerController::IsShopperMarked(const AVNHShopperCharacter* Shopper) const
{
	if (!Shopper)
	{
		return false;
	}

	for (const TWeakObjectPtr<AVNHShopperCharacter>& MarkedSuspect : MarkedSuspects)
	{
		if (MarkedSuspect.Get() == Shopper)
		{
			return true;
		}
	}

	return false;
}

void AVNHPlayerController::RefreshShopperOutline(AVNHShopperCharacter* Shopper, bool bHovered) const
{
	if (!Shopper)
	{
		return;
	}

	int32 StencilValue = 0;
	if (Shopper == TargetedShopper.Get())
	{
		StencilValue = TargetedShopperStencil;
	}
	else if (IsShopperMarked(Shopper))
	{
		StencilValue = MarkedShopperStencil;
	}
	else if (bHovered)
	{
		StencilValue = HoveredShopperStencil;
	}

	Shopper->SetInteractionHighlightStencil(StencilValue);
}

void AVNHPlayerController::ClearMarkedSuspectsForNewRound()
{
	TArray<TWeakObjectPtr<AVNHShopperCharacter>> PreviousMarks = MarkedSuspects;
	MarkedSuspects.Reset();

	for (const TWeakObjectPtr<AVNHShopperCharacter>& PreviousMark : PreviousMarks)
	{
		if (AVNHShopperCharacter* Shopper = PreviousMark.Get())
		{
			RefreshShopperOutline(Shopper, Shopper == FocusedShopper.Get());
		}
	}
}

void AVNHPlayerController::UpdateFocusedShopper()
{
	AVNHShopperCharacter* PreviousFocusedShopper = FocusedShopper.Get();
	FocusedShopper.Reset();
	FocusedLobbyPlayButton.Reset();

	if (!PlayerCameraManager || !GetWorld())
	{
		if (PreviousFocusedShopper)
		{
			RefreshShopperOutline(PreviousFocusedShopper, false);
		}
		return;
	}

	FVector MouseWorldLocation = FVector::ZeroVector;
	FVector MouseWorldDirection = FVector::ForwardVector;
	if (!DeprojectMousePositionToWorld(MouseWorldLocation, MouseWorldDirection))
	{
		if (PreviousFocusedShopper)
		{
			RefreshShopperOutline(PreviousFocusedShopper, false);
		}
		return;
	}

	const FVector MouseTraceEnd = MouseWorldLocation + MouseWorldDirection * 2500.0f;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VNHInteractionCursorTrace), false);
	if (APawn* ControlledPawn = GetPawn())
	{
		QueryParams.AddIgnoredActor(ControlledPawn);
	}

	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, MouseWorldLocation, MouseTraceEnd, ECC_Visibility, QueryParams))
	{
		if (AVNHLobbyPlayButton* LobbyPlayButton = Cast<AVNHLobbyPlayButton>(Hit.GetActor()))
		{
			FocusedLobbyPlayButton = LobbyPlayButton;
			if (PreviousFocusedShopper)
			{
				RefreshShopperOutline(PreviousFocusedShopper, false);
			}
			return;
		}
	}

	const bool bIsHunter = IsAssignedHunter();
	if (!bIsHunter)
	{
		ClearTargetedShopper();
	}

	if (GetWorld()->LineTraceSingleByChannel(Hit, MouseWorldLocation, MouseTraceEnd, ECC_Pawn, QueryParams))
	{
		if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(Hit.GetActor()))
		{
			FocusedShopper = Shopper;
			if (!bIsHunter)
			{
				if (PreviousFocusedShopper)
				{
					RefreshShopperOutline(PreviousFocusedShopper, false);
				}
				return;
			}

			if (Shopper == TargetedShopper.Get())
			{
				RefreshShopperOutline(Shopper, true);
				return;
			}

			if (Shopper != PreviousFocusedShopper)
			{
				if (PreviousFocusedShopper)
				{
					RefreshShopperOutline(PreviousFocusedShopper, false);
				}

				RefreshShopperOutline(Shopper, true);
			}
			return;
		}
	}

	if (PreviousFocusedShopper)
	{
		RefreshShopperOutline(PreviousFocusedShopper, false);
	}

	if (AVNHShopperCharacter* Target = TargetedShopper.Get())
	{
		RefreshShopperOutline(Target, false);
	}
}

AVNHShopperCharacter* AVNHPlayerController::GetInteractionShopper() const
{
	if (AVNHShopperCharacter* Shopper = TargetedShopper.Get())
	{
		return Shopper;
	}

	return FocusedShopper.Get();
}

bool AVNHPlayerController::IsAssignedHunter() const
{
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	return VNHPlayerState && VNHPlayerState->IsHunter();
}

void AVNHPlayerController::SetTargetedShopper(AVNHShopperCharacter* NewTarget)
{
	if (TargetedShopper.Get() == NewTarget)
	{
		return;
	}

	ClearTargetedShopper();
	TargetedShopper = NewTarget;
	if (NewTarget)
	{
		RefreshShopperOutline(NewTarget, NewTarget == FocusedShopper.Get());
	}
}

void AVNHPlayerController::ClearTargetedShopper()
{
	if (AVNHShopperCharacter* PreviousTarget = TargetedShopper.Get())
	{
		TargetedShopper.Reset();
		RefreshShopperOutline(PreviousTarget, PreviousTarget == FocusedShopper.Get());
		return;
	}

	TargetedShopper.Reset();
}
