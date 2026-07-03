#include "VNHLobbyMenuWidget.h"

#include "AdvancedSteamFriendsLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Font.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Interfaces/OnlinePresenceInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Styling/SlateBrush.h"
#include "VNHGameInstance.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"

namespace
{
const FLinearColor Accent(0.0f, 1.0f, 0.92f, 1.0f);
const FLinearColor Panel(0.005f, 0.035f, 0.04f, 0.82f);
const FLinearColor PanelStrong(0.005f, 0.025f, 0.03f, 0.94f);
const FLinearColor Muted(0.62f, 0.70f, 0.70f, 0.95f);
const FLinearColor White(0.94f, 0.96f, 0.95f, 1.0f);
constexpr int32 DefaultLobbyMaxPlayers = 8;

UFont* GetLobbyFont()
{
	static TWeakObjectPtr<UFont> CachedFont;
	if (!CachedFont.IsValid())
	{
		CachedFont = LoadObject<UFont>(nullptr, TEXT("/Game/ThirdPerson/Demo/BurbankBigCondensed-Black_Font.BurbankBigCondensed-Black_Font"));
	}
	return CachedFont.Get();
}

FSlateFontInfo LobbyFont(const int32 Size)
{
	if (UFont* Font = GetLobbyFont())
	{
		return FSlateFontInfo(Font, Size, FName(TEXT("Default")));
	}
	return FSlateFontInfo(FCoreStyle::GetDefaultFont(), Size);
}

FSlateBrush BoxBrush(const FLinearColor& Color)
{
	FSlateBrush Brush;
	Brush.DrawAs = ESlateBrushDrawType::Box;
	Brush.TintColor = FSlateColor(Color);
	Brush.Margin = FMargin(0.0f);
	return Brush;
}

FButtonStyle ButtonStyle(const bool bDisabled = false)
{
	FButtonStyle Style;
	Style.SetNormal(BoxBrush(bDisabled ? FLinearColor(0.06f, 0.07f, 0.075f, 0.82f) : FLinearColor(0.0f, 0.12f, 0.12f, 0.90f)));
	Style.SetHovered(BoxBrush(FLinearColor(0.0f, 0.24f, 0.22f, 0.96f)));
	Style.SetPressed(BoxBrush(FLinearColor(0.0f, 0.34f, 0.31f, 1.0f)));
	Style.SetDisabled(BoxBrush(FLinearColor(0.035f, 0.04f, 0.045f, 0.72f)));
	Style.NormalPadding = FMargin(14.0f, 8.0f);
	Style.PressedPadding = FMargin(14.0f, 9.0f, 14.0f, 7.0f);
	return Style;
}

FSlateChildSize FillSize(const float Value = 1.0f)
{
	FSlateChildSize Size;
	Size.SizeRule = ESlateSizeRule::Fill;
	Size.Value = Value;
	return Size;
}

int32 GetLocalUserNum(const UUserWidget* Widget)
{
	const APlayerController* PlayerController = Widget ? Widget->GetOwningPlayer() : nullptr;
	const ULocalPlayer* LocalPlayer = PlayerController ? Cast<ULocalPlayer>(PlayerController->Player) : nullptr;
	return LocalPlayer ? LocalPlayer->GetControllerId() : 0;
}

UTextBlock* Text(UObject* Outer, const FString& Value, const int32 Size, const FLinearColor& Color = White, const ETextJustify::Type Justification = ETextJustify::Left)
{
	UTextBlock* TextBlock = NewObject<UTextBlock>(Outer);
	TextBlock->SetText(FText::FromString(Value));
	TextBlock->SetFont(LobbyFont(Size));
	TextBlock->SetColorAndOpacity(FSlateColor(Color));
	TextBlock->SetJustification(Justification);
	TextBlock->SetClipping(EWidgetClipping::ClipToBounds);
#if ENGINE_MAJOR_VERSION >= 5
	TextBlock->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
#endif
	return TextBlock;
}

UClass* GetAdvancedSteamFriendsLibraryClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/AdvancedSteamSessions.AdvancedSteamFriendsLibrary"));
}

