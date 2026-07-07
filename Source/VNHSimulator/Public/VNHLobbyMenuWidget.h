#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Interfaces/OnlineFriendsInterface.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "VNHLobbyMenuWidget.generated.h"

class UBorder;
class UCanvasPanel;
class UCanvasPanelSlot;
class UCircularThrobber;
class UEditableTextBox;
class UHorizontalBox;
class UImage;
class UScrollBox;
class UTextBlock;
class UTexture2D;
class UVerticalBox;
class UVNHLobbyMenuWidget;

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

	void InviteFriendById(const FString& FriendId);

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
	void BuildLobbyHud();
	void BuildInviteDialog();
	void UpdateResponsiveLayout(const FGeometry& MyGeometry);
	void ApplyResponsiveLobbyLayout(const FVector2D& LocalSize);
	void UpdateLobbyStartPrompt();
	void RefreshLobbyLabels();
	void RefreshPlayers();
	void RequestFriendsList();
	void HandleReadFriendsListComplete(int32 LocalUserNum, bool bWasSuccessful, const FString& ListName, const FString& ErrorString);
	void RebuildFriendsList();
	void SetInviteDialogVisible(bool bVisible);
	void SetStatus(const FText& NewStatusText);

	UFUNCTION()
	void HandleInviteClicked();

	UFUNCTION()
	void HandleCustomizeClicked();

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

	IOnlineFriendsPtr ActiveFriendsInterface;
	IOnlineSessionPtr ActiveSessionInterface;
	TArray<FSteamFriendEntry> Friends;
	float RefreshAccumulator = 0.0f;
	int32 CachedMaxPlayers = 8;
	FVector2D CachedActionPosition = FVector2D::ZeroVector;
	FVector2D CachedActionSize = FVector2D::ZeroVector;
};
