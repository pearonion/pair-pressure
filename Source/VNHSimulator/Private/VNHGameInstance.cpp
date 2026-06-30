#include "VNHGameInstance.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "VNHCharacterCustomizerWidget.h"
#include "VNHCharacterProfileSave.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"

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

FString MeshLabel(const TSoftObjectPtr<USkeletalMesh>& Mesh)
{
	return Mesh.IsNull() ? FString(TEXT("NONE")) : Mesh.ToSoftObjectPath().GetAssetName();
}

const TArray<const TCHAR*>& BodyOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT("/Game/TNG/Characters/BaseBodies/SK_TNG_Body_009.SK_TNG_Body_009"),
		TEXT("/Game/TNG/Characters/BaseBodies/SK_TNG_Body_001.SK_TNG_Body_001"),
		TEXT("/Game/TNG/Characters/BaseBodies/SK_TNG_Body_004.SK_TNG_Body_004"),
		TEXT("/Game/TNG/Characters/BaseBodies/SK_TNG_Body_012.SK_TNG_Body_012")
	};
	return Options;
}

const TArray<const TCHAR*>& HairOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT(""),
		TEXT("/Game/TNG/Characters/Cosmetics/Hair/SK_TNG_Hair_Male_003.SK_TNG_Hair_Male_003"),
		TEXT("/Game/TNG/Characters/Cosmetics/Hair/SK_TNG_Hair_Male_006.SK_TNG_Hair_Male_006"),
		TEXT("/Game/TNG/Characters/Cosmetics/Hair/SK_TNG_Hair_Female_003.SK_TNG_Hair_Female_003")
	};
	return Options;
}

const TArray<const TCHAR*>& FaceOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT("/Game/TNG/Characters/Cosmetics/Faces/SK_TNG_Face_Happy_001.SK_TNG_Face_Happy_001"),
		TEXT("/Game/TNG/Characters/Cosmetics/Faces/SK_TNG_Face_Surprised_001.SK_TNG_Face_Surprised_001"),
		TEXT("/Game/TNG/Characters/Cosmetics/Faces/SK_TNG_Face_Evil_001.SK_TNG_Face_Evil_001"),
		TEXT("")
	};
	return Options;
}

const TArray<const TCHAR*>& HatOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT(""),
		TEXT("/Game/TNG/Characters/Cosmetics/Hats/SK_TNG_Hat_003.SK_TNG_Hat_003"),
		TEXT("/Game/TNG/Characters/Cosmetics/Hats/SK_TNG_Hat_010.SK_TNG_Hat_010"),
		TEXT("/Game/TNG/Characters/Cosmetics/Hats/SK_TNG_Hat_020.SK_TNG_Hat_020")
	};
	return Options;
}

const TArray<const TCHAR*>& MustacheOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT(""),
		TEXT("/Game/TNG/Characters/Cosmetics/Mustaches/SK_TNG_Mustache_001.SK_TNG_Mustache_001"),
		TEXT("/Game/TNG/Characters/Cosmetics/Mustaches/SK_TNG_Mustache_004.SK_TNG_Mustache_004"),
		TEXT("/Game/TNG/Characters/Cosmetics/Mustaches/SK_TNG_Mustache_010.SK_TNG_Mustache_010")
	};
	return Options;
}

const TArray<const TCHAR*>& OutfitOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT("/Game/TNG/Characters/Cosmetics/Outfits/SK_TNG_Outfit_001.SK_TNG_Outfit_001"),
		TEXT("/Game/TNG/Characters/Cosmetics/Outfits/SK_TNG_Outfit_004.SK_TNG_Outfit_004"),
		TEXT("/Game/TNG/Characters/Cosmetics/Outfits/SK_TNG_Outfit_007.SK_TNG_Outfit_007"),
		TEXT("/Game/TNG/Characters/Cosmetics/Outfits/SK_TNG_Costume_7_001.SK_TNG_Costume_7_001")
	};
	return Options;
}

const TArray<const TCHAR*>& OutwearOptions()
{
	static const TArray<const TCHAR*> Options = {
		TEXT(""),
		TEXT("/Game/TNG/Characters/Cosmetics/Outwear/SK_TNG_Outwear_001.SK_TNG_Outwear_001"),
		TEXT("/Game/TNG/Characters/Cosmetics/Outwear/SK_TNG_Outwear_014.SK_TNG_Outwear_014"),
		TEXT("/Game/TNG/Characters/Cosmetics/Outwear/SK_TNG_Outwear_033.SK_TNG_Outwear_033")
	};
	return Options;
}

void CycleMesh(TSoftObjectPtr<USkeletalMesh>& Mesh, const TArray<const TCHAR*>& Options, int32 Direction)
{
	if (Options.IsEmpty())
	{
		return;
	}

	const FString CurrentPath = Mesh.ToSoftObjectPath().ToString();
	int32 CurrentIndex = 0;
	for (int32 Index = 0; Index < Options.Num(); ++Index)
	{
		if (CurrentPath == FString(Options[Index]))
		{
			CurrentIndex = Index;
			break;
		}
	}

	const int32 NextIndex = (CurrentIndex + Direction + Options.Num()) % Options.Num();
	Mesh = Options[NextIndex][0] == TEXT('\0') ? TSoftObjectPtr<USkeletalMesh>() : MeshRef(Options[NextIndex]);
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
}

