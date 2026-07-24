#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Interfaces/OnlineFriendsInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "PairPressure/PPGameplayTypes.h"
#include "TimerManager.h"
#include "VNHLobbyMenuWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UCanvasPanelSlot;
class UCircularThrobber;
class UEditableTextBox;
class UHorizontalBox;
class UImage;
class UOverlay;
class UScrollBox;
class UTextBlock;
class UTexture2D;
class UTextureRenderTarget2D;
class UUniformGridPanel;
class UVerticalBox;
class UVNHLobbyMenuWidget;
class APPMascotPreviewActor;

UCLASS()
class VNHSIMULATOR_API UVNHLobbyFriendInviteButton : public UButton
{
	GENERATED_BODY()

public:
	void Initialize(UVNHLobbyMenuWidget* InOwner, const FString& InFriendId);

private:
	UFUNCTION()
	void HandleClicked();

	UPROPERTY(Transient)
	TObjectPtr<UVNHLobbyMenuWidget> Owner;

	FString FriendId;
};

UCLASS()
class VNHSIMULATOR_API UPPMascotTileButton : public UButton
{
	GENERATED_BODY()

public:
	void Initialize(UVNHLobbyMenuWidget* InOwner, FName InMascotRowName);

private:
	UFUNCTION()
	void HandleClicked();

	UPROPERTY(Transient)
	TObjectPtr<UVNHLobbyMenuWidget> Owner;

	FName MascotRowName = NAME_None;
};

UCLASS()
class VNHSIMULATOR_API UVNHLobbyMenuWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	void InviteFriendById(const FString& FriendId);
	void SelectMascotPreview(FName MascotRowName);

