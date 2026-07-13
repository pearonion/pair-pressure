#include "VNHPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimInstance.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PostProcessComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ProgressBar.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/Widget.h"
#include "Engine/DataTable.h"
#include "GameFramework/SaveGame.h"
#include "GameFramework/Character.h"
#include "Engine/Scene.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UnrealType.h"
#include "EngineUtils.h"
#include "VNHGameMode.h"
#include "VNHGameState.h"
#include "VNHAlienLocomotionComponent.h"
#include "VNHCharacterCustomizerWidget.h"
#include "VNHDebugHUD.h"
#include "VNHGameInstance.h"
#include "VNHLobbyPlayButton.h"
#include "VNHLobbyMenuWidget.h"
#include "VNHLog.h"
#include "VNHPlayerState.h"
#include "VNHShopperCharacter.h"
#include "PairPressure/PPPlayerActionRouterComponent.h"

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
constexpr float HumanDrillCooldownSeconds = 30.0f;
constexpr float HumanDrillPromptSeconds = 5.0f;
constexpr float EveryonePointCooldownSeconds = 60.0f;
constexpr float LobbyStartHoldRequiredSeconds = 1.5f;
constexpr int32 EveryonePointMaxUsesPerRound = 2;
constexpr int32 RoleHudActionSlotCount = 6;
constexpr float RoleHudFartCooldownDisplayTolerance = 0.05f;
constexpr float ThrowMaxChargeSeconds = 1.5f;
constexpr float ThrowMinSpeed = 850.0f;
constexpr float ThrowMaxSpeed = 2400.0f;
constexpr const TCHAR* VNHSettingsSection = TEXT("/Script/VNHSimulator.VNHSettings");
constexpr const TCHAR* VNHSettingsSaveSlot = TEXT("PlayerSettings");

const TCHAR* const RoleHudActionSuffixes[RoleHudActionSlotCount] =
{
	TEXT("Inspect"),
	TEXT("Wave"),
	TEXT("Point"),
	TEXT("Laugh"),
	TEXT("Fart"),
	TEXT("PlaceDecoy")
};

const TCHAR* const HunterRoleHudActionSuffixes[RoleHudActionSlotCount] =
{
	TEXT("Mark"),
	TEXT("Accuse"),
	TEXT("Pressure"),
	TEXT("HumanDrill"),
	TEXT("EveryonePoint"),
	TEXT("FakeDrill")
};

const TCHAR* const RoleHudKeyboardHotkeyLabels[RoleHudActionSlotCount] =
{
	TEXT("1"),
	TEXT("2"),
	TEXT("3"),
	TEXT("4"),
	TEXT("5"),
	TEXT("6")
};

const TCHAR* const RoleHudControllerHotkeyLabels[RoleHudActionSlotCount] =
{
	TEXT("L3"),
	TEXT("R3"),
	TEXT("Y/TR"),
	TEXT("X/SQ"),
	TEXT("VW/CR"),
	TEXT("MN/OP")
};

struct FVNHRuntimeSettingsValues
{
	bool bInvertLook = false;
	float MouseSensitivity = 0.5f;
};

float GetFloatPropertyValue(const UObject* Object, const FName PropertyName, float DefaultValue)
{
	if (!Object)
	{
		return DefaultValue;
	}

	if (const FFloatProperty* Property = FindFProperty<FFloatProperty>(Object->GetClass(), PropertyName))
	{
		return Property->GetPropertyValue_InContainer(Object);
	}

	return DefaultValue;
}

bool GetBoolPropertyValue(const UObject* Object, const FName PropertyName, bool DefaultValue)
{
	if (!Object)
	{
		return DefaultValue;
	}

	if (const FBoolProperty* Property = FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName))
	{
		return Property->GetPropertyValue_InContainer(Object);
	}

	return DefaultValue;
}

FVNHRuntimeSettingsValues LoadSavedRuntimeSettings()
{
	FVNHRuntimeSettingsValues Values;
	if (GConfig)
	{
		GConfig->GetBool(VNHSettingsSection, TEXT("bInvertLook"), Values.bInvertLook, GGameUserSettingsIni);
		GConfig->GetFloat(VNHSettingsSection, TEXT("MouseSensitivity"), Values.MouseSensitivity, GGameUserSettingsIni);
	}

	if (UGameplayStatics::DoesSaveGameExist(VNHSettingsSaveSlot, 0))
	{
		if (USaveGame* SaveGame = UGameplayStatics::LoadGameFromSlot(VNHSettingsSaveSlot, 0))
		{
			Values.bInvertLook = GetBoolPropertyValue(SaveGame, TEXT("InvertLook"), Values.bInvertLook);
			Values.MouseSensitivity = GetFloatPropertyValue(SaveGame, TEXT("MouseSensitivity"), Values.MouseSensitivity);
		}
	}

	Values.MouseSensitivity = FMath::Clamp(Values.MouseSensitivity, 0.0f, 1.0f);
	return Values;
}

const FVNHRuntimeSettingsValues& GetCachedSavedRuntimeSettings()
{
	static FVNHRuntimeSettingsValues CachedValues;
	static double LastLoadSeconds = -1.0;

	const double CurrentSeconds = FPlatformTime::Seconds();
	if (LastLoadSeconds < 0.0 || CurrentSeconds - LastLoadSeconds > 0.25)
	{
		CachedValues = LoadSavedRuntimeSettings();
		LastLoadSeconds = CurrentSeconds;
	}

	return CachedValues;
}

float GetSavedLookSensitivityMultiplier()
{
	return FMath::Lerp(0.2f, 2.0f, GetCachedSavedRuntimeSettings().MouseSensitivity);
}

bool GetSavedInvertLook()
{
	return GetCachedSavedRuntimeSettings().bInvertLook;
}

bool IsControllerInputPresetSelected()
{
	FString InputPreset;
	if (GConfig)
	{
		GConfig->GetString(VNHSettingsSection, TEXT("InputPreset"), InputPreset, GGameUserSettingsIni);
	}

	return InputPreset == TEXT("Controller");
}

const TCHAR* ToUniversalActionText(EVNHUniversalAction Action)
{
	switch (Action)
	{
	case EVNHUniversalAction::Inspect:
		return TEXT("INSPECT");
	case EVNHUniversalAction::Point:
		return TEXT("POINT");
	case EVNHUniversalAction::Wave:
		return TEXT("WAVE");
	case EVNHUniversalAction::Laugh:
		return TEXT("LAUGH");
	case EVNHUniversalAction::Fart:
		return TEXT("FART");
	case EVNHUniversalAction::PlaceDecoy:
		return TEXT("PLACE DECOY");
	case EVNHUniversalAction::PickUp:
		return TEXT("PICK UP");
	case EVNHUniversalAction::Drop:
		return TEXT("DROP");
	case EVNHUniversalAction::Throw:
		return TEXT("THROW");
	default:
		return TEXT("NONE");
	}
}

const TCHAR* ToHumanDrillActionText(EVNHHumanDrillAction Action)
{
	switch (Action)
	{
	case EVNHHumanDrillAction::Wave:
		return TEXT("WAVE");
	case EVNHHumanDrillAction::Point:
		return TEXT("POINT");
	case EVNHHumanDrillAction::Laugh:
		return TEXT("LAUGH");
	case EVNHHumanDrillAction::Jump:
		return TEXT("JUMP");
	case EVNHHumanDrillAction::Crouch:
		return TEXT("CROUCH");
	case EVNHHumanDrillAction::PickUpNearestItem:
		return TEXT("PICK UP NEAREST ITEM");
	case EVNHHumanDrillAction::None:
	default:
		return TEXT("NONE");
	}
}

EVNHHumanDrillAction ChooseRandomHumanDrillAction()
{
	static constexpr EVNHHumanDrillAction DrillActions[] =
	{
		EVNHHumanDrillAction::Wave,
		EVNHHumanDrillAction::Point,
		EVNHHumanDrillAction::Laugh,
		EVNHHumanDrillAction::Jump,
		EVNHHumanDrillAction::Crouch,
		EVNHHumanDrillAction::PickUpNearestItem
	};

	return DrillActions[FMath::RandRange(0, UE_ARRAY_COUNT(DrillActions) - 1)];
}

FName ToUniversalActionRowName(EVNHUniversalAction Action)
{
	switch (Action)
	{
	case EVNHUniversalAction::Inspect:
		return TEXT("Inspect");
	case EVNHUniversalAction::Point:
		return TEXT("Point");
	case EVNHUniversalAction::Wave:
		return TEXT("Wave");
	case EVNHUniversalAction::Laugh:
		return TEXT("Laugh");
	case EVNHUniversalAction::PlaceDecoy:
		return TEXT("PlaceDecoy");
	case EVNHUniversalAction::PickUp:
		return TEXT("PickUp");
	case EVNHUniversalAction::Drop:
		return TEXT("Drop");
	case EVNHUniversalAction::Throw:
		return TEXT("Throw");
	default:
		return NAME_None;
	}
}

bool IsRoleAnimationAction(EVNHUniversalAction Action)
{
	switch (Action)
	{
	case EVNHUniversalAction::Inspect:
	case EVNHUniversalAction::Point:
	case EVNHUniversalAction::Wave:
	case EVNHUniversalAction::Laugh:
	case EVNHUniversalAction::PlaceDecoy:
	case EVNHUniversalAction::Throw:
		return true;
	default:
		return false;
	}
}

const FArrayProperty* FindRoleActionArrayFieldByPrefix(const UScriptStruct* RowStruct, const TCHAR* FieldPrefix)
{
	if (!RowStruct || !FieldPrefix)
	{
		return nullptr;
	}

	const FString PrefixString(FieldPrefix);
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(*It);
		if (ArrayProperty && ArrayProperty->GetName().StartsWith(PrefixString))
		{
			return ArrayProperty;
		}
	}

	return nullptr;
}

UAnimMontage* ChooseRoleActionMontageFromField(const uint8* RowData, const FArrayProperty* ArrayProperty)
{
	if (!RowData || !ArrayProperty)
	{
		return nullptr;
	}

	const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
	if (!ObjectProperty || !ObjectProperty->PropertyClass || !ObjectProperty->PropertyClass->IsChildOf(UAnimMontage::StaticClass()))
	{
		return nullptr;
	}

	FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(RowData));
	TArray<UAnimMontage*> Montages;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		if (UAnimMontage* Montage = Cast<UAnimMontage>(ObjectProperty->GetObjectPropertyValue(ArrayHelper.GetRawPtr(Index))))
		{
			Montages.Add(Montage);
		}
	}

	if (Montages.IsEmpty())
	{
		return nullptr;
	}

	return Montages[FMath::RandRange(0, Montages.Num() - 1)];
}

bool IsVNHInteractable(const AActor* Actor)
{
	if (!Actor)
	{
		return false;
	}

	const FString ClassName = Actor->GetClass()->GetName();
	return Actor->ActorHasTag(TEXT("VNH.Interactable"))
		|| ClassName.Contains(TEXT("BP_VNHInteractableBase"))
		|| ClassName.Contains(TEXT("BP_VNHProp_"));
}

bool IsVNHSuspicious(const AActor* Actor)
{
	return Actor && (Actor->ActorHasTag(TEXT("VNH.Suspicious")) || Actor->GetClass()->GetName().Contains(TEXT("Suspicious")));
}

bool IsPlayerControllerMainMenuWorld(const UWorld* World)
{
	return World && World->GetMapName().Contains(TEXT("MainMenu"));
}

bool IsCustomizationPhase(const AVNHGameState* VNHGameState)
{
	if (!VNHGameState)
	{
		return false;
	}

	const EVNHRoundPhase RoundPhase = VNHGameState->GetRoundPhase();
	return RoundPhase == EVNHRoundPhase::WaitingForPlayers
		|| RoundPhase == EVNHRoundPhase::AssigningRoles;
}

bool IsCharacterCustomizerScreenOpen(const APlayerController* PlayerController)
{
	const UVNHGameInstance* VNHGameInstance = PlayerController ? PlayerController->GetGameInstance<UVNHGameInstance>() : nullptr;
	return VNHGameInstance && VNHGameInstance->IsCharacterCustomizerOpen();
}
}

AVNHPlayerController::AVNHPlayerController()
{
	HumanActionAnimationTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_HumanActionAnimations.DT_HumanActionAnimations")));
	AlienActionAnimationTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_AlienActionAnimations.DT_AlienActionAnimations")));
}

