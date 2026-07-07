#include "VNHServerBrowserWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/Border.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ContentWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Font.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Styling/SlateBrush.h"
#include "VNHLog.h"

namespace
{
const FName BrowserSessionKeyServerName(TEXT("SERVER_NAME"));
const FName BrowserSessionKeyIsPrivate(TEXT("IS_PRIVATE"));
const FName BrowserSessionKeyMaxPlayers(TEXT("MAX_PLAYERS"));
const FName BrowserSessionKeyRegion(TEXT("REGION"));
const FName BrowserSessionKeyMapName(TEXT("MAP_NAME"));
const FName BrowserSessionKeyGameId(TEXT("VNH_GAME_ID"));

const FString DefaultMapName(TEXT("MVP_Clothing Store"));
const FString DefaultRegion(TEXT("USEAST"));
const FString BrowserSessionGameId(TEXT("VNHSimulator"));
constexpr double DoubleClickWindowSeconds = 0.35;
constexpr float ColumnServerNameWeight = 0.36f;
constexpr float ColumnPlayersWeight = 0.12f;
constexpr float ColumnMapWeight = 0.24f;
constexpr float ColumnRegionWeight = 0.18f;
constexpr float ColumnPingWeight = 0.10f;
constexpr float RowFontSize = 23.0f;
constexpr float HeaderFontSize = 21.0f;
const FMargin DefaultCellPadding(10.0f, 6.0f);
const FMargin PingCellPadding(10.0f, 6.0f, 6.0f, 6.0f);

const FString MapPlaceholder(TEXT("Any Map"));
const FString MinPlayersPlaceholder(TEXT("Min Players"));
const FString MinOpenSlotsPlaceholder(TEXT("Min Open Slots"));

FString GetSessionString(const FOnlineSessionSearchResult& Result, const FName Key, const FString& Fallback)
{
	FString Value;
	if (Result.Session.SessionSettings.Get(Key, Value) && !Value.IsEmpty())
	{
		return Value;
	}
	return Fallback;
}

bool GetSessionBool(const FOnlineSessionSearchResult& Result, const FName Key, const bool Fallback)
{
	bool Value = Fallback;
	Result.Session.SessionSettings.Get(Key, Value);
	return Value;
}

FString EncodeBrowserTravelOption(const FString& Value)
{
	FString Encoded = Value;
	Encoded.ReplaceInline(TEXT("%"), TEXT("%25"));
	Encoded.ReplaceInline(TEXT(" "), TEXT("%20"));
	Encoded.ReplaceInline(TEXT("?"), TEXT("%3F"));
	Encoded.ReplaceInline(TEXT("="), TEXT("%3D"));
	Encoded.ReplaceInline(TEXT("&"), TEXT("%26"));
	return Encoded;
}

int32 GetSessionInt(const FOnlineSessionSearchResult& Result, const FName Key, const int32 Fallback)
{
	int32 Value = Fallback;
	Result.Session.SessionSettings.Get(Key, Value);
	return Value;
}

TSharedPtr<const FUniqueNetId> ResolveBrowserUserId(const APlayerController* PlayerController)
{
	if (!PlayerController)
	{
		return nullptr;
	}

	if (const APlayerState* BrowserPlayerState = PlayerController->PlayerState)
	{
		TSharedPtr<const FUniqueNetId> PlayerStateUserId = BrowserPlayerState->GetUniqueId().GetUniqueNetId();
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

	return nullptr;
}

UFont* GetServerBrowserFont()
{
	static TWeakObjectPtr<UFont> CachedFont;
	if (!CachedFont.IsValid())
	{
		CachedFont = LoadObject<UFont>(nullptr, TEXT("/Game/ThirdPerson/Demo/BurbankBigCondensed-Black_Font.BurbankBigCondensed-Black_Font"));
	}
	return CachedFont.Get();
}

FSlateFontInfo MakeServerBrowserFont(const int32 Size)
{
	if (UFont* Font = GetServerBrowserFont())
	{
		return FSlateFontInfo(Font, Size, FName(TEXT("Default")));
	}
	return FSlateFontInfo(FCoreStyle::GetDefaultFont(), Size);
}

USizeBox* MakeCell(UHorizontalBox* Row, UObject* Outer, const float FillWeight, const FMargin Padding = DefaultCellPadding)
{
	USizeBox* SizeBox = NewObject<USizeBox>(Outer);

	UHorizontalBoxSlot* Slot = Row->AddChildToHorizontalBox(SizeBox);
	FSlateChildSize SlotSize;
	SlotSize.SizeRule = ESlateSizeRule::Fill;
	SlotSize.Value = FillWeight;
	Slot->SetSize(SlotSize);
	Slot->SetPadding(Padding);
	Slot->SetHorizontalAlignment(HAlign_Fill);
	Slot->SetVerticalAlignment(VAlign_Center);
	return SizeBox;
}

void AddCellText(UContentWidget* Parent, const FString& Text, const FLinearColor Color = FLinearColor::White, const ETextJustify::Type Justification = ETextJustify::Left)
{
	if (!Parent)
	{
		return;
	}

	UTextBlock* TextBlock = NewObject<UTextBlock>(Parent);
	TextBlock->SetText(FText::FromString(Text));
	TextBlock->SetColorAndOpacity(FSlateColor(Color));
	TextBlock->SetFont(MakeServerBrowserFont(RowFontSize));
	TextBlock->SetJustification(Justification);
	TextBlock->SetClipping(EWidgetClipping::ClipToBounds);
	TextBlock->SetToolTipText(FText::FromString(Text));
#if ENGINE_MAJOR_VERSION >= 5
	TextBlock->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
#endif
	if (USizeBoxSlot* Slot = Cast<USizeBoxSlot>(Parent->AddChild(TextBlock)))
	{
		Slot->SetHorizontalAlignment(Justification == ETextJustify::Center ? HAlign_Center : Justification == ETextJustify::Right ? HAlign_Right : HAlign_Left);
		Slot->SetVerticalAlignment(VAlign_Center);
	}
}

void AddRowText(UHorizontalBox* Row, UObject* Outer, const FString& Text, const float FillWeight, const FLinearColor Color = FLinearColor::White)
{
	AddCellText(MakeCell(Row, Outer, FillWeight), Text, Color);
}

void AddRowText(UHorizontalBox* Row, UObject* Outer, const FString& Text, const float FillWeight, const FLinearColor Color, const ETextJustify::Type Justification)
{
	AddCellText(MakeCell(Row, Outer, FillWeight), Text, Color, Justification);
}

void AddServerNameCell(UHorizontalBox* Row, UObject* Outer, const FString& ServerName, const bool bSelected)
{
	USizeBox* SizeBox = MakeCell(Row, Outer, ColumnServerNameWeight);
	AddCellText(SizeBox, ServerName, bSelected ? FLinearColor(1.0f, 1.0f, 1.0f, 1.0f) : FLinearColor(0.92f, 0.94f, 0.94f, 1.0f));
}

FLinearColor ServerBrowserPingColor(const int32 Ping)
{
	if (Ping >= 110)
	{
		return FLinearColor(1.0f, 0.12f, 0.04f, 1.0f);
	}
	if (Ping >= 80)
	{
		return FLinearColor(1.0f, 0.48f, 0.0f, 1.0f);
	}
	if (Ping >= 50)
	{
		return FLinearColor(1.0f, 0.82f, 0.0f, 1.0f);
	}
	return FLinearColor(0.0f, 1.0f, 0.22f, 1.0f);
}

int32 ServerBrowserPingBarCount(const int32 Ping)
{
	if (Ping >= 110)
	{
		return 1;
	}
	if (Ping >= 80)
	{
		return 2;
	}
	if (Ping >= 50)
	{
		return 3;
	}
	return 4;
}

FSlateBrush MakeBoxBrush(const FLinearColor& Color, const FVector2D ImageSize = FVector2D(8.0f, 8.0f))
{
	FSlateBrush Brush;
	Brush.DrawAs = ESlateBrushDrawType::Box;
	Brush.TintColor = FSlateColor(Color);
	Brush.Margin = FMargin(0.0f);
	Brush.SetImageSize(ImageSize);
	return Brush;
}

void AddPingCell(UHorizontalBox* Row, UObject* Outer, const int32 Ping)
{
	USizeBox* SizeBox = MakeCell(Row, Outer, ColumnPingWeight, PingCellPadding);

	UHorizontalBox* PingBox = NewObject<UHorizontalBox>(SizeBox);
	USizeBox* TextBox = NewObject<USizeBox>(PingBox);
	TextBox->SetWidthOverride(58.0f);

	const FLinearColor Color = ServerBrowserPingColor(Ping);
	UTextBlock* TextBlock = NewObject<UTextBlock>(TextBox);
	TextBlock->SetText(FText::AsNumber(Ping));
	TextBlock->SetColorAndOpacity(FSlateColor(Color));
	TextBlock->SetFont(MakeServerBrowserFont(RowFontSize));
	TextBlock->SetJustification(ETextJustify::Center);
	if (USizeBoxSlot* TextSlot = Cast<USizeBoxSlot>(TextBox->AddChild(TextBlock)))
	{
		TextSlot->SetHorizontalAlignment(HAlign_Center);
		TextSlot->SetVerticalAlignment(VAlign_Center);
	}

	UHorizontalBoxSlot* PingTextSlot = PingBox->AddChildToHorizontalBox(TextBox);
	PingTextSlot->SetVerticalAlignment(VAlign_Center);

	const int32 ActiveBars = ServerBrowserPingBarCount(Ping);
	for (int32 BarIndex = 0; BarIndex < 4; ++BarIndex)
	{
		USizeBox* BarBox = NewObject<USizeBox>(PingBox);
		BarBox->SetWidthOverride(5.0f);
		BarBox->SetHeightOverride(6.0f + (BarIndex * 4.0f));

		UBorder* Bar = NewObject<UBorder>(BarBox);
		const bool bActive = BarIndex < ActiveBars;
		Bar->SetBrushColor(bActive ? Color : FLinearColor(0.20f, 0.24f, 0.25f, 0.65f));
		BarBox->AddChild(Bar);

		UHorizontalBoxSlot* BarSlot = PingBox->AddChildToHorizontalBox(BarBox);
		BarSlot->SetPadding(FMargin(2.0f, 0.0f));
		BarSlot->SetVerticalAlignment(VAlign_Bottom);
	}

	if (USizeBoxSlot* PingBoxSlot = Cast<USizeBoxSlot>(SizeBox->AddChild(PingBox)))
	{
		PingBoxSlot->SetHorizontalAlignment(HAlign_Center);
		PingBoxSlot->SetVerticalAlignment(VAlign_Center);
	}
}

void ConfigureHeaderButton(UButton* Button, UTextBlock* Label, const float FillWeight, const ETextJustify::Type Justification, const EHorizontalAlignment Alignment)
{
	if (Button)
	{
		if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Button->Slot))
		{
			FSlateChildSize SlotSize;
			SlotSize.SizeRule = ESlateSizeRule::Fill;
			SlotSize.Value = FillWeight;
			HorizontalSlot->SetSize(SlotSize);
			HorizontalSlot->SetPadding(FMargin(0.0f));
			HorizontalSlot->SetHorizontalAlignment(HAlign_Fill);
			HorizontalSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}

	if (Label)
	{
		Label->SetFont(MakeServerBrowserFont(HeaderFontSize));
		Label->SetJustification(Justification);
		if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Label->Slot))
		{
			ButtonSlot->SetPadding(FMargin(10.0f, 2.0f, 10.0f, 2.0f));
			ButtonSlot->SetHorizontalAlignment(Alignment);
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
}
}

