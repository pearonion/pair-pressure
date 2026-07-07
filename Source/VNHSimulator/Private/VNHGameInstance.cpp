#include "VNHGameInstance.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Modules/ModuleManager.h"
#include "VNHCharacterCustomizerWidget.h"
#include "VNHCreateServerWidget.h"
#include "VNHCharacterProfileSave.h"
#include "VNHDebugHUD.h"
#include "VNHGameState.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"

#include <initializer_list>

#if WITH_EDITOR
#include "Editor.h"
#endif

namespace
{
const TCHAR* CharacterProfileSlotName = TEXT("VNHCharacterProfile");
constexpr int32 CharacterProfileUserIndex = 0;

TSoftObjectPtr<USkeletalMesh> MeshRef(const TCHAR* Path)
{
	if (!Path || Path[0] == TEXT('\0'))
	{
		return TSoftObjectPtr<USkeletalMesh>();
	}
	return TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(Path));
}

TSoftObjectPtr<USkeletalMesh> MeshRef(const FSoftObjectPath& Path)
{
	return Path.IsNull() ? TSoftObjectPtr<USkeletalMesh>() : TSoftObjectPtr<USkeletalMesh>(Path);
}

FString MeshLabel(const TSoftObjectPtr<USkeletalMesh>& Mesh)
{
	return Mesh.IsNull() ? FString(TEXT("NONE")) : Mesh.ToSoftObjectPath().GetAssetName();
}

FString OptionLabel(const FSoftObjectPath& Path)
{
	if (Path.IsNull())
	{
		return FString(TEXT("None"));
	}

	FString Name = Path.GetAssetName();
	Name.RemoveFromStart(TEXT("SK_"));
	Name.ReplaceInline(TEXT("_"), TEXT(" "));
	return Name;
}

bool NameStartsWithAny(const FString& Name, std::initializer_list<const TCHAR*> Prefixes)
{
	for (const TCHAR* Prefix : Prefixes)
	{
		if (Name.StartsWith(Prefix))
		{
			return true;
		}
	}
	return false;
}

bool NameContainsAny(const FString& Name, std::initializer_list<const TCHAR*> Fragments)
{
	for (const TCHAR* Fragment : Fragments)
	{
		if (Name.Contains(Fragment))
		{
			return true;
		}
	}
	return false;
}

bool MatchesCustomizationSlot(const FString& Name, EVNHCustomizationSlot CustomizationSlot)
{
	switch (CustomizationSlot)
	{
	case EVNHCustomizationSlot::Body:
		return NameStartsWithAny(Name, {TEXT("SK_Body_")});
	case EVNHCustomizationSlot::Hair:
		return NameStartsWithAny(Name, {TEXT("SK_Hairstyle_")});
	case EVNHCustomizationSlot::Face:
		return NameContainsAny(Name, {TEXT("_emotion_")});
	case EVNHCustomizationSlot::Hat:
		return NameStartsWithAny(Name, {TEXT("SK_Hat_"), TEXT("SK_Hat_Single_")});
	case EVNHCustomizationSlot::Mustache:
		return NameStartsWithAny(Name, {TEXT("SK_Mustache_"), TEXT("SK_Beard_")});
	case EVNHCustomizationSlot::Outfit:
		return NameStartsWithAny(Name, {TEXT("SK_Outfit_"), TEXT("SK_Costume_"), TEXT("SK_Mascot_")});
	case EVNHCustomizationSlot::Outwear:
		return NameStartsWithAny(Name, {TEXT("SK_Outwear_")});
	case EVNHCustomizationSlot::Pants:
		return NameStartsWithAny(Name, {TEXT("SK_Pants_"), TEXT("SK_Shorts_")});
	case EVNHCustomizationSlot::Shoes:
		return NameStartsWithAny(Name, {TEXT("SK_Shoe_"), TEXT("SK_Socks_")});
	case EVNHCustomizationSlot::Accessory:
		return NameStartsWithAny(Name, {
			TEXT("SK_Bandage_"),
			TEXT("SK_Clown_nose_"),
			TEXT("SK_Earrings_"),
			TEXT("SK_Glasses_"),
			TEXT("SK_Gloves_"),
			TEXT("SK_Headphones_"),
			TEXT("SK_Mask_"),
			TEXT("SK_Pacifier_"),
			TEXT("SK_Piercing_")
		});
	default:
		return false;
	}
}

