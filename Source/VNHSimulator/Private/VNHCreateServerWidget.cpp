#include "VNHCreateServerWidget.h"

#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Online/OnlineSessionNames.h"
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

FString EncodeCreateServerTravelOption(const FString& Value)
{
	FString Encoded = Value;
	Encoded.ReplaceInline(TEXT("%"), TEXT("%25"));
	Encoded.ReplaceInline(TEXT(" "), TEXT("%20"));
	Encoded.ReplaceInline(TEXT("?"), TEXT("%3F"));
	Encoded.ReplaceInline(TEXT("="), TEXT("%3D"));
	Encoded.ReplaceInline(TEXT("&"), TEXT("%26"));
	return Encoded;
}
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

void UVNHCreateServerWidget::ConfigureInitialMode(bool bInitialPrivateMode)
{
	SetPrivateMode(bInitialPrivateMode);
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
		OnlineSubsystem = Online::GetSubsystem(GetWorld(), FName(TEXT("STEAM")));
		if (OnlineSubsystem)
		{
			UE_LOG(LogVNH, Display, TEXT("CreateServer: default online subsystem was unavailable; using explicit %s subsystem."), *OnlineSubsystem->GetSubsystemName().ToString());
		}
	}
	if (!OnlineSubsystem)
	{
		bSteamSessionRequired = false;
		OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalNoSubsystem", "Steam is unavailable. Opening a local listen lobby..."), TEXT("no online subsystem"));
		return;
	}

	bSteamSessionRequired = OnlineSubsystem->GetSubsystemName().ToString().Equals(TEXT("STEAM"), ESearchCase::IgnoreCase);
	ActiveSessionInterface = OnlineSubsystem->GetSessionInterface();
	UE_LOG(LogVNH, Display, TEXT("CreateServer: subsystem=%s steam_required=%s session_interface=%s."),
		*OnlineSubsystem->GetSubsystemName().ToString(),
		bSteamSessionRequired ? TEXT("true") : TEXT("false"),
		ActiveSessionInterface.IsValid() ? TEXT("valid") : TEXT("invalid"));
	if (!ActiveSessionInterface.IsValid())
	{
		if (bSteamSessionRequired)
		{
			AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamNoSessions", "Steam sessions are unavailable. Game was not hosted."), TEXT("no session interface"));
		}
		else
		{
			OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalNoSessions", "Steam sessions are unavailable. Opening a local listen lobby..."), TEXT("no session interface"));
		}
		return;
	}

	PendingHostUserId = ResolveHostUserId(OnlineSubsystem);
	if (bSteamSessionRequired && !PendingHostUserId.IsValid())
	{
		UE_LOG(LogVNH, Warning, TEXT("CreateServer: Steam is active but no local Steam unique net id was resolved before CreateSession; falling back to local user index 0."));
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
	SessionSettings.bAntiCheatProtected = false;
	SessionSettings.bUsesStats = false;
	SessionSettings.Set(SessionKeyServerName, ServerName, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyIsPrivate, bPrivateMode, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyPassword, bPrivateMode ? Password : FString(), EOnlineDataAdvertisementType::DontAdvertise);
	SessionSettings.Set(SessionKeyRoundSeconds, RoundSeconds, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyMaxPlayers, ClampedPlayers, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyRegion, FString(TEXT("USEAST")), EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyMapName, LobbyMapName.ToString(), EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SessionKeyGameId, SessionGameId, EOnlineDataAdvertisementType::ViaOnlineService);
	SessionSettings.Set(SETTING_MAPNAME, LobbyMapName.ToString(), EOnlineDataAdvertisementType::ViaOnlineService);

	bCreateSessionInFlight = true;
	if (ActiveSessionInterface->GetNamedSession(NAME_GameSession))
	{
		PendingSessionSettings = SessionSettings;
		DestroySessionCompleteHandle = ActiveSessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
			FOnDestroySessionCompleteDelegate::CreateUObject(this, &UVNHCreateServerWidget::HandleExistingSessionDestroyed));

		SetStatus(NSLOCTEXT("VNH", "CreateServerClearingOldSession", "Clearing previous Steam session..."));
		if (!ActiveSessionInterface->DestroySession(NAME_GameSession))
		{
			ActiveSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
			if (bSteamSessionRequired)
			{
				AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamDestroyFailed", "Could not clear the previous Steam session. Game was not hosted."), TEXT("destroy session request failed"));
			}
			else
			{
				OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalDestroyFailed", "Could not clear the old Steam session. Opening a local listen lobby..."), TEXT("destroy session request failed"));
			}
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
	if (ActiveSessionInterface.IsValid())
	{
		ActiveSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteHandle);
	}

	if (!bWasSuccessful)
	{
		if (bSteamSessionRequired)
		{
			AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamCreateFailed", "Steam session creation failed. Game was not hosted."), TEXT("create session completed unsuccessfully"));
		}
		else
		{
			OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalCreateFailed", "Steam session creation failed. Opening a local listen lobby..."), TEXT("create session completed unsuccessfully"));
		}
		return;
	}

	if (ActiveSessionInterface.IsValid())
	{
		StartSessionCompleteHandle = ActiveSessionInterface->AddOnStartSessionCompleteDelegate_Handle(
			FOnStartSessionCompleteDelegate::CreateUObject(this, &UVNHCreateServerWidget::HandleSessionStarted));

		SetStatus(NSLOCTEXT("VNH", "CreateServerStartingSession", "Steam session created. Starting session..."));
		if (ActiveSessionInterface->StartSession(NAME_GameSession))
		{
			return;
		}

		ActiveSessionInterface->ClearOnStartSessionCompleteDelegate_Handle(StartSessionCompleteHandle);
		UE_LOG(LogVNH, Warning, TEXT("CreateServer: StartSession request failed after creation; opening lobby because the Steam lobby exists."));
	}

	OpenLobbyAfterSessionReady();
}