void UVNHServerBrowserRowButton::InitializeRow(UVNHServerBrowserWidget* InOwner, const int32 InEntryIndex)
{
	Owner = InOwner;
	EntryIndex = InEntryIndex;
	OnClicked.Clear();
	OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserRowButton::HandleClicked);
}

void UVNHServerBrowserRowButton::HandleClicked()
{
	if (Owner.IsValid())
	{
		Owner->HandleRowClicked(EntryIndex);
	}
}

void UVNHServerBrowserWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);

	if (SearchTextBox)
	{
		SearchTextBox->OnTextChanged.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSearchChanged);
		SearchTextBox->OnTextCommitted.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSearchCommitted);
	}
	if (FilterSearchTextBox)
	{
		FilterSearchTextBox->OnTextCommitted.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleFilterSearchCommitted);
	}
	if (FilterButton)
	{
		FilterButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleFilterClicked);
	}
	if (RefreshButton)
	{
		RefreshButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleRefreshClicked);
	}
	if (PublicTabButton)
	{
		PublicTabButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandlePublicTabClicked);
	}
	if (PrivateTabButton)
	{
		PrivateTabButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandlePrivateTabClicked);
	}
	if (SortServerNameButton)
	{
		SortServerNameButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSortServerNameClicked);
	}
	if (SortPlayersButton)
	{
		SortPlayersButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSortPlayersClicked);
	}
	if (SortMapButton)
	{
		SortMapButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSortMapClicked);
	}
	if (SortRegionButton)
	{
		SortRegionButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSortRegionClicked);
	}
	if (SortPingButton)
	{
		SortPingButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleSortPingClicked);
	}
	if (JoinNowButton)
	{
		JoinNowButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleJoinNowClicked);
	}
	if (CloseButton)
	{
		CloseButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleCloseClicked);
	}
	if (ResetFiltersButton)
	{
		ResetFiltersButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleResetFiltersClicked);
	}
	if (ApplyFiltersButton)
	{
		ApplyFiltersButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleApplyFiltersClicked);
	}
	if (CloseFiltersButton)
	{
		CloseFiltersButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandleCloseFiltersClicked);
	}

	ConfigureFilterCombos();
	ConfigureHeaderLayout();
	SetFilterOverlayVisible(false);
	PopulateExampleServers();
	UpdateTabLabels();
	UpdateSortHeaderLabels();
	RebuildVisibleEntries();
	SetStatus(NSLOCTEXT("VNH", "ServerBrowserReady", "Showing example servers. Click REFRESH to search for Steam servers."));
	UE_LOG(LogVNH, Display, TEXT("ServerBrowser: constructed and ready."));
}