void AVNHPlayerController::BeginPlay()
{
	Super::BeginPlay();

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	const bool bIsMainMenuMap = IsPlayerControllerMainMenuWorld(GetWorld());
	if (bIsMainMenuMap)
	{
		RemoveComposureWidget();
	}

	if (!bIsMainMenuMap)
	{
		EnsureTargetOutlinePostProcess();
		EnsureRoleHudWidget();
		EnsureMarkedSuspectsWidget();
		RemoveComposureWidget();
		RegisterGameplayHardwareCursors();
		EnsurePairPressureHUD();
	}

	if (MapName.Contains(TEXT("Lobby")))
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
	ApplyDebugHudInputMode(false);
	if (!bIsMainMenuMap)
	{
		ApplySavedCharacterCustomization();
	}
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

void AVNHPlayerController::EnsureRoleHudWidget()
{
	if (!IsLocalController() || IsPlayerControllerMainMenuWorld(GetWorld())
		|| (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_"))))
	{
		return;
	}

	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;

	const TCHAR* WidgetPath = nullptr;
	switch (AssignedRole)
	{
	case EVNHPlayerRole::Hunter:
		WidgetPath = TEXT("/Game/UI/WBP_HunterHUD.WBP_HunterHUD_C");
		break;
	case EVNHPlayerRole::Human:
		WidgetPath = TEXT("/Game/UI/WBP_HumanHUD.WBP_HumanHUD_C");
		break;
	case EVNHPlayerRole::Alien:
		WidgetPath = TEXT("/Game/UI/WBP_AlienHUD.WBP_AlienHUD_C");
		break;
	default:
		break;
	}

	if (!WidgetPath)
	{
		if (RoleHudWidget.IsValid())
		{
			RoleHudWidget->RemoveFromParent();
			RoleHudWidget.Reset();
			RoleHudRoundTimerTextBlock.Reset();
			RoleHudComposureStateTextBlock.Reset();
			RoleHudComposureValueTextBlock.Reset();
			RoleHudComposureProgressBar.Reset();
			RoleHudDrillPromptPanelWidget.Reset();
			RoleHudDrillPromptTextBlock.Reset();
			RoleHudHumanDrillCooldownPanelWidget.Reset();
			RoleHudHumanDrillCooldownTextBlock.Reset();
			RoleHudEveryonePointCooldownPanelWidget.Reset();
			RoleHudEveryonePointCooldownTextBlock.Reset();
			for (int32 SlotIndex = 0; SlotIndex < RoleHudActionSlotCount; ++SlotIndex)
			{
				RoleHudActionHotkeyTextBlocks[SlotIndex].Reset();
			}
			RoleHudFartCooldownPanelWidget.Reset();
			RoleHudFartCooldownTextBlock.Reset();
			RoleHudDisplayedFartCooldownRemaining = 0.0f;
			RoleHudLastReplicatedFartCooldownRemaining = 0.0f;
		}
		ActiveRoleHudRole = EVNHPlayerRole::Unassigned;
		return;
	}

	if (RoleHudWidget.IsValid() && ActiveRoleHudRole == AssignedRole)
	{
		return;
	}

	if (RoleHudWidget.IsValid())
	{
		RoleHudWidget->RemoveFromParent();
		RoleHudWidget.Reset();
		RoleHudRoundTimerTextBlock.Reset();
		RoleHudComposureStateTextBlock.Reset();
		RoleHudComposureValueTextBlock.Reset();
		RoleHudComposureProgressBar.Reset();
		RoleHudDrillPromptPanelWidget.Reset();
		RoleHudDrillPromptTextBlock.Reset();
		RoleHudHumanDrillCooldownPanelWidget.Reset();
		RoleHudHumanDrillCooldownTextBlock.Reset();
		RoleHudEveryonePointCooldownPanelWidget.Reset();
		RoleHudEveryonePointCooldownTextBlock.Reset();
		for (int32 SlotIndex = 0; SlotIndex < RoleHudActionSlotCount; ++SlotIndex)
		{
			RoleHudActionHotkeyTextBlocks[SlotIndex].Reset();
		}
		RoleHudFartCooldownPanelWidget.Reset();
		RoleHudFartCooldownTextBlock.Reset();
		RoleHudDisplayedFartCooldownRemaining = 0.0f;
		RoleHudLastReplicatedFartCooldownRemaining = 0.0f;
	}

	UClass* WidgetClass = LoadClass<UUserWidget>(nullptr, WidgetPath);
	if (!WidgetClass)
	{
		UE_LOG(LogVNH, Warning, TEXT("RoleHUD: could not load %s."), WidgetPath);
		ActiveRoleHudRole = EVNHPlayerRole::Unassigned;
		return;
	}

	UUserWidget* NewWidget = CreateWidget<UUserWidget>(this, WidgetClass);
	if (!NewWidget)
	{
		UE_LOG(LogVNH, Warning, TEXT("RoleHUD: CreateWidget failed for %s."), WidgetPath);
		ActiveRoleHudRole = EVNHPlayerRole::Unassigned;
		return;
	}

	NewWidget->AddToViewport(6200);
	NewWidget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	RoleHudWidget = NewWidget;
	ActiveRoleHudRole = AssignedRole;
	TimeUntilRoleHudWidgetLookup = 0.0f;
	BindRoleHudActionButtons();
	int32 ReticleViewportX = 0;
	int32 ReticleViewportY = 0;
	GetViewportSize(ReticleViewportX, ReticleViewportY);
	VirtualCursorPosition = FVector2D(ReticleViewportX * 0.5f, ReticleViewportY * 0.5f);
	bVirtualCursorInitialized = ReticleViewportX > 0 && ReticleViewportY > 0;
	UpdateRoleHudWidgetRuntimeLabels(0.0f);
}

void AVNHPlayerController::EnsureMarkedSuspectsWidget()
{
	if (!IsLocalController() || MarkedSuspectsWidget.IsValid()
		|| (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_"))))
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

void AVNHPlayerController::EnsureComposureWidget()
{
	if (!IsLocalController() || ComposureWidget.IsValid() || IsPlayerControllerMainMenuWorld(GetWorld())
		|| (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("PP_"))))
	{
		return;
	}

	UClass* WidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_VNHComposure.WBP_VNHComposure_C"));
	if (!WidgetClass)
	{
		return;
	}

	UUserWidget* NewWidget = CreateWidget<UUserWidget>(this, WidgetClass);
	if (!NewWidget)
	{
		return;
	}

	NewWidget->AddToViewport(6300);
	ComposureWidget = NewWidget;
	TimeUntilComposureWidgetLookup = 0.0f;
	BindComposureWidgetButtons();
	UpdateComposureWidgetRuntimeLabels(0.0f);
}

void AVNHPlayerController::RemoveComposureWidget()
{
	if (ComposureWidget.IsValid())
	{
		ComposureWidget->RemoveFromParent();
		ComposureWidget.Reset();
	}

	ComposurePanelWidget.Reset();
	ComposureStateTextBlock.Reset();
	ComposureValueTextBlock.Reset();
	FartRiskTextBlock.Reset();
	UniversalActionTextBlock.Reset();
	ComposureProgressBar.Reset();
	HudCustomizeButton.Reset();
	TimeUntilComposureWidgetLookup = 0.0f;

	if (UClass* WidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_VNHComposure.WBP_VNHComposure_C")))
	{
		TArray<UUserWidget*> ExistingComposureWidgets;
		UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, ExistingComposureWidgets, WidgetClass, false);
		for (UUserWidget* Widget : ExistingComposureWidgets)
		{
			if (Widget)
			{
				Widget->RemoveFromParent();
			}
		}
	}
}

void AVNHPlayerController::BindComposureWidgetButtons()
{
	UUserWidget* Widget = ComposureWidget.Get();
	if (!Widget)
	{
		HudCustomizeButton.Reset();
		return;
	}

	if (UButton* CustomizeButton = Cast<UButton>(Widget->GetWidgetFromName(TEXT("CustomizeButton"))))
	{
		CustomizeButton->OnClicked.AddUniqueDynamic(this, &AVNHPlayerController::HandleHudCustomizeClicked);
		HudCustomizeButton = CustomizeButton;
	}
}

void AVNHPlayerController::HandleHudCustomizeClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
	{
		VNHGameInstance->ShowCharacterCustomizer(true);
	}
}

void AVNHPlayerController::AcknowledgePossession(APawn* PossessedPawn)
{
	Super::AcknowledgePossession(PossessedPawn);
	UpdateAlienInputMapping();
	UpdateRoleCameraMode();
	RegisterGameplayHardwareCursors();
	ApplySavedCharacterCustomization();
	EnsurePairPressureHUD();
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
	InputComponent->BindAxis(TEXT("Turn Right / Left Gamepad"), this, &AVNHPlayerController::HandleTurnAxis);
	InputComponent->BindAxis(TEXT("Look Up / Down Gamepad"), this, &AVNHPlayerController::HandleLookUpAxis);
	InputComponent->BindAxis(TEXT("VNH_CursorX"), this, &AVNHPlayerController::HandleCursorXAxis);
	InputComponent->BindAxis(TEXT("VNH_CursorY"), this, &AVNHPlayerController::HandleCursorYAxis);
	InputComponent->BindAction(TEXT("VNH_AlienFastWalk"), IE_Pressed, this, &AVNHPlayerController::HandleAlienFastWalkStarted);
	InputComponent->BindAction(TEXT("VNH_AlienFastWalk"), IE_Released, this, &AVNHPlayerController::HandleAlienFastWalkStopped);
	InputComponent->BindAction(TEXT("VNH_AlienActNatural"), IE_Pressed, this, &AVNHPlayerController::HandleThrowChargePressed);
	InputComponent->BindAction(TEXT("VNH_AlienActNatural"), IE_Released, this, &AVNHPlayerController::HandleThrowChargeReleased);
	InputComponent->BindAction(TEXT("VNH_TargetFocus"), IE_Pressed, this, &AVNHPlayerController::HandleTargetFocusPressed);
	InputComponent->BindAction(TEXT("VNH_Interact"), IE_Pressed, this, &AVNHPlayerController::HandleInteractPressed);
	InputComponent->BindAction(TEXT("Jump"), IE_Pressed, this, &AVNHPlayerController::HandleJumpPressed);
	InputComponent->BindAction(TEXT("Jump"), IE_Released, this, &AVNHPlayerController::HandleJumpReleased);
	InputComponent->BindAction(TEXT("Crouch"), IE_Pressed, this, &AVNHPlayerController::HandleCrouchPressed);
	InputComponent->BindAction(TEXT("Crouch"), IE_Released, this, &AVNHPlayerController::HandleCrouchReleased);
	InputComponent->BindKey(EKeys::One, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot1Pressed);
	InputComponent->BindKey(EKeys::Gamepad_LeftThumbstick, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot1Pressed);
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot2Pressed);
	InputComponent->BindKey(EKeys::Gamepad_RightThumbstick, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot2Pressed);
	InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot3Pressed);
	InputComponent->BindKey(EKeys::Gamepad_FaceButton_Top, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot3Pressed);
	InputComponent->BindKey(EKeys::Four, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot4Pressed);
	InputComponent->BindKey(EKeys::Gamepad_FaceButton_Left, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot4Pressed);
	InputComponent->BindKey(EKeys::Five, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot5Pressed);
	InputComponent->BindKey(EKeys::Gamepad_Special_Left, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot5Pressed);
	InputComponent->BindKey(EKeys::Six, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot6Pressed);
	InputComponent->BindKey(EKeys::Gamepad_Special_Right, IE_Pressed, this, &AVNHPlayerController::HandleRoleHudActionSlot6Pressed);
	InputComponent->BindAction(TEXT("VNH_PickUp"), IE_Pressed, this, &AVNHPlayerController::HandlePickUpPressed);
	InputComponent->BindAction(TEXT("VNH_Drop"), IE_Pressed, this, &AVNHPlayerController::HandleDropPressed);
	InputComponent->BindAction(TEXT("VNH_MarkSuspect"), IE_Pressed, this, &AVNHPlayerController::MarkFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_FakeAccuse"), IE_Pressed, this, &AVNHPlayerController::FakeAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_Accuse"), IE_Pressed, this, &AVNHPlayerController::DebugAccuseFocusedShopper);
	InputComponent->BindAction(TEXT("VNH_QuickChat"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_Shirt"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatLookingForShirtPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_Friend"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatWaitingForFriendPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_NoThanks"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatNoThanksPressed);
	InputComponent->BindAction(TEXT("VNH_QuickChat_WrongSize"), IE_Pressed, this, &AVNHPlayerController::HandleQuickChatFoundWrongSizePressed);
	InputComponent->BindAction(TEXT("VNH_ToggleDebugHud"), IE_Pressed, this, &AVNHPlayerController::ToggleDebugHud);
	InputComponent->BindAction(TEXT("PP_Dive"), IE_Pressed, this, &AVNHPlayerController::HandlePairPressureDivePressed);
	InputComponent->BindAction(TEXT("PP_Assist"), IE_Pressed, this, &AVNHPlayerController::HandlePairPressureAssistPressed);
	InputComponent->BindAction(TEXT("PP_Assist"), IE_Released, this, &AVNHPlayerController::HandlePairPressureAssistReleased);
	InputComponent->BindAction(TEXT("PP_Grab"), IE_Pressed, this, &AVNHPlayerController::HandlePairPressureAssistPressed);
	InputComponent->BindAction(TEXT("PP_Grab"), IE_Released, this, &AVNHPlayerController::HandlePairPressureAssistReleased);

	UE_LOG(LogVNH, Display, TEXT("AlienInput: setup complete. Controller=%s InputComponent=%s EnhancedComponent=%s"),
		*GetClass()->GetName(),
		*GetNameSafe(InputComponent),
		Cast<UEnhancedInputComponent>(InputComponent) ? TEXT("true") : TEXT("false"));
}

void AVNHPlayerController::HandlePairPressureDivePressed()
{
	if (!GetWorld() || !GetWorld()->GetMapName().Contains(TEXT("PP_")))
	{
		return;
	}

	if (APawn* ControlledPawn = GetPawn())
	{
		if (UPPPlayerActionRouterComponent* ActionRouter = ControlledPawn->FindComponentByClass<UPPPlayerActionRouterComponent>())
		{
			ActionRouter->RequestDive();
		}
	}
}

void AVNHPlayerController::HandlePairPressureAssistPressed()
{
	if (!GetWorld() || !GetWorld()->GetMapName().Contains(TEXT("PP_")))
	{
		return;
	}

	if (APawn* ControlledPawn = GetPawn())
	{
		if (UPPPlayerActionRouterComponent* ActionRouter = ControlledPawn->FindComponentByClass<UPPPlayerActionRouterComponent>())
		{
			ActionRouter->BeginAssist();
		}
	}
}

void AVNHPlayerController::HandlePairPressureAssistReleased()
{
	if (APawn* ControlledPawn = GetPawn())
	{
		if (UPPPlayerActionRouterComponent* ActionRouter = ControlledPawn->FindComponentByClass<UPPPlayerActionRouterComponent>())
		{
			ActionRouter->EndAssist();
		}
	}
}

void AVNHPlayerController::EnsurePairPressureHUD()
{
	if (!IsLocalController() || !GetLocalPlayer() || !GetWorld()
		|| !GetWorld()->GetMapName().Contains(TEXT("PP_")) || PairPressureHUDWidget.IsValid())
	{
		return;
	}

	UClass* HUDClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/PairPressure/UI/HUD/WBP_PP_HUD_Root.WBP_PP_HUD_Root_C"));
	if (!HUDClass)
	{
		return;
	}

	UUserWidget* NewHUDWidget = CreateWidget<UUserWidget>(this, HUDClass);
	if (NewHUDWidget)
	{
		NewHUDWidget->AddToViewport(25);
		PairPressureHUDWidget = NewHUDWidget;
	}
}

void AVNHPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (IsPlayerControllerMainMenuWorld(GetWorld()))
	{
		RemoveLobbyMenu();
		return;
	}

	if (MapName.Contains(TEXT("Lobby")))
	{
		ShowLobbyMenu();
	}
	else
	{
		RemoveLobbyMenu();
	}

	if (!bHardwareCursorsRegistered)
	{
		HardwareCursorRegistrationRetrySeconds -= DeltaTime;
		if (HardwareCursorRegistrationRetrySeconds <= 0.0f)
		{
			RegisterGameplayHardwareCursors();
		}
	}
	UpdateMarkedSuspectsForRound();
	UpdatePreRoundCustomizationFlow();
	UpdateRoleCameraMode();
	UpdateFocusedShopper();
	UpdateGameplayCursor();
	UpdateLobbyStartHold(DeltaTime);
	PollAlienKeyboardInput();
	PollInteractionInput();
	UpdateRoleHudWidgetRuntimeLabels(DeltaTime);
	UpdateCursorReticleAndHeadLook(DeltaTime);
	UpdateDebugDeckRuntimeLabels(DeltaTime);
	UpdateMarkedSuspectsWidgetRuntimeLabels(DeltaTime);
}

void AVNHPlayerController::BindRoleHudActionButtons()
{
	if (!RoleHudWidget.IsValid())
	{
		return;
	}

	BindRoleHudActionButton(TEXT("ActionButton_Inspect"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudInspectClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Wave"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudWaveClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Point"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudPointClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Laugh"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudLaughClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Fart"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudFartClicked));
	BindRoleHudActionButton(TEXT("ActionButton_PlaceDecoy"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudPlaceDecoyClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Throw"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudThrowClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Mark"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudMarkClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Accuse"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudAccuseClicked));
	BindRoleHudActionButton(TEXT("ActionButton_Pressure"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudPressureClicked));
	BindRoleHudActionButton(TEXT("ActionButton_HumanDrill"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudHumanDrillClicked));
	BindRoleHudActionButton(TEXT("ActionButton_FakeDrill"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudFakeDrillClicked));
	BindRoleHudActionButton(TEXT("ActionButton_EveryonePoint"), GET_FUNCTION_NAME_CHECKED(AVNHPlayerController, HandleHudEveryonePointClicked));
}

void AVNHPlayerController::BindRoleHudActionButton(FName ButtonName, FName HandlerName)
{
	UUserWidget* Widget = RoleHudWidget.Get();
	UButton* Button = Widget ? Cast<UButton>(Widget->GetWidgetFromName(ButtonName)) : nullptr;
	if (!Button)
	{
		return;
	}

	FScriptDelegate Delegate;
	Delegate.BindUFunction(this, HandlerName);
	Button->OnClicked.AddUnique(Delegate);
}

void AVNHPlayerController::ExecuteRoleHudActionSlot(int32 SlotIndex)
{
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	if (AssignedRole == EVNHPlayerRole::Hunter)
	{
		switch (SlotIndex)
		{
		case 0:
			MarkFocusedShopper();
			break;
		case 1:
			DebugAccuseFocusedShopper();
			break;
		case 2:
			FakeAccuseFocusedShopper();
			break;
		case 3:
			RequestHumanDrill();
			break;
		case 4:
			RequestEveryonePoint();
			break;
		case 5:
			RequestFakeDrill();
			break;
		default:
			break;
		}
		return;
	}

	if (AssignedRole != EVNHPlayerRole::Human && AssignedRole != EVNHPlayerRole::Alien)
	{
		return;
	}

	switch (SlotIndex)
	{
	case 0:
		HandleInspectPressed();
		break;
	case 1:
		HandleWavePressed();
		break;
	case 2:
		HandlePointPressed();
		break;
	case 3:
		HandleLaughPressed();
		break;
	case 4:
		HandleFartPressed();
		break;
	case 5:
		if (AssignedRole == EVNHPlayerRole::Alien)
		{
			HandlePlaceDecoyPressed();
		}
		break;
	default:
		break;
	}
}

void AVNHPlayerController::UpdateRoleHudActionHotkeyLabels()
{
	UUserWidget* Widget = RoleHudWidget.Get();
	if (!Widget)
	{
		return;
	}

	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	const bool bController = IsControllerInputPresetSelected();
	const bool bHunter = AssignedRole == EVNHPlayerRole::Hunter;
	const TCHAR* const* ActiveActionSuffixes = bHunter ? HunterRoleHudActionSuffixes : RoleHudActionSuffixes;
	const int32 VisibleSlotCount = bHunter || AssignedRole == EVNHPlayerRole::Alien ? RoleHudActionSlotCount : 5;

	for (int32 SlotIndex = 0; SlotIndex < RoleHudActionSlotCount; ++SlotIndex)
	{
		if (!RoleHudActionHotkeyTextBlocks[SlotIndex].IsValid())
		{
			RoleHudActionHotkeyTextBlocks[SlotIndex] = Cast<UTextBlock>(Widget->GetWidgetFromName(
				FName(FString::Printf(TEXT("ActionHotkeyText_%s"), ActiveActionSuffixes[SlotIndex]))));
		}

		if (UTextBlock* HotkeyText = RoleHudActionHotkeyTextBlocks[SlotIndex].Get())
		{
			const bool bShowSlot = SlotIndex < VisibleSlotCount;
			HotkeyText->SetVisibility(bShowSlot ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
			const TCHAR* HotkeyLabel = RoleHudKeyboardHotkeyLabels[SlotIndex];
			if (bController)
			{
				HotkeyLabel = RoleHudControllerHotkeyLabels[SlotIndex];
			}
			HotkeyText->SetText(FText::FromString(FString(HotkeyLabel)));
		}
	}
}

void AVNHPlayerController::HandleHudInspectClicked()
{
	HandleInspectPressed();
}

void AVNHPlayerController::HandleHudWaveClicked()
{
	HandleWavePressed();
}

void AVNHPlayerController::HandleHudPointClicked()
{
	HandlePointPressed();
}

void AVNHPlayerController::HandleHudLaughClicked()
{
	HandleLaughPressed();
}

void AVNHPlayerController::HandleHudFartClicked()
{
	HandleFartPressed();
}

void AVNHPlayerController::HandleHudPlaceDecoyClicked()
{
	HandlePlaceDecoyPressed();
}

void AVNHPlayerController::HandleHudThrowClicked()
{
	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	if (!Shopper || !Shopper->GetHeldProp())
	{
		ClientReceiveInteractionText(TEXT("Pick up an item before throwing."));
		return;
	}

	const FVector ThrowDirection = (GetControlRotation().Vector() + FVector(0.0f, 0.0f, 0.12f)).GetSafeNormal();
	ServerThrowHeldProp(0.25f, ThrowDirection);
}

void AVNHPlayerController::HandleHudMarkClicked()
{
	MarkFocusedShopper();
}

void AVNHPlayerController::HandleHudAccuseClicked()
{
	DebugAccuseFocusedShopper();
}

void AVNHPlayerController::HandleHudPressureClicked()
{
	FakeAccuseFocusedShopper();
}

void AVNHPlayerController::HandleHudHumanDrillClicked()
{
	RequestHumanDrill();
}

void AVNHPlayerController::HandleHudFakeDrillClicked()
{
	RequestFakeDrill();
}

void AVNHPlayerController::HandleHudEveryonePointClicked()
{
	RequestEveryonePoint();
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

FString AVNHPlayerController::GetRoundTimerText() const
{
	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return TEXT("00:00");
	}

	const float PhaseEndsAt = VNHGameState->GetPhaseEndsAtServerTime();
	const float RemainingSecondsFloat = PhaseEndsAt > 0.0f ? FMath::Max(0.0f, PhaseEndsAt - VNHGameState->GetServerWorldTimeSeconds()) : 0.0f;
	const int32 RemainingSeconds = FMath::FloorToInt(RemainingSecondsFloat);
	const int32 Minutes = RemainingSeconds / 60;
	const int32 Seconds = RemainingSeconds % 60;
	return FString::Printf(TEXT("%02d:%02d"), Minutes, Seconds);
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

void AVNHPlayerController::RequestHumanDrill()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can trigger Human Drill.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestHumanDrill();
}

void AVNHPlayerController::RequestFakeDrill()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can trigger Fake Drill.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestFakeDrill();
}

void AVNHPlayerController::RequestEveryonePoint()
{
	if (!IsAssignedHunter())
	{
		LastInteractionText = TEXT("Only the Hunter can trigger Everyone Point.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestEveryonePoint();
}

void AVNHPlayerController::RequestActNatural()
{
	DismissActiveHunterCommandPrompt();
	ServerRequestActNatural();
	LastInteractionText = TEXT("Act Natural requested.");
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::RequestUniversalAction(EVNHUniversalAction Action)
{
	DismissActiveHunterCommandPrompt();
	AActor* Target = GetUniversalActionTarget(Action);
	ServerPerformUniversalAction(Action, Target);
	LastInteractionText = FString::Printf(TEXT("%s // %s"), ToUniversalActionText(Action), Target ? *GetNameSafe(Target) : TEXT("NO TARGET"));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

void AVNHPlayerController::RequestInteract()
{
	if (FocusedLobbyPlayButton.IsValid())
	{
		if (!IsLocalLobbyHost())
		{
			LastInteractionText = TEXT("Only the host can start the round.");
			LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
			return;
		}

		bLobbyStartHoldActive = true;
		bLobbyStartRequestSent = false;
		LobbyStartHoldSeconds = 0.0f;
		LastInteractionText = TEXT("Hold E to start the round.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("Lobby")))
	{
		LastInteractionText = IsLocalLobbyHost()
			? TEXT("Stand next to the start console, hover it, and hold E.")
			: TEXT("Waiting for the host to start the round.");
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
		LastInteractionText = TEXT("No target to mark. Hover over a shopper.");
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
		LastInteractionText = TEXT("No target to pressure. Hover over a shopper.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	ServerRequestPressure(Shopper);
	LastInteractionText = FString::Printf(TEXT("Accusation pressure on %s."), *GetNameSafe(Shopper));
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
		LastInteractionText = TEXT("No target to accuse. Hover over a shopper.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	RequestAccusation(Shopper);
	LastInteractionText = FString::Printf(TEXT("Accused %s."), *GetNameSafe(Shopper));
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
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

void AVNHPlayerController::RequestPreRoundCustomizationReady()
{
	ServerSetPreRoundCustomizationReady();
	LastInteractionText = TEXT("READY // fit locked. You are now responsible for this outfit.");
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
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
		if (AccusedShopper)
		{
			AccusedShopper->ApplyComposureDelta(-15.0f, TEXT("Accused"));
		}
		VNHGameMode->RequestAccusation(this, AccusedShopper);
	}
}

void AVNHPlayerController::ServerRequestHumanDrill_Implementation()
{
	AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState || !IsAssignedHunter())
	{
		return;
	}

	const float Now = VNHGameState->GetServerWorldTimeSeconds();
	const FVNHHumanDrillPrompt CurrentPrompt = VNHGameState->GetHumanDrillPrompt();
	if (CurrentPrompt.CooldownEndsAtServerTime > Now)
	{
		ClientReceiveInteractionText(FString::Printf(TEXT("Human Drill cooldown %.0fs."), CurrentPrompt.CooldownEndsAtServerTime - Now));
		return;
	}

	const EVNHHumanDrillAction DrillAction = ChooseRandomHumanDrillAction();
	VNHGameState->SetHumanDrillPrompt(DrillAction, Now + HumanDrillPromptSeconds, Now + HumanDrillCooldownSeconds);
	ClientReceiveInteractionText(FString::Printf(TEXT("HUMAN DRILL // %s"), ToHumanDrillActionText(DrillAction)));
}

void AVNHPlayerController::ServerRequestFakeDrill_Implementation()
{
	AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState || !IsAssignedHunter())
	{
		return;
	}

	const float Now = VNHGameState->GetServerWorldTimeSeconds();
	VNHGameState->SetFakeDrillPrompt(Now + HumanDrillPromptSeconds);
	ClientReceiveInteractionText(TEXT("FAKE DRILL // DO NOTHING"));
}

void AVNHPlayerController::ServerRequestEveryonePoint_Implementation()
{
	AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState || !IsAssignedHunter())
	{
		return;
	}

	const float Now = VNHGameState->GetServerWorldTimeSeconds();
	const FVNHHumanDrillPrompt CurrentPrompt = VNHGameState->GetHumanDrillPrompt();
	if (CurrentPrompt.EveryonePointUsesThisRound >= EveryonePointMaxUsesPerRound)
	{
		ClientReceiveInteractionText(TEXT("Everyone Point already used twice this round."));
		return;
	}
	if (CurrentPrompt.EveryonePointCooldownEndsAtServerTime > Now)
	{
		ClientReceiveInteractionText(FString::Printf(TEXT("Everyone Point cooldown %.0fs."), CurrentPrompt.EveryonePointCooldownEndsAtServerTime - Now));
		return;
	}

	VNHGameState->SetEveryonePointPrompt(
		Now + HumanDrillPromptSeconds,
		Now + EveryonePointCooldownSeconds,
		CurrentPrompt.EveryonePointUsesThisRound + 1);
	ClientReceiveInteractionText(TEXT("EVERYONE POINT // Point at who you think is the alien."));
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

void AVNHPlayerController::ServerPerformUniversalAction_Implementation(EVNHUniversalAction Action, AActor* Target)
{
	AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn());
	if (!Shopper || Action == EVNHUniversalAction::None)
	{
		return;
	}

	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	if (Action == EVNHUniversalAction::PlaceDecoy && AssignedRole != EVNHPlayerRole::Alien)
	{
		ClientReceiveInteractionText(TEXT("Only the Alien can place a decoy."));
		return;
	}
	if ((Action == EVNHUniversalAction::PickUp || Action == EVNHUniversalAction::Drop)
		&& AssignedRole != EVNHPlayerRole::Human
		&& AssignedRole != EVNHPlayerRole::Alien)
	{
		ClientReceiveInteractionText(TEXT("Only Humans and Aliens can carry props."));
		return;
	}
	const bool bRoleAnimationAction = IsRoleAnimationAction(Action);
	if ((bRoleAnimationAction || Action == EVNHUniversalAction::Fart) && AssignedRole == EVNHPlayerRole::Hunter)
	{
		ClientReceiveInteractionText(TEXT("That action is unavailable for the Hunter."));
		return;
	}

	const bool bRepeatedAction = Shopper->WasActionRepeatedRecently(Action);
	AActor* EffectiveTarget = Target;
	const bool bTargetInRange = !EffectiveTarget
		|| FVector::DistSquared(EffectiveTarget->GetActorLocation(), Shopper->GetActorLocation()) <= FMath::Square(Action == EVNHUniversalAction::Point ? 2000.0f : 350.0f);
	if (!bTargetInRange)
	{
		if (bRoleAnimationAction)
		{
			EffectiveTarget = nullptr;
		}
		else
		{
			ClientReceiveInteractionText(TEXT("That target is out of reach."));
			return;
		}
	}

	switch (Action)
	{
	case EVNHUniversalAction::Inspect:
		if (!EffectiveTarget)
		{
			break;
		}
		if (IsVNHSuspicious(EffectiveTarget))
		{
			if (Shopper->IsBeingWatchedByHunter())
			{
				Shopper->ApplyComposureDelta(-5.0f, TEXT("WatchedSuspiciousInspect"));
			}
		}
		else
		{
			Shopper->ApplyComposureDelta(4.0f, TEXT("NormalInspect"));
		}
		break;

	case EVNHUniversalAction::Point:
		if (!EffectiveTarget)
		{
			break;
		}
		if (AVNHShopperCharacter* PointedShopper = Cast<AVNHShopperCharacter>(EffectiveTarget))
		{
			PointedShopper->ApplyComposureDelta(-8.0f, TEXT("PointedAt"));
		}
		else if (IsVNHSuspicious(EffectiveTarget))
		{
			Shopper->ApplyComposureDelta(5.0f, TEXT("PointedAtSuspiciousObject"));
		}
		if (bRepeatedAction)
		{
			Shopper->ApplyComposureDelta(-4.0f, TEXT("RepeatedPoint"));
		}
		break;

	case EVNHUniversalAction::Wave:
		Shopper->ApplyComposureDelta(bRepeatedAction ? -4.0f : 3.0f, bRepeatedAction ? TEXT("RepeatedWave") : TEXT("CasualWave"));
		break;

	case EVNHUniversalAction::Laugh:
		Shopper->ApplyComposureDelta(bRepeatedAction ? -4.0f : 2.0f, bRepeatedAction ? TEXT("RepeatedLaugh") : TEXT("CasualLaugh"));
		break;

	case EVNHUniversalAction::Fart:
		if (!Shopper->TriggerFartFromAction())
		{
			ClientReceiveInteractionText(TEXT("Fart action is on cooldown."));
			return;
		}
		Shopper->RegisterMeaningfulAction(Action, nullptr);
		ClientReceiveInteractionText(FString::Printf(TEXT("FART // action cooldown %ds"), FMath::Max(1, FMath::CeilToInt(Shopper->GetFartCooldownRemaining()))));
		return;

	case EVNHUniversalAction::PlaceDecoy:
		break;

	case EVNHUniversalAction::PickUp:
		if (!IsVNHInteractable(EffectiveTarget) || Shopper->IsHoldingProp(EffectiveTarget))
		{
			ClientReceiveInteractionText(TEXT("No available prop under the cursor."));
			return;
		}
		Shopper->ApplyComposureDelta(IsVNHSuspicious(EffectiveTarget) ? -12.0f : 3.0f, IsVNHSuspicious(EffectiveTarget) ? TEXT("SuspiciousPickup") : TEXT("NormalPickup"));
		if (Shopper->IsBeingWatchedByHunter())
		{
			Shopper->ApplyComposureDelta(-5.0f, TEXT("WatchedPickup"));
		}
		PerformPickUp(Shopper, EffectiveTarget);
		break;

	case EVNHUniversalAction::Drop:
		if (!Shopper->GetHeldProp())
		{
			ClientReceiveInteractionText(TEXT("You are not holding a prop."));
			return;
		}
		EffectiveTarget = Shopper->GetHeldProp();
		if (IsVNHSuspicious(EffectiveTarget))
		{
			Shopper->ApplyComposureDelta(-15.0f, TEXT("SuspiciousDrop"));
		}
		else
		{
			bool bNearAnotherPlayer = false;
			for (TActorIterator<AVNHShopperCharacter> It(GetWorld()); It; ++It)
			{
				const AVNHShopperCharacter* Other = *It;
				if (Other && Other != Shopper && FVector::DistSquared(Other->GetActorLocation(), Shopper->GetActorLocation()) <= FMath::Square(220.0f))
				{
					bNearAnotherPlayer = true;
					break;
				}
			}
			Shopper->ApplyComposureDelta(bNearAnotherPlayer ? -8.0f : 5.0f, bNearAnotherPlayer ? TEXT("DropNearPlayer") : TEXT("NormalDrop"));
		}
		PerformDrop(Shopper);
		break;

	default:
		return;
	}

	PlayRoleActionAnimation(AssignedRole, Shopper, Action);
	Shopper->RegisterMeaningfulAction(Action, EffectiveTarget);
	ClientReceiveInteractionText(FString::Printf(TEXT("%s // %s"), ToUniversalActionText(Action), EffectiveTarget ? *GetNameSafe(EffectiveTarget) : TEXT("NO TARGET")));
}

void AVNHPlayerController::ServerThrowHeldProp_Implementation(float ChargeAlpha, FVector_NetQuantizeNormal ThrowDirection)
{
	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	AActor* Prop = Shopper ? Shopper->GetHeldProp() : nullptr;
	if (!Shopper || !Prop || (AssignedRole != EVNHPlayerRole::Human && AssignedRole != EVNHPlayerRole::Alien))
	{
		ClientReceiveInteractionText(TEXT("Only Humans and Aliens can throw a held item."));
		return;
	}

	const FVector SafeThrowDirection = FVector(ThrowDirection).GetSafeNormal();
	if (SafeThrowDirection.IsNearlyZero())
	{
		return;
	}

	const float ClampedChargeAlpha = FMath::Clamp(ChargeAlpha, 0.0f, 1.0f);
	const float ThrowSpeed = FMath::Lerp(ThrowMinSpeed, ThrowMaxSpeed, ClampedChargeAlpha);
	Prop->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Prop->SetOwner(nullptr);
	Prop->SetInstigator(Shopper);
	Prop->SetActorLocation(Shopper->GetActorLocation() + SafeThrowDirection * 90.0f + FVector(0.0f, 0.0f, 55.0f));
	Prop->Tags.AddUnique(TEXT("VNH.Thrown"));
	Prop->OnActorHit.AddUniqueDynamic(this, &AVNHPlayerController::HandleThrownPropHit);

	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Prop->GetComponents(PrimitiveComponents);
	UPrimitiveComponent* PhysicsRoot = PrimitiveComponents.IsEmpty() ? nullptr : PrimitiveComponents[0];
	if (PhysicsRoot && Prop->GetRootComponent() != PhysicsRoot)
	{
		PhysicsRoot->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		Prop->SetRootComponent(PhysicsRoot);
	}
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (PrimitiveComponent)
		{
			PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			PrimitiveComponent->SetNotifyRigidBodyCollision(true);
			PrimitiveComponent->SetSimulatePhysics(true);
			PrimitiveComponent->SetPhysicsLinearVelocity(SafeThrowDirection * ThrowSpeed);
			PrimitiveComponent->SetPhysicsAngularVelocityInDegrees(FVector(0.0f, 900.0f, 180.0f));
		}
	}

	Shopper->SetHeldProp(nullptr);
	PlayRoleActionAnimation(AssignedRole, Shopper, EVNHUniversalAction::Throw);
	Shopper->RegisterMeaningfulAction(EVNHUniversalAction::Throw, Prop);
	ClientReceiveInteractionText(FString::Printf(TEXT("THROW // %.0f%% POWER"), ClampedChargeAlpha * 100.0f));
}

void AVNHPlayerController::HandleThrownPropHit(AActor* ThrownActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit)
{
	if (!HasAuthority() || !ThrownActor || !ThrownActor->ActorHasTag(TEXT("VNH.Thrown"))
		|| ThrownActor->GetInstigatorController() != this)
	{
		return;
	}

	AVNHShopperCharacter* HitShopper = Cast<AVNHShopperCharacter>(OtherActor);
	if (!HitShopper || HitShopper == GetPawn())
	{
		return;
	}

	const AVNHPlayerState* HitPlayerState = HitShopper->GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole HitRole = HitPlayerState ? HitPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	if (HitRole != EVNHPlayerRole::Human && HitRole != EVNHPlayerRole::Alien && HitRole != EVNHPlayerRole::Hunter)
	{
		return;
	}

	ThrownActor->Tags.Remove(TEXT("VNH.Thrown"));
	ThrownActor->OnActorHit.RemoveDynamic(this, &AVNHPlayerController::HandleThrownPropHit);
	const APawn* ThrowingPawn = ThrownActor->GetInstigator();
	HitShopper->PlayDirectionalKnockdown(ThrowingPawn ? ThrowingPawn->GetActorLocation() : Hit.ImpactPoint - Hit.ImpactNormal * 50.0f);
}

void AVNHPlayerController::ServerRequestPressure_Implementation(AVNHShopperCharacter* PressuredShopper)
{
	AVNHShopperCharacter* HunterShopper = Cast<AVNHShopperCharacter>(GetPawn());
	if (!IsAssignedHunter() || !HunterShopper || !PressuredShopper)
	{
		return;
	}

	if (FVector::DistSquared(HunterShopper->GetActorLocation(), PressuredShopper->GetActorLocation()) > FMath::Square(2000.0f))
	{
		ClientReceiveInteractionText(TEXT("Pressure target is out of range."));
		return;
	}

	PressuredShopper->ApplyComposureDelta(-8.0f, TEXT("HunterPressure"));
	ClientReceiveInteractionText(FString::Printf(TEXT("PRESSURE // %s"), *GetNameSafe(PressuredShopper)));
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

void AVNHPlayerController::RemoveBlockingMenuWidgets()
{
	if (!IsLocalController())
	{
		return;
	}

	int32 RemovedWidgetCount = 0;
	TArray<UUserWidget*> TopLevelWidgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, TopLevelWidgets, UUserWidget::StaticClass(), true);
	for (UUserWidget* ExistingWidget : TopLevelWidgets)
	{
		if (ExistingWidget && !ExistingWidget->IsA<UVNHLobbyMenuWidget>() && !ExistingWidget->IsA<UVNHCharacterCustomizerWidget>())
		{
			ExistingWidget->RemoveFromParent();
			++RemovedWidgetCount;
		}
	}

	const TCHAR* BlockingWidgetPaths[] =
	{
		TEXT("/Game/UI/WBP_CreateServerDialog.WBP_CreateServerDialog_C"),
		TEXT("/Game/UI/WBP_ServerBrowser.WBP_ServerBrowser_C"),
		TEXT("/Game/UI/WBP_VNHMainMenu.WBP_VNHMainMenu_C"),
		TEXT("/Game/UI/WBP_MainMenuRuntime.WBP_MainMenuRuntime_C")
	};

	for (const TCHAR* WidgetPath : BlockingWidgetPaths)
	{
		UClass* WidgetClass = LoadClass<UUserWidget>(nullptr, WidgetPath);
		if (!WidgetClass)
		{
			continue;
		}

		TArray<UUserWidget*> ExistingWidgets;
		UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, ExistingWidgets, WidgetClass, false);
		for (UUserWidget* ExistingWidget : ExistingWidgets)
		{
			if (ExistingWidget)
			{
				ExistingWidget->RemoveFromParent();
				++RemovedWidgetCount;
				UE_LOG(LogVNH, Display, TEXT("LobbyMenu: removed blocking menu widget %s before showing lobby HUD."), WidgetPath);
			}
		}
	}

	if (RemovedWidgetCount > 0)
	{
		UE_LOG(LogVNH, Display, TEXT("LobbyMenu: removed %d blocking top-level widget(s) before showing lobby HUD."), RemovedWidgetCount);
	}
}

void AVNHPlayerController::HandleLobbyCustomizeClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
	{
		VNHGameInstance->ShowCharacterCustomizer(true);
	}
}

void AVNHPlayerController::OpenCharacterCustomizerFromMainMenu()
{
	if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
	{
		VNHGameInstance->ShowCharacterCustomizer(false);
	}
}

void AVNHPlayerController::ApplySavedCharacterCustomization()
{
	if (!IsLocalController())
	{
		return;
	}

	UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>();
	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	if (!VNHGameInstance || !Shopper)
	{
		return;
	}

	const FVNHCharacterCustomization Customization = VNHGameInstance->GetActiveCustomization();
	if (HasAuthority())
	{
		Shopper->SetCharacterCustomization(Customization);
	}
	else
	{
		ServerSetCharacterCustomization(Customization);
	}
}

void AVNHPlayerController::ServerSetCharacterCustomization_Implementation(const FVNHCharacterCustomization& Customization)
{
	if (AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>())
	{
		Shopper->SetCharacterCustomization(Customization);
	}
}

void AVNHPlayerController::ServerSetPreRoundCustomizationReady_Implementation()
{
	if (AVNHGameMode* VNHGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AVNHGameMode>() : nullptr)
	{
		VNHGameMode->RequestPreRoundReady(this);
	}
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
		EnhancedInputComponent->BindAction(AlienActNaturalAction, ETriggerEvent::Started, this, &AVNHPlayerController::HandleThrowChargePressed);
		EnhancedInputComponent->BindAction(AlienActNaturalAction, ETriggerEvent::Completed, this, &AVNHPlayerController::HandleThrowChargeReleased);
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

	AddYawInput(Value * GetSavedLookSensitivityMultiplier() * 1.6f);
}

void AVNHPlayerController::HandleLookUpAxis(float Value)
{
	if (FMath::IsNearlyZero(Value))
	{
		return;
	}

	const float InvertMultiplier = GetSavedInvertLook() ? -1.0f : 1.0f;
	AddPitchInput(Value * GetSavedLookSensitivityMultiplier() * InvertMultiplier);
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

void AVNHPlayerController::HandleInspectPressed()
{
	RequestUniversalAction(EVNHUniversalAction::Inspect);
}

void AVNHPlayerController::HandleCursorXAxis(float Value)
{
	int32 ViewportX = 0;
	int32 ViewportY = 0;
	GetViewportSize(ViewportX, ViewportY);
	if (ViewportX <= 0 || ViewportY <= 0)
	{
		return;
	}
	if (!bVirtualCursorInitialized)
	{
		VirtualCursorPosition = FVector2D(ViewportX * 0.5f, ViewportY * 0.5f);
		bVirtualCursorInitialized = true;
	}
	VirtualCursorPosition.X = FMath::Clamp(VirtualCursorPosition.X + Value * 18.0f, 4.0f, ViewportX - 4.0f);
}

void AVNHPlayerController::HandleCursorYAxis(float Value)
{
	int32 ViewportX = 0;
	int32 ViewportY = 0;
	GetViewportSize(ViewportX, ViewportY);
	if (ViewportX <= 0 || ViewportY <= 0)
	{
		return;
	}
	if (!bVirtualCursorInitialized)
	{
		VirtualCursorPosition = FVector2D(ViewportX * 0.5f, ViewportY * 0.5f);
		bVirtualCursorInitialized = true;
	}
	VirtualCursorPosition.Y = FMath::Clamp(VirtualCursorPosition.Y - Value * 18.0f, 4.0f, ViewportY - 4.0f);
}

void AVNHPlayerController::HandlePointPressed()
{
	RequestUniversalAction(EVNHUniversalAction::Point);
}

void AVNHPlayerController::HandleWavePressed()
{
	RequestUniversalAction(EVNHUniversalAction::Wave);
}

void AVNHPlayerController::HandleLaughPressed()
{
	RequestUniversalAction(EVNHUniversalAction::Laugh);
}

void AVNHPlayerController::HandleFartPressed()
{
	RequestUniversalAction(EVNHUniversalAction::Fart);
}

void AVNHPlayerController::HandlePlaceDecoyPressed()
{
	RequestUniversalAction(EVNHUniversalAction::PlaceDecoy);
}

void AVNHPlayerController::HandleJumpPressed()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->Jump();
	}
}

void AVNHPlayerController::HandleJumpReleased()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->StopJumping();
	}
}

void AVNHPlayerController::HandleCrouchPressed()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->Crouch();
	}
}

void AVNHPlayerController::HandleCrouchReleased()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->UnCrouch();
	}
}

