#include "VNHLobbyMenuWidget.h"

#include "AdvancedSteamFriendsLibrary.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CircularThrobber.h"
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
#include "VNHGameState.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"
#include "VNHPlayerState.h"

namespace
{
TMap<TWeakObjectPtr<UVNHLobbyMenuWidget>, FString> VNHLobbyPlayerRowSignatures;

const FLinearColor Accent(0.0f, 1.0f, 0.92f, 1.0f);
const FLinearColor Panel(0.005f, 0.035f, 0.04f, 0.82f);
const FLinearColor PanelStrong(0.005f, 0.025f, 0.03f, 0.94f);
const FLinearColor Muted(0.62f, 0.70f, 0.70f, 0.95f);
const FLinearColor White(0.94f, 0.96f, 0.95f, 1.0f);
const FLinearColor LobbyOrange(1.0f, 0.42f, 0.02f, 1.0f);
const FLinearColor LobbyBlue(0.02f, 0.38f, 0.88f, 1.0f);
const FLinearColor LobbyGreen(0.12f, 0.62f, 0.20f, 1.0f);
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

FButtonStyle TeamButtonStyle(const FLinearColor& TeamColor, const bool bSelected)
{
	const FLinearColor NormalColor = bSelected ? TeamColor : FLinearColor(TeamColor.R * 0.28f, TeamColor.G * 0.28f, TeamColor.B * 0.28f, 0.94f);
	const FLinearColor HoverColor = FLinearColor(
		FMath::Min(TeamColor.R * 0.72f + 0.08f, 1.0f),
		FMath::Min(TeamColor.G * 0.72f + 0.08f, 1.0f),
		FMath::Min(TeamColor.B * 0.72f + 0.08f, 1.0f),
		1.0f);
	FButtonStyle Style;
	Style.SetNormal(BoxBrush(NormalColor));
	Style.SetHovered(BoxBrush(HoverColor));
	Style.SetPressed(BoxBrush(TeamColor));
	Style.SetDisabled(BoxBrush(FLinearColor(NormalColor.R, NormalColor.G, NormalColor.B, 0.46f)));
	Style.NormalPadding = FMargin(16.0f, 11.0f);
	Style.PressedPadding = FMargin(16.0f, 12.0f, 16.0f, 10.0f);
	return Style;
}

FString LobbyModeText(EPPGameMode LobbyMode)
{
	return LobbyMode == EPPGameMode::AttachedAtTheHip ? TEXT("Attached at the Hip") : TEXT("Together, United");
}

FString LobbyCourseText(EPPLobbyCourseType LobbyCourse)
{
	switch (LobbyCourse)
	{
	case EPPLobbyCourseType::BuildForOtherTeam:
		return TEXT("Build for Other Team");
	case EPPLobbyCourseType::OneCustomObstacle:
		return TEXT("One Custom Obstacle");
	case EPPLobbyCourseType::PresetMaps:
	default:
		return TEXT("Preset Maps");
	}
}

FString LobbyPresetMapText(EPPPresetMap LobbyMap)
{
	switch (LobbyMap)
	{
	case EPPPresetMap::SkyScramble:
		return TEXT("Sky Scramble");
	case EPPPresetMap::JungleJam:
		return TEXT("Jungle Jam");
	case EPPPresetMap::PipePanic:
		return TEXT("Pipe Panic");
	case EPPPresetMap::FactoryFiasco:
	default:
		return TEXT("Factory Fiasco");
	}
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
	const bool bIsNativeFallbackClass = GetClass() == UVNHLobbyMenuWidget::StaticClass();
	if (bIsNativeFallbackClass)
	{
		EnsureLobbyRootWidget();
	}
	TSharedRef<SWidget> RebuiltWidget = Super::RebuildWidget();
	if (bIsNativeFallbackClass)
	{
		BuildLobbyHud();
	}
	return RebuiltWidget;
}

void UVNHLobbyMenuWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);
	bUsingDesignerLobbyHud = BindDesignerLobbyHud();
	if (!bUsingDesignerLobbyHud)
	{
		const FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
		ApplyResponsiveLobbyLayout(ViewportSize);
	}
	RequestFriendsList();
	RefreshLobbyLabels();
	RefreshPlayers();
	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: NativeConstruct complete. WidgetTree=%s Root=%s DesignerHud=%s"),
		WidgetTree ? TEXT("true") : TEXT("false"),
		WidgetTree && WidgetTree->RootWidget ? *WidgetTree->RootWidget->GetName() : TEXT("None"),
		bUsingDesignerLobbyHud ? TEXT("true") : TEXT("false"));
}

void UVNHLobbyMenuWidget::NativeDestruct()
{
	VNHLobbyPlayerRowSignatures.Remove(this);
	Super::NativeDestruct();
}

void UVNHLobbyMenuWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bUsingDesignerLobbyHud)
	{
		UpdateResponsiveLayout(MyGeometry);
	}
	UpdateLobbyStartPrompt();
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
	if (InKeyEvent.GetKey() == EKeys::H)
	{
		bLobbyHudHidden = !bLobbyHudHidden;
		ApplyLobbyHudVisibility();
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::C)
	{
		HandleMascotClicked();
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::Escape && MatchSetupDialog.IsValid() && MatchSetupDialog->GetVisibility() == ESlateVisibility::Visible)
	{
		SetMatchSetupDialogVisible(false);
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::Escape && InviteDialog.IsValid() && InviteDialog->GetVisibility() == ESlateVisibility::Visible)
	{
		SetInviteDialogVisible(false);
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UVNHLobbyMenuWidget::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (!bUsingDesignerLobbyHud && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton && CachedActionSize.X > 0.0f && CachedActionSize.Y > 0.0f)
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

bool UVNHLobbyMenuWidget::HasDesignerLobbyHud() const
{
	return WidgetTree
		&& WidgetTree->RootWidget
		&& WidgetTree->FindWidget(TEXT("LobbyHudLayer"))
		&& WidgetTree->FindWidget(TEXT("LobbyNameText"))
		&& WidgetTree->FindWidget(TEXT("PlayerRowsBox"))
		&& WidgetTree->FindWidget(TEXT("InviteButton"))
		&& WidgetTree->FindWidget(TEXT("MatchSetupButton"))
		&& WidgetTree->FindWidget(TEXT("OrangeTeamButton"));
}

bool UVNHLobbyMenuWidget::BindDesignerLobbyHud()
{
	if (!HasDesignerLobbyHud())
	{
		return false;
	}

	LobbyNameText = Cast<UTextBlock>(GetWidgetFromName(TEXT("LobbyNameText")));
	LobbyHudLayer = Cast<UCanvasPanel>(GetWidgetFromName(TEXT("LobbyHudLayer")));
	HudHiddenHint = Cast<UTextBlock>(GetWidgetFromName(TEXT("HudHiddenHint")));
	LobbySubtitleText = Cast<UTextBlock>(GetWidgetFromName(TEXT("LobbySubtitleText")));
	LobbyStatusText = Cast<UTextBlock>(GetWidgetFromName(TEXT("LobbyStatusText")));
	LobbyCodeText = Cast<UTextBlock>(GetWidgetFromName(TEXT("LobbyCodeText")));
	PlayerCountText = Cast<UTextBlock>(GetWidgetFromName(TEXT("PlayerCountText")));
	PingText = Cast<UTextBlock>(GetWidgetFromName(TEXT("PingText")));
	LobbyStartPromptPanel = Cast<UBorder>(GetWidgetFromName(TEXT("LobbyStartPromptPanel")));
	LobbyStartPromptText = Cast<UTextBlock>(GetWidgetFromName(TEXT("LobbyStartPromptText")));
	LobbyStartProgressCircle = Cast<UCircularThrobber>(GetWidgetFromName(TEXT("LobbyStartProgressCircle")));
	PingBarsBox = Cast<UHorizontalBox>(GetWidgetFromName(TEXT("PingBarsBox")));
	PlayerRowsBox = Cast<UVerticalBox>(GetWidgetFromName(TEXT("PlayerRowsBox")));
	InviteDialog = Cast<UBorder>(GetWidgetFromName(TEXT("InviteDialog")));
	SearchTextBox = Cast<UEditableTextBox>(GetWidgetFromName(TEXT("InviteSearchTextBox")));
	FriendsScrollBox = Cast<UScrollBox>(GetWidgetFromName(TEXT("FriendsScrollBox")));
	InviteStatusText = Cast<UTextBlock>(GetWidgetFromName(TEXT("InviteStatusText")));
	ModeValueText = Cast<UTextBlock>(GetWidgetFromName(TEXT("ModeValueText")));
	SetupValueText = Cast<UTextBlock>(GetWidgetFromName(TEXT("SetupValueText")));
	MatchSetupButton = Cast<UButton>(GetWidgetFromName(TEXT("MatchSetupButton")));
	MatchSetupButtonLabel = Cast<UTextBlock>(GetWidgetFromName(TEXT("MatchSetupButton_Label")));
	PrimaryActionButton = Cast<UButton>(GetWidgetFromName(TEXT("PrimaryActionButton")));
	PrimaryActionTitleText = Cast<UTextBlock>(GetWidgetFromName(TEXT("PrimaryActionTitleText")));
	PrimaryActionSubtitleText = Cast<UTextBlock>(GetWidgetFromName(TEXT("PrimaryActionSubtitleText")));
	OrangeTeamButton = Cast<UButton>(GetWidgetFromName(TEXT("OrangeTeamButton")));
	BlueTeamButton = Cast<UButton>(GetWidgetFromName(TEXT("BlueTeamButton")));
	GreenTeamButton = Cast<UButton>(GetWidgetFromName(TEXT("GreenTeamButton")));
	OrangeTeamCountText = Cast<UTextBlock>(GetWidgetFromName(TEXT("OrangeTeamCountText")));
	BlueTeamCountText = Cast<UTextBlock>(GetWidgetFromName(TEXT("BlueTeamCountText")));
	GreenTeamCountText = Cast<UTextBlock>(GetWidgetFromName(TEXT("GreenTeamCountText")));
	MascotButton = Cast<UButton>(GetWidgetFromName(TEXT("MascotButton")));
	MatchSetupDialog = Cast<UBorder>(GetWidgetFromName(TEXT("MatchSetupDialog")));
	ModeTogetherButton = Cast<UButton>(GetWidgetFromName(TEXT("ModeTogetherButton")));
	ModeAttachedButton = Cast<UButton>(GetWidgetFromName(TEXT("ModeAttachedButton")));
	CoursePresetButton = Cast<UButton>(GetWidgetFromName(TEXT("CoursePresetButton")));
	CourseBuildButton = Cast<UButton>(GetWidgetFromName(TEXT("CourseBuildButton")));
	CourseObstacleButton = Cast<UButton>(GetWidgetFromName(TEXT("CourseObstacleButton")));
	MapFactoryButton = Cast<UButton>(GetWidgetFromName(TEXT("MapFactoryButton")));
	MapSkyButton = Cast<UButton>(GetWidgetFromName(TEXT("MapSkyButton")));
	MapJungleButton = Cast<UButton>(GetWidgetFromName(TEXT("MapJungleButton")));
	MapPipeButton = Cast<UButton>(GetWidgetFromName(TEXT("MapPipeButton")));
	PresetMapsSection = GetWidgetFromName(TEXT("PresetMapsSection"));

	LobbyNameSlot = LobbyNameText.IsValid() ? Cast<UCanvasPanelSlot>(LobbyNameText->Slot) : nullptr;
	LobbyStatusSlot = LobbyStatusText.IsValid() ? Cast<UCanvasPanelSlot>(LobbyStatusText->Slot) : nullptr;
	LobbyCodeSlot = LobbyCodeText.IsValid() ? Cast<UCanvasPanelSlot>(LobbyCodeText->Slot) : nullptr;
	LobbyStatsSlot = PlayerCountText.IsValid() ? Cast<UCanvasPanelSlot>(PlayerCountText->Slot) : nullptr;
	PlayersPanelSlot = PlayerRowsBox.IsValid() ? Cast<UCanvasPanelSlot>(PlayerRowsBox->Slot) : nullptr;
	LobbyStartPromptSlot = LobbyStartPromptPanel.IsValid() ? Cast<UCanvasPanelSlot>(LobbyStartPromptPanel->Slot) : nullptr;
	InviteDialogSlot = InviteDialog.IsValid() ? Cast<UCanvasPanelSlot>(InviteDialog->Slot) : nullptr;

	if (UButton* InviteButton = Cast<UButton>(GetWidgetFromName(TEXT("InviteButton"))))
	{
		InviteButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleInviteClicked);
	}
	if (UButton* PlayerInviteButton = Cast<UButton>(GetWidgetFromName(TEXT("PlayerInviteButton"))))
	{
		PlayerInviteButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleInviteClicked);
	}
	if (MatchSetupButton.IsValid()) MatchSetupButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMatchSetupClicked);
	if (PrimaryActionButton.IsValid()) PrimaryActionButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandlePrimaryActionClicked);
	if (OrangeTeamButton.IsValid()) OrangeTeamButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleOrangeTeamClicked);
	if (BlueTeamButton.IsValid()) BlueTeamButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleBlueTeamClicked);
	if (GreenTeamButton.IsValid()) GreenTeamButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleGreenTeamClicked);
	if (MascotButton.IsValid()) MascotButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMascotClicked);
	if (UButton* HideHudButton = Cast<UButton>(GetWidgetFromName(TEXT("HideHudButton")))) HideHudButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleHideHudClicked);
	if (UButton* CloseSetupButton = Cast<UButton>(GetWidgetFromName(TEXT("CloseMatchSetupButton")))) CloseSetupButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCloseMatchSetupClicked);
	if (UButton* CancelSetupButton = Cast<UButton>(GetWidgetFromName(TEXT("CancelMatchSetupButton")))) CancelSetupButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCloseMatchSetupClicked);
	if (UButton* SaveSetupButton = Cast<UButton>(GetWidgetFromName(TEXT("SaveMatchSetupButton")))) SaveSetupButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleSaveMatchSetupClicked);
	if (ModeTogetherButton.IsValid()) ModeTogetherButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleModeTogetherClicked);
	if (ModeAttachedButton.IsValid()) ModeAttachedButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleModeAttachedClicked);
	if (CoursePresetButton.IsValid()) CoursePresetButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCoursePresetClicked);
	if (CourseBuildButton.IsValid()) CourseBuildButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCourseBuildClicked);
	if (CourseObstacleButton.IsValid()) CourseObstacleButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCourseObstacleClicked);
	if (MapFactoryButton.IsValid()) MapFactoryButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMapFactoryClicked);
	if (MapSkyButton.IsValid()) MapSkyButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMapSkyClicked);
	if (MapJungleButton.IsValid()) MapJungleButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMapJungleClicked);
	if (MapPipeButton.IsValid()) MapPipeButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleMapPipeClicked);
	if (UButton* CloseInviteButton = Cast<UButton>(GetWidgetFromName(TEXT("CloseInviteButton"))))
	{
		CloseInviteButton->OnClicked.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleCloseInviteClicked);
	}
	if (UEditableTextBox* InviteSearchTextBox = SearchTextBox.Get())
	{
		InviteSearchTextBox->OnTextChanged.AddUniqueDynamic(this, &UVNHLobbyMenuWidget::HandleSearchChanged);
	}
	if (UBorder* Dialog = InviteDialog.Get())
	{
		Dialog->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (UBorder* SetupDialog = MatchSetupDialog.Get())
	{
		SetupDialog->SetVisibility(ESlateVisibility::Collapsed);
	}
	ApplyLobbyHudVisibility();

	UE_LOG(LogVNH, Display, TEXT("LobbyMenuWidget: bound designer WBP_LobbyMenu widgets."));
	return true;
}

