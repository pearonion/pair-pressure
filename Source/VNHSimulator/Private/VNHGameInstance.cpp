#include "VNHGameInstance.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "VNHLog.h"

void UVNHGameInstance::Init()
{
	Super::Init();

	if (!MainMenuWidgetClass)
	{
		MainMenuWidgetClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_VNHMainMenu.WBP_VNHMainMenu_C"));
	}
}

void UVNHGameInstance::ShowMainMenu()
{
	if (!MainMenuWidgetClass)
	{
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: no widget class configured."));
		return;
	}

	if (ActiveMainMenu)
	{
		ActiveMainMenu->RemoveFromParent();
		ActiveMainMenu = nullptr;
	}

	ActiveMainMenu = CreateWidget<UUserWidget>(this, MainMenuWidgetClass);
	if (!ActiveMainMenu)
	{
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: failed to create widget."));
		return;
	}

	ActiveMainMenu->AddToViewport(100);
	BindMainMenuButtons(ActiveMainMenu);
	SetMainMenuStatus(NSLOCTEXT("VNH", "MainMenuStatusReady", "Private listen-server flow ready. Host, invite friends, then start from the lobby."));

	if (APlayerController* PlayerController = GetFirstLocalPlayerController())
	{
		PlayerController->bShowMouseCursor = true;
		PlayerController->bEnableClickEvents = true;
		PlayerController->bEnableMouseOverEvents = true;

		FInputModeUIOnly InputMode;
		InputMode.SetWidgetToFocus(ActiveMainMenu->TakeWidget());
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}
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
