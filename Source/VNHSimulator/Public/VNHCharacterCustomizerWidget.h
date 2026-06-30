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
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void Rebuild();
	bool BindDesignerWidgets();
	void BindDesignerEvents();
	void ApplyModeText();
	void AddCategoryButton(UVerticalBox* Parent, EVNHCustomizationSlot CustomizationSlot, const FText& Label);
	void AddPresetButton(UVerticalBox* Parent, int32 PresetIndex);
	void AddOptionButton(UUniformGridPanel* Parent, const FText& Label, int32 Direction, int32 Column, int32 Row);
	void RefreshLabels();
	void RefreshLobbyCountdown();
	void ApplyAndPreview(int32 Direction = 1);
	UVNHGameInstance* GetVNHGameInstance() const;

	UFUNCTION()
	void HandleBackClicked();

	UFUNCTION()
	void HandleReadyClicked();

	UFUNCTION()
	void HandleBackReadyClicked();

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
	void HandlePantsClicked();

	UFUNCTION()
	void HandleShoesClicked();

	UFUNCTION()
	void HandleAccessoryClicked();

	UFUNCTION()
	void HandlePresetOneClicked();

	UFUNCTION()
	void HandlePresetTwoClicked();

	UFUNCTION()
	void HandlePresetThreeClicked();

	bool bLobbyMode = false;
	bool bUsingDesignerWidget = false;
	EVNHCustomizationSlot ActiveSlot = EVNHCustomizationSlot::Body;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PreviewText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetOneButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetTwoButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetThreeButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> RandomButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> BodyButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> HairButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> FaceButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> HatButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> MustacheButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> OutfitButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> OutwearButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PantsButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> ShoesButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> AccessoryButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PreviousButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> NextButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> BackReadyButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> CloseButton;
};
