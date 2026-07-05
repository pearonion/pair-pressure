#include "VNHCreateServerWidget.h"

#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Kismet/GameplayStatics.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "VNHLog.h"

namespace
{
constexpr int32 MinPlayers = 3;
constexpr int32 MaxPlayers = 20;
constexpr int32 DefaultPlayers = 3;
constexpr int32 DefaultRoundSeconds = 120;

const FName SessionKeyServerName(TEXT("SERVER_NAME"));
const FName SessionKeyIsPrivate(TEXT("IS_PRIVATE"));
const FName SessionKeyPassword(TEXT("PASSWORD"));
const FName SessionKeyRoundSeconds(TEXT("ROUND_SECONDS"));
const FName SessionKeyMaxPlayers(TEXT("MAX_PLAYERS"));
const FName SessionKeyRegion(TEXT("REGION"));
const FName SessionKeyMapName(TEXT("MAP_NAME"));
const FName SessionKeyGameId(TEXT("VNH_GAME_ID"));
const FString SessionGameId(TEXT("VNHSimulator"));
}

void UVNHCreateServerWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);

	if (PublicButton)
	{
		PublicButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandlePublicClicked);
	}
	if (PrivateButton)
	{
		PrivateButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandlePrivateClicked);
	}
	if (PasswordVisibilityButton)
	{
		PasswordVisibilityButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandlePasswordVisibilityClicked);
	}
	if (DecreasePlayersButton)
	{
		DecreasePlayersButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleDecreasePlayersClicked);
	}
	if (IncreasePlayersButton)
	{
		IncreasePlayersButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleIncreasePlayersClicked);
	}
	if (MaxPlayersTextBox)
	{
		MaxPlayersTextBox->OnTextCommitted.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleMaxPlayersCommitted);
	}
	if (CreateGameButton)
	{
		CreateGameButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleCreateGameClicked);
	}
	if (CancelButton)
	{
		CancelButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleCancelClicked);
	}
	if (CloseButton)
	{
		CloseButton->OnClicked.AddUniqueDynamic(this, &UVNHCreateServerWidget::HandleCancelClicked);
	}

	if (RoundTimeComboBox)
	{
		RoundTimeComboBox->ClearOptions();
		RoundTimeComboBox->AddOption(TEXT("2 Minutes"));
		RoundTimeComboBox->AddOption(TEXT("3 Minutes"));
		RoundTimeComboBox->AddOption(TEXT("5 Minutes"));
		RoundTimeComboBox->SetSelectedOption(TEXT("2 Minutes"));
	}

	SetMaxPlayers(DefaultPlayers);
	SetPrivateMode(false);
	SetPasswordVisible(false);
	SetStatus(NSLOCTEXT("VNH", "CreateServerReady", "Choose your server settings."));
}

FReply UVNHCreateServerWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleCancelClicked();
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UVNHCreateServerWidget::HandlePublicClicked()
{
	SetPrivateMode(false);
}

void UVNHCreateServerWidget::HandlePrivateClicked()
{
	SetPrivateMode(true);
}

void UVNHCreateServerWidget::HandlePasswordVisibilityClicked()
{
	SetPasswordVisible(!bPasswordVisible);
}

void UVNHCreateServerWidget::HandleDecreasePlayersClicked()
{
	SetMaxPlayers(GetClampedMaxPlayers() - 1);
}

void UVNHCreateServerWidget::HandleIncreasePlayersClicked()
{
	SetMaxPlayers(GetClampedMaxPlayers() + 1);
}

void UVNHCreateServerWidget::HandleMaxPlayersCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	SetMaxPlayers(FCString::Atoi(*Text.ToString()));
}