void UVNHGameInstance::Shutdown()
{
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
	const FString Address = GetJoinAddressFromMenu();
	SetMainMenuStatus(FText::FromString(FString::Printf(TEXT("Joining %s..."), Address.IsEmpty() ? *DefaultJoinAddress : *Address)));
	JoinGameByAddress(Address);
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
	HideMainMenu();

	const FString Options = FString::Printf(TEXT("listen?Public=%s"), bPublic ? TEXT("1") : TEXT("0"));
	UGameplayStatics::OpenLevel(this, LobbyMapName, true, Options);
	UE_LOG(LogVNH, Display, TEXT("MainMenu: hosting %s lobby on %s."), bPublic ? TEXT("public") : TEXT("private"), *LobbyMapName.ToString());
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

	ActiveCustomizer = CreateWidget<UVNHCharacterCustomizerWidget>(PlayerController, UVNHCharacterCustomizerWidget::StaticClass());
	if (!ActiveCustomizer)
	{
		return;
	}

	ActiveCustomizer->ClearFlags(RF_Transactional);
	ActiveCustomizer->SetLobbyMode(bLobbyMode);
	ActiveCustomizer->AddToViewport(bLobbyMode ? 8000 : 9000);

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
		CycleMesh(Customization.BodyMesh, BodyOptions(), Direction);
		break;
	case EVNHCustomizationSlot::Hair:
		CycleMesh(Customization.HairMesh, HairOptions(), Direction);
		break;
	case EVNHCustomizationSlot::Face:
		CycleMesh(Customization.FaceMesh, FaceOptions(), Direction);
		Customization.bNoFace = Customization.FaceMesh.IsNull();
		break;
	case EVNHCustomizationSlot::Hat:
		CycleMesh(Customization.HatMesh, HatOptions(), Direction);
		break;
	case EVNHCustomizationSlot::Mustache:
		CycleMesh(Customization.MustacheMesh, MustacheOptions(), Direction);
		break;
	case EVNHCustomizationSlot::Outfit:
		CycleMesh(Customization.OutfitMesh, OutfitOptions(), Direction);
		break;
	case EVNHCustomizationSlot::Outwear:
		CycleMesh(Customization.OutwearMesh, OutwearOptions(), Direction);
		break;
	default:
		break;
	}

	SaveCharacterProfile();
	PreviewActiveCustomizationOnLocalPawn();
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
	Customization.BodyMesh = MeshRef(BodyOptions()[FMath::RandRange(0, BodyOptions().Num() - 1)]);
	Customization.HairMesh = MeshRef(HairOptions()[FMath::RandRange(1, HairOptions().Num() - 1)]);
	Customization.FaceMesh = MeshRef(FaceOptions()[FMath::RandRange(0, FaceOptions().Num() - 2)]);
	Customization.HatMesh = MeshRef(HatOptions()[FMath::RandRange(0, HatOptions().Num() - 1)]);
	Customization.MustacheMesh = MeshRef(MustacheOptions()[FMath::RandRange(0, MustacheOptions().Num() - 1)]);
	Customization.OutfitMesh = MeshRef(OutfitOptions()[FMath::RandRange(0, OutfitOptions().Num() - 1)]);
	Customization.OutwearMesh = MeshRef(OutwearOptions()[FMath::RandRange(0, OutwearOptions().Num() - 1)]);
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
		TEXT("PRESET %d // %s // Body %s // Hair %s // Hat %s // Face %s"),
		Customization.PresetIndex + 1,
		*Customization.Nickname.ToString(),
		*MeshLabel(Customization.BodyMesh),
		Customization.HairMesh.IsNull() ? TEXT("BALD POWER") : *MeshLabel(Customization.HairMesh),
		Customization.HatMesh.IsNull() ? TEXT("NO HAT") : *MeshLabel(Customization.HatMesh),
		Customization.bNoFace ? TEXT("NO FACE, NO PROBLEM") : *MeshLabel(Customization.FaceMesh));
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
	Customization.BodyMesh = MeshRef(BodyOptions()[PresetIndex % BodyOptions().Num()]);
	Customization.HairMesh = MeshRef(HairOptions()[1 + (PresetIndex % (HairOptions().Num() - 1))]);
	Customization.FaceMesh = MeshRef(FaceOptions()[PresetIndex % (FaceOptions().Num() - 1)]);
	Customization.HatMesh = PresetIndex == 1 ? MeshRef(HatOptions()[2]) : TSoftObjectPtr<USkeletalMesh>();
	Customization.MustacheMesh = PresetIndex == 2 ? MeshRef(MustacheOptions()[2]) : TSoftObjectPtr<USkeletalMesh>();
	Customization.OutfitMesh = MeshRef(OutfitOptions()[PresetIndex % OutfitOptions().Num()]);
	Customization.OutwearMesh = PresetIndex == 0 ? TSoftObjectPtr<USkeletalMesh>() : MeshRef(OutwearOptions()[PresetIndex]);
	Customization.BodyColor = PresetIndex == 0 ? FLinearColor(0.95f, 0.58f, 0.36f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.58f, 0.36f, 0.22f, 1.0f) : FLinearColor(0.78f, 0.70f, 0.54f, 1.0f);
	Customization.HairColor = PresetIndex == 0 ? FLinearColor(0.10f, 0.06f, 0.03f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.40f, 0.22f, 0.08f, 1.0f) : FLinearColor(0.02f, 0.02f, 0.025f, 1.0f);
	Customization.OutfitColor = PresetIndex == 0 ? FLinearColor(0.05f, 0.58f, 0.82f, 1.0f) : PresetIndex == 1 ? FLinearColor(0.90f, 0.18f, 0.30f, 1.0f) : FLinearColor(0.95f, 0.76f, 0.10f, 1.0f);
	Customization.Nickname = PresetIndex == 0 ? TEXT("Greg Adjacent") : PresetIndex == 1 ? TEXT("Mall Cryptid") : TEXT("No Notes");
	return Customization;
}
