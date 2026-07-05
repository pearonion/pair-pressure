#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "VNHCreateServerWidget.generated.h"

class UButton;
class UCheckBox;
class UComboBoxString;
class UEditableTextBox;
class UTextBlock;

UCLASS()
class VNHSIMULATOR_API UVNHCreateServerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> ServerNameTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> PublicButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> PrivateButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> PasswordTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> PasswordVisibilityButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UEditableTextBox> MaxPlayersTextBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> DecreasePlayersButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> IncreasePlayersButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> RoundTimeComboBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CreateGameButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CancelButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CloseButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PublicButtonLabel;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PrivateButtonLabel;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Server")
	FName LobbyMapName = TEXT("/Game/Maps/Lobby");

private:
	UFUNCTION()
	void HandlePublicClicked();

	UFUNCTION()
	void HandlePrivateClicked();

	UFUNCTION()
	void HandlePasswordVisibilityClicked();

	UFUNCTION()
	void HandleDecreasePlayersClicked();

	UFUNCTION()
	void HandleIncreasePlayersClicked();

	UFUNCTION()
	void HandleMaxPlayersCommitted(const FText& Text, ETextCommit::Type CommitMethod);

	UFUNCTION()
	void HandleCreateGameClicked();

	UFUNCTION()
	void HandleCancelClicked();

	void HandleSessionCreated(FName SessionName, bool bWasSuccessful);
	void HandleExistingSessionDestroyed(FName SessionName, bool bWasSuccessful);

	void BeginCreateSession(const FOnlineSessionSettings& SessionSettings);
	void SetPrivateMode(bool bInPrivateMode);
	void SetPasswordVisible(bool bInPasswordVisible);
	int32 GetClampedMaxPlayers() const;
	int32 GetSelectedRoundSeconds() const;
	void SetMaxPlayers(int32 NewMaxPlayers);
	void SetStatus(const FText& NewStatus);
	FString BuildTravelOptions() const;

	IOnlineSessionPtr ActiveSessionInterface;
	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FOnlineSessionSettings PendingSessionSettings;
	bool bCreateSessionInFlight = false;
	bool bPrivateMode = false;
	bool bPasswordVisible = false;
};