void UVNHCreateServerWidget::HandleCreateGameClicked()
{
	if (bCreateSessionInFlight)
	{
		return;
	}

	const FString ServerName = ServerNameTextBox ? ServerNameTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (ServerName.IsEmpty())
	{
		SetStatus(NSLOCTEXT("VNH", "CreateServerNameRequired", "Server name is required."));
		return;
	}

	const FString Password = PasswordTextBox ? PasswordTextBox->GetText().ToString() : FString();
	if (bPrivateMode && Password.IsEmpty())
	{
		SetStatus(NSLOCTEXT("VNH", "CreateServerPasswordRequired", "Private games require a password."));
		return;
	}

	const int32 ClampedPlayers = GetClampedMaxPlayers();
	const int32 RoundSeconds = GetSelectedRoundSeconds();

	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld());
	if (!OnlineSubsystem)
	{
		SetStatus(NSLOCTEXT("VNH", "CreateServerNoSubsystem", "Steam online subsystem is not available."));
		return;
	}

	ActiveSessionInterface = OnlineSubsystem->GetSessionInterface();
	if (!ActiveSessionInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "CreateServerNoSessions", "Steam sessions are not available."));
		return;
	}

	FOnlineSessionSettings SessionSettings;
	SessionSettings.NumPublicConnections = ClampedPlayers;
	SessionSettings.NumPrivateConnections = 0;
	SessionSettings.bShouldAdvertise = true;
	SessionSettings.bAllowInvites = true;
	SessionSettings.bAllowJoinInProgress = true;
	SessionSettings.bIsLANMatch = false;
	SessionSettings.bIsDedicated = false;
	SessionSettings.bUsesPresence = true;
	SessionSettings.bUseLobbiesIfAvailable = true;
	SessionSettings.bAllowJoinViaPresence = true;
	SessionSettings.bAllowJoinViaPresenceFriendsOnly = false;
	SessionSettings.bUseLobbiesVoiceChatIfAvailable = false;
	SessionSettings.Set(SessionKeyServerName, ServerName, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyIsPrivate, bPrivateMode, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyPassword, bPrivateMode ? Password : FString(), EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyRoundSeconds, RoundSeconds, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyMaxPlayers, ClampedPlayers, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyRegion, FString(TEXT("USEAST")), EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyMapName, LobbyMapName.ToString(), EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyGameId, SessionGameId, EOnlineDataAdvertisementType::ViaOnlineService);

	bCreateSessionInFlight = true;
	if (ActiveSessionInterface->GetNamedSession(NAME_GameSession))
	{
		PendingSessionSettings = SessionSettings;
		DestroySessionCompleteHandle = ActiveSessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
			FOnDestroySessionCompleteDelegate::CreateUObject(this, &UVNHCreateServerWidget::HandleExistingSessionDestroyed));

		SetStatus(NSLOCTEXT("VNH", "CreateServerClearingOldSession", "Clearing previous Steam session..."));
		if (!ActiveSessionInterface->DestroySession(NAME_GameSession))
		{
			bCreateSessionInFlight = false;
			ActiveSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
			SetStatus(NSLOCTEXT("VNH", "CreateServerDestroyFailed", "Could not clear the previous Steam session."));
		}
		return;
	}

	BeginCreateSession(SessionSettings);
}

void UVNHCreateServerWidget::HandleCancelClicked()
{
	RemoveFromParent();
}

void UVNHCreateServerWidget::HandleSessionCreated(FName SessionName, bool bWasSuccessful)
{
	bCreateSessionInFlight = false;
	if (ActiveSessionInterface.IsValid())
	{
		ActiveSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteHandle);
	}

	if (!bWasSuccessful)
	{
		SetStatus(NSLOCTEXT("VNH", "CreateServerFailed", "Steam session creation failed. Check Steam is running and AppID 480 is active."));
		return;
	}

	const FString TravelOptions = FString::Printf(TEXT("listen%s"), *BuildTravelOptions());
	SetStatus(NSLOCTEXT("VNH", "CreateServerOpeningLobby", "Session created. Opening lobby..."));
	UGameplayStatics::OpenLevel(this, LobbyMapName, true, TravelOptions);
	UE_LOG(LogVNH, Display, TEXT("CreateServer: created Advanced Steam session and opening %s?%s."), *LobbyMapName.ToString(), *TravelOptions);
}

void UVNHCreateServerWidget::HandleExistingSessionDestroyed(FName SessionName, bool bWasSuccessful)
{
	if (ActiveSessionInterface.IsValid())
	{
		ActiveSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
	}

	if (!bWasSuccessful)
	{
		bCreateSessionInFlight = false;
		SetStatus(NSLOCTEXT("VNH", "CreateServerDestroyCompleteFailed", "Could not clear the previous Steam session."));
		return;
	}

	BeginCreateSession(PendingSessionSettings);
}