const TArray<FSoftObjectPath>& CreativeCharacterOptions(EVNHCustomizationSlot CustomizationSlot)
{
	static TMap<EVNHCustomizationSlot, TArray<FSoftObjectPath>> CachedOptions;
	if (const TArray<FSoftObjectPath>* ExistingOptions = CachedOptions.Find(CustomizationSlot))
	{
		return *ExistingOptions;
	}

	TArray<FSoftObjectPath>& Options = CachedOptions.Add(CustomizationSlot);
	if (CustomizationSlot != EVNHCustomizationSlot::Body)
	{
		Options.Add(FSoftObjectPath());
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(TEXT("/Game/Creative_Characters/Skeleton_Meshes")));
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;

	TArray<FAssetData> AssetDataList;
	AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);
	AssetDataList.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.AssetName.LexicalLess(Right.AssetName);
	});

	for (const FAssetData& AssetData : AssetDataList)
	{
		if (MatchesCustomizationSlot(AssetData.AssetName.ToString(), CustomizationSlot))
		{
			Options.Add(AssetData.GetSoftObjectPath());
		}
	}

	UE_LOG(LogVNH, Display, TEXT("CustomizerOptions: slot %d loaded %d Creative_Characters options."), static_cast<int32>(CustomizationSlot), Options.Num());
	return Options;
}

FSoftObjectPath GetOptionOrNone(EVNHCustomizationSlot CustomizationSlot, int32 Index)
{
	const TArray<FSoftObjectPath>& Options = CreativeCharacterOptions(CustomizationSlot);
	return Options.IsValidIndex(Index) ? Options[Index] : FSoftObjectPath();
}

void CycleMesh(TSoftObjectPtr<USkeletalMesh>& Mesh, const TArray<FSoftObjectPath>& Options, int32 Direction)
{
	if (Options.IsEmpty())
	{
		return;
	}

	const FString CurrentPath = Mesh.ToSoftObjectPath().ToString();
	int32 CurrentIndex = 0;
	for (int32 Index = 0; Index < Options.Num(); ++Index)
	{
		if (CurrentPath == Options[Index].ToString())
		{
			CurrentIndex = Index;
			break;
		}
	}

	const int32 NextIndex = (CurrentIndex + Direction + Options.Num()) % Options.Num();
	Mesh = MeshRef(Options[NextIndex]);
}

void ApplyMeshToCustomizationSlot(FVNHCharacterCustomization& Customization, EVNHCustomizationSlot CustomizationSlot, const FSoftObjectPath& Option)
{
	switch (CustomizationSlot)
	{
	case EVNHCustomizationSlot::Body:
		if (!Option.IsNull())
		{
			Customization.BodyMesh = MeshRef(Option);
		}
		break;
	case EVNHCustomizationSlot::Hair:
		Customization.HairMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Face:
		Customization.FaceMesh = MeshRef(Option);
		Customization.bNoFace = Option.IsNull();
		break;
	case EVNHCustomizationSlot::Hat:
		Customization.HatMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Mustache:
		Customization.MustacheMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Outfit:
		Customization.OutfitMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Outwear:
		Customization.OutwearMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Pants:
		Customization.PantsMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Shoes:
		Customization.ShoesMesh = MeshRef(Option);
		break;
	case EVNHCustomizationSlot::Accessory:
		Customization.AccessoryMesh = MeshRef(Option);
		break;
	default:
		break;
	}
}

bool AllowsNoneOption(EVNHCustomizationSlot CustomizationSlot)
{
	return CustomizationSlot != EVNHCustomizationSlot::Body;
}

FVNHCustomizationItem MakeNoneCustomizationItem(EVNHCustomizationSlot CustomizationSlot)
{
	FVNHCustomizationItem Item;
	Item.Category = CustomizationSlot;
	Item.DisplayName = NSLOCTEXT("VNH", "CustomizerNoneOption", "None");
	Item.SortOrder = MIN_int32;
	Item.bEnabled = true;
	return Item;
}

void ResetPIETransactionBuffer()
{
#if WITH_EDITOR
	if (GEditor)
	{
		GEditor->ResetTransaction(NSLOCTEXT("VNH", "ResetPIETransactions", "Clearing PIE-only VNH runtime references"));
	}
#endif
}