UCanvasPanel* UVNHLobbyMenuWidget::EnsureLobbyRootWidget()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}

	UCanvasPanel* Root = Cast<UCanvasPanel>(WidgetTree->RootWidget);
	if (!Root)
	{
		Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
		WidgetTree->RootWidget = Root;
	}

	Root->SetVisibility(ESlateVisibility::Visible);
	return Root;
}

void UVNHLobbyMenuWidget::BuildLobbyHud()
{
	UCanvasPanel* Root = EnsureLobbyRootWidget();
	if (!Root)
	{
		UE_LOG(LogVNH, Warning, TEXT("LobbyMenuWidget: BuildLobbyHud skipped because root canvas could not be created."));
		return;
	}
	Root->ClearChildren();
	Root->SetVisibility(ESlateVisibility::Visible);

	UBorder* TopLeft = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyNamePanel"));
	TopLeft->SetBrushColor(PanelStrong);
	LobbyNameSlot = AddCanvas(Root, TopLeft, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(24.0f, 24.0f, 390.0f, 104.0f), FVector2D(0.0f, 0.0f), 10);

	UVerticalBox* HeaderStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LobbyNameStack"));
	TopLeft->SetContent(HeaderStack);
	HeaderStack->AddChildToVerticalBox(Text(HeaderStack, TEXT("HOST LOBBY"), 31, Accent));
	HeaderStack->AddChildToVerticalBox(Text(HeaderStack, TEXT("Start from the console when ready."), 17, White));
	LobbyNameText = Cast<UTextBlock>(HeaderStack->GetChildAt(0));
	LobbySubtitleText = Cast<UTextBlock>(HeaderStack->GetChildAt(1));

	UBorder* TopCenter = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyStatusPanel"));
	TopCenter->SetBrushColor(PanelStrong);
	LobbyStatusSlot = AddCanvas(Root, TopCenter, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(810.0f, 28.0f, 300.0f, 64.0f), FVector2D::ZeroVector, 10);
	LobbyStatusText = Text(TopCenter, TEXT("LOBBY OPEN"), 28, Accent, ETextJustify::Center);
	TopCenter->SetContent(LobbyStatusText.Get());

	UBorder* TopRight = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyStatsPanel"));
	TopRight->SetBrushColor(PanelStrong);
	LobbyStatsSlot = AddCanvas(Root, TopRight, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(1604.0f, 24.0f, 292.0f, 58.0f), FVector2D::ZeroVector, 10);

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
	PlayersPanelSlot = AddCanvas(Root, PlayersPanel, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(1596.0f, 375.0f, 300.0f, 330.0f), FVector2D::ZeroVector, 10);
	UVerticalBox* PlayersStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PlayersStack"));
	PlayersPanel->SetContent(PlayersStack);
	PlayersStack->AddChildToVerticalBox(Text(PlayersStack, TEXT("PLAYERS"), 22, Accent));
	PlayerRowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PlayerRows"));
	PlayersStack->AddChildToVerticalBox(PlayerRowsBox.Get());

	UHorizontalBox* ButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LobbyActionButtons"));
	ButtonRow->SetVisibility(ESlateVisibility::Visible);
	ActionButtonsSlot = AddCanvas(Root, ButtonRow, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(630.0f, 994.0f, 660.0f, 54.0f), FVector2D::ZeroVector, 20);
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

	UBorder* LobbyCodePanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyCodePanel"));
	LobbyCodePanel->SetBrushColor(PanelStrong);
	LobbyCodeSlot = AddCanvas(Root, LobbyCodePanel, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(24.0f, 902.0f, 310.0f, 118.0f), FVector2D::ZeroVector, 20);
	UVerticalBox* LobbyCodeStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LobbyCodeStack"));
	LobbyCodePanel->SetContent(LobbyCodeStack);
	LobbyCodeStack->AddChildToVerticalBox(Text(LobbyCodeStack, TEXT("LOBBY CODE"), 18, Accent));
	LobbyCodeText = Text(LobbyCodeStack, TEXT("LOCAL"), 36, Accent);
	LobbyCodeStack->AddChildToVerticalBox(LobbyCodeText.Get());

	UBorder* StartPromptPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("LobbyStartPromptPanel"));
	StartPromptPanel->SetBrushColor(FLinearColor(0.005f, 0.035f, 0.04f, 0.92f));
	StartPromptPanel->SetVisibility(ESlateVisibility::Collapsed);
	LobbyStartPromptSlot = AddCanvas(Root, StartPromptPanel, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FMargin(0.0f, 0.0f, 440.0f, 72.0f), FVector2D::ZeroVector, 30);
	LobbyStartPromptPanel = StartPromptPanel;

	UHorizontalBox* StartPromptRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("LobbyStartPromptRow"));
	StartPromptPanel->SetContent(StartPromptRow);
	LobbyStartProgressCircle = WidgetTree->ConstructWidget<UCircularThrobber>(UCircularThrobber::StaticClass(), TEXT("LobbyStartProgressCircle"));
	LobbyStartProgressCircle->SetNumberOfPieces(18);
	LobbyStartProgressCircle->SetRadius(19.0f);
	LobbyStartProgressCircle->SetPeriod(0.85f);
	UHorizontalBoxSlot* CircleSlot = StartPromptRow->AddChildToHorizontalBox(LobbyStartProgressCircle.Get());
	CircleSlot->SetPadding(FMargin(18.0f, 0.0f, 14.0f, 0.0f));
	CircleSlot->SetVerticalAlignment(VAlign_Center);
	UVerticalBox* StartPromptTextStack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LobbyStartPromptTextStack"));
	StartPromptTextStack->AddChildToVerticalBox(Text(StartPromptTextStack, TEXT("START GAME"), 24, Accent));
	LobbyStartPromptText = Text(StartPromptTextStack, TEXT("HOLD E  0%"), 18, White);
	StartPromptTextStack->AddChildToVerticalBox(LobbyStartPromptText.Get());
	StartPromptRow->AddChildToHorizontalBox(StartPromptTextStack)->SetVerticalAlignment(VAlign_Center);

	BuildInviteDialog();
	const FVector2D InitialViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
	if (InitialViewportSize.X > 0.0f && InitialViewportSize.Y > 0.0f)
	{
		ApplyResponsiveLobbyLayout(InitialViewportSize);
	}
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
	FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
	if (ViewportSize.X <= 0.0f || ViewportSize.Y <= 0.0f)
	{
		ViewportSize = MyGeometry.GetLocalSize();
	}
	ApplyResponsiveLobbyLayout(ViewportSize);
}