void RequestSteamFriendInfoReflective(const FBPUniqueNetId& UniqueNetId)
{
	UClass* LibraryClass = GetAdvancedSteamFriendsLibraryClass();
	UObject* LibraryObject = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
	UFunction* Function = LibraryClass ? LibraryClass->FindFunctionByName(TEXT("RequestSteamFriendInfo")) : nullptr;
	if (!LibraryObject || !Function)
	{
		return;
	}

	struct FRequestParams
	{
		FBPUniqueNetId UniqueNetId;
		bool bRequireNameOnly = false;
		bool ReturnValue = false;
	};

	FRequestParams Params;
	Params.UniqueNetId = UniqueNetId;
	LibraryObject->ProcessEvent(Function, &Params);
}

UTexture2D* GetSteamFriendAvatarReflective(const FBPUniqueNetId& UniqueNetId)
{
	UClass* LibraryClass = GetAdvancedSteamFriendsLibraryClass();
	UObject* LibraryObject = LibraryClass ? LibraryClass->GetDefaultObject() : nullptr;
	UFunction* Function = LibraryClass ? LibraryClass->FindFunctionByName(TEXT("GetSteamFriendAvatar")) : nullptr;
	if (!LibraryObject || !Function)
	{
		return nullptr;
	}

	struct FAvatarParams
	{
		FBPUniqueNetId UniqueNetId;
		EBlueprintAsyncResultSwitch Result = EBlueprintAsyncResultSwitch::OnFailure;
		SteamAvatarSize AvatarSize = SteamAvatarSize::SteamAvatar_Medium;
		UTexture2D* ReturnValue = nullptr;
	};

	FAvatarParams Params;
	Params.UniqueNetId = UniqueNetId;
	LibraryObject->ProcessEvent(Function, &Params);
	return Params.ReturnValue;
}

UCanvasPanelSlot* AddCanvas(UCanvasPanel* Root, UWidget* Child, const FAnchors& Anchors, const FMargin& Offsets, const FVector2D Alignment, const int32 ZOrder)
{
	if (UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Child))
	{
		Slot->SetAnchors(Anchors);
		Slot->SetOffsets(Offsets);
		Slot->SetAlignment(Alignment);
		Slot->SetZOrder(ZOrder);
		return Slot;
	}
	return nullptr;
}

UButton* MakeTextButton(UObject* Outer, const FString& Label, const int32 FontSize = 22)
{
	UButton* Button = NewObject<UButton>(Outer);
	Button->SetStyle(ButtonStyle());
	UTextBlock* LabelText = Text(Button, Label, FontSize, Accent, ETextJustify::Center);
	if (UButtonSlot* Slot = Cast<UButtonSlot>(Button->AddChild(LabelText)))
	{
		Slot->SetHorizontalAlignment(HAlign_Center);
		Slot->SetVerticalAlignment(VAlign_Center);
	}
	return Button;
}

FLinearColor PingColor(const int32 Ping)
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

int32 PingBarCount(const int32 Ping)
{
	if (Ping >= 110) return 1;
	if (Ping >= 80) return 2;
	if (Ping >= 50) return 3;
	return 4;
}

FString PresenceText(const FOnlineUserPresence& Presence, FLinearColor& OutColor, bool& bOutOnline)
{
	bOutOnline = Presence.bIsOnline;
	if (Presence.bIsPlayingThisGame)
	{
		OutColor = FLinearColor(1.0f, 0.48f, 0.0f, 1.0f);
		return TEXT("In Match");
	}
	if (!Presence.bIsOnline)
	{
		OutColor = FLinearColor(0.45f, 0.48f, 0.48f, 1.0f);
		return TEXT("Offline");
	}
	if (Presence.Status.State == EOnlinePresenceState::Away || Presence.Status.State == EOnlinePresenceState::ExtendedAway)
	{
		OutColor = FLinearColor(1.0f, 0.82f, 0.0f, 1.0f);
		return TEXT("Away");
	}
	OutColor = FLinearColor(0.45f, 1.0f, 0.15f, 1.0f);
	return TEXT("Online");
}
}