void UVNHCreateServerWidget::HandleSessionStarted(FName SessionName, bool bWasSuccessful)
{
	if (ActiveSessionInterface.IsValid())
	{
		ActiveSessionInterface->ClearOnStartSessionCompleteDelegate_Handle(StartSessionCompleteHandle);
	}

	if (!bWasSuccessful)
	{
		UE_LOG(LogVNH, Warning, TEXT("CreateServer: StartSession completed unsuccessfully for %s; opening lobby because the Steam lobby exists."), *SessionName.ToString());
	}

	OpenLobbyAfterSessionReady();
}

void UVNHCreateServerWidget::HandleExistingSessionDestroyed(FName SessionName, bool bWasSuccessful)
{
	if (ActiveSessionInterface.IsValid())
	{
		ActiveSessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionCompleteHandle);
	}

	if (!bWasSuccessful)
	{
		if (bSteamSessionRequired)
		{
			AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamDestroyCompleteFailed", "Could not clear the previous Steam session. Game was not hosted."), TEXT("destroy session completed unsuccessfully"));
		}
		else
		{
			OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalDestroyCompleteFailed", "Could not clear the previous Steam session. Opening a local listen lobby..."), TEXT("destroy session completed unsuccessfully"));
		}
		return;
	}

	BeginCreateSession(PendingSessionSettings);
}

void UVNHCreateServerWidget::BeginCreateSession(const FOnlineSessionSettings& SessionSettings)
{
	if (!ActiveSessionInterface.IsValid())
	{
		if (bSteamSessionRequired)
		{
			AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamSessionsLost", "Steam session interface was lost. Game was not hosted."), TEXT("session interface lost"));
		}
		else
		{
			OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalNoSessionsAfterDestroy", "Steam sessions are unavailable. Opening a local listen lobby..."), TEXT("session interface lost"));
		}
		return;
	}

	CreateSessionCompleteHandle = ActiveSessionInterface->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &UVNHCreateServerWidget::HandleSessionCreated));

	SetStatus(NSLOCTEXT("VNH", "CreateServerCreating", "Creating Steam session..."));
	const bool bUsingResolvedHostId = PendingHostUserId.IsValid();
	UE_LOG(LogVNH, Display, TEXT("CreateServer: CreateSession starting via %s. PublicConnections=%d Advertise=%s Presence=%s Lobbies=%s."),
		bUsingResolvedHostId ? TEXT("resolved unique id") : TEXT("local user index 0"),
		SessionSettings.NumPublicConnections,
		SessionSettings.bShouldAdvertise ? TEXT("true") : TEXT("false"),
		SessionSettings.bUsesPresence ? TEXT("true") : TEXT("false"),
		SessionSettings.bUseLobbiesIfAvailable ? TEXT("true") : TEXT("false"));
	const bool bCreateStarted = bUsingResolvedHostId
		? ActiveSessionInterface->CreateSession(*PendingHostUserId, NAME_GameSession, SessionSettings)
		: ActiveSessionInterface->CreateSession(0, NAME_GameSession, SessionSettings);
	if (!bCreateStarted)
	{
		ActiveSessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionCompleteHandle);
		if (bSteamSessionRequired)
		{
			AbortSteamSessionCreate(NSLOCTEXT("VNH", "CreateServerSteamStartFailed", "Could not start Steam session creation. Game was not hosted."), TEXT("create session request failed"));
		}
		else
		{
			OpenListenLobbyFallback(NSLOCTEXT("VNH", "CreateServerLocalStartFailed", "Could not start Steam session creation. Opening a local listen lobby..."), TEXT("create session request failed"));
		}
	}
}