void UVNHLobbyMenuWidget::ApplyResponsiveLobbyLayout(const FVector2D& LocalSize)
{
	if (LocalSize.X <= 0.0f || LocalSize.Y <= 0.0f)
	{
		return;
	}

	const FVector2D ViewportSize(FMath::Max(1.0f, LocalSize.X), FMath::Max(1.0f, LocalSize.Y));
	const float EdgePadding = ViewportSize.X < 1280.0f ? 18.0f : 24.0f;
	const float AvailableWidth = FMath::Max(1.0f, ViewportSize.X - (EdgePadding * 2.0f));
	const auto ConfigureSlot = [](UCanvasPanelSlot* CanvasPanelSlot, const FAnchors& Anchors, const FVector2D& Alignment, const FVector2D& Position, const FVector2D& Size)
	{
		if (!CanvasPanelSlot)
		{
			return;
		}
		CanvasPanelSlot->SetAnchors(Anchors);
		CanvasPanelSlot->SetAlignment(Alignment);
		CanvasPanelSlot->SetPosition(Position);
		CanvasPanelSlot->SetSize(Size);
	};

	const FVector2D NameSize(FMath::Min(430.0f, AvailableWidth), 112.0f);
	const FVector2D StatusSize(FMath::Min(320.0f, AvailableWidth), 64.0f);
	const FVector2D StatsSize(FMath::Min(340.0f, AvailableWidth), 64.0f);
	const FVector2D PlayerPanelSize(FMath::Min(350.0f, AvailableWidth), 385.0f);
	const FVector2D CodeSize(FMath::Min(310.0f, AvailableWidth), 118.0f);
	const FVector2D ActionSize(FMath::Min(660.0f, AvailableWidth), 54.0f);
	const FVector2D PromptSize(FMath::Min(360.0f, AvailableWidth), 74.0f);

	if (UCanvasPanelSlot* NameSlot = LobbyNameSlot.Get())
	{
		ConfigureSlot(NameSlot, FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FVector2D::ZeroVector, FVector2D(EdgePadding, EdgePadding), NameSize);
	}

	if (UCanvasPanelSlot* StatusSlot = LobbyStatusSlot.Get())
	{
		ConfigureSlot(StatusSlot, FAnchors(0.5f, 0.0f, 0.5f, 0.0f), FVector2D(0.5f, 0.0f), FVector2D(0.0f, EdgePadding + 4.0f), StatusSize);
	}

	if (UCanvasPanelSlot* StatsSlot = LobbyStatsSlot.Get())
	{
		ConfigureSlot(StatsSlot, FAnchors(1.0f, 0.0f, 1.0f, 0.0f), FVector2D(1.0f, 0.0f), FVector2D(-EdgePadding, EdgePadding), StatsSize);
	}

	if (UCanvasPanelSlot* PlayerSlot = PlayersPanelSlot.Get())
	{
		ConfigureSlot(PlayerSlot, FAnchors(1.0f, 0.0f, 1.0f, 0.0f), FVector2D(1.0f, 0.0f), FVector2D(-EdgePadding, 150.0f), PlayerPanelSize);
	}

	if (UCanvasPanelSlot* CodeSlot = LobbyCodeSlot.Get())
	{
		ConfigureSlot(CodeSlot, FAnchors(0.0f, 1.0f, 0.0f, 1.0f), FVector2D(0.0f, 1.0f), FVector2D(EdgePadding, -EdgePadding), CodeSize);
	}

	if (UCanvasPanelSlot* ActionSlot = ActionButtonsSlot.Get())
	{
		const FVector2D ActionPosition(0.0f, -32.0f);
		ConfigureSlot(ActionSlot, FAnchors(0.5f, 1.0f, 0.5f, 1.0f), FVector2D(0.5f, 1.0f), ActionPosition, ActionSize);
		CachedActionPosition = FVector2D((ViewportSize.X - ActionSize.X) * 0.5f, ViewportSize.Y - 32.0f - ActionSize.Y);
		CachedActionSize = ActionSize;
	}

	if (UCanvasPanelSlot* PromptSlot = LobbyStartPromptSlot.Get())
	{
		ConfigureSlot(PromptSlot, FAnchors(0.5f, 1.0f, 0.5f, 1.0f), FVector2D(0.5f, 1.0f), FVector2D(0.0f, -122.0f), PromptSize);
	}

	if (UCanvasPanelSlot* DialogSlot = InviteDialogSlot.Get())
	{
		const FVector2D DialogSize(FMath::Min(960.0f, AvailableWidth), FMath::Min(600.0f, FMath::Max(260.0f, ViewportSize.Y - (EdgePadding * 2.0f))));
		ConfigureSlot(DialogSlot, FAnchors(0.5f, 0.5f, 0.5f, 0.5f), FVector2D(0.5f, 0.5f), FVector2D::ZeroVector, DialogSize);
	}
}