bool IsMainMenuWorld(const UWorld* World)
{
	return World && World->GetMapName().Contains(TEXT("MainMenu"));
}

bool IsLobbyWorld(const UWorld* World)
{
	return World && World->GetMapName().Contains(TEXT("Lobby"));
}

bool IsInGameCustomizationAllowed(const UWorld* World)
{
	if (IsMainMenuWorld(World))
	{
		return true;
	}

	const AVNHGameState* VNHGameState = World ? World->GetGameState<AVNHGameState>() : nullptr;
	if (!VNHGameState)
	{
		return false;
	}

	const EVNHRoundPhase RoundPhase = VNHGameState->GetRoundPhase();
	return RoundPhase == EVNHRoundPhase::WaitingForPlayers
		|| RoundPhase == EVNHRoundPhase::AssigningRoles;
}
}

void UVNHGameInstance::Init()
{
	Super::Init();
	ClearFlags(RF_Transactional);
	EnsureCharacterProfileLoaded();

	if (!MainMenuWidgetClass)
	{
		MainMenuWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_VNHMainMenu.WBP_VNHMainMenu_C"));
	}

	if (CustomizationItemsTable.IsNull())
	{
		CustomizationItemsTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/UI/Customizer/DT_VNHCustomizationItems.DT_VNHCustomizationItems")));
	}
}

void UVNHGameInstance::Shutdown()
{
	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
		PlayerController->bShowMouseCursor = false;
		PlayerController->bEnableClickEvents = false;
		PlayerController->bEnableMouseOverEvents = false;
	}

	if (ActiveCustomizer)
	{
		ActiveCustomizer->RemoveFromParent();
		ActiveCustomizer = nullptr;
	}

	if (ActiveMainMenu)
	{
		ActiveMainMenu->RemoveFromParent();
		ActiveMainMenu = nullptr;
	}

	CharacterProfile = nullptr;
	ResetPIETransactionBuffer();
	Super::Shutdown();
}

void UVNHGameInstance::ShowMainMenu()
{
	UE_LOG(LogVNH, Display, TEXT("MainMenu: native GameInstance menu path skipped; BP_MainMenuBootstrap owns the menu screen."));
}

void UVNHGameInstance::HideMainMenu()
{
	if (ActiveMainMenu)
	{
		ActiveMainMenu->RemoveFromParent();
		ActiveMainMenu = nullptr;
	}

	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		PlayerController->bShowMouseCursor = true;
		PlayerController->bEnableClickEvents = true;
		PlayerController->bEnableMouseOverEvents = true;

		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}
}

void UVNHGameInstance::HostPrivateGame()
{
	HostGame(false);
}

void UVNHGameInstance::HostPublicGame()
{
	HostGame(true);
}

void UVNHGameInstance::JoinGameByAddress(const FString& Address)
{
	const FString TravelAddress = Address.IsEmpty() ? DefaultJoinAddress : Address;
	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		HideMainMenu();
		PlayerController->ClientTravel(TravelAddress, TRAVEL_Absolute);
		UE_LOG(LogVNH, Display, TEXT("MainMenu: joining %s."), *TravelAddress);
	}
}

void UVNHGameInstance::HandleMenuHostPrivateClicked()
{
	SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusHostPrivate", "Opening private lobby..."));
	HostPrivateGame();
}

void UVNHGameInstance::HandleMenuHostPublicClicked()
{
	SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusHostPublic", "Opening public lobby..."));
	HostPublicGame();
}

void UVNHGameInstance::HandleMenuJoinClicked()
{
	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		if (TSubclassOf<UUserWidget> ServerBrowserClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_ServerBrowser.WBP_ServerBrowser_C")))
		{
			if (UUserWidget* ServerBrowser = CreateWidget<UUserWidget>(PlayerController, ServerBrowserClass))
			{
				ServerBrowser->AddToViewport(9500);
				FInputModeGameAndUI InputMode;
				InputMode.SetWidgetToFocus(ServerBrowser->TakeWidget());
				InputMode.SetHideCursorDuringCapture(false);
				InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
				PlayerController->SetInputMode(InputMode);
				PlayerController->bShowMouseCursor = true;
				SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusServerBrowser", "Opening server browser..."));
				return;
			}
		}
	}

	SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusServerBrowserMissing", "Server browser is not available."));
}

void UVNHGameInstance::HandleMenuQuitClicked()
{
	UKismetSystemLibrary::QuitGame(this, GetFirstLocalPlayerController(), EQuitPreference::Quit, false);
}