FReply UVNHServerBrowserWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (PasswordPromptOverlay.IsValid())
		{
			HidePasswordPrompt();
		}
		else if (FilterOverlay && FilterOverlay->GetVisibility() == ESlateVisibility::Visible)
		{
			SetFilterOverlayVisible(false);
		}
		else
		{
			HandleCloseClicked();
		}
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UVNHServerBrowserWidget::HandleRowClicked(const int32 EntryIndex)
{
	const double Now = FPlatformTime::Seconds();
	const bool bDoubleClick = LastClickedVisibleIndex == EntryIndex && (Now - LastClickTime) <= DoubleClickWindowSeconds;
	LastClickedVisibleIndex = EntryIndex;
	LastClickTime = Now;

	SelectVisibleEntry(EntryIndex);
	if (bDoubleClick)
	{
		JoinSelectedServer();
	}
}

void UVNHServerBrowserWidget::HandleSearchChanged(const FText& Text)
{
	const FString Search = Text.ToString().TrimStartAndEnd();
	if (FilterSearchTextBox && (Search.IsEmpty() || FilterSearchTextBox->GetText().IsEmpty()))
	{
		FilterSearchTextBox->SetText(Text);
	}
	RebuildVisibleEntries();
}

void UVNHServerBrowserWidget::HandleSearchCommitted(const FText& Text, const ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter)
	{
		return;
	}

	if (Text.ToString().TrimStartAndEnd().IsEmpty())
	{
		if (SearchTextBox)
		{
			SearchTextBox->SetText(FText::GetEmpty());
		}
		if (FilterSearchTextBox)
		{
			FilterSearchTextBox->SetText(FText::GetEmpty());
		}
		RebuildVisibleEntries();
	}
}

void UVNHServerBrowserWidget::HandleFilterSearchCommitted(const FText& Text, const ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter)
	{
		return;
	}

	const FString Search = Text.ToString().TrimStartAndEnd();
	if (Search.IsEmpty())
	{
		if (FilterSearchTextBox)
		{
			FilterSearchTextBox->SetText(FText::GetEmpty());
		}
		if (SearchTextBox)
		{
			SearchTextBox->SetText(FText::GetEmpty());
		}
	}
	else if (SearchTextBox)
	{
		SearchTextBox->SetText(Text);
	}

	RebuildVisibleEntries();
}

void UVNHServerBrowserWidget::HandleFilterClicked()
{
	SetFilterOverlayVisible(true);
}

void UVNHServerBrowserWidget::HandleRefreshClicked()
{
	RefreshServerList();
}

void UVNHServerBrowserWidget::HandlePublicTabClicked()
{
	bShowingPrivateTab = false;
	UpdateTabLabels();
	RebuildVisibleEntries();
}

void UVNHServerBrowserWidget::HandlePrivateTabClicked()
{
	bShowingPrivateTab = true;
	UpdateTabLabels();
	RebuildVisibleEntries();
}

void UVNHServerBrowserWidget::HandleSortServerNameClicked()
{
	ToggleSort(EVNHServerBrowserSortKey::ServerName);
}

void UVNHServerBrowserWidget::HandleSortPlayersClicked()
{
	ToggleSort(EVNHServerBrowserSortKey::Players);
}

void UVNHServerBrowserWidget::HandleSortMapClicked()
{
	ToggleSort(EVNHServerBrowserSortKey::Map);
}

void UVNHServerBrowserWidget::HandleSortRegionClicked()
{
	ToggleSort(EVNHServerBrowserSortKey::Region);
}

void UVNHServerBrowserWidget::HandleSortPingClicked()
{
	ToggleSort(EVNHServerBrowserSortKey::Ping);
}

void UVNHServerBrowserWidget::HandleJoinNowClicked()
{
	JoinSelectedServer();
}

void UVNHServerBrowserWidget::HandleCloseClicked()
{
	RemoveFromParent();
}

void UVNHServerBrowserWidget::HandleResetFiltersClicked()
{
	if (FilterSearchTextBox)
	{
		FilterSearchTextBox->SetText(FText::GetEmpty());
	}
	if (SearchTextBox)
	{
		SearchTextBox->SetText(FText::GetEmpty());
	}
	for (UCheckBox* CheckBox : { Region_USEAST.Get(), Region_USCENTRAL.Get(), Region_USWEST.Get(), Region_EUEAST.Get(), Region_EUWEST.Get(), Region_SOUTHAMERICA.Get(), Region_ASIA.Get() })
	{
		if (CheckBox)
		{
			CheckBox->SetIsChecked(false);
		}
	}
	if (MapFilterComboBox)
	{
		MapFilterComboBox->SetSelectedOption(MapPlaceholder);
	}
	if (MinPlayersComboBox)
	{
		MinPlayersComboBox->SetSelectedOption(MinPlayersPlaceholder);
	}
	if (MinOpenSlotsComboBox)
	{
		MinOpenSlotsComboBox->SetSelectedOption(MinOpenSlotsPlaceholder);
	}
	if (HideFullServersCheckBox)
	{
		HideFullServersCheckBox->SetIsChecked(false);
	}
	RebuildVisibleEntries();
}

void UVNHServerBrowserWidget::HandleApplyFiltersClicked()
{
	if (SearchTextBox && FilterSearchTextBox)
	{
		SearchTextBox->SetText(FilterSearchTextBox->GetText());
	}
	SetFilterOverlayVisible(false);
	RefreshServerList();
}

void UVNHServerBrowserWidget::HandleCloseFiltersClicked()
{
	SetFilterOverlayVisible(false);
}