void UVNHLobbyMenuWidget::UpdateLobbyStartPrompt()
{
	AVNHPlayerController* VNHPlayerController = Cast<AVNHPlayerController>(GetOwningPlayer());
	const bool bShowPrompt = VNHPlayerController && VNHPlayerController->IsLobbyStartFocused();
	if (UBorder* PromptPanel = LobbyStartPromptPanel.Get())
	{
		PromptPanel->SetVisibility(bShowPrompt ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	if (!bShowPrompt)
	{
		return;
	}

	if (UCanvasPanelSlot* PromptSlot = LobbyStartPromptSlot.Get())
	{
		FVector2D ScreenPosition = FVector2D::ZeroVector;
		if (VNHPlayerController->GetLobbyStartPromptScreenPosition(ScreenPosition))
		{
			FVector2D ViewportSize = UWidgetLayoutLibrary::GetViewportSize(this);
			if (ViewportSize.X <= 0.0f || ViewportSize.Y <= 0.0f)
			{
				int32 ViewportX = 0;
				int32 ViewportY = 0;
				VNHPlayerController->GetViewportSize(ViewportX, ViewportY);
				ViewportSize = FVector2D(FMath::Max(1, ViewportX), FMath::Max(1, ViewportY));
			}
			const FVector2D PromptSize(360.0f, 74.0f);
			const float PromptPadding = 18.0f;
			const FVector2D DesiredPosition(ScreenPosition.X - (PromptSize.X * 0.5f), ScreenPosition.Y - PromptSize.Y - 22.0f);
			PromptSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 0.0f));
			PromptSlot->SetAlignment(FVector2D::ZeroVector);
			PromptSlot->SetPosition(FVector2D(
				FMath::Clamp(DesiredPosition.X, PromptPadding, FMath::Max(PromptPadding, ViewportSize.X - PromptSize.X - PromptPadding)),
				FMath::Clamp(DesiredPosition.Y, PromptPadding, FMath::Max(PromptPadding, ViewportSize.Y - PromptSize.Y - PromptPadding))));
			PromptSlot->SetSize(PromptSize);
		}
	}

	const float Progress = VNHPlayerController->GetLobbyStartHoldProgress();
	if (UTextBlock* PromptText = LobbyStartPromptText.Get())
	{
		PromptText->SetText(FText::FromString(FString::Printf(TEXT("HOLD E  %.0f%%"), Progress * 100.0f)));
	}
	if (UCircularThrobber* ProgressCircle = LobbyStartProgressCircle.Get())
	{
		ProgressCircle->SetPeriod(FMath::Lerp(0.85f, 0.18f, Progress));
		ProgressCircle->SetRenderOpacity(FMath::Lerp(0.45f, 1.0f, Progress));
	}
}

