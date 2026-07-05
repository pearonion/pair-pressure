#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "VNHServerBrowserWidget.generated.h"

class UCheckBox;
class UComboBoxString;
class UEditableTextBox;
class UHorizontalBox;
class UBorder;
class UTextBlock;
class UVerticalBox;
class UWidget;
class UVNHServerBrowserWidget;

enum class EVNHServerBrowserSortKey : uint8
{
	ServerName,
	Players,
	Map,
	Region,
	Ping
};

struct FVNHServerBrowserEntry
{
	FString ServerName;
	FString MapName;
	FString Region;
	int32 CurrentPlayers = 0;
	int32 MaxPlayers = 0;
	int32 OpenSlots = 0;
	int32 Ping = 999;
	bool bPrivate = false;
	bool bExample = false;
	FOnlineSessionSearchResult SearchResult;
};

UCLASS()
class VNHSIMULATOR_API UVNHServerBrowserRowButton : public UButton
{
	GENERATED_BODY()

public:
	void InitializeRow(UVNHServerBrowserWidget* InOwner, int32 InEntryIndex);

private:
	UFUNCTION()
	void HandleClicked();

	TWeakObjectPtr<UVNHServerBrowserWidget> Owner;
	int32 EntryIndex = INDEX_NONE;
};

UCLASS()
class VNHSIMULATOR_API UVNHServerBrowserWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	void HandleRowClicked(int32 EntryIndex);

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> SearchTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> FilterButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> RefreshButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> PublicTabButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> PrivateTabButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PublicTabLabel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PrivateTabLabel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> SortServerNameButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> SortPlayersButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> SortMapButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> SortRegionButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> SortPingButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UHorizontalBox> HeaderRow;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SortServerNameButton_Label;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SortPlayersButton_Label;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> MapHeaderText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RegionHeaderText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SortPingButton_Label;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UVerticalBox> ServerRowsBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedServerNameText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedPlayersText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedMapText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelectedRegionText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> JoinNowButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CloseButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UWidget> FilterOverlay;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> FilterSearchTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_USEAST;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_USCENTRAL;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_USWEST;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_EUEAST;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_EUWEST;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_SOUTHAMERICA;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> Region_ASIA;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> MapFilterComboBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> MinPlayersComboBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> MinOpenSlotsComboBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> HideFullServersCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> ResetFiltersButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> ApplyFiltersButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CloseFiltersButton;

private:
	UFUNCTION()
	void HandleSearchChanged(const FText& Text);

	UFUNCTION()
	void HandleSearchCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleFilterSearchCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleFilterClicked();

	UFUNCTION()
	void HandleRefreshClicked();

	UFUNCTION()
	void HandlePublicTabClicked();

	UFUNCTION()
	void HandlePrivateTabClicked();

	UFUNCTION()
	void HandleSortServerNameClicked();

	UFUNCTION()
	void HandleSortPlayersClicked();

	UFUNCTION()
	void HandleSortMapClicked();

	UFUNCTION()
	void HandleSortRegionClicked();

	UFUNCTION()
	void HandleSortPingClicked();

	UFUNCTION()
	void HandleJoinNowClicked();

	UFUNCTION()
	void HandleCloseClicked();

	UFUNCTION()
	void HandleResetFiltersClicked();

	UFUNCTION()
	void HandleApplyFiltersClicked();

	UFUNCTION()
	void HandleCloseFiltersClicked();

	UFUNCTION()
	void HandlePasswordConfirmClicked();

	UFUNCTION()
	void HandlePasswordCancelClicked();

	void RefreshServerList();
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void PopulateExampleServers();
	void RebuildVisibleEntries();
	void PopulateRows();
	void SelectVisibleEntry(int32 EntryIndex);
	void ApplyRowSelectionStyles();
	void JoinSelectedServer();
	void ShowPasswordPromptForSelectedServer();
	void HidePasswordPrompt();
	void SetStatus(const FText& NewStatus);
	void SetFilterOverlayVisible(bool bVisible);
	void ConfigureFilterCombos();
	void ConfigureHeaderLayout();
	void UpdateTabLabels();
	void UpdateSortHeaderLabels();
	FButtonStyle MakeRowButtonStyle(bool bSelected) const;
	FButtonStyle MakeTabButtonStyle(bool bSelected) const;
	bool PassesFilters(const FVNHServerBrowserEntry& Entry) const;
	void ToggleSort(EVNHServerBrowserSortKey NewSortKey);
	TSet<FString> GetSelectedRegions() const;
	int32 GetComboNumber(const UComboBoxString* ComboBox) const;
	FText FormatPlayers(const FVNHServerBrowserEntry& Entry) const;

	IOnlineSessionPtr ActiveSessionInterface;
	TSharedPtr<FOnlineSessionSearch> ActiveSearch;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;

	TArray<FVNHServerBrowserEntry> AllEntries;
	TArray<FVNHServerBrowserEntry> VisibleEntries;
	TWeakObjectPtr<UBorder> PasswordPromptOverlay;
	TWeakObjectPtr<UEditableTextBox> PasswordPromptTextBox;
	FString AcceptedPrivateJoinPassword;
	int32 SelectedVisibleIndex = INDEX_NONE;
	int32 PendingPasswordJoinVisibleIndex = INDEX_NONE;
	bool bAcceptedPasswordForPendingJoin = false;
	int32 LastClickedVisibleIndex = INDEX_NONE;
	double LastClickTime = 0.0;
	bool bShowingPrivateTab = false;
	EVNHServerBrowserSortKey SortKey = EVNHServerBrowserSortKey::ServerName;
	bool bSortAscending = true;
};