void UVNHServerBrowserWidget::HandlePasswordConfirmClicked()
{
	if (!VisibleEntries.IsValidIndex(PendingPasswordJoinVisibleIndex))
	{
		HidePasswordPrompt();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserPasswordNoSelection", "Select a private server first."));
		return;
	}

	const FString SuppliedPassword = PasswordPromptTextBox.IsValid() ? PasswordPromptTextBox->GetText().ToString() : FString();
	if (SuppliedPassword.IsEmpty())
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserPasswordRequired", "Enter the private server password."));
		return;
	}

	SelectedVisibleIndex = PendingPasswordJoinVisibleIndex;
	AcceptedPrivateJoinPassword = SuppliedPassword;
	bAcceptedPasswordForPendingJoin = true;
	HidePasswordPrompt();
	JoinSelectedServer();
}

void UVNHServerBrowserWidget::HandlePasswordCancelClicked()
{
	HidePasswordPrompt();
	SetStatus(NSLOCTEXT("VNH", "ServerBrowserPasswordCancelled", "Private join cancelled."));
}

void UVNHServerBrowserWidget::RefreshServerList()
{
	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld());
	if (!OnlineSubsystem)
	{
		AllEntries.Reset();
		PopulateExampleServers();
		RebuildVisibleEntries();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserNoSubsystem", "Steam online subsystem is not available. Showing example servers."));
		return;
	}

	if (!OnlineSubsystem->GetSubsystemName().ToString().Equals(TEXT("STEAM"), ESearchCase::IgnoreCase))
	{
		AllEntries.Reset();
		PopulateExampleServers();
		RebuildVisibleEntries();
		SetStatus(FText::FromString(FString::Printf(TEXT("Steam server search is not active in this session (%s). Showing example servers."), *OnlineSubsystem->GetSubsystemName().ToString())));
		UE_LOG(LogVNH, Warning, TEXT("ServerBrowser: refresh skipped because online subsystem is %s, not STEAM."), *OnlineSubsystem->GetSubsystemName().ToString());
		return;
	}

	ActiveSessionInterface = OnlineSubsystem->GetSessionInterface();
	if (!ActiveSessionInterface.IsValid())
	{
		AllEntries.Reset();
		PopulateExampleServers();
		RebuildVisibleEntries();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserNoSessions", "Steam sessions are not available. Showing example servers."));
		return;
	}

	if (FindSessionsCompleteHandle.IsValid())
	{
		ActiveSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteHandle);
	}

	ActiveSearch = MakeShared<FOnlineSessionSearch>();
	ActiveSearch->MaxSearchResults = 500;
	ActiveSearch->bIsLanQuery = false;
	ActiveSearch->QuerySettings.Set(SEARCH_LOBBIES, true, EOnlineComparisonOp::Equals);
	const TSharedPtr<const FUniqueNetId> SearchUserId = ResolveBrowserUserId(GetOwningPlayer());
	if (!SearchUserId.IsValid())
	{
		AllEntries.Reset();
		PopulateExampleServers();
		RebuildVisibleEntries();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserNoSteamUserId", "Steam is active, but no Steam user id is available. Restart Steam and launch the packaged game again."));
		UE_LOG(LogVNH, Warning, TEXT("ServerBrowser: Steam search skipped because no local unique net id was available."));
		return;
	}

	FindSessionsCompleteHandle = ActiveSessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(this, &UVNHServerBrowserWidget::HandleFindSessionsComplete));

	SetStatus(NSLOCTEXT("VNH", "ServerBrowserRefreshing", "Refreshing Steam server list..."));
	if (!ActiveSessionInterface->FindSessions(*SearchUserId, ActiveSearch.ToSharedRef()))
	{
		ActiveSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteHandle);
		AllEntries.Reset();
		PopulateExampleServers();
		RebuildVisibleEntries();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserRefreshFailed", "Could not start Steam server search. Showing example servers."));
	}
}

void UVNHServerBrowserWidget::HandleFindSessionsComplete(const bool bWasSuccessful)
{
	if (ActiveSessionInterface.IsValid() && FindSessionsCompleteHandle.IsValid())
	{
		ActiveSessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsCompleteHandle);
	}

	AllEntries.Reset();
	int32 SteamLobbyResultCount = 0;
	int32 VNHSteamLobbyCount = 0;
	if (bWasSuccessful && ActiveSearch.IsValid())
	{
		SteamLobbyResultCount = ActiveSearch->SearchResults.Num();
		for (const FOnlineSessionSearchResult& Result : ActiveSearch->SearchResults)
		{
			const FString GameId = GetSessionString(Result, BrowserSessionKeyGameId, FString());
			if (!GameId.Equals(BrowserSessionGameId, ESearchCase::IgnoreCase))
			{
				continue;
			}
			++VNHSteamLobbyCount;

			const int32 AdvertisedMaxPlayers = GetSessionInt(Result, BrowserSessionKeyMaxPlayers, Result.Session.SessionSettings.NumPublicConnections);
			const int32 OpenSlots = FMath::Clamp(Result.Session.NumOpenPublicConnections, 0, AdvertisedMaxPlayers);
			FOnlineSessionSearchResult LobbyResult = Result;
			LobbyResult.Session.SessionSettings.bUseLobbiesIfAvailable = true;
			LobbyResult.Session.SessionSettings.bUsesPresence = true;

			FVNHServerBrowserEntry Entry;
			Entry.ServerName = GetSessionString(LobbyResult, BrowserSessionKeyServerName, LobbyResult.Session.OwningUserName);
			if (Entry.ServerName.IsEmpty())
			{
				Entry.ServerName = LobbyResult.GetSessionIdStr();
			}
			Entry.MapName = GetSessionString(LobbyResult, BrowserSessionKeyMapName, DefaultMapName);
			Entry.Region = GetSessionString(LobbyResult, BrowserSessionKeyRegion, DefaultRegion).ToUpper();
			Entry.MaxPlayers = FMath::Max(AdvertisedMaxPlayers, 1);
			Entry.OpenSlots = OpenSlots;
			Entry.CurrentPlayers = FMath::Clamp(Entry.MaxPlayers - OpenSlots, 0, Entry.MaxPlayers);
			Entry.Ping = LobbyResult.PingInMs >= 0 ? LobbyResult.PingInMs : 999;
			Entry.bPrivate = GetSessionBool(LobbyResult, BrowserSessionKeyIsPrivate, false);
			Entry.SearchResult = LobbyResult;
			AllEntries.Add(MoveTemp(Entry));
		}
	}

	if (AllEntries.IsEmpty())
	{
		PopulateExampleServers();
	}

	RebuildVisibleEntries();
	const bool bShowingExamples = AllEntries.ContainsByPredicate([](const FVNHServerBrowserEntry& Entry) { return Entry.bExample; });
	SetStatus(bShowingExamples
		? FText::FromString(FString::Printf(TEXT("No VNH Steam lobbies found (%d Steam lobby result%s scanned). Showing example servers."), SteamLobbyResultCount, SteamLobbyResultCount == 1 ? TEXT("") : TEXT("s")))
		: FText::FromString(FString::Printf(TEXT("Found %d Steam server%s."), VisibleEntries.Num(), VisibleEntries.Num() == 1 ? TEXT("") : TEXT("s"))));
	UE_LOG(LogVNH, Display, TEXT("ServerBrowser: Steam search success=%s, raw lobby results=%d, VNH lobby results=%d."),
		bWasSuccessful ? TEXT("true") : TEXT("false"),
		SteamLobbyResultCount,
		VNHSteamLobbyCount);
}