void UVNHLobbyFriendInviteButton::Initialize(UVNHLobbyMenuWidget* InOwner, const FString& InFriendId)
{
	Owner = InOwner;
	FriendId = InFriendId;
	OnClicked.Clear();
	OnClicked.AddUniqueDynamic(this, &UVNHLobbyFriendInviteButton::HandleClicked);
}

void UVNHLobbyFriendInviteButton::HandleClicked()
{
	if (Owner)
	{
		Owner->InviteFriendById(FriendId);
	}
}

TSharedRef<SWidget> UVNHLobbyMenuWidget::RebuildWidget()
{
	BuildLobbyHud();
	return Super::RebuildWidget();
}

void UVNHLobbyMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);
	RequestFriendsList();
	RefreshLobbyLabels();
	RefreshPlayers();
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: NativeConstruct complete. WidgetTree=%s Root=%s"),
		WidgetTree ? TEXT("true") : TEXT("false"),
		WidgetTree && WidgetTree->RootWidget ? *WidgetTree->RootWidget->GetName() : TEXT("None"));
}

void UVNHLobbyMenuWidget::NativeDestruct()
{
	Super::NativeDestruct();
}

void UVNHLobbyMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	UpdateResponsiveLayout(MyGeometry);
	RefreshAccumulator += InDeltaTime;
	if (RefreshAccumulator >= 0.35f)
	{
		RefreshAccumulator = 0.0f;
		RefreshLobbyLabels();
		RefreshPlayers();
	}
}