void UVNHLobbyMenuWidget::RefreshLobbyLabels()
{
	const UWorld* World = GetWorld();
	const FString UrlOptions = World ? World->URL.ToString() : FString();
	const FString ServerName = UGameplayStatics::ParseOption(UrlOptions, TEXT("ServerName"));
	if (UTextBlock* TextBlock = LobbyNameText.Get())
	{
		TextBlock->SetText(NSLOCTEXT("VNH", "TwoToTangleLobbyTitle", "LOBBY"));
	}
	if (UTextBlock* TextBlock = LobbySubtitleText.Get())
	{
		TextBlock->SetText(FText::FromString(ServerName.IsEmpty() ? TEXT("TWO TO TANGLE") : ServerName.ToUpper()));
	}
	if (UTextBlock* TextBlock = LobbyStatusText.Get())
	{
		TextBlock->SetText(NSLOCTEXT("VNH", "LobbyWaitingForHost", "Waiting for host to start"));
	}
	if (UTextBlock* TextBlock = LobbyCodeText.Get())
	{
		TextBlock->SetText(FText::FromString(ServerName.IsEmpty() ? TEXT("LOCAL") : ServerName.ToUpper()));
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
		TextBlock->SetText(FText::FromString(FString::Printf(TEXT("%d / %d PLAYERS"), PlayerCount, CachedMaxPlayers)));
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

	RefreshMatchSetupPresentation();
	RefreshTeamPresentation();
	RefreshHostJoinedPresentation();
}

void UVNHLobbyMenuWidget::RefreshPlayers()
{
	UVerticalBox* Rows = PlayerRowsBox.Get();
	if (!Rows)
	{
		return;
	}
	const AGameStateBase* LobbyState = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	const APlayerState* LocalPlayerState = GetOwningPlayer() ? GetOwningPlayer()->PlayerState : nullptr;
	const int32 PlayerArrayCount = LobbyState ? LobbyState->PlayerArray.Num() : 0;
	FString PlayerRowsSignature = FString::Printf(TEXT("%d|"), PlayerArrayCount);
	for (int32 PlayerIndex = 0; PlayerIndex < PlayerArrayCount; ++PlayerIndex)
	{
		const APlayerState* CandidatePlayerState = LobbyState->PlayerArray[PlayerIndex];
		const AVNHPlayerState* CandidateLobbyState = Cast<AVNHPlayerState>(CandidatePlayerState);
		PlayerRowsSignature += FString::Printf(
			TEXT("%s|%s|%d;"),
			*GetPathNameSafe(CandidatePlayerState),
			CandidatePlayerState ? *CandidatePlayerState->GetPlayerName() : TEXT("None"),
			CandidateLobbyState ? CandidateLobbyState->GetLobbyTeamId() : INDEX_NONE);
	}
	FString& PreviousPlayerRowsSignature = VNHLobbyPlayerRowSignatures.FindOrAdd(this);
	if (PlayerRowsSignature == PreviousPlayerRowsSignature)
	{
		return;
	}
	PreviousPlayerRowsSignature = MoveTemp(PlayerRowsSignature);
	Rows->ClearChildren();

	for (int32 PlayerIndex = 0; PlayerIndex < PlayerArrayCount; ++PlayerIndex)
	{
		const APlayerState* CandidatePlayerState = LobbyState->PlayerArray[PlayerIndex];
		if (!CandidatePlayerState)
		{
			continue;
		}

		const AVNHPlayerState* CandidateLobbyState = Cast<AVNHPlayerState>(CandidatePlayerState);
		const int32 TeamId = CandidateLobbyState ? CandidateLobbyState->GetLobbyTeamId() : INDEX_NONE;
		const FLinearColor TeamColor = TeamId == 1 ? LobbyBlue : (TeamId == 2 ? LobbyGreen : LobbyOrange);
		const bool bIsHostPlayer = PlayerIndex == 0;

		FString DisplayName = CandidatePlayerState->GetPlayerName().TrimStartAndEnd();
		if (DisplayName.IsEmpty() || (CandidatePlayerState == LocalPlayerState && DisplayName.StartsWith(TEXT("Player"))))
		{
			DisplayName = TEXT("Desch");
		}

		UBorder* RowPanel = NewObject<UBorder>(Rows);
		RowPanel->SetBrushColor(FLinearColor(0.015f, 0.055f, 0.065f, 0.96f));
		RowPanel->SetPadding(FMargin(10.0f, 8.0f));
		UHorizontalBox* Row = NewObject<UHorizontalBox>(RowPanel);
		RowPanel->AddChild(Row);

		USizeBox* PortraitSize = NewObject<USizeBox>(Row);
		PortraitSize->SetWidthOverride(52.0f);
		PortraitSize->SetHeightOverride(52.0f);
		UImage* PortraitImage = NewObject<UImage>(PortraitSize);
		PortraitImage->SetColorAndOpacity(FLinearColor(0.92f, 0.94f, 0.90f, 1.0f));
		const FUniqueNetIdRepl CandidateUniqueId = CandidatePlayerState->GetUniqueId();
		if (CandidateUniqueId.IsValid())
		{
			FBPUniqueNetId BlueprintUniqueId;
			BlueprintUniqueId.SetUniqueNetId(CandidateUniqueId.GetUniqueNetId());
			RequestSteamFriendInfoReflective(BlueprintUniqueId);
			if (UTexture2D* SteamAvatar = GetSteamFriendAvatarReflective(BlueprintUniqueId))
			{
				PortraitImage->SetBrushFromTexture(SteamAvatar, true);
			}
		}
		PortraitSize->AddChild(PortraitImage);
		UHorizontalBoxSlot* PortraitSlot = Row->AddChildToHorizontalBox(PortraitSize);
		PortraitSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
		PortraitSlot->SetVerticalAlignment(VAlign_Center);

		UTextBlock* TeamCircle = Text(Row, TEXT("●"), 25, TeamColor, ETextJustify::Center);
		UHorizontalBoxSlot* TeamCircleSlot = Row->AddChildToHorizontalBox(TeamCircle);
		TeamCircleSlot->SetPadding(FMargin(0.0f, 0.0f, 9.0f, 0.0f));
		TeamCircleSlot->SetVerticalAlignment(VAlign_Center);

		UTextBlock* PlayerNameText = Text(Row, DisplayName, 21, White);
		UHorizontalBoxSlot* PlayerNameSlot = Row->AddChildToHorizontalBox(PlayerNameText);
		PlayerNameSlot->SetSize(FillSize());
		PlayerNameSlot->SetVerticalAlignment(VAlign_Center);

		if (bIsHostPlayer)
		{
			UTextBlock* HostIcon = Text(Row, TEXT("HOST"), 16, FLinearColor(1.0f, 0.78f, 0.08f, 1.0f), ETextJustify::Center);
			UHorizontalBoxSlot* HostSlot = Row->AddChildToHorizontalBox(HostIcon);
			HostSlot->SetPadding(FMargin(10.0f, 0.0f, 0.0f, 0.0f));
			HostSlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* RowSlot = Rows->AddChildToVerticalBox(RowPanel);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 7.0f));
	}

	UButton* InvitePlayerButton = MakeTextButton(Rows, TEXT("+  INVITE PLAYER"), 19);
	InvitePlayerButton->OnClicked.AddDynamic(this, &UVNHLobbyMenuWidget::HandleInviteClicked);
	UVerticalBoxSlot* InviteSlot = Rows->AddChildToVerticalBox(InvitePlayerButton);
	InviteSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));
}