void AVNHPlayerController::HandleRoleHudActionSlot1Pressed()
{
	ExecuteRoleHudActionSlot(0);
}

void AVNHPlayerController::HandleRoleHudActionSlot2Pressed()
{
	ExecuteRoleHudActionSlot(1);
}

void AVNHPlayerController::HandleRoleHudActionSlot3Pressed()
{
	ExecuteRoleHudActionSlot(2);
}

void AVNHPlayerController::HandleRoleHudActionSlot4Pressed()
{
	ExecuteRoleHudActionSlot(3);
}

void AVNHPlayerController::HandleRoleHudActionSlot5Pressed()
{
	ExecuteRoleHudActionSlot(4);
}

void AVNHPlayerController::HandleRoleHudActionSlot6Pressed()
{
	ExecuteRoleHudActionSlot(5);
}

void AVNHPlayerController::HandlePickUpPressed()
{
	RequestUniversalAction(EVNHUniversalAction::PickUp);
}

void AVNHPlayerController::HandleDropPressed()
{
	RequestUniversalAction(EVNHUniversalAction::Drop);
}

void AVNHPlayerController::HandleThrowChargePressed()
{
	if (bThrowInputDown)
	{
		return;
	}
	bThrowInputDown = true;

	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	if ((AssignedRole == EVNHPlayerRole::Human || AssignedRole == EVNHPlayerRole::Alien) && Shopper && Shopper->GetHeldProp())
	{
		bThrowChargeActive = true;
		ThrowChargeStartedAtSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		ClientReceiveInteractionText(TEXT("Charging throw... release Q to throw."));
		return;
	}

	RequestActNatural();
}