void UVNHGameInstance::HandleMenuCustomizerClicked()
{
	ShowCharacterCustomizer(false);
}

void UVNHGameInstance::HostGame(bool bPublic)
{
	APlayerController* PlayerController = GetFirstLocalPlayerController();
	if (!PlayerController)
	{
		SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusHostNoController", "Create Game is unavailable without a local player."));
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: cannot open create-server dialog without a local player controller."));
		return;
	}

	UClass* CreateServerWidgetClass = LoadClass<UVNHCreateServerWidget>(nullptr, TEXT("/Game/UI/WBP_CreateServerDialog.WBP_CreateServerDialog_C"));
	if (!CreateServerWidgetClass)
	{
		CreateServerWidgetClass = UVNHCreateServerWidget::StaticClass();
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: /Game/UI/WBP_CreateServerDialog unavailable; using native create-server widget."));
	}

	UVNHCreateServerWidget* CreateServerWidget = CreateWidget<UVNHCreateServerWidget>(PlayerController, CreateServerWidgetClass);
	if (!CreateServerWidget)
	{
		SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusCreateServerMissing", "Create Game dialog is not available."));
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: CreateWidget failed for create-server dialog."));
		return;
	}

	CreateServerWidget->AddToViewport(9600);
	CreateServerWidget->ConfigureInitialMode(!bPublic);

	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(CreateServerWidget->TakeWidget());
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
	PlayerController->bShowMouseCursor = true;
	PlayerController->bEnableClickEvents = true;
	PlayerController->bEnableMouseOverEvents = true;

	SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusCreateServerDialog", "Choose your server settings."));
	UE_LOG(LogVNH, Display, TEXT("MainMenu: opened create-server dialog for %s game."), bPublic ? TEXT("public") : TEXT("private"));
}

void UVNHGameInstance::BindMainMenuButtons(UUserWidget* MainMenuWidget)
{
	if (!MainMenuWidget)
	{
		return;
	}

	if (UButton* HostPrivateButton = Cast<UButton>(MainMenuWidget->GetWidgetFromName(TEXT("HostPrivateButton"))))
	{
		HostPrivateButton->OnClicked.AddUniqueDynamic(this, &UVNHGameInstance::HandleMenuHostPrivateClicked);
	}

	if (UButton* HostPublicButton = Cast<UButton>(MainMenuWidget->GetWidgetFromName(TEXT("HostPublicButton"))))
	{
		HostPublicButton->OnClicked.AddUniqueDynamic(this, &UVNHGameInstance::HandleMenuHostPublicClicked);
	}

	if (UButton* JoinButton = Cast<UButton>(MainMenuWidget->GetWidgetFromName(TEXT("JoinButton"))))
	{
		JoinButton->OnClicked.AddUniqueDynamic(this, &UVNHGameInstance::HandleMenuJoinClicked);
	}

	if (UButton* QuitButton = Cast<UButton>(MainMenuWidget->GetWidgetFromName(TEXT("QuitButton"))))
	{
		QuitButton->OnClicked.AddUniqueDynamic(this, &UVNHGameInstance::HandleMenuQuitClicked);
	}

	if (UEditableTextBox* JoinAddressTextBox = Cast<UEditableTextBox>(MainMenuWidget->GetWidgetFromName(TEXT("JoinAddressTextBox"))))
	{
		if (JoinAddressTextBox->GetText().IsEmpty())
		{
			JoinAddressTextBox->SetText(FText::FromString(DefaultJoinAddress));
		}
	}
}

void UVNHGameInstance::SetMainMenuStatus(const FText& StatusText)
{
	if (!ActiveMainMenu)
	{
		return;
	}

	if (UTextBlock* MenuStatusText = Cast<UTextBlock>(ActiveMainMenu->GetWidgetFromName(TEXT("MenuStatusText"))))
	{
		MenuStatusText->SetText(StatusText);
	}
}

FString UVNHGameInstance::GetJoinAddressFromMenu() const
{
	if (!ActiveMainMenu)
	{
		return DefaultJoinAddress;
	}

	if (const UEditableTextBox* JoinAddressTextBox = Cast<UEditableTextBox>(ActiveMainMenu->GetWidgetFromName(TEXT("JoinAddressTextBox"))))
	{
		return JoinAddressTextBox->GetText().ToString();
	}

	return DefaultJoinAddress;
}