void UVNHCreateServerWidget::BeginCreateSession(const FOnlineSessionSettings& SessionSettings)
{
	if (!ActiveSessionInterface.IsValid())
	{
		bCreateSessionInFlight = false;
		SetStatus(NSLOCTEXT("VNH", "CreateServerNoSessionsAfterDestroy", "Steam sessions are not available."));
		return;
	}

	CreateSessionCompleteHandle = ActiveSessionInterface->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &UVNHCreateServerWidget::HandleSessionCreated));

	SetStatus(NSLOCTEXT("VNH", "CreateServerCreating", "Creating Steam session..."));
	if (!ActiveSessionInterface->CreateSession(0, NAME_GameSession, SessionSettings))
	{
		bCreateSessionInFlight = false;
		ActiveSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteHandle);
		SetStatus(NSLOCTEXT("VNH", "CreateServerStartFailed", "Could not start Steam session creation."));
	}
}

void UVNHCreateServerWidget::SetPrivateMode(bool bInPrivateMode)
{
	bPrivateMode = bInPrivateMode;

	if (PasswordTextBox)
	{
		PasswordTextBox->SetIsEnabled(bPrivateMode);
	}
	if (PublicButtonLabel)
	{
		PublicButtonLabel->SetText(bPrivateMode ? NSLOCTEXT("VNH", "CreateServerPublic", "PUBLIC") : NSLOCTEXT("VNH", "CreateServerPublicSelected", "> PUBLIC"));
	}
	if (PrivateButtonLabel)
	{
		PrivateButtonLabel->SetText(bPrivateMode ? NSLOCTEXT("VNH", "CreateServerPrivateSelected", "> PRIVATE") : NSLOCTEXT("VNH", "CreateServerPrivate", "PRIVATE"));
	}
}

void UVNHCreateServerWidget::SetPasswordVisible(bool bInPasswordVisible)
{
	bPasswordVisible = bInPasswordVisible;
	if (PasswordTextBox)
	{
		PasswordTextBox->SetIsPassword(!bPasswordVisible);
	}
}

int32 UVNHCreateServerWidget::GetClampedMaxPlayers() const
{
	const FString TextValue = MaxPlayersTextBox ? MaxPlayersTextBox->GetText().ToString() : FString();
	return FMath::Clamp(FCString::Atoi(*TextValue), MinPlayers, MaxPlayers);
}

int32 UVNHCreateServerWidget::GetSelectedRoundSeconds() const
{
	const FString Selected = RoundTimeComboBox ? RoundTimeComboBox->GetSelectedOption() : FString();
	if (Selected.StartsWith(TEXT("3")))
	{
		return 180;
	}
	if (Selected.StartsWith(TEXT("5")))
	{
		return 300;
	}
	return DefaultRoundSeconds;
}

void UVNHCreateServerWidget::SetMaxPlayers(int32 NewMaxPlayers)
{
	const int32 ClampedPlayers = FMath::Clamp(NewMaxPlayers, MinPlayers, MaxPlayers);
	if (MaxPlayersTextBox)
	{
		MaxPlayersTextBox->SetText(FText::AsNumber(ClampedPlayers));
	}
}

void UVNHCreateServerWidget::SetStatus(const FText& NewStatus)
{
	if (StatusText)
	{
		StatusText->SetText(NewStatus);
	}
}

FString UVNHCreateServerWidget::BuildTravelOptions() const
{
	const FString ServerName = ServerNameTextBox ? ServerNameTextBox->GetText().ToString() : FString();
	const FString Password = PasswordTextBox ? PasswordTextBox->GetText().ToString() : FString();
	return FString::Printf(
		TEXT("?Public=%s?ServerName=%s?Private=%s?Password=%s?MaxPlayers=%d?RoundSeconds=%d"),
		bPrivateMode ? TEXT("0") : TEXT("1"),
		*ServerName.Replace(TEXT("?"), TEXT("")),
		bPrivateMode ? TEXT("1") : TEXT("0"),
		bPrivateMode ? *Password.Replace(TEXT("?"), TEXT("")) : TEXT(""),
		GetClampedMaxPlayers(),
		GetSelectedRoundSeconds());
}