void AVNHPlayerController::HandleThrowChargeReleased()
{
	if (!bThrowInputDown)
	{
		return;
	}
	bThrowInputDown = false;
	if (!bThrowChargeActive)
	{
		return;
	}

	bThrowChargeActive = false;
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : ThrowChargeStartedAtSeconds;
	const float ChargeAlpha = FMath::Clamp((Now - ThrowChargeStartedAtSeconds) / ThrowMaxChargeSeconds, 0.0f, 1.0f);
	const FVector ThrowDirection = (GetControlRotation().Vector() + FVector(0.0f, 0.0f, 0.12f)).GetSafeNormal();
	ServerThrowHeldProp(ChargeAlpha, ThrowDirection);
}

void AVNHPlayerController::HandleInteractPressed()
{
	HandleInspectPressed();
}

void AVNHPlayerController::UpdateLobbyStartHold(float DeltaTime)
{
	if (!bLobbyStartHoldActive)
	{
		return;
	}

	if (!FocusedLobbyPlayButton.IsValid() || !IsInputKeyDown(EKeys::E))
	{
		ResetLobbyStartHold();
		return;
	}

	if (!IsLocalLobbyHost())
	{
		ResetLobbyStartHold();
		LastInteractionText = TEXT("Only the host can start the round.");
		LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
		return;
	}

	LobbyStartHoldSeconds += DeltaTime;
	LastInteractionText = FString::Printf(TEXT("Starting round %.0f%%"), GetLobbyStartHoldProgress() * 100.0f);
	LastInteractionTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	if (!bLobbyStartRequestSent && LobbyStartHoldSeconds >= LobbyStartHoldRequiredSeconds)
	{
		bLobbyStartRequestSent = true;
		RequestStartRoundFromLobby();
	}
}