FReply UVNHLobbyMenuWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape && InviteDialog.IsValid() && InviteDialog->GetVisibility() == ESlateVisibility::Visible)
	{
		SetInviteDialogVisible(false);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UVNHLobbyMenuWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && CachedActionSize.X > 0.0f && CachedActionSize.Y > 0.0f)
	{
		const FVector2D LocalPosition = InGeometry.AbsoluteToLocal(InMouseEvent.GetScreenSpacePosition());
		const bool bInsideActionRow =
			LocalPosition.X >= CachedActionPosition.X &&
			LocalPosition.X <= CachedActionPosition.X + CachedActionSize.X &&
			LocalPosition.Y >= CachedActionPosition.Y &&
			LocalPosition.Y <= CachedActionPosition.Y + CachedActionSize.Y;
		if (bInsideActionRow)
		{
			const float SegmentWidth = CachedActionSize.X / 3.0f;
			const int32 SegmentIndex = FMath::Clamp(FMath::FloorToInt((LocalPosition.X - CachedActionPosition.X) / SegmentWidth), 0, 2);
			if (SegmentIndex == 1)
			{
				HandleInviteClicked();
			}
			else if (SegmentIndex == 2)
			{
				HandleCustomizeClicked();
			}
			return FReply::Handled();
		}
	}

	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UVNHLobbyMenuWidget::BuildLobbyHud()
{
	if (!WidgetTree)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenuWidget: BuildLobbyHud skipped because WidgetTree is null."));
		return;
	}

	UCanvasPanel* Root = Cast<UCanvasPanel>(WidgetTree->RootWidget);
	if (!Root)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenuWidget: BuildLobbyHud skipped because root widget is not a CanvasPanel."));
		return;
	}
	Root->ClearChildren();
	Root->SetVisibility(ESlateVisibility::Visible);

	UBorder* TopLeft = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyNamePanel"));
	TopLeft->SetBrushColor(PanelStrong);
	AddCanvas(Root, TopLeft, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(24.0f, 24.0f, 390.0f, 104.0f), FVector2D(0.0f, 0.0f), 10);

	UVerticalBox* HeaderStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LobbyNameStack"));
	TopLeft->SetContent(HeaderStack);
	HeaderStack->AddChildToVerticalBox(Text(HeaderStack, TEXT("HOST LOBBY"), 31, Accent));
	HeaderStack->AddChildToVerticalBox(Text(HeaderStack, TEXT("You can start the match anytime."), 17, White));
	LobbyNameText = Cast<UTextBlock>(HeaderStack->GetChildAt(0));

	UBorder* TopRight = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyStatsPanel"));
	TopRight->SetBrushColor(PanelStrong);
	AddCanvas(Root, TopRight, FAnchors(1.0f, 0.0f, 1.0f, 0.0f), FMargin(-24.0f, 24.0f, 292.0f, 58.0f), FVector2D(1.0f, 0.0f), 10);

	UHorizontalBox* StatsRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LobbyStatsRow"));
	TopRight->SetContent(StatsRow);
	PlayerCountText = Text(StatsRow, TEXT("1 / 8"), 24, White, ETextJustify::Center);
	StatsRow->AddChildToHorizontalBox(PlayerCountText.Get())->SetPadding(FMargin(18.0f, 0.0f, 18.0f, 0.0f));
	PingBarsBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("PingBars"));
	StatsRow->AddChildToHorizontalBox(PingBarsBox.Get())->SetPadding(FMargin(12.0f, 0.0f, 8.0f, 0.0f));
	PingText = Text(StatsRow, TEXT("0ms"), 22, White, ETextJustify::Center);
	StatsRow->AddChildToHorizontalBox(PingText.Get())->SetPadding(FMargin(4.0f, 0.0f, 14.0f, 0.0f));

	UBorder* PlayersPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PlayersPanel"));
	PlayersPanel->SetBrushColor(PanelStrong);
	AddCanvas(Root, PlayersPanel, FAnchors(1.0f, 0.5f, 1.0f, 0.5f), FMargin(-24.0f, 0.0f, 300.0f, 330.0f), FVector2D(1.0f, 0.5f), 10);
	UVerticalBox* PlayersStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PlayersStack"));
	PlayersPanel->SetContent(PlayersStack);
	PlayersStack->AddChildToVerticalBox(Text(PlayersStack, TEXT("PLAYERS"), 22, Accent));
	PlayerRowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PlayerRows"));
	PlayersStack->AddChildToVerticalBox(PlayerRowsBox.Get());

	UHorizontalBox* ButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LobbyActionButtons"));
	ButtonRow->SetVisibility(ESlateVisibility::Visible);
	ActionButtonsSlot = AddCanvas(Root, ButtonRow, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(0.0f, 0.0f, 660.0f, 54.0f), FVector2D::ZeroVector, 20);
	UButton* EmoteButton = MakeTextButton(ButtonRow, TEXT("EMOTE"), 20);
	UButton* InviteButton = MakeTextButton(ButtonRow, TEXT("INVITE"), 20);
	UButton* CustomizeButton = MakeTextButton(ButtonRow, TEXT("CUSTOMIZE"), 20);
	InviteButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleInviteClicked);
	CustomizeButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCustomizeClicked);
	for (UButton* Button : { EmoteButton, InviteButton, CustomizeButton })
	{
		UHorizontalBoxSlot* ActionButtonSlot = ButtonRow->AddChildToHorizontalBox(Button);
		ActionButtonSlot->SetPadding(FMargin(8.0f, 0.0f));
		ActionButtonSlot->SetSize(FillSize());
		Button->SetVisibility(ESlateVisibility::Visible);
	}

	BuildInviteDialog();
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: HUD tree built."));
}

