#include "VNHPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/TextBlock.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "EngineUtils.h"
#include "VNHGameMode.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHDebugHUD.h"
#include "VNHLog.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"

void AVNHPlayerController::BeginPlay()
{
	Super::BeginPlay();
	UpdateAlienInputMapping();
	ApplyDebugHudInputMode(true);
}

void AVNHPlayerController::AcknowledgePossession(APawn* PossessedPawn)
{
	Super::AcknowledgePossession(PossessedPawn);
	UpdateAlienInputMapping();
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
	InputComponent->BindAction(TEXT("VNH_Interact"), IE_Pressed, this, &AVNHPlayerController::HandleInteractPressed);
	InputComponent->BindAction(TEXT("VNH_MarkSuspect"), IE_Pressed, this, &AVNHPlayerController::MarkFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_FakeAccuse"), IE_Pressed, this, &AVNHPlayerController::FakeAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_Accuse"), IE_Pressed, this, &AVNHPlayerController::DebugAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_ToggleDebugHud"), IE_Pressed, this, &AVNHPlayerController::ToggleDebugHud);

	UE_LOG(LogVNH, Display, TEXT("AlienInput: setup complete. Controller=%s InputComponent=%s EnhancedComponent=%s"),
		*GetClass()->GetName(),
		*GetNameSafe(InputComponent),
		Cast<UEnhancedInputComponent>(InputComponent) ? TEXT("true") : TEXT("false"));
}

void AVNHPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	UpdateFocusedShopper();
	PollAlienKeyboardInput();
	UpdateDebugDeckRuntimeLabels(DeltaTime);
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
	case EVNHPlayerRole::Alien:
		return TEXT("ROLE: ALIEN  //  AWAITING POSSESSION");
	case EVNHPlayerRole::Hunter:
		return TEXT("ROLE: HUMAN  //  HUNTER");
	default:
		return TEXT("ROLE: HUMAN  //  UNASSIGNED");
	}
}

FString AVNHPlayerController::GetLocomotionStatusText() const
{
	const UVNHAlienLocomotionComponent* AlienLocomotionComponent = GetAlienLocomotionComponent();
	if (!AlienLocomotionComponent)
	{
		return TEXT("LOCO: HUMAN OBSERVER  //  POSSESS A SHOPPER TO TEST 11.3");
	}

	const FVNHAlienLocomotionState State = AlienLocomotionComponent->GetLocomotionState();
	return FString::Printf(
		TEXT("LOCO: %.0f/%.0f CM/S  //  FAST %s  //  BRAKE %s  //  TURN %.0f"),
		State.CurrentSpeed,
		State.DesiredSpeed,
		State.bFastWalkRequested ? TEXT("ON") : TEXT("OFF"),
		State.bManualBrake ? TEXT("ON") : TEXT("OFF"),
		State.BodyYawDeltaDegrees);
}

void AVNHPlayerController::RequestPublicTest(EVNHPublicTestType TestType)
{
	ServerRequestPublicTest(TestType);
}

void AVNHPlayerController::RequestAccusation(AVNHShopperCharacter* AccusedShopper)
{
	ServerRequestAccusation(AccusedShopper);
}

void AVNHPlayerController::RequestActNatural()
{
	ServerRequestActNatural();
}

void AVNHPlayerController::RequestInteract()
{
	AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No shopper in focus.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
		return;
	}

	LastInteractionText = FString::Printf(TEXT("%s: %s"), *GetNameSafe(Shopper), *Shopper->BuildQuestionResponse());
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
}

void AVNHPlayerController::MarkFocusedShopper()
{
	AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No shopper in focus to mark.");
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
	LastInteractionText = FString::Printf(TEXT("Marked suspect %d: %s."), MarkedSuspects.Num(), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::FakeAccuseFocusedShopper()
{
	AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No shopper in focus to pressure.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	LastInteractionText = FString::Printf(TEXT("Fake accusation pressure on %s."), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	UE_LOG(LogVNH, Display, TEXT("Interaction: %s"), *LastInteractionText);
}

void AVNHPlayerController::DebugAccuseFocusedShopper()
{
	AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	if (!Shopper)
	{
		LastInteractionText = TEXT("No shopper in focus to accuse.");
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

void AVNHPlayerController::ServerRequestPublicTest_Implementation(EVNHPublicTestType TestType)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestPublicTest(this, TestType);
	}
}

void AVNHPlayerController::ServerRequestAccusation_Implementation(AVNHShopperCharacter* AccusedShopper)
{
	if (AVNHGameMode* VNHGameMode = GetWorld()->GetAuthGameMode<AVNHGameMode>())
	{
		VNHGameMode->RequestAccusation(this, AccusedShopper);
	}
}

void AVNHPlayerController::ServerRequestActNatural_Implementation()
{
	if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn()))
	{
		Shopper->UseActNatural();
	}
}

UVNHAlienLocomotionComponent* AVNHPlayerController::GetAlienLocomotionComponent() const
{
	const AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn());
	if (!Shopper || !Shopper->IsPossessedByAlien())
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
	if (FMath::IsNearlyZero(Value) || !GetAlienLocomotionComponent())
	{
		return;
	}

	AddYawInput(Value);
}