private:
	struct FSteamFriendEntry
	{
		FString Id;
		FString DisplayName;
		FString StatusText;
		FLinearColor StatusColor = FLinearColor::Gray;
		bool bOnline = false;
		bool bInvited = false;
		TSharedPtr<const FUniqueNetId> UserId;
		TWeakObjectPtr<UTexture2D> Avatar;
	};

	UCanvasPanel* EnsureLobbyRootWidget();
	bool HasDesignerLobbyHud() const;
	bool BindDesignerLobbyHud();
	void BuildLobbyHud();
	void BuildInviteDialog();
	void UpdateResponsiveLayout(const FGeometry& MyGeometry);
	void ApplyResponsiveLobbyLayout(const FVector2D& LocalSize);
	void UpdateLobbyStartPrompt();
	void RefreshLobbyLabels();
	void RefreshPlayers();
	void RefreshMatchSetupPresentation();
	void RefreshTeamPresentation();
	void RefreshHostJoinedPresentation();
	void RefreshMatchSetupDialogSelection();
	void SetMatchSetupDialogVisible(bool bVisible);
	void ApplyLobbyHudVisibility();
	void RequestFriendsList();
	void HandleReadFriendsListComplete(int32 LocalUserNum, bool bWasSuccessful, const FString& ListName, const FString& ErrorString);
	void RebuildFriendsList();
	void SetInviteDialogVisible(bool bVisible);
	bool BindDesignerMascotDialog();
	void SetMascotDialogVisible(bool bVisible);
	void RebuildMascotGrid();
	void RefreshMascotSelectionPresentation();
	void FinishMascotConfirmation();
	void CompleteMascotConfirmation();
	void DestroyMascotPreviewActor();
	void SetStatus(const FText& NewStatusText);

	UFUNCTION()
	void HandleInviteClicked();

	UFUNCTION()
	void HandleCustomizeClicked();

	UFUNCTION()
	void HandleMascotBackClicked();

	UFUNCTION()
	void HandleMascotConfirmClicked();

	UFUNCTION()
	void HandleMatchSetupClicked();

	UFUNCTION()
	void HandleCloseMatchSetupClicked();

	UFUNCTION()
	void HandleSaveMatchSetupClicked();

	UFUNCTION()
	void HandleModeTogetherClicked();

	UFUNCTION()
	void HandleModeAttachedClicked();

	UFUNCTION()
	void HandleCoursePresetClicked();

	UFUNCTION()
	void HandleCourseBuildClicked();

	UFUNCTION()
	void HandleCourseObstacleClicked();

	UFUNCTION()
	void HandleMapFactoryClicked();

	UFUNCTION()
	void HandleMapSkyClicked();

	UFUNCTION()
	void HandleMapJungleClicked();

	UFUNCTION()
	void HandleMapPipeClicked();

	UFUNCTION()
	void HandleOrangeTeamClicked();

	UFUNCTION()
	void HandleBlueTeamClicked();

	UFUNCTION()
	void HandleGreenTeamClicked();

	UFUNCTION()
	void HandleMascotClicked();

	UFUNCTION()
	void HandleHideHudClicked();

	UFUNCTION()
	void HandlePrimaryActionClicked();

	UFUNCTION()
	void HandleCloseInviteClicked();

	UFUNCTION()
	void HandleSearchChanged(const FText& Text);

	TWeakObjectPtr<UTextBlock> LobbyNameText;
	TWeakObjectPtr<UTextBlock> LobbySubtitleText;
	TWeakObjectPtr<UTextBlock> LobbyStatusText;
	TWeakObjectPtr<UTextBlock> LobbyCodeText;
	TWeakObjectPtr<UTextBlock> PlayerCountText;
	TWeakObjectPtr<UTextBlock> PingText;
	TWeakObjectPtr<UCanvasPanel> LobbyHudLayer;
	TWeakObjectPtr<UTextBlock> HudHiddenHint;
	TWeakObjectPtr<UTextBlock> ModeValueText;
	TWeakObjectPtr<UTextBlock> SetupValueText;
	TWeakObjectPtr<UButton> MatchSetupButton;
	TWeakObjectPtr<UTextBlock> MatchSetupButtonLabel;
	TWeakObjectPtr<UButton> PrimaryActionButton;
	TWeakObjectPtr<UTextBlock> PrimaryActionTitleText;
	TWeakObjectPtr<UTextBlock> PrimaryActionSubtitleText;
	TWeakObjectPtr<UButton> OrangeTeamButton;
	TWeakObjectPtr<UButton> BlueTeamButton;
	TWeakObjectPtr<UButton> GreenTeamButton;
	TWeakObjectPtr<UTextBlock> OrangeTeamCountText;
	TWeakObjectPtr<UTextBlock> BlueTeamCountText;
	TWeakObjectPtr<UTextBlock> GreenTeamCountText;
	TWeakObjectPtr<UButton> MascotButton;
	TWeakObjectPtr<UBorder> MatchSetupDialog;
	TWeakObjectPtr<UButton> ModeTogetherButton;
	TWeakObjectPtr<UButton> ModeAttachedButton;
	TWeakObjectPtr<UButton> CoursePresetButton;
	TWeakObjectPtr<UButton> CourseBuildButton;
	TWeakObjectPtr<UButton> CourseObstacleButton;
	TWeakObjectPtr<UButton> MapFactoryButton;
	TWeakObjectPtr<UButton> MapSkyButton;
	TWeakObjectPtr<UButton> MapJungleButton;
	TWeakObjectPtr<UButton> MapPipeButton;
	TWeakObjectPtr<UWidget> PresetMapsSection;
	TWeakObjectPtr<UBorder> LobbyStartPromptPanel;
	TWeakObjectPtr<UTextBlock> LobbyStartPromptText;
	TWeakObjectPtr<UCircularThrobber> LobbyStartProgressCircle;
	TWeakObjectPtr<UHorizontalBox> PingBarsBox;
	TWeakObjectPtr<UVerticalBox> PlayerRowsBox;
	TWeakObjectPtr<UBorder> InviteDialog;
	TWeakObjectPtr<UCanvasPanelSlot> LobbyNameSlot;
	TWeakObjectPtr<UCanvasPanelSlot> LobbyStatusSlot;
	TWeakObjectPtr<UCanvasPanelSlot> LobbyCodeSlot;
	TWeakObjectPtr<UCanvasPanelSlot> LobbyStatsSlot;
	TWeakObjectPtr<UCanvasPanelSlot> PlayersPanelSlot;
	TWeakObjectPtr<UCanvasPanelSlot> ActionButtonsSlot;
	TWeakObjectPtr<UCanvasPanelSlot> LobbyStartPromptSlot;
	TWeakObjectPtr<UCanvasPanelSlot> InviteDialogSlot;
	TWeakObjectPtr<UEditableTextBox> SearchTextBox;
	TWeakObjectPtr<UScrollBox> FriendsScrollBox;
	TWeakObjectPtr<UTextBlock> InviteStatusText;
	TWeakObjectPtr<UBorder> MascotDialog;
	TWeakObjectPtr<UImage> MascotPreviewImage;
	TWeakObjectPtr<UTextBlock> MascotNameText;
	TWeakObjectPtr<UUniformGridPanel> MascotGrid;
	TWeakObjectPtr<UScrollBox> MascotGridScroll;
	TWeakObjectPtr<UButton> MascotBackButton;
	TWeakObjectPtr<UButton> MascotConfirmButton;
	TArray<TWeakObjectPtr<UTextBlock>> MascotCheckmarks;
	TArray<FName> MascotGridRowNames;

	UPROPERTY(Transient)
	TObjectPtr<APPMascotPreviewActor> MascotPreviewActor;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> MascotMainRenderTarget;

	IOnlineFriendsPtr ActiveFriendsInterface;
	IOnlineSessionPtr ActiveSessionInterface;
	TArray<FSteamFriendEntry> Friends;
	float RefreshAccumulator = 0.0f;
	int32 CachedMaxPlayers = 8;
	FVector2D CachedActionPosition = FVector2D::ZeroVector;
	FVector2D CachedActionSize = FVector2D::ZeroVector;
	bool bUsingDesignerLobbyHud = false;
	bool bLobbyHudHidden = false;
	bool bDraggingMascotPreview = false;
	bool bMascotConfirmationPending = false;
	FVector2D LastMascotDragScreenPosition = FVector2D::ZeroVector;
	FName CandidateMascotRowName = NAME_None;
	FName ConfirmedMascotRowName = NAME_None;
	FTimerHandle MascotConfirmTimerHandle;
	EPPGameMode PendingGameMode = EPPGameMode::BringYourIdiotHome;
	EPPLobbyCourseType PendingCourseType = EPPLobbyCourseType::PresetMaps;
	EPPPresetMap PendingPresetMap = EPPPresetMap::FactoryFiasco;
};