void UVNHLobbyMenuWidget::RefreshMatchSetupPresentation()
{
	const AVNHGameState* LobbyGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	const EPPGameMode CurrentMode = LobbyGameState ? LobbyGameState->GetLobbyGameMode() : EPPGameMode::BringYourIdiotHome;
	const EPPLobbyCourseType CurrentCourse = LobbyGameState ? LobbyGameState->GetLobbyCourseType() : EPPLobbyCourseType::PresetMaps;
	const EPPPresetMap CurrentMap = LobbyGameState ? LobbyGameState->GetLobbyPresetMap() : EPPPresetMap::FactoryFiasco;

	if (UTextBlock* ModeTextBlock = ModeValueText.Get())
	{
		ModeTextBlock->SetText(FText::FromString(LobbyModeText(CurrentMode)));
	}
	if (UTextBlock* SetupTextBlock = SetupValueText.Get())
	{
		const FString SetupLabel = CurrentCourse == EPPLobbyCourseType::PresetMaps
			? FString::Printf(TEXT("%s - %s"), *LobbyCourseText(CurrentCourse), *LobbyPresetMapText(CurrentMap))
			: LobbyCourseText(CurrentCourse);
		SetupTextBlock->SetText(FText::FromString(SetupLabel));
	}
}

void UVNHLobbyMenuWidget::RefreshTeamPresentation()
{
	const AVNHGameState* LobbyGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
	const AVNHPlayerState* LocalLobbyState = GetOwningPlayer() ? GetOwningPlayer()->GetPlayerState<AVNHPlayerState>() : nullptr;
	int32 TeamCounts[3] = {0, 0, 0};
	if (LobbyGameState)
	{
		for (const APlayerState* CandidateState : LobbyGameState->PlayerArray)
		{
			const AVNHPlayerState* CandidateLobbyState = Cast<AVNHPlayerState>(CandidateState);
			if (CandidateLobbyState && CandidateLobbyState->GetLobbyTeamId() >= 0 && CandidateLobbyState->GetLobbyTeamId() < 3)
			{
				++TeamCounts[CandidateLobbyState->GetLobbyTeamId()];
			}
		}
	}

	const int32 LocalTeamId = LocalLobbyState ? LocalLobbyState->GetLobbyTeamId() : INDEX_NONE;
	const int32 PlayerTotal = LobbyGameState ? LobbyGameState->PlayerArray.Num() : 1;
	const int32 AvailableTeamCount = FMath::Clamp((PlayerTotal + 1) / 2, 2, 3);
	const auto FormatTeamCount = [](int32 Count)
	{
		return FText::FromString(FString::Printf(TEXT("%d PLAYER%s"), Count, Count == 1 ? TEXT("") : TEXT("S")));
	};

	if (UButton* OrangeButtonWidget = OrangeTeamButton.Get())
	{
		OrangeButtonWidget->SetStyle(TeamButtonStyle(LobbyOrange, LocalTeamId == 0));
		OrangeButtonWidget->SetIsEnabled(LocalTeamId == 0 || TeamCounts[0] < 2);
	}
	if (UButton* BlueButtonWidget = BlueTeamButton.Get())
	{
		BlueButtonWidget->SetStyle(TeamButtonStyle(LobbyBlue, LocalTeamId == 1));
		BlueButtonWidget->SetIsEnabled(LocalTeamId == 1 || TeamCounts[1] < 2);
	}
	if (UButton* GreenButtonWidget = GreenTeamButton.Get())
	{
		GreenButtonWidget->SetVisibility(AvailableTeamCount >= 3 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		GreenButtonWidget->SetStyle(TeamButtonStyle(LobbyGreen, LocalTeamId == 2));
		GreenButtonWidget->SetIsEnabled(LocalTeamId == 2 || TeamCounts[2] < 2);
	}
	if (UTextBlock* CountText = OrangeTeamCountText.Get()) CountText->SetText(FormatTeamCount(TeamCounts[0]));
	if (UTextBlock* CountText = BlueTeamCountText.Get()) CountText->SetText(FormatTeamCount(TeamCounts[1]));
	if (UTextBlock* CountText = GreenTeamCountText.Get()) CountText->SetText(FormatTeamCount(TeamCounts[2]));
}

void UVNHLobbyMenuWidget::RefreshHostJoinedPresentation()
{
	AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer());
	const bool bHost = LobbyController && LobbyController->IsLocalLobbyHost();
	const AVNHPlayerState* LocalLobbyState = LobbyController ? LobbyController->GetPlayerState<AVNHPlayerState>() : nullptr;
	const bool bReady = LocalLobbyState && LocalLobbyState->IsPreRoundReady();

	if (UButton* SetupButtonWidget = MatchSetupButton.Get()) SetupButtonWidget->SetIsEnabled(bHost);
	if (UTextBlock* SetupButtonText = MatchSetupButtonLabel.Get())
	{
		SetupButtonText->SetText(bHost
			? NSLOCTEXT("VNH", "LobbyMatchSetupButton", "MATCH SETUP")
			: NSLOCTEXT("VNH", "LobbyHostControlsSetupButton", "HOST CONTROLS SETUP"));
	}
	if (UButton* ActionButtonWidget = PrimaryActionButton.Get())
	{
		ActionButtonWidget->SetIsEnabled(!bHost && !bReady);
	}
	if (UTextBlock* ActionTitle = PrimaryActionTitleText.Get())
	{
		ActionTitle->SetText(bHost
			? NSLOCTEXT("VNH", "LobbyStartGameTitle", "START GAME")
			: (bReady ? NSLOCTEXT("VNH", "LobbyReadyTitle", "READY") : NSLOCTEXT("VNH", "LobbyReadyUpTitle", "READY UP")));
	}
	if (UTextBlock* ActionSubtitle = PrimaryActionSubtitleText.Get())
	{
		ActionSubtitle->SetText(bHost
			? NSLOCTEXT("VNH", "LobbyHostStartHint", "Use the start console once everyone is ready")
			: (bReady ? NSLOCTEXT("VNH", "LobbyReadyWaitingHint", "Waiting for the host to start") : NSLOCTEXT("VNH", "LobbyReadyClickHint", "Click to ready up")));
	}
}

