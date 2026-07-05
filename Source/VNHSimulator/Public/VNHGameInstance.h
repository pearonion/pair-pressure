#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "VNHGameplayTypes.h"
#include "VNHGameInstance.generated.h"

class UVNHMainMenuWidget;
class UVNHCharacterCustomizerWidget;
class UUserWidget;
class UDataTable;
class UEditableTextBox;
class UTextBlock;
class UTexture2D;

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

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	bool IsCharacterCustomizerOpen() const { return ActiveCustomizer != nullptr; }

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void SelectCharacterPreset(int32 PresetIndex);

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void SaveActiveCharacterPreset();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void LoadActiveCharacterPreset();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void ClearActiveCustomizationCosmetics();

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void CycleCustomizationSlot(EVNHCustomizationSlot CustomizationSlot, int32 Direction);

	UFUNCTION(BlueprintCallable, Category = "VNH|Customization")
	void SelectCustomizationSlotOption(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex);

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	int32 GetCustomizationSlotOptionCount(EVNHCustomizationSlot CustomizationSlot) const;

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	FString GetCustomizationSlotOptionLabel(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const;

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	TSoftObjectPtr<UTexture2D> GetCustomizationSlotOptionIcon(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const;

	UFUNCTION(BlueprintPure, Category = "VNH|Customization")
	bool IsCustomizationSlotOptionEmpty(EVNHCustomizationSlot CustomizationSlot, int32 OptionIndex) const;

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
	FName LobbyMapName = TEXT("/Game/Maps/Lobby");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Maps")
	FName StoreMapName = TEXT("MVP_ClothingStore");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Lobby")
	FString DefaultJoinAddress = TEXT("127.0.0.1");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VNH|Customization")
	TSoftObjectPtr<UDataTable> CustomizationItemsTable;

private:
	UPROPERTY(Transient, NonTransactional)
	TObjectPtr<UUserWidget> ActiveMainMenu;

	UPROPERTY(Transient, NonTransactional)
	TObjectPtr<UVNHCharacterCustomizerWidget> ActiveCustomizer;

	UPROPERTY(Transient, NonTransactional)
	TObjectPtr<class UVNHCharacterProfileSave> CharacterProfile;

	void HostGame(bool bPublic);
	void BindMainMenuButtons(UUserWidget* MainMenuWidget);
	void SetMainMenuStatus(const FText& StatusText);
	FString GetJoinAddressFromMenu() const;
	void EnsureCharacterProfileLoaded();
	void SaveCharacterProfile();
	FVNHCharacterCustomization MakeDefaultCustomization(int32 PresetIndex) const;
	UDataTable* GetCustomizationItemsTable() const;
	TArray<FVNHCustomizationItem> GetCustomizationItemsForSlot(EVNHCustomizationSlot CustomizationSlot) const;

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