void UVNHLobbyMenuWidget::BuildInviteDialog()
{
	UCanvasPanel* Root = Cast<UCanvasPanel>(WidgetTree ? WidgetTree->RootWidget : nullptr);
	if (!Root)
	{
		return;
	}

	UBorder* Dialog = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("InviteFriendsDialog"));
	Dialog->SetBrushColor(PanelStrong);
	Dialog->SetVisibility(ESlateVisibility::Collapsed);
	InviteDialogSlot = AddCanvas(Root, Dialog, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(0.0f, 0.0f, 960.0f, 600.0f), FVector2D::ZeroVector, 100);
	InviteDialog = Dialog;

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InviteDialogStack"));
	Dialog->SetContent(Stack);

	UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InviteTitleRow"));
	Stack->AddChildToVerticalBox(TitleRow)->SetPadding(FMargin(28.0f, 20.0f, 28.0f, 12.0f));
	UTextBlock* Title = Text(TitleRow, TEXT("INVITE FRIENDS"), 46, White);
	TitleRow->AddChildToHorizontalBox(Title)->SetSize(FillSize());
	UButton* CloseButton = MakeTextButton(TitleRow, TEXT("X"), 28);
	CloseButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCloseInviteClicked);
	if (UTexture2D* CloseTexture = LoadObject<UTexture2D>(nullptr, TEXT("/Game/UI/Images/x_button_ui__1_.x_button_ui__1_")))
	{
		UImage* CloseImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("InviteCloseImage"));
		CloseImage->SetBrushFromTexture(CloseTexture, true);
		CloseButton->SetContent(CloseImage);
	}
	UHorizontalBoxSlot* CloseSlot = TitleRow->AddChildToHorizontalBox(CloseButton);
	CloseSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));

	UHorizontalBox* SearchRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("InviteSearchRow"));
	Stack->AddChildToVerticalBox(SearchRow)->SetPadding(FMargin(28.0f, 0.0f, 28.0f, 10.0f));
	SearchRow->AddChildToHorizontalBox(Text(SearchRow, TEXT("STEAM FRIENDS"), 22, Accent))->SetSize(FillSize());
	SearchTextBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("InviteSearchTextBox"));
	SearchTextBox->SetHintText(NSLOCTEXT("VNH", "LobbyInviteSearchHint", "Search friends..."));
	SearchTextBox->OnTextChanged.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleSearchChanged);
	SearchRow->AddChildToHorizontalBox(SearchTextBox.Get())->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));

	FriendsScrollBox = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("FriendsScrollBox"));
	UVerticalBoxSlot* ScrollSlot = Stack->AddChildToVerticalBox(FriendsScrollBox.Get());
	ScrollSlot->SetPadding(FMargin(28.0f, 0.0f, 28.0f, 10.0f));
	ScrollSlot->SetSize(FillSize());

	InviteStatusText = Text(Stack, TEXT("Invite friends to your lobby."), 17, Muted);
	Stack->AddChildToVerticalBox(InviteStatusText.Get())->SetPadding(FMargin(28.0f, 0.0f, 28.0f, 20.0f));
}

void UVNHLobbyMenuWidget::UpdateResponsiveLayout(const FGeometry& MyGeometry)
{
	const FVector2D LocalSize = MyGeometry.GetLocalSize();
	if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
	{
		return;
	}

	const FVector2D UsableSize(LocalSize.X, FMath::Min(LocalSize.Y, LocalSize.X * 9.0f / 16.0f));

	if (UCanvasPanelSlot* ActionSlot = ActionButtonsSlot.Get())
	{
		const float ButtonRowWidth = FMath::Clamp(UsableSize.X - 48.0f, 360.0f, 660.0f);
		const FVector2D ButtonRowPosition((UsableSize.X - ButtonRowWidth) * 0.5f, FMath::Max(0.0f, UsableSize.Y - 86.0f));
		ActionSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 0.0f));
		ActionSlot->SetAlignment(FVector2D::ZeroVector);
		ActionSlot->SetPosition(ButtonRowPosition);
		ActionSlot->SetSize(FVector2D(ButtonRowWidth, 54.0f));
		CachedActionPosition = ButtonRowPosition;
		CachedActionSize = FVector2D(ButtonRowWidth, 54.0f);
	}

	if (UCanvasPanelSlot* DialogSlot = InviteDialogSlot.Get())
	{
		const FVector2D DialogSize(FMath::Clamp(UsableSize.X - 48.0f, 520.0f, 960.0f), FMath::Clamp(UsableSize.Y - 64.0f, 420.0f, 600.0f));
		DialogSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 0.0f));
		DialogSlot->SetAlignment(FVector2D::ZeroVector);
		DialogSlot->SetPosition((UsableSize - DialogSize) * 0.5f);
		DialogSlot->SetSize(DialogSize);
	}
}

