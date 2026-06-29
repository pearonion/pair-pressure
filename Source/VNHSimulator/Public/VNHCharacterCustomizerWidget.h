#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VNHGameplayTypes.h"
#include "VNHCharacterCustomizerWidget.generated.h"

class UButton;
class UTextBlock;
class UUniformGridPanel;
class UVerticalBox;
class UVNHGameInstance;

UCLASS()
class VNHSIMULATOR_API UVNHCharacterCustomizerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetLobbyMode(bool bInLobbyMode);

protected:
	virtual void NativeConstruct() override;

private:
	void Rebuild();
	void AddCategoryButton(UVerticalBox* Parent, EVNHCustomizationSlot CustomizationSlot, const FText& Label);
	void AddPresetButton(UVerticalBox* Parent, int32 PresetIndex);
	void AddOptionButton(UUniformGridPanel* Parent, const FText& Label, int32 Direction, int32 Column, int32 Row);
	void RefreshLabels();
	void ApplyAndPreview(int32 Direction = 1);
	UVNHGameInstance* GetVNHGameInstance() const;

	UFUNCTION()
	void HandleBackClicked();

	UFUNCTION()
	void HandleReadyClicked();

	UFUNCTION()
	void HandleRandomClicked();

	UFUNCTION()
	void HandlePreviousClicked();

	UFUNCTION()
	void HandleNextClicked();

	UFUNCTION()
	void HandleBodyClicked();

	UFUNCTION()
	void HandleHairClicked();

	UFUNCTION()
	void HandleFaceClicked();

	UFUNCTION()
	void HandleHatClicked();

	UFUNCTION()
	void HandleMustacheClicked();

	UFUNCTION()
	void HandleOutfitClicked();

	UFUNCTION()
	void HandleOutwearClicked();

	UFUNCTION()
	void HandlePresetOneClicked();

	UFUNCTION()
	void HandlePresetTwoClicked();

	UFUNCTION()
	void HandlePresetThreeClicked();

	bool bLobbyMode = false;
	EVNHCustomizationSlot ActiveSlot = EVNHCustomizationSlot::Body;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PreviewText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusText;
};
