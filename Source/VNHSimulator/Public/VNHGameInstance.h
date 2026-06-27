#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "VNHGameInstance.generated.h"

class UVNHMainMenuWidget;
class UUserWidget;
class UEditableTextBox;
class UTextBlock;

UCLASS()
class VNHSIMULATOR_API UVNHGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Menu")
	void ShowMainMenu();

	UFUNCTION(BlueprintCallable, Category = "VNH|Menu")
	void HideMainMenu();

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void HostPrivateGame();

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void HostPublicGame();

	UFUNCTION(BlueprintCallable, Category = "VNH|Lobby")
	void JoinGameByAddress(const FString& Address);

	UFUNCTION(BlueprintPure, Category = "VNH|Lobby")
	FName GetLobbyMapName() const { return LobbyMapName; }

	UFUNCTION(BlueprintPure, Category = "VNH|Menu")
	FString GetDefaultJoinAddress() const { return DefaultJoinAddress; }

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Menu")
	TSubclassOf<UUserWidget> MainMenuWidgetClass;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Maps")
	FName MainMenuMapName = TEXT("MainMenu");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Maps")
	FName LobbyMapName = TEXT("Lobby");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Maps")
	FName StoreMapName = TEXT("MVP_ClothingStore");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Lobby")
	FString DefaultJoinAddress = TEXT("127.0.0.1");

private:
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> ActiveMainMenu;

	void HostGame(bool bPublic);
	void BindMainMenuButtons(UUserWidget* MainMenuWidget);
	void SetMainMenuStatus(const FText& StatusText);
	FString GetJoinAddressFromMenu() const;

	UFUNCTION()
	void HandleMenuHostPrivateClicked();

	UFUNCTION()
	void HandleMenuHostPublicClicked();

	UFUNCTION()
	void HandleMenuJoinClicked();

	UFUNCTION()
	void HandleMenuQuitClicked();
};