void UVNHLobbyMenuWidget::RefreshLobbyLabels()
{
	const UWorld* World = GetWorld();
	const FString UrlOptions = World ? World->URL.ToString() : FString();
	const FString ServerName = UGameplayStatics::ParseOption(UrlOptions, TEXT("ServerName"));
	const FString LobbyName = ServerName.IsEmpty() ? TEXT("HOST LOBBY") : ServerName.ToUpper();
	if (UTextBlock* TextBlock = LobbyNameText.Get())
	{
		TextBlock->SetText(FText::FromString(LobbyName));
	}

	CachedMaxPlayers = DefaultLobbyMaxPlayers;
	if (World)
	{
		const FString MaxPlayersOption = UGameplayStatics::ParseOption(UrlOptions, TEXT("MaxPlayers"));
		if (!MaxPlayersOption.IsEmpty())
		{
			CachedMaxPlayers = FMath::Max(FCString::Atoi(*MaxPlayersOption), 1);
		}
	}

	const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	const int32 PlayerCount = GameState ? GameState->PlayerArray.Num() : 1;
	if (UTextBlock* TextBlock = PlayerCountText.Get())
	{
		TextBlock->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), PlayerCount, CachedMaxPlayers)));
	}

	const APlayerController* OwningPlayerController = GetOwningPlayer();
	const APlayerState* LocalPlayerState = OwningPlayerController ? OwningPlayerController->PlayerState : nullptr;
	const int32 Ping = LocalPlayerState ? FMath::Max(0, FMath::RoundToInt(LocalPlayerState->GetPingInMilliseconds())) : 0;
	const FLinearColor Color = PingColor(Ping);
	if (UTextBlock* TextBlock = PingText.Get())
	{
		TextBlock->SetText(FText::FromString(FString::Printf(TEXT("%dms"), Ping)));
		TextBlock->SetColorAndOpacity(FSlateColor(Color));
	}
	if (UHorizontalBox* Bars = PingBarsBox.Get())
	{
		Bars->ClearChildren();
		const int32 ActiveBars = PingBarCount(Ping);
		for (int32 Index = 0; Index < 4; ++Index)
		{
			USizeBox* BarBox = NewObject<USizeBox>(Bars);
			BarBox->SetWidthOverride(5.0f);
			BarBox->SetHeightOverride(7.0f + Index * 4.0f);
			UBorder* Bar = NewObject<UBorder>(BarBox);
			Bar->SetBrushColor(Index < ActiveBars ? Color : FLinearColor(0.20f, 0.24f, 0.25f, 0.65f));
			BarBox->AddChild(Bar);
			UHorizontalBoxSlot* PingBarSlot = Bars->AddChildToHorizontalBox(BarBox);
			PingBarSlot->SetPadding(FMargin(2.0f, 0.0f));
			PingBarSlot->SetVerticalAlignment(VAlign_Bottom);
		}
	}
}

void UVNHLobbyMenuWidget::RefreshPlayers()
{
	UVerticalBox* Rows = PlayerRowsBox.Get();
	if (!Rows)
	{
		return;
	}
	Rows->ClearChildren();

	const AGameStateBase* GameState = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	const int32 PlayerArrayCount = GameState ? GameState->PlayerArray.Num() : 0;
	for (int32 Index = 0; Index < PlayerArrayCount; ++Index)
	{
		const APlayerState* PlayerState = GameState->PlayerArray[Index];
		if (!PlayerState)
		{
			continue;
		}

		UHorizontalBox* Row = NewObject<UHorizontalBox>(Rows);
		Row->AddChildToHorizontalBox(Text(Row, Index == 0 ? TEXT("^") : TEXT(" "), 18, Index == 0 ? FLinearColor(1.0f, 0.82f, 0.0f, 1.0f) : Muted));
		Row->AddChildToHorizontalBox(Text(Row, PlayerState->GetPlayerName(), 19, White))->SetSize(FillSize());
		if (Index == 0)
		{
			Row->AddChildToHorizontalBox(Text(Row, TEXT("HOST"), 16, Accent));
		}
		Rows->AddChildToVerticalBox(Row)->SetPadding(FMargin(14.0f, 8.0f, 14.0f, 8.0f));
	}
}

