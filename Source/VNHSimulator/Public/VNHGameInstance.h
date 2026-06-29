#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "VNHGameplayTypes.h"
#include "VNHGameInstance.generated.h"

class UVNHMainMenuWidget;
class UVNHCharacterCustomizerWidget;
class UUserWidget;
class UEditableTextBox;
class UTextBlock;

UCLASS()
class VNHSIMULATOR_API UVNHGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

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

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void ShowCharacterCustomizer(bool bLobbyMode = false);

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void HideCharacterCustomizer();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void SelectCharacterPreset(int32 PresetIndex);

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void CycleCustomizationSlot(EVNHCustomizationSlot CustomizationSlot, int32 Direction);

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void RandomizeActiveCustomization();

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	FVNHCharacterCustomization GetActiveCustomization();

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	FString GetActiveCustomizationSummary();

	void PreviewActiveCustomizationOnLocalPawn();

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

	UPROPERTY(Transient)
	TObjectPtr<UVNHCharacterCustomizerWidget> ActiveCustomizer;

	UPROPERTY(Transient)
	TObjectPtr<class UVNHCharacterProfileSave> CharacterProfile;

	void HostGame(bool bPublic);
	void BindMainMenuButtons(UUserWidget* MainMenuWidget);
	void SetMainMenuStatus(const FText& StatusText);
	FString GetJoinAddressFromMenu() const;
	void EnsureCharacterProfileLoaded();
	void SaveCharacterProfile();
	FVNHCharacterCustomization MakeDefaultCustomization(int32 PresetIndex) const;

	UFUNCTION()
	void HandleMenuHostPrivateClicked();

	UFUNCTION()
	void HandleMenuHostPublicClicked();

	UFUNCTION()
	void HandleMenuJoinClicked();

	UFUNCTION()
	void HandleMenuQuitClicked();

	UFUNCTION()
	void HandleMenuCustomizerClicked();
};