void UVNHGameInstance::ShowCharacterCustomizer(bool bLobbyMode)
{
	UWorld* World = GetWorld();
	if (!bLobbyMode && !IsInGameCustomizationAllowed(World))
	{
		HideCharacterCustomizer();
		UE_LOG(LogVNH, Display, TEXT("Customizer: blocked because the round is already active."));
		return;
	}
	if (bLobbyMode && !IsLobbyWorld(World) && !IsInGameCustomizationAllowed(World))
	{
		HideCharacterCustomizer();
		UE_LOG(LogVNH, Display, TEXT("Customizer: blocked because the round is already active."));
		return;
	}

	EnsureCharacterProfileLoaded();

	if (ActiveCustomizer)
	{
		ActiveCustomizer->RemoveFromParent();
		ActiveCustomizer = nullptr;
	}

	APlayerController* PlayerController = GetFirstLocalPlayerController();
	if (!PlayerController)
	{
		UE_LOG(LogVNH, Warning, TEXT("Customizer: cannot open without a local player controller."));
		return;
	}
	if (AVNHDebugHUD* DebugHUD = PlayerController->GetHUD<AVNHDebugHUD>())
	{
		DebugHUD->SetDebugPanelVisible(false);
	}

	UClass* CustomizerWidgetClass = LoadClass<UVNHCharacterCustomizerWidget>(nullptr, TEXT("/Game/UI/WBP_VNHCharacterCustomizer.WBP_VNHCharacterCustomizer_C"));
	if (!CustomizerWidgetClass)
	{
		CustomizerWidgetClass = UVNHCharacterCustomizerWidget::StaticClass();
	}

	ActiveCustomizer = CreateWidget<UVNHCharacterCustomizerWidget>(PlayerController, CustomizerWidgetClass);
	if (!ActiveCustomizer)
	{
		return;
	}

	ActiveCustomizer->ClearFlags(RF_Transactional);
	ActiveCustomizer->SetLobbyMode(bLobbyMode);
	ActiveCustomizer->AddToViewport(bLobbyMode ? 21000 : 9000);

	PlayerController->bShowMouseCursor = true;
	PlayerController->bEnableClickEvents = true;
	PlayerController->bEnableMouseOverEvents = true;

	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(ActiveCustomizer->TakeWidget());
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);
}

void UVNHGameInstance::HideCharacterCustomizer()
{
	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		PlayerController->SetInputMode(FInputModeGameAndUI());
	}

	if (ActiveCustomizer)
	{
		ActiveCustomizer->RemoveFromParent();
		ActiveCustomizer = nullptr;
	}
	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();

	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		PlayerController->bShowMouseCursor = true;
		PlayerController->bEnableClickEvents = true;
		PlayerController->bEnableMouseOverEvents = true;

		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}

	ResetPIETransactionBuffer();
}

void UVNHGameInstance::SelectCharacterPreset(int32 PresetIndex)
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(PresetIndex))
	{
		return;
	}

	CharacterProfile->ActivePresetIndex = PresetIndex;
	CharacterProfile->Presets[PresetIndex].PresetIndex = PresetIndex;
	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

void UVNHGameInstance::SaveActiveCharacterPreset()
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return;
	}

	CharacterProfile->Presets[CharacterProfile->ActivePresetIndex].PresetIndex = CharacterProfile->ActivePresetIndex;
	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

void UVNHGameInstance::LoadActiveCharacterPreset()
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile)
	{
		return;
	}

	SelectCharacterPreset(CharacterProfile->ActivePresetIndex);
}

void UVNHGameInstance::ClearActiveCustomizationCosmetics()
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return;
	}

	FVNHCharacterCustomization& Customization = CharacterProfile->Presets[CharacterProfile->ActivePresetIndex];
	Customization.HairMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.FaceMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.HatMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.MustacheMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.OutfitMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.OutwearMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.PantsMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.ShoesMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.AccessoryMesh = TSoftObjectPtr<USkeletalMesh>();
	Customization.bNoFace = true;
	Customization.PresetIndex = CharacterProfile->ActivePresetIndex;

	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