void UVNHLobbyMenuWidget::RequestFriendsList()
{
	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld());
	if (!OnlineSubsystem)
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteNoSubsystem", "Steam friends are unavailable."));
		return;
	}

	ActiveFriendsInterface = OnlineSubsystem->GetFriendsInterface();
	ActiveSessionInterface = OnlineSubsystem->GetSessionInterface();
	if (!ActiveFriendsInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteNoFriends", "Steam friends are unavailable."));
		return;
	}

	SetStatus(NSLOCTEXT("VNH", "LobbyInviteLoading", "Loading Steam friends..."));
	if (!ActiveFriendsInterface->ReadFriendsList(
		GetLocalUserNum(this),
		EFriendsLists::ToString(EFriendsLists::Default),
		FOnReadFriendsListComplete::CreateUObject(this, &UVNHLobbyMenuWidget::HandleReadFriendsListComplete)))
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteLoadFailed", "Could not start Steam friends request."));
	}
}

void UVNHLobbyMenuWidget::HandleReadFriendsListComplete(int32 LocalUserNum, bool bWasSuccessful, const FString& ListName, const FString& ErrorString)
{
	Friends.Reset();
	if (!bWasSuccessful || !ActiveFriendsInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteLoadFailedStatus", "Could not load Steam friends."));
		RebuildFriendsList();
		return;
	}

	TArray<TSharedRef<FOnlineFriend>> OnlineFriends;
	ActiveFriendsInterface->GetFriendsList(LocalUserNum, ListName, OnlineFriends);
	for (const TSharedRef<FOnlineFriend>& Friend : OnlineFriends)
	{
		FSteamFriendEntry Entry;
		Entry.UserId = Friend->GetUserId();
		Entry.Id = Entry.UserId.IsValid() ? Entry.UserId->ToString() : FString();
		Entry.DisplayName = Friend->GetDisplayName();
		Entry.StatusText = PresenceText(Friend->GetPresence(), Entry.StatusColor, Entry.bOnline);
		if (Entry.UserId.IsValid())
		{
			FBPUniqueNetId BpId;
			BpId.SetUniqueNetId(Entry.UserId);
			RequestSteamFriendInfoReflective(BpId);
			Entry.Avatar = GetSteamFriendAvatarReflective(BpId);
		}
		Friends.Add(MoveTemp(Entry));
	}

	Friends.Sort([](const FSteamFriendEntry& Left, const FSteamFriendEntry& Right)
	{
		if (Left.bOnline != Right.bOnline)
		{
			return Left.bOnline;
		}
		return Left.DisplayName < Right.DisplayName;
	});

	SetStatus(FText::FromString(FString::Printf(TEXT("%d Steam friend%s loaded."), Friends.Num(), Friends.Num() == 1 ? TEXT("") : TEXT("s"))));
	RebuildFriendsList();
}