void UVNHServerBrowserWidget::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result)
{
	if (ActiveSessionInterface.IsValid() && JoinSessionCompleteHandle.IsValid())
	{
		ActiveSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteHandle);
	}

	if (Result != EOnJoinSessionCompleteResult::Success || !ActiveSessionInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserJoinFailed", "Could not join the selected Steam server."));
		return;
	}

	FString ConnectString;
	if (!ActiveSessionInterface->GetResolvedConnectString(SessionName, ConnectString) || ConnectString.IsEmpty())
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserJoinNoAddress", "Steam did not return a connection address for this server."));
		return;
	}

	if (APlayerController* PlayerController = GetOwningPlayer())
	{
		if (VisibleEntries.IsValidIndex(SelectedVisibleIndex) && VisibleEntries[SelectedVisibleIndex].bPrivate)
		{
			const FString Password = AcceptedPrivateJoinPassword;
			if (!Password.IsEmpty())
			{
				ConnectString += FString::Printf(TEXT("?Password=%s"), *EncodeBrowserTravelOption(Password));
			}
		}
		AcceptedPrivateJoinPassword.Reset();
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserJoining", "Joining selected server..."));
		RemoveFromParent();
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
	}
}

void UVNHServerBrowserWidget::PopulateExampleServers()
{
	auto AddExample = [this](const TCHAR* ServerName, const int32 CurrentPlayers, const int32 ExampleMaxPlayers, const TCHAR* Region, const int32 Ping, const bool bPrivate = false)
	{
		FVNHServerBrowserEntry Entry;
		Entry.ServerName = ServerName;
		Entry.MapName = DefaultMapName;
		Entry.Region = Region;
		Entry.CurrentPlayers = FMath::Clamp(CurrentPlayers, 0, ExampleMaxPlayers);
		Entry.MaxPlayers = ExampleMaxPlayers;
		Entry.OpenSlots = FMath::Max(ExampleMaxPlayers - Entry.CurrentPlayers, 0);
		Entry.Ping = Ping;
		Entry.bPrivate = bPrivate;
		Entry.bExample = true;
		AllEntries.Add(MoveTemp(Entry));
	};

	AddExample(TEXT("Greg Is SUS"), 5, 8, TEXT("USEAST"), 28);
	AddExample(TEXT("Totally Not Greg #42"), 6, 8, TEXT("USCENTRAL"), 36);
	AddExample(TEXT("No GREGs Allowed"), 3, 8, TEXT("EU EAST"), 52);
	AddExample(TEXT("Greg? Never Heard Of Him"), 7, 8, TEXT("USWEST"), 61);
	AddExample(TEXT("Who's Greg?"), 4, 8, TEXT("EU WEST"), 74);
	AddExample(TEXT("Definitely Not Greg Server"), 8, 8, TEXT("SOUTH AMERICA"), 104);
	AddExample(TEXT("Greg Be Gone"), 2, 8, TEXT("ASIA"), 162);
	AddExample(TEXT("Impostor Training Grounds"), 5, 8, TEXT("USEAST"), 30);
	AddExample(TEXT("The Official Super Long Greg Detection Training Server For UI Stress Testing"), 4, 8, TEXT("USCENTRAL"), 88);
	AddExample(TEXT("Private Greg Check"), 3, 8, TEXT("USEAST"), 42, true);
}

void UVNHServerBrowserWidget::RebuildVisibleEntries()
{
	VisibleEntries.Reset();
	for (const FVNHServerBrowserEntry& Entry : AllEntries)
	{
		if (PassesFilters(Entry))
		{
			VisibleEntries.Add(Entry);
		}
	}

	VisibleEntries.Sort([this](const FVNHServerBrowserEntry& Left, const FVNHServerBrowserEntry& Right)
	{
		int32 Compare = 0;
		switch (SortKey)
		{
		case EVNHServerBrowserSortKey::Players:
			Compare = Left.CurrentPlayers == Right.CurrentPlayers ? 0 : (Left.CurrentPlayers < Right.CurrentPlayers ? -1 : 1);
			break;
		case EVNHServerBrowserSortKey::Map:
			Compare = Left.MapName.Compare(Right.MapName, ESearchCase::IgnoreCase);
			break;
		case EVNHServerBrowserSortKey::Region:
			Compare = Left.Region.Compare(Right.Region, ESearchCase::IgnoreCase);
			break;
		case EVNHServerBrowserSortKey::Ping:
			Compare = Left.Ping == Right.Ping ? 0 : (Left.Ping < Right.Ping ? -1 : 1);
			break;
		case EVNHServerBrowserSortKey::ServerName:
		default:
			Compare = Left.ServerName.Compare(Right.ServerName, ESearchCase::IgnoreCase);
			break;
		}

		return bSortAscending ? Compare < 0 : Compare > 0;
	});

	PopulateRows();
	SelectVisibleEntry(VisibleEntries.IsValidIndex(SelectedVisibleIndex) ? SelectedVisibleIndex : (VisibleEntries.Num() > 0 ? 0 : INDEX_NONE));
}