void UVNHGameInstance::CycleCustomizationSlot(EVNHCustomizationSlot CustomizationSlot, int32 Direction)
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return;
	}

	FVNHCharacterCustomization& Customization = CharacterProfile->Presets[CharacterProfile->ActivePresetIndex];
	switch (CustomizationSlot)
	{
	case EVNHCustomizationSlot::Body:
		CycleMesh(Customization.BodyMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Hair:
		CycleMesh(Customization.HairMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Face:
		CycleMesh(Customization.FaceMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		Customization.bNoFace = Customization.FaceMesh.IsNull();
		break;
	case EVNHCustomizationSlot::Hat:
		CycleMesh(Customization.HatMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Mustache:
		CycleMesh(Customization.MustacheMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Outfit:
		CycleMesh(Customization.OutfitMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Outwear:
		CycleMesh(Customization.OutwearMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Pants:
		CycleMesh(Customization.PantsMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Shoes:
		CycleMesh(Customization.ShoesMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	case EVNHCustomizationSlot::Accessory:
		CycleMesh(Customization.AccessoryMesh, CreativeCharacterOptions(CustomizationSlot), Direction);
		break;
	default:
		break;
	}

	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

void UVNHGameInstance::SelectCustomizationSlotOption(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex)
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return;
	}

	const TArray<FVNHCustomizationItem> Options = GetCustomizationItemsForSlot(CustomizationSlot);
	if (!Options.IsValidIndex(OptionIndex))
	{
		return;
	}

	FVNHCharacterCustomization& Customization = CharacterProfile->Presets[CharacterProfile->ActivePresetIndex];
	ApplyMeshToCustomizationSlot(Customization, CustomizationSlot, Options[OptionIndex].Mesh.ToSoftObjectPath());
	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

int32 UVNHGameInstance::GetCustomizationSlotOptionCount(EVNHCustomizationSlot CustomizationSlot) const
{
	return GetCustomizationItemsForSlot(CustomizationSlot).Num();
}

FString UVNHGameInstance::GetCustomizationSlotOptionLabel(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const
{
	const TArray<FVNHCustomizationItem> Options = GetCustomizationItemsForSlot(CustomizationSlot);
	if (!Options.IsValidIndex(OptionIndex))
	{
		return FString();
	}

	if (!Options[OptionIndex].DisplayName.IsEmpty())
	{
		return Options[OptionIndex].DisplayName.ToString();
	}

	return OptionLabel(Options[OptionIndex].Mesh.ToSoftObjectPath());
}

TSoftObjectPtr<UTexture2D> UVNHGameInstance::GetCustomizationSlotOptionIcon(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const
{
	const TArray<FVNHCustomizationItem> Options = GetCustomizationItemsForSlot(CustomizationSlot);
	return Options.IsValidIndex(OptionIndex) ? Options[OptionIndex].Icon : TSoftObjectPtr<UTexture2D>();
}

bool UVNHGameInstance::IsCustomizationSlotOptionEmpty(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const
{
	const TArray<FVNHCustomizationItem> Options = GetCustomizationItemsForSlot(CustomizationSlot);
	return Options.IsValidIndex(OptionIndex) && Options[OptionIndex].Mesh.IsNull();
}

void UVNHGameInstance::RandomizeActiveCustomization()
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return;
	}

	FVNHCharacterCustomization& Customization = CharacterProfile->Presets[CharacterProfile->ActivePresetIndex];
	const int32 PresetIndex = CharacterProfile->ActivePresetIndex;
	auto PickRandomMesh = [this](EVNHCustomizationSlot CustomizationSlot)
	{
		const TArray<FVNHCustomizationItem> Options = GetCustomizationItemsForSlot(CustomizationSlot);
		return Options.IsEmpty() ? TSoftObjectPtr<USkeletalMesh>() : Options[FMath::RandRange(0, Options.Num() - 1)].Mesh;
	};

	Customization.BodyMesh = PickRandomMesh(EVNHCustomizationSlot::Body);
	Customization.HairMesh = PickRandomMesh(EVNHCustomizationSlot::Hair);
	Customization.FaceMesh = PickRandomMesh(EVNHCustomizationSlot::Face);
	Customization.HatMesh = PickRandomMesh(EVNHCustomizationSlot::Hat);
	Customization.MustacheMesh = PickRandomMesh(EVNHCustomizationSlot::Mustache);
	Customization.OutfitMesh = PickRandomMesh(EVNHCustomizationSlot::Outfit);
	Customization.OutwearMesh = PickRandomMesh(EVNHCustomizationSlot::Outwear);
	Customization.PantsMesh = PickRandomMesh(EVNHCustomizationSlot::Pants);
	Customization.ShoesMesh = PickRandomMesh(EVNHCustomizationSlot::Shoes);
	Customization.AccessoryMesh = PickRandomMesh(EVNHCustomizationSlot::Accessory);
	Customization.BodyColor = FLinearColor::MakeRandomColor();
	Customization.HairColor = FLinearColor::MakeRandomColor();
	Customization.OutfitColor = FLinearColor::MakeRandomColor();
	Customization.bNoFace = false;
	Customization.PresetIndex = PresetIndex;
	Customization.Nickname = TEXT("Deeply Questionable");

	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
}

FVNHCharacterCustomization UVNHGameInstance::GetActiveCustomization()
{
	EnsureCharacterProfileLoaded();
	if (!CharacterProfile || !CharacterProfile->Presets.IsValidIndex(CharacterProfile->ActivePresetIndex))
	{
		return MakeDefaultCustomization(0);
	}

	return CharacterProfile->Presets[CharacterProfile->ActivePresetIndex];
}

FString UVNHGameInstance::GetActiveCustomizationSummary()
{
	const FVNHCharacterCustomization& Customization = GetActiveCustomization();
	return FString::Printf(
		TEXT("PRESET %d // %s // Body %s // Hair %s // Face %s // Fit %s // Pants %s // Shoes %s // Hat %s // Accessory %s"),
		Customization.PresetIndex + 1,
		*Customization.Nickname.ToString(),
		*MeshLabel(Customization.BodyMesh),
		Customization.HairMesh.IsNull() ? TEXT("BALD POWER") : *MeshLabel(Customization.HairMesh),
		Customization.bNoFace ? TEXT("NO FACE, NO PROBLEM") : *MeshLabel(Customization.FaceMesh),
		Customization.OutfitMesh.IsNull() ? TEXT("NO FIT") : *MeshLabel(Customization.OutfitMesh),
		Customization.PantsMesh.IsNull() ? TEXT("NO PANTS") : *MeshLabel(Customization.PantsMesh),
		Customization.ShoesMesh.IsNull() ? TEXT("NO SHOES") : *MeshLabel(Customization.ShoesMesh),
		Customization.HatMesh.IsNull() ? TEXT("NO HAT") : *MeshLabel(Customization.HatMesh),
		Customization.AccessoryMesh.IsNull() ? TEXT("NO ACCESSORY") : *MeshLabel(Customization.AccessoryMesh));
}

void UVNHGameInstance::PreviewActiveCustomizationOnLocalPawn()
{
	if (AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetFirstLocalPlayerController()))
	{
		VNHPlayerController->ApplySavedCharacterCustomization();
	}
}

void UVNHGameInstance::EnsureCharacterProfileLoaded()
{
	if (CharacterProfile)
	{
		return;
	}

	if (UGameplayStatics::DoesSaveGameExist(CharacterProfileSlotName, CharacterProfileUserIndex))
	{
		CharacterProfile = Cast<UVNHCharacterProfileSave>(UGameplayStatics::LoadGameFromSlot(CharacterProfileSlotName, CharacterProfileUserIndex));
	}

	if (!CharacterProfile)
	{
		CharacterProfile = Cast<UVNHCharacterProfileSave>(UGameplayStatics::CreateSaveGameObject(UVNHCharacterProfileSave::StaticClass()));
	}

	if (!CharacterProfile)
	{
		return;
	}

	CharacterProfile->ClearFlags(RF_Transactional);

	while (CharacterProfile->Presets.Num() < 3)
	{
		CharacterProfile->Presets.Add(MakeDefaultCustomization(CharacterProfile->Presets.Num()));
	}
	CharacterProfile->ActivePresetIndex = FMath::Clamp(CharacterProfile->ActivePresetIndex, 0, CharacterProfile->Presets.Num() - 1);
}

void UVNHGameInstance::SaveCharacterProfile()
{
	if (CharacterProfile)
	{
		UGameplayStatics::SaveGameToSlot(CharacterProfile, CharacterProfileSlotName, CharacterProfileUserIndex);
	}
}

FVNHCharacterCustomization UVNHGameInstance::MakeDefaultCustomization(int32 PresetIndex) const
{
	FVNHCharacterCustomization Customization;
	Customization.PresetIndex = PresetIndex;
	Customization.BodyMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Body, 8 + PresetIndex));
	Customization.HairMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Hair, 1 + PresetIndex));
	Customization.FaceMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Face, 1 + PresetIndex));
	Customization.HatMesh = PresetIndex == 1 ? MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Hat, 3)) : TSoftObjectPtr<USkeletalMesh>();
	Customization.MustacheMesh = PresetIndex == 2 ? MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Mustache, 2)) : TSoftObjectPtr<USkeletalMesh>();
	Customization.OutfitMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Outfit, 1 + PresetIndex));
	Customization.OutwearMesh = PresetIndex == 0 ? TSoftObjectPtr<USkeletalMesh>() : MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Outwear, PresetIndex));
	Customization.PantsMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Pants, PresetIndex));
	Customization.ShoesMesh = MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Shoes, PresetIndex));
	Customization.AccessoryMesh = PresetIndex == 2 ? MeshRef(GetOptionOrNone(EVNHCustomizationSlot::Accessory, 3)) : TSoftObjectPtr<USkeletalMesh>();
	Customization.BodyColor = PresetIndex == 0 ? FLinearColor(0.95f, 0.58f, 0.36f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.58f, 0.36f, 0.22f, 1.0f) : FLinearColor(0.78f, 0.70f, 0.54f, 1.0f);
	Customization.HairColor = PresetIndex == 0 ? FLinearColor(0.10f, 0.06f, 0.03f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.40f, 0.22f, 0.08f, 1.0f) : FLinearColor(0.02f, 0.02f, 0.025f, 1.0f);
	Customization.OutfitColor = PresetIndex == 0 ? FLinearColor(0.05f, 0.58f, 0.82f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.90f, 0.18f, 0.30f, 1.0f) : FLinearColor(0.95f, 0.76f, 0.10f, 1.0f);
	Customization.Nickname = PresetIndex == 0 ? TEXT("Greg Adjacent") : PresetIndex == 1 ? TEXT("Mall Cryptid") : TEXT("No Notes");
	return Customization;
}