void UVNHLobbyMenuWidget::RefreshMatchSetupDialogSelection()
{
	if (UButton* SelectionButton = ModeTogetherButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingGameMode == EPPGameMode::BringYourIdiotHome));
	if (UButton* SelectionButton = ModeAttachedButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingGameMode == EPPGameMode::AttachedAtTheHip));
	if (UButton* SelectionButton = CoursePresetButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingCourseType == EPPLobbyCourseType::PresetMaps));
	if (UButton* SelectionButton = CourseBuildButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingCourseType == EPPLobbyCourseType::BuildForOtherTeam));
	if (UButton* SelectionButton = CourseObstacleButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingCourseType == EPPLobbyCourseType::OneCustomObstacle));
	if (UButton* SelectionButton = MapFactoryButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingPresetMap == EPPPresetMap::FactoryFiasco));
	if (UButton* SelectionButton = MapSkyButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingPresetMap == EPPPresetMap::SkyScramble));
	if (UButton* SelectionButton = MapJungleButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingPresetMap == EPPPresetMap::JungleJam));
	if (UButton* SelectionButton = MapPipeButton.Get()) SelectionButton->SetStyle(TeamButtonStyle(LobbyOrange, PendingPresetMap == EPPPresetMap::PipePanic));
	if (UWidget* PresetSectionWidget = PresetMapsSection.Get())
	{
		PresetSectionWidget->SetVisibility(PendingCourseType == EPPLobbyCourseType::PresetMaps ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UVNHLobbyMenuWidget::SetMatchSetupDialogVisible(bool bVisible)
{
	AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer());
	if (bVisible && (!LobbyController || !LobbyController->IsLocalLobbyHost()))
	{
		return;
	}
	if (bVisible)
	{
		const AVNHGameState* LobbyGameState = GetWorld() ? GetWorld()->GetGameState<AVNHGameState>() : nullptr;
		PendingGameMode = LobbyGameState ? LobbyGameState->GetLobbyGameMode() : EPPGameMode::BringYourIdiotHome;
		PendingCourseType = LobbyGameState ? LobbyGameState->GetLobbyCourseType() : EPPLobbyCourseType::PresetMaps;
		PendingPresetMap = LobbyGameState ? LobbyGameState->GetLobbyPresetMap() : EPPPresetMap::FactoryFiasco;
		RefreshMatchSetupDialogSelection();
	}
	if (UBorder* SetupDialogWidget = MatchSetupDialog.Get())
	{
		SetupDialogWidget->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void UVNHLobbyMenuWidget::ApplyLobbyHudVisibility()
{
	if (UCanvasPanel* HudLayerWidget = LobbyHudLayer.Get())
	{
		HudLayerWidget->SetVisibility(bLobbyHudHidden ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
	if (UTextBlock* HiddenHintText = HudHiddenHint.Get())
	{
		HiddenHintText->SetVisibility(bLobbyHudHidden ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	if (bLobbyHudHidden)
	{
		SetInviteDialogVisible(false);
		SetMatchSetupDialogVisible(false);
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

void UVNHLobbyMenuWidget::SetStatus(const FText& NewStatusText)
{
	if (UTextBlock* TextBlock = InviteStatusText.Get())
	{
		TextBlock->SetText(NewStatusText);
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

void UVNHLobbyMenuWidget::HandleMatchSetupClicked()
{
	SetMatchSetupDialogVisible(true);
}

void UVNHLobbyMenuWidget::HandleCloseMatchSetupClicked()
{
	SetMatchSetupDialogVisible(false);
}

void UVNHLobbyMenuWidget::HandleSaveMatchSetupClicked()
{
	if (AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer()))
	{
		LobbyController->RequestLobbyMatchSetup(PendingGameMode, PendingCourseType, PendingPresetMap);
	}
	SetMatchSetupDialogVisible(false);
}

void UVNHLobbyMenuWidget::HandleModeTogetherClicked()
{
	PendingGameMode = EPPGameMode::BringYourIdiotHome;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleModeAttachedClicked()
{
	PendingGameMode = EPPGameMode::AttachedAtTheHip;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleCoursePresetClicked()
{
	PendingCourseType = EPPLobbyCourseType::PresetMaps;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleCourseBuildClicked()
{
	PendingCourseType = EPPLobbyCourseType::BuildForOtherTeam;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleCourseObstacleClicked()
{
	PendingCourseType = EPPLobbyCourseType::OneCustomObstacle;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleMapFactoryClicked()
{
	PendingPresetMap = EPPPresetMap::FactoryFiasco;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleMapSkyClicked()
{
	PendingPresetMap = EPPPresetMap::SkyScramble;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleMapJungleClicked()
{
	PendingPresetMap = EPPPresetMap::JungleJam;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleMapPipeClicked()
{
	PendingPresetMap = EPPPresetMap::PipePanic;
	RefreshMatchSetupDialogSelection();
}

void UVNHLobbyMenuWidget::HandleOrangeTeamClicked()
{
	if (AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer())) LobbyController->RequestLobbyTeam(0);
}

void UVNHLobbyMenuWidget::HandleBlueTeamClicked()
{
	if (AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer())) LobbyController->RequestLobbyTeam(1);
}

void UVNHLobbyMenuWidget::HandleGreenTeamClicked()
{
	if (AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer())) LobbyController->RequestLobbyTeam(2);
}

void UVNHLobbyMenuWidget::HandleMascotClicked()
{
	if (AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer()))
	{
		LobbyController->ClientReceiveInteractionText(TEXT("Choose Your Mascot is coming in the next lobby phase."));
	}
}

void UVNHLobbyMenuWidget::HandleHideHudClicked()
{
	bLobbyHudHidden = !bLobbyHudHidden;
	ApplyLobbyHudVisibility();
}

void UVNHLobbyMenuWidget::HandlePrimaryActionClicked()
{
	AVNHPlayerController* LobbyController = Cast<AVNHPlayerController>(GetOwningPlayer());
	if (LobbyController && !LobbyController->IsLocalLobbyHost())
	{
		LobbyController->RequestPreRoundCustomizationReady();
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