void UVNHServerBrowserWidget::PopulateRows()
{
	if (!ServerRowsBox)
	{
		return;
	}

	ServerRowsBox->ClearChildren();
	for (int32 Index = 0; Index < VisibleEntries.Num(); ++Index)
	{
		const FVNHServerBrowserEntry& Entry = VisibleEntries[Index];
		UVNHServerBrowserRowButton* RowButton = NewObject<UVNHServerBrowserRowButton>(this);
		RowButton->InitializeRow(this, Index);
		RowButton->SetStyle(MakeRowButtonStyle(Index == SelectedVisibleIndex));

		UHorizontalBox* Row = NewObject<UHorizontalBox>(RowButton);
		AddServerNameCell(Row, RowButton, Entry.ServerName, Index == SelectedVisibleIndex);
		AddRowText(Row, RowButton, FString::Printf(TEXT("%d / %d"), Entry.CurrentPlayers, Entry.MaxPlayers), ColumnPlayersWeight, FLinearColor::White, ETextJustify::Center);
		AddRowText(Row, RowButton, Entry.MapName, ColumnMapWeight);
		AddRowText(Row, RowButton, Entry.Region, ColumnRegionWeight, FLinearColor::White, ETextJustify::Center);
		AddPingCell(Row, RowButton, Entry.Ping);
		if (UButtonSlot* RowContentSlot = Cast<UButtonSlot>(RowButton->AddChild(Row)))
		{
			RowContentSlot->SetHorizontalAlignment(HAlign_Fill);
			RowContentSlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* RowSlot = ServerRowsBox->AddChildToVerticalBox(RowButton);
		RowSlot->SetPadding(FMargin(0.0f, 1.0f));
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UVNHServerBrowserWidget::SelectVisibleEntry(const int32 EntryIndex)
{
	SelectedVisibleIndex = EntryIndex;
	const FVNHServerBrowserEntry* Entry = VisibleEntries.IsValidIndex(EntryIndex) ? &VisibleEntries[EntryIndex] : nullptr;

	if (SelectedServerNameText)
	{
		SelectedServerNameText->SetText(Entry ? FText::FromString(Entry->ServerName) : NSLOCTEXT("VNH", "ServerBrowserNoSelection", "No server selected"));
	}
	if (SelectedPlayersText)
	{
		SelectedPlayersText->SetText(Entry ? FormatPlayers(*Entry) : FText::FromString(TEXT("- / -")));
	}
	if (SelectedMapText)
	{
		SelectedMapText->SetText(Entry ? FText::FromString(Entry->MapName) : FText::FromString(TEXT("-")));
	}
	if (SelectedRegionText)
	{
		SelectedRegionText->SetText(Entry ? FText::FromString(Entry->Region) : FText::FromString(TEXT("-")));
	}

	ApplyRowSelectionStyles();
}

void UVNHServerBrowserWidget::ApplyRowSelectionStyles()
{
	if (!ServerRowsBox)
	{
		return;
	}

	for (int32 ChildIndex = 0; ChildIndex < ServerRowsBox->GetChildrenCount(); ++ChildIndex)
	{
		if (UVNHServerBrowserRowButton* RowButton = Cast<UVNHServerBrowserRowButton>(ServerRowsBox->GetChildAt(ChildIndex)))
		{
			RowButton->SetStyle(MakeRowButtonStyle(ChildIndex == SelectedVisibleIndex));
		}
	}
}

void UVNHServerBrowserWidget::JoinSelectedServer()
{
	if (!VisibleEntries.IsValidIndex(SelectedVisibleIndex))
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserSelectFirst", "Select a server first."));
		return;
	}

	if (VisibleEntries[SelectedVisibleIndex].bExample)
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserExampleJoin", "Example server selected. Refresh Steam servers to join a real session."));
		return;
	}

	if (VisibleEntries[SelectedVisibleIndex].bPrivate && !bAcceptedPasswordForPendingJoin)
	{
		ShowPasswordPromptForSelectedServer();
		return;
	}
	bAcceptedPasswordForPendingJoin = false;

	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld());
	ActiveSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!ActiveSessionInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserJoinNoSessions", "Steam sessions are not available."));
		return;
	}

	if (JoinSessionCompleteHandle.IsValid())
	{
		ActiveSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteHandle);
	}

	JoinSessionCompleteHandle = ActiveSessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateUObject(this, &UVNHServerBrowserWidget::HandleJoinSessionComplete));

	SetStatus(FText::FromString(FString::Printf(TEXT("Joining %s..."), *VisibleEntries[SelectedVisibleIndex].ServerName)));
	if (!ActiveSessionInterface->JoinSession(0, NAME_GameSession, VisibleEntries[SelectedVisibleIndex].SearchResult))
	{
		ActiveSessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionCompleteHandle);
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserJoinStartFailed", "Could not start Steam join."));
	}
}

void UVNHServerBrowserWidget::ShowPasswordPromptForSelectedServer()
{
	if (!VisibleEntries.IsValidIndex(SelectedVisibleIndex))
	{
		return;
	}

	PendingPasswordJoinVisibleIndex = SelectedVisibleIndex;

	UWidget* RootWidget = WidgetTree ? WidgetTree->RootWidget : nullptr;
	UPanelWidget* RootPanel = Cast<UPanelWidget>(RootWidget);
	if (!RootPanel)
	{
		SetStatus(NSLOCTEXT("VNH", "ServerBrowserPasswordPromptUnavailable", "Password entry is unavailable for this server browser layout."));
		return;
	}

	UBorder* Overlay = NewObject<UBorder>(this);
	Overlay->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.82f));

	UVerticalBox* Dialog = NewObject<UVerticalBox>(Overlay);
	UTextBlock* Title = NewObject<UTextBlock>(Dialog);
	Title->SetText(FText::FromString(FString::Printf(TEXT("PASSWORD // %s"), *VisibleEntries[SelectedVisibleIndex].ServerName)));
	Title->SetFont(MakeServerBrowserFont(28));
	Title->SetColorAndOpacity(FSlateColor(FLinearColor(0.0f, 1.0f, 0.92f, 1.0f)));
	Dialog->AddChildToVerticalBox(Title)->SetPadding(FMargin(32.0f, 24.0f, 32.0f, 8.0f));

	UEditableTextBox* PasswordBox = NewObject<UEditableTextBox>(Dialog);
	PasswordBox->SetIsPassword(true);
	PasswordBox->SetHintText(NSLOCTEXT("VNH", "ServerBrowserPasswordHint", "Server password"));
	Dialog->AddChildToVerticalBox(PasswordBox)->SetPadding(FMargin(32.0f, 8.0f, 32.0f, 16.0f));

	UHorizontalBox* ButtonRow = NewObject<UHorizontalBox>(Dialog);
	UButton* JoinButton = NewObject<UButton>(ButtonRow);
	UTextBlock* JoinLabel = NewObject<UTextBlock>(JoinButton);
	JoinLabel->SetText(NSLOCTEXT("VNH", "ServerBrowserPasswordJoin", "JOIN"));
	JoinLabel->SetFont(MakeServerBrowserFont(22));
	JoinButton->AddChild(JoinLabel);
	JoinButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandlePasswordConfirmClicked);
	ButtonRow->AddChildToHorizontalBox(JoinButton)->SetPadding(FMargin(32.0f, 0.0f, 8.0f, 24.0f));

	UButton* CancelButton = NewObject<UButton>(ButtonRow);
	UTextBlock* CancelLabel = NewObject<UTextBlock>(CancelButton);
	CancelLabel->SetText(NSLOCTEXT("VNH", "ServerBrowserPasswordCancel", "CANCEL"));
	CancelLabel->SetFont(MakeServerBrowserFont(22));
	CancelButton->AddChild(CancelLabel);
	CancelButton->OnClicked.AddUniqueDynamic(this, &UVNHServerBrowserWidget::HandlePasswordCancelClicked);
	ButtonRow->AddChildToHorizontalBox(CancelButton)->SetPadding(FMargin(8.0f, 0.0f, 32.0f, 24.0f));
	Dialog->AddChildToVerticalBox(ButtonRow);

	Overlay->AddChild(Dialog);
	UPanelSlot* OverlaySlot = RootPanel->AddChild(Overlay);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(OverlaySlot))
	{
		CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		CanvasSlot->SetOffsets(FMargin(0.0f));
		CanvasSlot->SetZOrder(10000);
	}
	PasswordPromptOverlay = Overlay;
	PasswordPromptTextBox = PasswordBox;
	SetStatus(NSLOCTEXT("VNH", "ServerBrowserPasswordRequired", "Enter the private server password."));
}

