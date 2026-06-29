#include "VNHMainMenuWidget.h"

#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "VNHLog.h"
#include "VNHGameInstance.h"

void UVNHMainMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (HostPrivateButton)
	{
		HostPrivateButton->OnClicked.AddUniqueDynamic(this, &UVNHMainMenuWidget::HandleHostPrivateClicked);
	}

	if (HostPublicButton)
	{
		HostPublicButton->OnClicked.AddUniqueDynamic(this, &UVNHMainMenuWidget::HandleHostPublicClicked);
	}

	if (JoinButton)
	{
		JoinButton->OnClicked.AddUniqueDynamic(this, &UVNHMainMenuWidget::HandleJoinClicked);
	}

	if (QuitButton)
	{
		QuitButton->OnClicked.AddUniqueDynamic(this, &UVNHMainMenuWidget::HandleQuitClicked);
	}

	if (CharacterCustomizerButton)
	{
		CharacterCustomizerButton->OnClicked.AddUniqueDynamic(this, &UVNHMainMenuWidget::HandleCharacterCustomizerClicked);
		UE_LOG(LogVNH, Display, TEXT("MainMenu: CharacterCustomizerButton native bind ready."));
	}
	else
	{
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: CharacterCustomizerButton was not found for native bind."));
	}

	if (APlayerController* PlayerController = GetOwningPlayer())
	{
		PlayerController->bShowMouseCursor = true;
		PlayerController->bEnableClickEvents = true;
		PlayerController->bEnableMouseOverEvents = true;

		FInputModeUIOnly InputMode;
		InputMode.SetWidgetToFocus(TakeWidget());
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}

	RefreshStatusText();
}

void UVNHMainMenuWidget::RefreshStatusText()
{
	const UVNHGameInstance* VNHGameInstance = GetVNHGameInstance();
	if (JoinAddressTextBox && VNHGameInstance && JoinAddressTextBox->GetText().IsEmpty())
	{
		JoinAddressTextBox->SetText(FText::FromString(VNHGameInstance->GetDefaultJoinAddress()));
	}

	SetStatus(NSLOCTEXT("VNH", "MainMenuStatusReady", "Private listen-server flow ready. Host, invite friends, then start from the lobby."));
}

void UVNHMainMenuWidget::HandleHostPrivateClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		SetStatus(NSLOCTEXT("VNH", "MainMenuStatusHostPrivate", "Opening private lobby..."));
		VNHGameInstance->HostPrivateGame();
	}
}

void UVNHMainMenuWidget::HandleHostPublicClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		SetStatus(NSLOCTEXT("VNH", "MainMenuStatusHostPublic", "Opening public lobby..."));
		VNHGameInstance->HostPublicGame();
	}
}

void UVNHMainMenuWidget::HandleJoinClicked()
{
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		const FString Address = JoinAddressTextBox ? JoinAddressTextBox->GetText().ToString() : FString();
		SetStatus(FText::FromString(FString::Printf(TEXT("Joining %s..."), Address.IsEmpty() ? *VNHGameInstance->GetDefaultJoinAddress() : *Address)));
		VNHGameInstance->JoinGameByAddress(Address);
	}
}

void UVNHMainMenuWidget::HandleQuitClicked()
{
	UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
}

void UVNHMainMenuWidget::HandleCharacterCustomizerClicked()
{
	UE_LOG(LogVNH, Display, TEXT("MainMenu: CharacterCustomizerButton clicked."));
	if (UVNHGameInstance* VNHGameInstance = GetVNHGameInstance())
	{
		VNHGameInstance->ShowCharacterCustomizer(false);
	}
	else
	{
		UE_LOG(LogVNH, Warning, TEXT("MainMenu: cannot open customizer because VNHGameInstance is missing."));
	}
}

UVNHGameInstance* UVNHMainMenuWidget::GetVNHGameInstance() const
{
	return GetGameInstance<UVNHGameInstance>();
}

void UVNHMainMenuWidget::SetStatus(const FText& StatusText)
{
	if (MenuStatusText)
	{
		MenuStatusText->SetText(StatusText);
	}
}