UDataTable* UVNHGameInstance::GetCustomizationItemsTable() const
{
	return CustomizationItemsTable.IsNull() ? nullptr : CustomizationItemsTable.LoadSynchronous();
}

TArray<FVNHCustomizationItem> UVNHGameInstance::GetCustomizationItemsForSlot(EVNHCustomizationSlot CustomizationSlot) const
{
	TArray<FVNHCustomizationItem> Items;
	if (const UDataTable* DataTable = GetCustomizationItemsTable())
	{
		TArray<FVNHCustomizationItem*> Rows;
		DataTable->GetAllRows<FVNHCustomizationItem>(TEXT("VNHCustomizationItems"), Rows);
		for (const FVNHCustomizationItem* Row : Rows)
		{
			if (!Row || !Row->bEnabled || Row->Category != CustomizationSlot)
			{
				continue;
			}

			FVNHCustomizationItem Item = *Row;
			if (Item.DisplayName.IsEmpty())
			{
				Item.DisplayName = FText::FromString(OptionLabel(Item.Mesh.ToSoftObjectPath()));
			}
			Items.Add(Item);
		}

		Items.Sort([](const FVNHCustomizationItem& Left, const FVNHCustomizationItem& Right)
		{
			if (Left.SortOrder != Right.SortOrder)
			{
				return Left.SortOrder < Right.SortOrder;
			}
			return Left.DisplayName.ToString() < Right.DisplayName.ToString();
		});

		if (!Items.IsEmpty())
		{
			if (AllowsNoneOption(CustomizationSlot))
			{
				Items.Insert(MakeNoneCustomizationItem(CustomizationSlot), 0);
			}
			return Items;
		}
	}

	const TArray<FSoftObjectPath>& FallbackOptions = CreativeCharacterOptions(CustomizationSlot);
	for (int32 Index = 0; Index < FallbackOptions.Num(); ++Index)
	{
		FVNHCustomizationItem Item;
		Item.Category = CustomizationSlot;
		Item.DisplayName = FText::FromString(OptionLabel(FallbackOptions[Index]));
		Item.Mesh = MeshRef(FallbackOptions[Index]);
		Item.SortOrder = Index;
		Item.bEnabled = true;
		Items.Add(Item);
	}

	return Items;
}