void UVNHCreateServerWidget::OpenLobbyAfterSessionReady()
{
	bCreateSessionInFlight = false;
	bSteamSessionRequired = false;
	PendingHostUserId.Reset();
	const FString TravelOptions = FString::Printf(TEXT("listen%s"), *BuildTravelOptions());
	SetStatus(NSLOCTEXT("VNH", "CreateServerOpeningLobby", "Steam session ready. Opening lobby..."));
	RemoveFromParent();
	UGameplayStatics::OpenLevel(this, LobbyMapName, true, TravelOptions);
	UE_LOG(LogVNH, Display, TEXT("CreateServer: created Advanced Steam session and opening %s?%s."), *LobbyMapName.ToString(), *TravelOptions);
}

void UVNHCreateServerWidget::OpenListenLobbyFallback(const FText& FallbackStatusText, const TCHAR* Reason)
{
	bCreateSessionInFlight = false;
	bSteamSessionRequired = false;
	PendingHostUserId.Reset();
	SetStatus(FallbackStatusText);

	const FString TravelOptions = FString::Printf(TEXT("listen%s"), *BuildTravelOptions());
	UE_LOG(LogVNH, Warning, TEXT("CreateServer: falling back to local listen lobby because %s. Opening %s?%s."),
		Reason ? Reason : TEXT("session setup was unavailable"),
		*LobbyMapName.ToString(),
		*TravelOptions);
	RemoveFromParent();
	UGameplayStatics::OpenLevel(this, LobbyMapName, true, TravelOptions);
}

void UVNHCreateServerWidget::AbortSteamSessionCreate(const FText& FailureStatusText, const TCHAR* Reason)
{
	bCreateSessionInFlight = false;
	bSteamSessionRequired = false;
	PendingHostUserId.Reset();
	SetStatus(FailureStatusText);
	UE_LOG(LogVNH, Error, TEXT("CreateServer: Steam session create aborted because %s."), Reason ? Reason : TEXT("session setup failed"));
}

TSharedPtr<const FUniqueNetId> UVNHCreateServerWidget::ResolveHostUserId(IOnlineSubsystem* OnlineSubsystem) const
{
	const APlayerController* PlayerController = GetOwningPlayer();
	if (!PlayerController)
	{
		return nullptr;
	}

	if (const APlayerState* HostPlayerState = PlayerController->PlayerState)
	{
		TSharedPtr<const FUniqueNetId> PlayerStateUserId = HostPlayerState->GetUniqueId().GetUniqueNetId();
		if (PlayerStateUserId.IsValid())
		{
			return PlayerStateUserId;
		}
	}

	if (const ULocalPlayer* LocalPlayer = PlayerController->GetLocalPlayer())
	{
		const FUniqueNetIdRepl LocalUserId = LocalPlayer->GetPreferredUniqueNetId();
		if (LocalUserId.IsValid())
		{
			return LocalUserId.GetUniqueNetId();
		}
	}

	if (OnlineSubsystem)
	{
		const IOnlineIdentityPtr IdentityInterface = OnlineSubsystem->GetIdentityInterface();
		if (IdentityInterface.IsValid())
		{
			TSharedPtr<const FUniqueNetId> IdentityUserId = IdentityInterface->GetUniquePlayerId(0);
			if (IdentityUserId.IsValid())
			{
				return IdentityUserId;
			}
		}
	}

	return nullptr;
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
		*EncodeCreateServerTravelOption(ServerName.TrimStartAndEnd()),
		bPrivateMode ? TEXT("1") : TEXT("0"),
		bPrivateMode ? *EncodeCreateServerTravelOption(Password) : TEXT(""),
		GetClampedMaxPlayers(),
		GetSelectedRoundSeconds());
}