void AVNHPlayerController::HandleLookUpAxis(float Value)
{
	if (FMath::IsNearlyZero(Value) || !GetAlienLocomotionComponent())
	{
		return;
	}

	AddPitchInput(Value);
}

void AVNHPlayerController::HandleInteractPressed()
{
	RequestInteract();
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
	bShowMouseCursor = bDebugHudVisible;
	bEnableClickEvents = bDebugHudVisible;
	bEnableMouseOverEvents = bDebugHudVisible;

	if (bDebugHudVisible)
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);
	}
	else
	{
		SetInputMode(FInputModeGameOnly());
	}
}

void AVNHPlayerController::UpdateDebugDeckRuntimeLabels(float DeltaTime)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	TimeUntilDebugDeckLabelLookup -= DeltaTime;
	if ((!RoleStatusTextBlock.IsValid() || !LocomotionStatusTextBlock.IsValid()) && TimeUntilDebugDeckLabelLookup <= 0.0f)
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

			if (!RoleStatusTextBlock.IsValid())
			{
				RoleStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("RoleStatusText")));
			}

			if (!LocomotionStatusTextBlock.IsValid())
			{
				LocomotionStatusTextBlock = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("LocomotionStatusText")));
			}
		}
	}

	if (UTextBlock* RoleText = RoleStatusTextBlock.Get())
	{
		RoleText->SetText(FText::FromString(GetRoleStatusText()));
	}

	if (UTextBlock* LocomotionText = LocomotionStatusTextBlock.Get())
	{
		LocomotionText->SetText(FText::FromString(GetLocomotionStatusText()));
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

FString AVNHPlayerController::GetInteractionPromptText() const
{
	const AVNHShopperCharacter* Shopper = FocusedShopper.Get();
	return Shopper ? FString::Printf(TEXT("E: Question %s"), *GetNameSafe(Shopper)) : FString();
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

void AVNHPlayerController::UpdateFocusedShopper()
{
	FocusedShopper.Reset();

	if (!PlayerCameraManager || !GetWorld())
	{
		return;
	}

	const FVector Start = PlayerCameraManager->GetCameraLocation();
	const FVector End = Start + PlayerCameraManager->GetCameraRotation().Vector() * 900.0f;

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VNHInteractionTrace), false);
	if (APawn* ControlledPawn = GetPawn())
	{
		QueryParams.AddIgnoredActor(ControlledPawn);
	}

	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Pawn, QueryParams))
	{
		if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(Hit.GetActor()))
		{
			FocusedShopper = Shopper;
			return;
		}
	}

	const FVector ViewDirection = PlayerCameraManager->GetCameraRotation().Vector();
	AVNHShopperCharacter* BestShopper = nullptr;
	float BestScore = -1.0f;

	for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
	{
		AVNHShopperCharacter* Shopper = *It;
		if (!IsValid(Shopper) || Shopper == GetPawn())
		{
			continue;
		}

		const FVector ToShopper = Shopper->GetActorLocation() - Start;
		const float Distance = ToShopper.Size();
		if (Distance > 1100.0f || Distance < KINDA_SMALL_NUMBER)
		{
			continue;
		}

		const float FacingDot = FVector::DotProduct(ViewDirection, ToShopper / Distance);
		if (FacingDot < 0.55f)
		{
			continue;
		}

		const float Score = FacingDot * 2.0f - Distance / 1100.0f;
		if (Score > BestScore)
		{
			BestScore = Score;
			BestShopper = Shopper;
		}
	}

	FocusedShopper = BestShopper;
}