void AVNHPlayerController::ResetLobbyStartHold()
{
	bLobbyStartHoldActive = false;
	bLobbyStartRequestSent = false;
	LobbyStartHoldSeconds = 0.0f;
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
	bDebugDeckVisible = !bDebugDeckVisible;

	TArray<UUserWidget*> DebugDeckWidgets;
	UWidgetBlueprintLibrary::GetAllWidgetsOfClass(this, DebugDeckWidgets, UUserWidget::StaticClass(), false);
	for (UUserWidget* DebugDeckWidget : DebugDeckWidgets)
	{
		if (DebugDeckWidget && DebugDeckWidget->GetClass()->GetName().Contains(TEXT("WBP_VNHDebugDeck")))
		{
			DebugDeckWidget->SetVisibility(bDebugDeckVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}

	AVNHDebugHUD* DebugHUD = GetHUD<AVNHDebugHUD>();
	if (!DebugHUD)
	{
		ApplyDebugHudInputMode(bDebugDeckVisible);
		return;
	}

	DebugHUD->SetDebugPanelVisible(bDebugDeckVisible);
	ApplyDebugHudInputMode(bDebugDeckVisible);
}

void AVNHPlayerController::ApplyDebugHudInputMode(bool bDebugHudVisible)
{
	bGameplayInputModeApplied = false;
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

void AVNHPlayerController::RestoreGameplayInputMode()
{
	if (!IsLocalController())
	{
		return;
	}

	bShowMouseCursor = false;
	bEnableClickEvents = false;
	bEnableMouseOverEvents = false;
	DefaultMouseCursor = EMouseCursor::Default;
	CurrentMouseCursor = EMouseCursor::None;

	FInputModeGameOnly InputMode;
	InputMode.SetConsumeCaptureMouseDown(false);
	SetInputMode(InputMode);
	bGameplayInputModeApplied = true;
}

void AVNHPlayerController::UpdatePreRoundCustomizationFlow()
{
	if (!IsLocalController() || !GetWorld() || IsPlayerControllerMainMenuWorld(GetWorld()))
	{
		return;
	}

	const AVNHGameState* VNHGameState = GetWorld()->GetGameState<AVNHGameState>();
	if (!VNHGameState)
	{
		return;
	}

	const EVNHRoundPhase CurrentPhase = VNHGameState->GetRoundPhase();
	const int32 CurrentRound = VNHGameState->GetRoundNumber();
	const bool bPhaseChanged = !bObservedRoundPhaseInitialized || LastObservedRoundPhase != CurrentPhase;
	bObservedRoundPhaseInitialized = true;

	if (CurrentPhase == EVNHRoundPhase::AssigningRoles)
	{
		if (LastPreRoundCustomizerRound != CurrentRound)
		{
			LastPreRoundCustomizerRound = CurrentRound;
			if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
			{
				VNHGameInstance->ShowCharacterCustomizer(true);
			}

			LastInteractionText = TEXT("30-second drip check started. READY locks your look; the round timer still wins.");
			LastInteractionTimeSeconds = GetWorld()->GetTimeSeconds();
			UE_LOG(LogVNH, Display, TEXT("PreroundCustomization: opened lobby-mode customizer for round %d."), CurrentRound);
		}
	}
	else if (bPhaseChanged)
	{
		if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
		{
			VNHGameInstance->HideCharacterCustomizer();
		}
		RestoreGameplayInputMode();
		UE_LOG(LogVNH, Display, TEXT("PreroundCustomization: closed customizer as phase advanced to %s."), ToPhaseText(CurrentPhase));
	}

	LastObservedRoundPhase = CurrentPhase;
}

void AVNHPlayerController::ShowLobbyMenu()
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	RemoveBlockingMenuWidgets();

	if (UVNHLobbyMenuWidget* ExistingLobbyMenu = Cast<UVNHLobbyMenuWidget>(LobbyMenuWidget.Get()))
	{
		if (!ExistingLobbyMenu->IsInViewport())
		{
			ExistingLobbyMenu->AddToViewport(20000);
		}
		ExistingLobbyMenu->SetAnchorsInViewport(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		ExistingLobbyMenu->SetAlignmentInViewport(FVector2D::ZeroVector);
		ExistingLobbyMenu->SetPositionInViewport(FVector2D::ZeroVector, false);
		int32 ExistingViewportX = 0;
		int32 ExistingViewportY = 0;
		GetViewportSize(ExistingViewportX, ExistingViewportY);
		const float ExistingViewportScale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(ExistingLobbyMenu), 0.01f);
		ExistingLobbyMenu->SetDesiredSizeInViewport(FVector2D(FMath::Max(1, ExistingViewportX), FMath::Max(1, ExistingViewportY)) / ExistingViewportScale);
		ExistingLobbyMenu->SetVisibility(ESlateVisibility::Visible);
		UE_LOG(LogVNH, Display, TEXT("LobbyMenu: existing native overlay restored."));
		return;
	}

	UClass* LobbyWidgetClass = LoadClass<UVNHLobbyMenuWidget>(nullptr, TEXT("/Game/UI/WBP_LobbyMenu.WBP_LobbyMenu_C"));
	if (!LobbyWidgetClass)
	{
		LobbyWidgetClass = UVNHLobbyMenuWidget::StaticClass();
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenu: /Game/UI/WBP_LobbyMenu unavailable; using native fallback widget."));
	}

	UVNHLobbyMenuWidget* NewLobbyMenu = CreateWidget<UVNHLobbyMenuWidget>(this, LobbyWidgetClass);
	if (!NewLobbyMenu)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenu: CreateWidget failed."));
		return;
	}

	NewLobbyMenu->AddToViewport(20000);
	NewLobbyMenu->SetAnchorsInViewport(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	NewLobbyMenu->SetAlignmentInViewport(FVector2D::ZeroVector);
	NewLobbyMenu->SetPositionInViewport(FVector2D::ZeroVector, false);
	int32 NewViewportX = 0;
	int32 NewViewportY = 0;
	GetViewportSize(NewViewportX, NewViewportY);
	const float NewViewportScale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(NewLobbyMenu), 0.01f);
	NewLobbyMenu->SetDesiredSizeInViewport(FVector2D(FMath::Max(1, NewViewportX), FMath::Max(1, NewViewportY)) / NewViewportScale);
	NewLobbyMenu->SetVisibility(ESlateVisibility::Visible);
	LobbyMenuWidget = NewLobbyMenu;
	bGameplayInputModeApplied = false;

	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	DefaultMouseCursor = EMouseCursor::Default;
	UpdateGameplayCursor();
	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(NewLobbyMenu->TakeWidget());
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	NewLobbyMenu->SetKeyboardFocus();
	NewLobbyMenu->SetUserFocus(this);

	UE_LOG(LogVNH, Display, TEXT("LobbyMenu: shown as non-blocking overlay."));
}

void AVNHPlayerController::RemoveLobbyMenu()
{
	if (LobbyMenuWidget.IsValid())
	{
		LobbyMenuWidget->RemoveFromParent();
		LobbyMenuWidget.Reset();
	}
}

void AVNHPlayerController::UpdateRoleHudWidgetRuntimeLabels(float DeltaTime)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	EnsureRoleHudWidget();
	if (!RoleHudWidget.IsValid())
	{
		return;
	}

	TimeUntilRoleHudWidgetLookup -= DeltaTime;
	if ((!RoleHudRoundTimerTextBlock.IsValid()
		|| !RoleHudComposureStateTextBlock.IsValid()
		|| !RoleHudComposureValueTextBlock.IsValid()
		|| !RoleHudComposureProgressBar.IsValid()
		|| !RoleHudDrillPromptPanelWidget.IsValid()
		|| !RoleHudDrillPromptTextBlock.IsValid()
		|| !RoleHudHumanDrillCooldownPanelWidget.IsValid()
		|| !RoleHudHumanDrillCooldownTextBlock.IsValid()
		|| !RoleHudEveryonePointCooldownPanelWidget.IsValid()
		|| !RoleHudEveryonePointCooldownTextBlock.IsValid()
		|| !RoleHudFartCooldownPanelWidget.IsValid()
		|| !RoleHudFartCooldownTextBlock.IsValid()) && TimeUntilRoleHudWidgetLookup <= 0.0f)
	{
		TimeUntilRoleHudWidgetLookup = 0.5f;
		UUserWidget* Widget = RoleHudWidget.Get();
		if (!RoleHudRoundTimerTextBlock.IsValid())
		{
			RoleHudRoundTimerTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("RoundTimerText"))) : nullptr;
		}
		if (!RoleHudComposureStateTextBlock.IsValid())
		{
			RoleHudComposureStateTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("ComposureStateText"))) : nullptr;
		}
		if (!RoleHudComposureValueTextBlock.IsValid())
		{
			RoleHudComposureValueTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("ComposureValueText"))) : nullptr;
		}
		if (!RoleHudComposureProgressBar.IsValid())
		{
			RoleHudComposureProgressBar = Widget ? Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("ComposureBar"))) : nullptr;
		}
		if (!RoleHudDrillPromptPanelWidget.IsValid())
		{
			RoleHudDrillPromptPanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("HumanDrillPromptPanel")) : nullptr;
		}
		if (!RoleHudDrillPromptTextBlock.IsValid())
		{
			RoleHudDrillPromptTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("HumanDrillPromptText"))) : nullptr;
		}
		if (!RoleHudHumanDrillCooldownPanelWidget.IsValid())
		{
			RoleHudHumanDrillCooldownPanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("HumanDrillCooldownPanel")) : nullptr;
		}
		if (!RoleHudHumanDrillCooldownTextBlock.IsValid())
		{
			RoleHudHumanDrillCooldownTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("HumanDrillCooldownText"))) : nullptr;
		}
		if (!RoleHudEveryonePointCooldownPanelWidget.IsValid())
		{
			RoleHudEveryonePointCooldownPanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("EveryonePointCooldownPanel")) : nullptr;
		}
		if (!RoleHudEveryonePointCooldownTextBlock.IsValid())
		{
			RoleHudEveryonePointCooldownTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("EveryonePointCooldownText"))) : nullptr;
		}
		if (!RoleHudFartCooldownPanelWidget.IsValid())
		{
			RoleHudFartCooldownPanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("FartCooldownPanel")) : nullptr;
		}
		if (!RoleHudFartCooldownTextBlock.IsValid())
		{
			RoleHudFartCooldownTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("FartCooldownText"))) : nullptr;
		}
	}

	UpdateRoleHudActionHotkeyLabels();

	const AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(GetPawn());
	const float ComposureValue = Shopper ? Shopper->GetComposure() : 0.0f;

	if (UTextBlock* TimerText = RoleHudRoundTimerTextBlock.Get())
	{
		TimerText->SetText(FText::FromString(GetRoundTimerText()));
	}

	if (UTextBlock* StateText = RoleHudComposureStateTextBlock.Get())
	{
		FText RoleHudComposureText = FText::GetEmpty();
		if (Shopper)
		{
			if (ComposureValue <= 0.0f)
			{
				RoleHudComposureText = NSLOCTEXT("VNH", "RoleHudComposureDepleted", "COMPOSURE DEPLETED");
			}
			else
			{
				switch (Shopper->GetComposureState())
				{
				case EVNHComposureState::Nervous:
					RoleHudComposureText = NSLOCTEXT("VNH", "RoleHudComposureNervous", "NERVOUS");
					break;
				case EVNHComposureState::Cracking:
				case EVNHComposureState::Panic:
					RoleHudComposureText = NSLOCTEXT("VNH", "RoleHudComposurePanicked", "PANICKED");
					break;
				case EVNHComposureState::Calm:
				case EVNHComposureState::Stable:
				default:
					RoleHudComposureText = NSLOCTEXT("VNH", "RoleHudComposureStable", "STABLE");
					break;
				}
			}
		}
		StateText->SetText(RoleHudComposureText);
	}

	if (UTextBlock* ValueText = RoleHudComposureValueTextBlock.Get())
	{
		ValueText->SetText(FText::FromString(FString::Printf(TEXT("%.0f%%"), ComposureValue)));
	}

	if (UProgressBar* ProgressBar = RoleHudComposureProgressBar.Get())
	{
		ProgressBar->SetPercent(ComposureValue / 100.0f);
	}

	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	const float ServerTime = VNHGameState ? VNHGameState->GetServerWorldTimeSeconds() : 0.0f;
	const FVNHHumanDrillPrompt HumanDrillPrompt = VNHGameState ? VNHGameState->GetHumanDrillPrompt() : FVNHHumanDrillPrompt();
	if (HumanDrillPrompt.PromptType == EVNHHunterCommandPromptType::None)
	{
		DismissedHunterCommandPromptSerial = INDEX_NONE;
	}
	const bool bShowDrillPrompt = HumanDrillPrompt.PromptType != EVNHHunterCommandPromptType::None
		&& HumanDrillPrompt.PromptEndsAtServerTime > ServerTime
		&& HumanDrillPrompt.Serial != DismissedHunterCommandPromptSerial
		&& (AssignedRole == EVNHPlayerRole::Human || AssignedRole == EVNHPlayerRole::Alien);
	if (UWidget* PromptPanel = RoleHudDrillPromptPanelWidget.Get())
	{
		PromptPanel->SetVisibility(bShowDrillPrompt ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		PromptPanel->SetRenderOpacity(bShowDrillPrompt ? 1.0f : 0.0f);
	}
	if (UTextBlock* PromptText = RoleHudDrillPromptTextBlock.Get())
	{
		FString PromptTextString;
		if (HumanDrillPrompt.PromptType == EVNHHunterCommandPromptType::EveryonePoint)
		{
			PromptTextString = TEXT("Everyone Point - Point at who you think is the alien");
		}
		else if (AssignedRole == EVNHPlayerRole::Alien)
		{
			PromptTextString = TEXT("HUMAN DRILL! COPY THE HUMANS");
		}
		else if (HumanDrillPrompt.PromptType == EVNHHunterCommandPromptType::FakeDrill)
		{
			PromptTextString = TEXT("FAKE DRILL: DO NOTHING");
		}
		else
		{
			PromptTextString = FString::Printf(TEXT("HUMAN DRILL: %s"), ToHumanDrillActionText(HumanDrillPrompt.Action));
		}
		PromptText->SetText(FText::FromString(PromptTextString));
	}

	const float CooldownRemaining = HumanDrillPrompt.CooldownEndsAtServerTime > ServerTime
		? HumanDrillPrompt.CooldownEndsAtServerTime - ServerTime
		: 0.0f;
	const bool bShowHumanDrillCooldown = AssignedRole == EVNHPlayerRole::Hunter && CooldownRemaining > 0.0f;
	if (UWidget* CooldownPanel = RoleHudHumanDrillCooldownPanelWidget.Get())
	{
		CooldownPanel->SetVisibility(bShowHumanDrillCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		CooldownPanel->SetRenderOpacity(bShowHumanDrillCooldown ? 1.0f : 0.0f);
	}
	if (UTextBlock* CooldownText = RoleHudHumanDrillCooldownTextBlock.Get())
	{
		CooldownText->SetText(FText::FromString(FString::Printf(TEXT("%.0fs"), CooldownRemaining)));
	}

	const float EveryonePointCooldownRemaining = HumanDrillPrompt.EveryonePointCooldownEndsAtServerTime > ServerTime
		? HumanDrillPrompt.EveryonePointCooldownEndsAtServerTime - ServerTime
		: 0.0f;
	const bool bShowEveryonePointCooldown = AssignedRole == EVNHPlayerRole::Hunter && EveryonePointCooldownRemaining > 0.0f;
	if (UWidget* CooldownPanel = RoleHudEveryonePointCooldownPanelWidget.Get())
	{
		CooldownPanel->SetVisibility(bShowEveryonePointCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		CooldownPanel->SetRenderOpacity(bShowEveryonePointCooldown ? 1.0f : 0.0f);
	}
	if (UTextBlock* CooldownText = RoleHudEveryonePointCooldownTextBlock.Get())
	{
		CooldownText->SetText(FText::FromString(FString::Printf(TEXT("%.0fs"), EveryonePointCooldownRemaining)));
	}

	const float AuthoritativeFartCooldownRemaining = Shopper ? Shopper->GetFartCooldownRemaining() : 0.0f;
	if (!Shopper || AuthoritativeFartCooldownRemaining <= RoleHudFartCooldownDisplayTolerance)
	{
		RoleHudDisplayedFartCooldownRemaining = 0.0f;
	}
	else if (RoleHudDisplayedFartCooldownRemaining <= 0.0f
		|| AuthoritativeFartCooldownRemaining > RoleHudLastReplicatedFartCooldownRemaining + 0.25f)
	{
		RoleHudDisplayedFartCooldownRemaining = AuthoritativeFartCooldownRemaining;
	}
	else
	{
		RoleHudDisplayedFartCooldownRemaining = FMath::Max(0.0f, RoleHudDisplayedFartCooldownRemaining - DeltaTime);
	}
	RoleHudLastReplicatedFartCooldownRemaining = AuthoritativeFartCooldownRemaining;

	const bool bShowFartCooldown = (AssignedRole == EVNHPlayerRole::Human || AssignedRole == EVNHPlayerRole::Alien)
		&& RoleHudDisplayedFartCooldownRemaining > RoleHudFartCooldownDisplayTolerance;
	if (UWidget* CooldownPanel = RoleHudFartCooldownPanelWidget.Get())
	{
		CooldownPanel->SetVisibility(bShowFartCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		CooldownPanel->SetRenderOpacity(bShowFartCooldown ? 1.0f : 0.0f);
	}
	if (UTextBlock* CooldownText = RoleHudFartCooldownTextBlock.Get())
	{
		const int32 CooldownSeconds = FMath::Max(1, FMath::CeilToInt(RoleHudDisplayedFartCooldownRemaining));
		CooldownText->SetText(FText::FromString(FString::Printf(TEXT("%ds"), CooldownSeconds)));
	}
}

void AVNHPlayerController::DismissActiveHunterCommandPrompt()
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	const AVNHGameState* VNHGameState = GetWorld()->GetGameState<AVNHGameState>();
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	const EVNHPlayerRole AssignedRole = VNHPlayerState ? VNHPlayerState->GetRole() : EVNHPlayerRole::Unassigned;
	const float ServerTime = VNHGameState ? VNHGameState->GetServerWorldTimeSeconds() : 0.0f;
	const FVNHHumanDrillPrompt Prompt = VNHGameState ? VNHGameState->GetHumanDrillPrompt() : FVNHHumanDrillPrompt();
	if ((AssignedRole == EVNHPlayerRole::Human || AssignedRole == EVNHPlayerRole::Alien)
		&& Prompt.PromptType != EVNHHunterCommandPromptType::None
		&& Prompt.PromptEndsAtServerTime > ServerTime)
	{
		DismissedHunterCommandPromptSerial = Prompt.Serial;
	}
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

			Widget->SetVisibility(bDebugDeckVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

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

	if (UWidget* SuspectsPanel = MarkedSuspectsPanelWidget.Get())
	{
		SuspectsPanel->SetVisibility(bShowPanel ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		SuspectsPanel->SetRenderOpacity(bShowPanel ? 1.0f : 0.0f);
	}
}

void AVNHPlayerController::UpdateComposureWidgetRuntimeLabels(float DeltaTime)
{
	if (!IsLocalController() || !GetWorld())
	{
		return;
	}

	if (IsPlayerControllerMainMenuWorld(GetWorld()))
	{
		if (ComposureWidget.IsValid())
		{
			ComposureWidget->RemoveFromParent();
			ComposureWidget.Reset();
		}
		return;
	}

	EnsureComposureWidget();
	if (!ComposureWidget.IsValid())
	{
		return;
	}

	TimeUntilComposureWidgetLookup -= DeltaTime;
	if ((!ComposurePanelWidget.IsValid()
		|| !ComposureStateTextBlock.IsValid()
		|| !ComposureValueTextBlock.IsValid()
		|| !FartRiskTextBlock.IsValid()
		|| !UniversalActionTextBlock.IsValid()
		|| !ComposureProgressBar.IsValid()) && TimeUntilComposureWidgetLookup <= 0.0f)
	{
		TimeUntilComposureWidgetLookup = 0.5f;
		UUserWidget* Widget = ComposureWidget.Get();
		ComposurePanelWidget = Widget ? Widget->GetWidgetFromName(TEXT("ComposurePanel")) : nullptr;
		ComposureStateTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("ComposureStateText"))) : nullptr;
		ComposureValueTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("ComposureValueText"))) : nullptr;
		FartRiskTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("FartRiskText"))) : nullptr;
		UniversalActionTextBlock = Widget ? Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("UniversalActionText"))) : nullptr;
		ComposureProgressBar = Widget ? Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("ComposureBar"))) : nullptr;
		HudCustomizeButton = Widget ? Cast<UButton>(Widget->GetWidgetFromName(TEXT("CustomizeButton"))) : nullptr;
		BindComposureWidgetButtons();
	}

	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	const bool bShow = Shopper != nullptr;
	const AVNHGameState* VNHGameState = GetWorld()->GetGameState<AVNHGameState>();
	const bool bShowCustomizeButton = bShow && IsCustomizationPhase(VNHGameState);
	if (UUserWidget* Widget = ComposureWidget.Get())
	{
		Widget->SetVisibility(bShow ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (UWidget* ComposurePanel = ComposurePanelWidget.Get())
	{
		ComposurePanel->SetVisibility(bShow ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (UButton* CustomizeButton = HudCustomizeButton.Get())
	{
		CustomizeButton->SetVisibility(bShowCustomizeButton ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (!Shopper)
	{
		return;
	}

	const float ComposureValue = Shopper->GetComposure();
	if (UTextBlock* StateText = ComposureStateTextBlock.Get())
	{
		StateText->SetText(Shopper->GetComposureStateText());
	}
	if (UTextBlock* ValueText = ComposureValueTextBlock.Get())
	{
		ValueText->SetText(FText::FromString(FString::Printf(TEXT("%03.0f"), ComposureValue)));
	}
	if (UProgressBar* ProgressBar = ComposureProgressBar.Get())
	{
		ProgressBar->SetPercent(ComposureValue / 100.0f);
		const FLinearColor FillColor = FLinearColor::LerpUsingHSV(
			FLinearColor(0.95f, 0.20f, 0.12f, 1.0f),
			FLinearColor(0.10f, 0.85f, 0.72f, 1.0f),
			ComposureValue / 100.0f);
		ProgressBar->SetFillColorAndOpacity(FillColor);
	}
	if (UTextBlock* FartText = FartRiskTextBlock.Get())
	{
		const float Remaining = FMath::Max(0.0f, Shopper->GetCurrentFartThreshold() - Shopper->GetInactivitySeconds());
		const float FartCooldownRemaining = Shopper->GetFartCooldownRemaining();
		const FString CooldownText = FartCooldownRemaining > RoleHudFartCooldownDisplayTolerance
			? FString::Printf(TEXT("ACTION CD %ds // BREAK %.1fs"), FMath::Max(1, FMath::CeilToInt(FartCooldownRemaining)), Remaining)
			: FString::Printf(TEXT("BREAK %.1fs"), Remaining);
		FartText->SetText(FText::FromString(CooldownText));
	}
	if (UTextBlock* ActionText = UniversalActionTextBlock.Get())
	{
		ActionText->SetText(FText::FromString(FString::Printf(TEXT("LAST // %s"), ToUniversalActionText(Shopper->GetLastUniversalAction()))));
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

	if (IsCharacterCustomizerScreenOpen(this))
	{
		LastPolledAlienMoveInput = FVector2D::ZeroVector;
		bLastPolledFastWalkRequested = false;
		bWasPolledActNaturalDown = IsInputKeyDown(EKeys::Q);
		bWasPolledInteractDown = IsInputKeyDown(EKeys::E);
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
		HandleThrowChargePressed();
	}
	else if (!bActNaturalDown && bWasPolledActNaturalDown)
	{
		HandleThrowChargeReleased();
	}

	if (bInteractDown && !bWasPolledInteractDown)
	{
		HandleInspectPressed();
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

	if (IsCharacterCustomizerScreenOpen(this))
	{
		bWasPolledTargetFocusDown = IsInputKeyDown(EKeys::RightMouseButton);
		bWasPolledInteractDown = IsInputKeyDown(EKeys::E);
		bWasPolledMarkDown = IsInputKeyDown(EKeys::R);
		bWasPolledFakeAccuseDown = IsInputKeyDown(EKeys::F);
		bWasPolledAccuseDown = IsInputKeyDown(EKeys::X);
		bWasPolledCancelTargetDown = IsInputKeyDown(EKeys::LeftMouseButton);
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
		HandleInspectPressed();
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

	EMouseCursor::Type DesiredCursor = EMouseCursor::None;
	if (IsCharacterCustomizerScreenOpen(this))
	{
		DesiredCursor = EMouseCursor::Default;
	}
	else if (bDebugDeckVisible)
	{
		DesiredCursor = EMouseCursor::Default;
	}
	else if (GetWorld() && GetWorld()->GetMapName().Contains(TEXT("Lobby")))
	{
		DesiredCursor = EMouseCursor::Default;
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
		TEXT("Slate/default-cursor"),
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

	if (bHardwareCursorsRegistered)
	{
		UE_LOG(
			LogVNH,
			Display,
			TEXT("HardwareCursors: Normal=true Regular=true Hunter=true"));
	}
	else
	{
		UE_LOG(
			LogVNH,
			Warning,
			TEXT("HardwareCursors: Normal=%s Regular=%s Hunter=%s"),
			bNormalRegistered ? TEXT("true") : TEXT("false"),
			bRegularRegistered ? TEXT("true") : TEXT("false"),
			bHunterRegistered ? TEXT("true") : TEXT("false"));
	}

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
		Shopper->SetFirstPersonViewEnabled(false);
	}

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (IsCharacterCustomizerScreenOpen(this))
	{
		bGameplayInputModeApplied = false;
	}
	if (!MapName.Contains(TEXT("Lobby"))
		&& !IsCharacterCustomizerScreenOpen(this)
		&& !bDebugDeckVisible
		&& !bGameplayInputModeApplied)
	{
		RestoreGameplayInputMode();
	}
}

void AVNHPlayerController::UpdateCursorReticleAndHeadLook(float DeltaTime)
{
	if (!IsLocalController() || !RoleHudWidget.IsValid() || IsCharacterCustomizerScreenOpen(this) || bDebugDeckVisible)
	{
		return;
	}

	int32 ViewportX = 0;
	int32 ViewportY = 0;
	GetViewportSize(ViewportX, ViewportY);
	if (ViewportX <= 0 || ViewportY <= 0)
	{
		return;
	}
	if (!bVirtualCursorInitialized)
	{
		VirtualCursorPosition = FVector2D(ViewportX * 0.5f, ViewportY * 0.5f);
		bVirtualCursorInitialized = true;
	}

	const float ViewportScale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(RoleHudWidget.Get()), 0.01f);
	const FVector2D ReticlePosition = VirtualCursorPosition / ViewportScale;
	for (const FName ReticleName : {FName(TEXT("CenterReticleShadow")), FName(TEXT("CenterReticleDot"))})
	{
		if (UWidget* ReticleWidget = RoleHudWidget->GetWidgetFromName(ReticleName))
		{
			if (UCanvasPanelSlot* ReticleSlot = Cast<UCanvasPanelSlot>(ReticleWidget->Slot))
			{
				ReticleSlot->SetPosition(ReticlePosition);
			}
		}
	}

	AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
	float TargetHeadLookYaw = 0.0f;
	float TargetHeadLookPitch = 0.0f;
	if (Shopper && Shopper->GetMesh())
	{
		FVector CursorWorldLocation = FVector::ZeroVector;
		FVector CursorWorldDirection = FVector::ForwardVector;
		if (DeprojectScreenPositionToWorld(VirtualCursorPosition.X, VirtualCursorPosition.Y, CursorWorldLocation, CursorWorldDirection))
		{
			const FVector CursorTraceEnd = CursorWorldLocation + CursorWorldDirection * 5000.0f;
			FCollisionQueryParams HeadLookQueryParams(SCENE_QUERY_STAT(VNHHeadLookCursorTrace), false, Shopper);
			FHitResult HeadLookHit;
			const FVector CursorTarget = GetWorld() && GetWorld()->LineTraceSingleByChannel(
				HeadLookHit,
				CursorWorldLocation,
				CursorTraceEnd,
				ECC_Visibility,
				HeadLookQueryParams)
				? HeadLookHit.ImpactPoint
				: CursorTraceEnd;
			const FVector HeadWorldLocation = Shopper->GetMesh()->GetBoneLocation(TEXT("Head"));
			const FVector LocalLookDirection = Shopper->GetActorTransform().InverseTransformVectorNoScale(
				CursorTarget - HeadWorldLocation);
			if (LocalLookDirection.X > 10.0f)
			{
				TargetHeadLookYaw = FMath::Clamp(
					FMath::RadiansToDegrees(FMath::Atan2(LocalLookDirection.Y, LocalLookDirection.X)),
					-60.0f,
					60.0f);
				TargetHeadLookPitch = FMath::Clamp(
					FMath::RadiansToDegrees(FMath::Atan2(LocalLookDirection.Z, LocalLookDirection.Size2D())),
					-30.0f,
					30.0f);
			}
		}
	}
	SmoothedHeadLookYaw = FMath::FInterpTo(SmoothedHeadLookYaw, TargetHeadLookYaw, DeltaTime, 7.0f);
	SmoothedHeadLookPitch = FMath::FInterpTo(SmoothedHeadLookPitch, TargetHeadLookPitch, DeltaTime, 7.0f);

	UAnimInstance* AnimInstance = Shopper && Shopper->GetMesh() ? Shopper->GetMesh()->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}
	if (FFloatProperty* YawProperty = FindFProperty<FFloatProperty>(AnimInstance->GetClass(), TEXT("HeadLookYaw")))
	{
		YawProperty->SetPropertyValue_InContainer(AnimInstance, SmoothedHeadLookYaw);
	}
	if (FFloatProperty* PitchProperty = FindFProperty<FFloatProperty>(AnimInstance->GetClass(), TEXT("HeadLookPitch")))
	{
		PitchProperty->SetPropertyValue_InContainer(AnimInstance, SmoothedHeadLookPitch);
	}
}

FString AVNHPlayerController::GetInteractionPromptText() const
{
	if (FocusedLobbyPlayButton.IsValid())
	{
		return bLobbyStartHoldActive
			? FString::Printf(TEXT("HOLD E // STARTING %.0f%%"), GetLobbyStartHoldProgress() * 100.0f)
			: TEXT("HOLD E // START ROUND");
	}

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
	if (MapName.Contains(TEXT("Lobby")))
	{
		return IsLocalLobbyHost() ? TEXT("START CONSOLE // HOLD E") : FString();
	}

	if (const AVNHShopperCharacter* Shopper = TargetedShopper.Get())
	{
		return FString::Printf(TEXT("LOCKED: %s  //  E ASK  R MARK  F FAKE  X ACCUSE  LMB CANCEL"), *GetNameSafe(Shopper));
	}

	if (FocusedInteractable.IsValid())
	{
		return FString::Printf(TEXT("E INSPECT  //  C PICK UP  //  %s"), GetPawn<AVNHShopperCharacter>() && GetPawn<AVNHShopperCharacter>()->GetHeldProp() ? TEXT("B DROP") : TEXT(""));
	}

	if (!IsAssignedHunter())
	{
		return FocusedShopper.IsValid() ? TEXT("E INSPECT  //  G POINT  //  V WAVE") : TEXT("G POINT  //  V WAVE");
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
	AActor* PreviousFocusedLobbyPlayButton = FocusedLobbyPlayButton.Get();
	AActor* PreviousFocusedInteractable = FocusedInteractable.Get();
	FocusedShopper.Reset();
	FocusedLobbyPlayButton.Reset();
	FocusedInteractable.Reset();

	if (IsCharacterCustomizerScreenOpen(this))
	{
		if (PreviousFocusedShopper)
		{
			RefreshShopperOutline(PreviousFocusedShopper, false);
		}
		SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
		SetInteractableOutline(PreviousFocusedInteractable, false);
		CurrentMouseCursor = EMouseCursor::Default;
		return;
	}

	if (!PlayerCameraManager || !GetWorld())
	{
		if (PreviousFocusedShopper)
		{
			RefreshShopperOutline(PreviousFocusedShopper, false);
		}
		SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
		SetInteractableOutline(PreviousFocusedInteractable, false);
		return;
	}

	FVector CursorWorldLocation = FVector::ZeroVector;
	FVector CursorWorldDirection = FVector::ForwardVector;
	if (!bVirtualCursorInitialized || !DeprojectScreenPositionToWorld(
		VirtualCursorPosition.X,
		VirtualCursorPosition.Y,
		CursorWorldLocation,
		CursorWorldDirection))
	{
		return;
	}
	const FVector CursorTraceEnd = CursorWorldLocation + CursorWorldDirection * 2500.0f;
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(VNHInteractionCursorTrace), false);
	if (APawn* ControlledPawn = GetPawn())
	{
		QueryParams.AddIgnoredActor(ControlledPawn);
	}

	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByChannel(Hit, CursorWorldLocation, CursorTraceEnd, ECC_Visibility, QueryParams))
	{
		if (AVNHLobbyPlayButton* LobbyPlayButton = Cast<AVNHLobbyPlayButton>(Hit.GetActor()))
		{
			if (IsLocalLobbyHost() && LobbyPlayButton->IsPawnWithinInteractionRange(GetPawn()))
			{
				FocusedLobbyPlayButton = LobbyPlayButton;
				if (PreviousFocusedShopper)
				{
					RefreshShopperOutline(PreviousFocusedShopper, false);
				}
				SetInteractableOutline(PreviousFocusedInteractable, false);
				if (PreviousFocusedLobbyPlayButton != LobbyPlayButton)
				{
					SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
					SetInteractableOutline(LobbyPlayButton, true);
				}
				return;
			}

			SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
		}

		if (IsVNHInteractable(Hit.GetActor()))
		{
			FocusedInteractable = Hit.GetActor();
			if (PreviousFocusedInteractable != Hit.GetActor())
			{
				SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
				SetInteractableOutline(PreviousFocusedInteractable, false);
				SetInteractableOutline(Hit.GetActor(), true);
			}
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

	if (GetWorld()->LineTraceSingleByChannel(Hit, CursorWorldLocation, CursorTraceEnd, ECC_Pawn, QueryParams))
	{
		if (AVNHShopperCharacter* Shopper = Cast<AVNHShopperCharacter>(Hit.GetActor()))
		{
			FocusedShopper = Shopper;
			SetInteractableOutline(PreviousFocusedInteractable, false);
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
	SetInteractableOutline(PreviousFocusedLobbyPlayButton, false);
	SetInteractableOutline(PreviousFocusedInteractable, false);

	if (AVNHShopperCharacter* Target = TargetedShopper.Get())
	{
		RefreshShopperOutline(Target, false);
	}
}

void AVNHPlayerController::SetInteractableOutline(AActor* Actor, bool bEnabled) const
{
	if (!Actor)
	{
		return;
	}

	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Actor->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (PrimitiveComponent)
		{
			PrimitiveComponent->SetRenderCustomDepth(bEnabled);
			PrimitiveComponent->SetCustomDepthStencilValue(bEnabled ? HoveredShopperStencil : 0);
		}
	}
}

AActor* AVNHPlayerController::GetUniversalActionTarget(EVNHUniversalAction Action) const
{
	if (Action == EVNHUniversalAction::Drop)
	{
		const AVNHShopperCharacter* Shopper = GetPawn<AVNHShopperCharacter>();
		return Shopper ? Shopper->GetHeldProp() : nullptr;
	}

	if (Action == EVNHUniversalAction::PickUp)
	{
		return FocusedInteractable.Get();
	}

	if (Action == EVNHUniversalAction::Point)
	{
		return FocusedShopper.IsValid() ? FocusedShopper.Get() : FocusedInteractable.Get();
	}

	return FocusedInteractable.IsValid() ? FocusedInteractable.Get() : FocusedShopper.Get();
}

UAnimMontage* AVNHPlayerController::ResolveRoleActionMontage(EVNHPlayerRole PlayerRole, const AVNHShopperCharacter* Shopper, EVNHUniversalAction Action) const
{
	if (!Shopper || Action == EVNHUniversalAction::Fart)
	{
		return nullptr;
	}

	UDataTable* ActionAnimationTable = nullptr;
	if (PlayerRole == EVNHPlayerRole::Human)
	{
		ActionAnimationTable = HumanActionAnimationTable.LoadSynchronous();
	}
	else if (PlayerRole == EVNHPlayerRole::Alien)
	{
		ActionAnimationTable = AlienActionAnimationTable.LoadSynchronous();
	}

	if (!ActionAnimationTable)
	{
		return nullptr;
	}

	const FName RowName = ToUniversalActionRowName(Action);
	if (RowName.IsNone())
	{
		return nullptr;
	}

	const TMap<FName, uint8*>& RowMap = ActionAnimationTable->GetRowMap();
	uint8* const* RowDataPtr = RowMap.Find(RowName);
	const uint8* RowData = RowDataPtr ? *RowDataPtr : nullptr;
	if (!RowData)
	{
		return nullptr;
	}

	const UScriptStruct* RowStruct = ActionAnimationTable->GetRowStruct();
	const TCHAR* AnimationFieldName = TEXT("HighComposureAnimations");
	const float ComposureValue = Shopper->GetComposure();
	if (ComposureValue <= 0.0f)
	{
		AnimationFieldName = TEXT("NoComposureAnimations");
	}
	else if (ComposureValue <= 50.0f)
	{
		AnimationFieldName = TEXT("LowComposureAnimations");
	}

	return ChooseRoleActionMontageFromField(RowData, FindRoleActionArrayFieldByPrefix(RowStruct, AnimationFieldName));
}

void AVNHPlayerController::PlayRoleActionAnimation(EVNHPlayerRole PlayerRole, AVNHShopperCharacter* Shopper, EVNHUniversalAction Action) const
{
	if (!Shopper)
	{
		return;
	}

	if (UAnimMontage* Montage = ResolveRoleActionMontage(PlayerRole, Shopper, Action))
	{
		Shopper->PlayUniversalActionMontage(Montage);
	}
}

void AVNHPlayerController::PerformPickUp(AVNHShopperCharacter* Shopper, AActor* Prop)
{
	if (!Shopper || !Prop || Shopper->IsHoldingProp(Prop))
	{
		return;
	}

	Prop->SetReplicates(true);
	Prop->SetReplicateMovement(true);
	Prop->SetOwner(Shopper);
	Prop->SetInstigator(nullptr);
	Prop->Tags.Remove(TEXT("VNH.Thrown"));
	Prop->OnActorHit.RemoveDynamic(this, &AVNHPlayerController::HandleThrownPropHit);
	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Prop->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (PrimitiveComponent)
		{
			PrimitiveComponent->SetSimulatePhysics(false);
			PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	USkeletalMeshComponent* Mesh = Shopper->GetMesh();
	const FName AttachSocket = Mesh && Mesh->DoesSocketExist(TEXT("hand_r")) ? FName(TEXT("hand_r")) : NAME_None;
	Prop->AttachToComponent(Mesh ? static_cast<USceneComponent*>(Mesh) : Shopper->GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale, AttachSocket);
	const int32 HeldPropIndex = Shopper->GetHeldPropCount();
	Prop->SetActorRelativeLocation(FVector(8.0f, (HeldPropIndex % 3 - 1) * 8.0f, (HeldPropIndex / 3) * 6.0f));
	Shopper->SetHeldProp(Prop);
	SetInteractableOutline(Prop, false);
}

void AVNHPlayerController::PerformDrop(AVNHShopperCharacter* Shopper)
{
	AActor* Prop = Shopper ? Shopper->GetHeldProp() : nullptr;
	if (!Shopper || !Prop)
	{
		return;
	}

	Prop->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Prop->SetOwner(nullptr);
	Prop->SetInstigator(nullptr);
	Prop->Tags.Remove(TEXT("VNH.Thrown"));
	Prop->OnActorHit.RemoveDynamic(this, &AVNHPlayerController::HandleThrownPropHit);
	Prop->SetActorLocation(Shopper->GetActorLocation() + Shopper->GetActorForwardVector() * 95.0f + FVector(0.0f, 0.0f, 30.0f));
	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Prop->GetComponents(PrimitiveComponents);
	for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
	{
		if (PrimitiveComponent)
		{
			PrimitiveComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			PrimitiveComponent->SetSimulatePhysics(true);
		}
	}
	Shopper->SetHeldProp(nullptr);
}

AVNHShopperCharacter* AVNHPlayerController::GetInteractionShopper() const
{
	if (AVNHShopperCharacter* Shopper = FocusedShopper.Get())
	{
		return Shopper;
	}

	return TargetedShopper.Get();
}

bool AVNHPlayerController::IsAssignedHunter() const
{
	const AVNHPlayerState* VNHPlayerState = GetPlayerState<AVNHPlayerState>();
	return VNHPlayerState && VNHPlayerState->IsHunter();
}

bool AVNHPlayerController::IsLocalLobbyHost() const
{
	if (IsLocalController() && GetNetMode() != NM_Client)
	{
		return true;
	}

	const AVNHGameState* VNHGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	return PlayerState && VNHGameState && !VNHGameState->PlayerArray.IsEmpty() && VNHGameState->PlayerArray[0] == PlayerState;
}

float AVNHPlayerController::GetLobbyStartHoldProgress() const
{
	return FMath::Clamp(LobbyStartHoldSeconds / LobbyStartHoldRequiredSeconds, 0.0f, 1.0f);
}

bool AVNHPlayerController::GetLobbyStartPromptScreenPosition(FVector2D& OutScreenPosition) const
{
	const AVNHLobbyPlayButton* LobbyPlayButton = FocusedLobbyPlayButton.Get();
	if (!LobbyPlayButton)
	{
		return false;
	}

	const FVector PromptWorldLocation = LobbyPlayButton->GetActorLocation() + FVector(0.0f, 0.0f, 190.0f);
	return UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition(this, PromptWorldLocation, OutScreenPosition, true);
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