void UVNHLobbyMenuWidget::RebuildFriendsList()
{
	UScrollBox* ScrollBox = FriendsScrollBox.Get();
	if (!ScrollBox)
	{
		return;
	}
	ScrollBox->ClearChildren();

	const FString Search = SearchTextBox.IsValid() ? SearchTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	for (const FSteamFriendEntry& Friend : Friends)
	{
		if (!Search.IsEmpty() && !Friend.DisplayName.Contains(Search, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UHorizontalBox* Row = NewObject<UHorizontalBox>(ScrollBox);
		UImage* Avatar = NewObject<UImage>(Row);
		if (Friend.Avatar.IsValid())
		{
			Avatar->SetBrushFromTexture(Friend.Avatar.Get(), true);
		}
		else
		{
			Avatar->SetColorAndOpacity(FLinearColor(0.0f, 0.18f, 0.17f, 1.0f));
		}
		UHorizontalBoxSlot* AvatarSlot = Row->AddChildToHorizontalBox(Avatar);
		AvatarSlot->SetPadding(FMargin(8.0f));
		AvatarSlot->SetVerticalAlignment(VAlign_Center);

		UVerticalBox* NameStack = NewObject<UVerticalBox>(Row);
		NameStack->AddChildToVerticalBox(Text(NameStack, Friend.DisplayName, 20, White));
		UHorizontalBox* StatusRow = NewObject<UHorizontalBox>(NameStack);
		StatusRow->AddChildToHorizontalBox(Text(StatusRow, TEXT("●"), 14, Friend.StatusColor));
		StatusRow->AddChildToHorizontalBox(Text(StatusRow, FString::Printf(TEXT(" %s"), *Friend.StatusText), 15, Friend.StatusColor));
		NameStack->AddChildToVerticalBox(StatusRow);
		Row->AddChildToHorizontalBox(NameStack)->SetSize(FillSize());

		UVNHLobbyFriendInviteButton* InviteButton = NewObject<UVNHLobbyFriendInviteButton>(Row);
		InviteButton->Initialize(this, Friend.Id);
		InviteButton->SetIsEnabled(!Friend.bInvited && Friend.UserId.IsValid());
		InviteButton->SetStyle(ButtonStyle(Friend.bInvited));
		UTextBlock* InviteLabel = Text(InviteButton, Friend.bInvited ? TEXT("INVITED") : TEXT("INVITE"), 18, Friend.bInvited ? Muted : Accent, ETextJustify::Center);
		InviteButton->AddChild(InviteLabel);
		UHorizontalBoxSlot* InviteSlot = Row->AddChildToHorizontalBox(InviteButton);
		InviteSlot->SetPadding(FMargin(10.0f, 8.0f));
		InviteSlot->SetVerticalAlignment(VAlign_Center);

		ScrollBox->AddChild(Row);
	}
}

void UVNHLobbyMenuWidget::SetInviteDialogVisible(bool bVisible)
{
	if (InviteDialog.IsValid())
	{
		InviteDialog->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
	if (bVisible)
	{
		RequestFriendsList();
	}
}

void UVNHLobbyMenuWidget::SetStatus(const FText& StatusText)
{
	if (UTextBlock* TextBlock = InviteStatusText.Get())
	{
		TextBlock->SetText(StatusText);
	}
}

void UVNHLobbyMenuWidget::HandleInviteClicked()
{
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: Invite clicked."));
	SetInviteDialogVisible(true);
}

void UVNHLobbyMenuWidget::HandleCustomizeClicked()
{
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: Customize clicked."));
	if (UVNHGameInstance* VNHGameInstance = GetGameInstance<UVNHGameInstance>())
	{
		VNHGameInstance->ShowCharacterCustomizer(true);
	}
}

void UVNHLobbyMenuWidget::HandleCloseInviteClicked()
{
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: Invite dialog closed."));
	SetInviteDialogVisible(false);
}

void UVNHLobbyMenuWidget::HandleSearchChanged(const FText& Text)
{
	RebuildFriendsList();
}

void UVNHLobbyMenuWidget::InviteFriendById(const FString& FriendId)
{
	FSteamFriendEntry* Friend = Friends.FindByPredicate([&FriendId](const FSteamFriendEntry& Entry)
	{
		return Entry.Id == FriendId;
	});
	if (!Friend || !Friend->UserId.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteInvalidFriend", "That friend is not available."));
		return;
	}

	IOnlineSubsystem* OnlineSubsystem = Online::GetSubsystem(GetWorld());
	ActiveSessionInterface = OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
	if (!ActiveSessionInterface.IsValid())
	{
		SetStatus(NSLOCTEXT("VNH", "LobbyInviteNoSessionInterface", "Steam session invites are unavailable."));
		return;
	}

	if (!ActiveSessionInterface->SendSessionInviteToFriend(GetLocalUserNum(this), NAME_GameSession, *Friend->UserId))
	{
		SetStatus(FText::FromString(FString::Printf(TEXT("Could not invite %s."), *Friend->DisplayName)));
		return;
	}

	Friend->bInvited = true;
	SetStatus(FText::FromString(FString::Printf(TEXT("Invited %s to the lobby."), *Friend->DisplayName)));
	RebuildFriendsList();
}