void UVNHServerBrowserWidget::HidePasswordPrompt()
{
	if (UBorder* Overlay = PasswordPromptOverlay.Get())
	{
		Overlay->RemoveFromParent();
	}

	PasswordPromptOverlay.Reset();
	PasswordPromptTextBox.Reset();
	PendingPasswordJoinVisibleIndex = INDEX_NONE;
}

void UVNHServerBrowserWidget::SetStatus(const FText& NewStatus)
{
	if (StatusText)
	{
		StatusText->SetText(NewStatus);
	}
}

void UVNHServerBrowserWidget::SetFilterOverlayVisible(const bool bVisible)
{
	if (FilterOverlay)
	{
		FilterOverlay->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UVNHServerBrowserWidget::ConfigureFilterCombos()
{
	if (MapFilterComboBox)
	{
		MapFilterComboBox->ClearOptions();
		MapFilterComboBox->AddOption(MapPlaceholder);
		MapFilterComboBox->AddOption(DefaultMapName);
		MapFilterComboBox->SetSelectedOption(MapPlaceholder);
	}

	auto ConfigureNumberCombo = [this](UComboBoxString* ComboBox, const int32 MaxValue, const FString& Placeholder, const TCHAR* Suffix)
	{
		if (!ComboBox)
		{
			return;
		}
		ComboBox->ClearOptions();
		ComboBox->AddOption(Placeholder);
		for (int32 Value = 1; Value <= MaxValue; ++Value)
		{
			ComboBox->AddOption(FString::Printf(TEXT("%d+ %s"), Value, Suffix));
		}
		ComboBox->SetSelectedOption(Placeholder);
	};

	ConfigureNumberCombo(MinPlayersComboBox, 8, MinPlayersPlaceholder, TEXT("Players"));
	ConfigureNumberCombo(MinOpenSlotsComboBox, 8, MinOpenSlotsPlaceholder, TEXT("Open Slots"));
}

void UVNHServerBrowserWidget::ConfigureHeaderLayout()
{
	ConfigureHeaderButton(SortServerNameButton, SortServerNameButton_Label, ColumnServerNameWeight, ETextJustify::Left, HAlign_Left);
	ConfigureHeaderButton(SortPlayersButton, SortPlayersButton_Label, ColumnPlayersWeight, ETextJustify::Center, HAlign_Center);
	ConfigureHeaderButton(SortMapButton, MapHeaderText, ColumnMapWeight, ETextJustify::Left, HAlign_Left);
	ConfigureHeaderButton(SortRegionButton, RegionHeaderText, ColumnRegionWeight, ETextJustify::Center, HAlign_Center);
	ConfigureHeaderButton(SortPingButton, SortPingButton_Label, ColumnPingWeight, ETextJustify::Center, HAlign_Center);
}

void UVNHServerBrowserWidget::UpdateTabLabels()
{
	if (PublicTabLabel)
	{
		PublicTabLabel->SetText(NSLOCTEXT("VNH", "ServerBrowserPublicTab", "PUBLIC"));
		PublicTabLabel->SetColorAndOpacity(FSlateColor(bShowingPrivateTab ? FLinearColor(0.62f, 0.62f, 0.66f, 1.0f) : FLinearColor(0.0f, 1.0f, 0.92f, 1.0f)));
	}
	if (PrivateTabLabel)
	{
		PrivateTabLabel->SetText(NSLOCTEXT("VNH", "ServerBrowserPrivateTab", "PRIVATE"));
		PrivateTabLabel->SetColorAndOpacity(FSlateColor(bShowingPrivateTab ? FLinearColor(0.0f, 1.0f, 0.92f, 1.0f) : FLinearColor(0.62f, 0.62f, 0.66f, 1.0f)));
	}
	if (PublicTabButton)
	{
		PublicTabButton->SetStyle(MakeTabButtonStyle(!bShowingPrivateTab));
	}
	if (PrivateTabButton)
	{
		PrivateTabButton->SetStyle(MakeTabButtonStyle(bShowingPrivateTab));
	}
}

void UVNHServerBrowserWidget::UpdateSortHeaderLabels()
{
	const FLinearColor ActiveColor(0.0f, 1.0f, 0.92f, 1.0f);
	const FLinearColor InactiveColor(1.0f, 1.0f, 1.0f, 0.92f);

	auto ApplyHeader = [this, &ActiveColor, &InactiveColor](UTextBlock* LabelWidget, const TCHAR* Label, const EVNHServerBrowserSortKey Key)
	{
		if (!LabelWidget)
		{
			return;
		}

		if (SortKey != Key)
		{
			LabelWidget->SetText(FText::FromString(FString::Printf(TEXT("%s v"), Label)));
			LabelWidget->SetColorAndOpacity(FSlateColor(InactiveColor));
			return;
		}

		LabelWidget->SetText(FText::FromString(FString::Printf(TEXT("%s %s"), Label, bSortAscending ? TEXT("^") : TEXT("v"))));
		LabelWidget->SetColorAndOpacity(FSlateColor(ActiveColor));
	};

	ApplyHeader(SortServerNameButton_Label, TEXT("SERVER NAME"), EVNHServerBrowserSortKey::ServerName);
	ApplyHeader(SortPlayersButton_Label, TEXT("PLAYERS"), EVNHServerBrowserSortKey::Players);
	ApplyHeader(MapHeaderText, TEXT("MAP"), EVNHServerBrowserSortKey::Map);
	ApplyHeader(RegionHeaderText, TEXT("REGION"), EVNHServerBrowserSortKey::Region);
	ApplyHeader(SortPingButton_Label, TEXT("PING"), EVNHServerBrowserSortKey::Ping);
}

FButtonStyle UVNHServerBrowserWidget::MakeRowButtonStyle(const bool bSelected) const
{
	const FLinearColor NormalColor = bSelected ? FLinearColor(0.00f, 0.20f, 0.18f, 0.92f) : FLinearColor(0.01f, 0.03f, 0.035f, 0.82f);
	const FLinearColor HoverColor = bSelected ? FLinearColor(0.00f, 0.30f, 0.27f, 0.95f) : FLinearColor(0.02f, 0.10f, 0.10f, 0.90f);
	const FLinearColor PressedColor = FLinearColor(0.00f, 0.42f, 0.37f, 1.0f);

	FButtonStyle Style;
	Style.SetNormal(MakeBoxBrush(NormalColor));
	Style.SetHovered(MakeBoxBrush(HoverColor));
	Style.SetPressed(MakeBoxBrush(PressedColor));
	Style.SetDisabled(MakeBoxBrush(FLinearColor(0.01f, 0.02f, 0.02f, 0.55f)));
	Style.NormalPadding = FMargin(0.0f);
	Style.PressedPadding = FMargin(0.0f, 1.0f, 0.0f, 0.0f);
	return Style;
}

FButtonStyle UVNHServerBrowserWidget::MakeTabButtonStyle(const bool bSelected) const
{
	const FLinearColor NormalColor = bSelected ? FLinearColor(0.00f, 0.23f, 0.20f, 0.96f) : FLinearColor(0.01f, 0.03f, 0.04f, 0.76f);
	const FLinearColor HoverColor = bSelected ? FLinearColor(0.00f, 0.33f, 0.30f, 1.0f) : FLinearColor(0.02f, 0.10f, 0.10f, 0.90f);

	FButtonStyle Style;
	Style.SetNormal(MakeBoxBrush(NormalColor, FVector2D(10.0f, 10.0f)));
	Style.SetHovered(MakeBoxBrush(HoverColor, FVector2D(10.0f, 10.0f)));
	Style.SetPressed(MakeBoxBrush(FLinearColor(0.00f, 0.42f, 0.37f, 1.0f), FVector2D(10.0f, 10.0f)));
	Style.SetDisabled(MakeBoxBrush(FLinearColor(0.01f, 0.02f, 0.02f, 0.55f), FVector2D(10.0f, 10.0f)));
	Style.NormalPadding = FMargin(8.0f, 4.0f);
	Style.PressedPadding = FMargin(8.0f, 5.0f, 8.0f, 3.0f);
	return Style;
}

bool UVNHServerBrowserWidget::PassesFilters(const FVNHServerBrowserEntry& Entry) const
{
	if (Entry.bPrivate != bShowingPrivateTab)
	{
		return false;
	}

	const FString Search = SearchTextBox ? SearchTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	const FString OverlaySearch = FilterSearchTextBox ? FilterSearchTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	const FString EffectiveSearch = !OverlaySearch.IsEmpty() ? OverlaySearch : Search;
	if (!EffectiveSearch.IsEmpty() && !Entry.ServerName.Contains(EffectiveSearch, ESearchCase::IgnoreCase))
	{
		return false;
	}

	const TSet<FString> Regions = GetSelectedRegions();
	if (Regions.Num() > 0 && !Regions.Contains(Entry.Region.ToUpper()))
	{
		return false;
	}

	if (MapFilterComboBox)
	{
		const FString SelectedMap = MapFilterComboBox->GetSelectedOption();
		if (!SelectedMap.IsEmpty() && SelectedMap != MapPlaceholder && !Entry.MapName.Equals(SelectedMap, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}

	const int32 MinPlayersFilter = GetComboNumber(MinPlayersComboBox);
	if (MinPlayersFilter > 0 && Entry.CurrentPlayers < MinPlayersFilter)
	{
		return false;
	}

	const int32 MinOpenSlots = GetComboNumber(MinOpenSlotsComboBox);
	if (MinOpenSlots > 0 && Entry.OpenSlots < MinOpenSlots)
	{
		return false;
	}

	if (HideFullServersCheckBox && HideFullServersCheckBox->IsChecked() && Entry.OpenSlots <= 0)
	{
		return false;
	}

	return true;
}

void UVNHServerBrowserWidget::ToggleSort(const EVNHServerBrowserSortKey NewSortKey)
{
	if (SortKey == NewSortKey)
	{
		bSortAscending = !bSortAscending;
	}
	else
	{
		SortKey = NewSortKey;
		bSortAscending = true;
	}

	RebuildVisibleEntries();
	UpdateSortHeaderLabels();
}

TSet<FString> UVNHServerBrowserWidget::GetSelectedRegions() const
{
	TSet<FString> Regions;
	auto AddIfChecked = [&Regions](const UCheckBox* CheckBox, const TCHAR* Region)
	{
		if (CheckBox && CheckBox->IsChecked())
		{
			Regions.Add(Region);
		}
	};

	AddIfChecked(Region_USEAST, TEXT("USEAST"));
	AddIfChecked(Region_USCENTRAL, TEXT("USCENTRAL"));
	AddIfChecked(Region_USWEST, TEXT("USWEST"));
	AddIfChecked(Region_EUEAST, TEXT("EU EAST"));
	AddIfChecked(Region_EUWEST, TEXT("EU WEST"));
	AddIfChecked(Region_SOUTHAMERICA, TEXT("SOUTH AMERICA"));
	AddIfChecked(Region_ASIA, TEXT("ASIA"));
	return Regions;
}

int32 UVNHServerBrowserWidget::GetComboNumber(const UComboBoxString* ComboBox) const
{
	if (!ComboBox)
	{
		return 0;
	}

	const FString Selected = ComboBox->GetSelectedOption();
	if (Selected.IsEmpty() || Selected == MinPlayersPlaceholder || Selected == MinOpenSlotsPlaceholder || Selected == MapPlaceholder)
	{
		return 0;
	}

	return FCString::Atoi(*Selected);
}

FText UVNHServerBrowserWidget::FormatPlayers(const FVNHServerBrowserEntry& Entry) const
{
	return FText::FromString(FString::Printf(TEXT("%d / %d"), Entry.CurrentPlayers, Entry.MaxPlayers));
}
