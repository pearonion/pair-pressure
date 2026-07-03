#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VNHGameplayTypes.h"
#include "VNHCharacterCustomizerWidget.generated.h"

class UButton;
class UCheckBox;
class UImage;
class UTextBlock;
class UUniformGridPanel;
class UVerticalBox;
class AVNHCustomizationPreviewActor;
class UVNHGameInstance;

UCLASS()
class VNHSIMULATOR_API UVNHCharacterCustomizerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetLobbyMode(bool bInLobbyMode);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void Rebuild();
	bool BindDesignerWidgets();
	void BindDesignerEvents();
	void ApplyModeText();
	void AddCategoryButton(UVerticalBox* Parent, EVNHCustomizationSlot CustomizationSlot, const FText& Label);
	void AddPresetButton(UVerticalBox* Parent, int32 PresetIndex);
	void AddOptionButton(UUniformGridPanel* Parent, const FText& Label, int32 Direction, int32 Column, int32 Row);
	void RefreshLabels();
	void RefreshPresetButtonStyles();
	void RefreshItemGrid();
	void RefreshLobbyCountdown();
	void EnsurePreviewActor();
	void DestroyPreviewActor();
	void RefreshPreviewActor();
	void BindPreviewRenderTarget();
	void EnsurePreviewAnimationToggle();
	void ApplyAndPreview(int32 Direction = 1);
	void EnsurePresetControlBar();
	void NormalizeItemGridSlots();
	void SelectCategory(EVNHCustomizationSlot NewActiveSlot);
	void HandleItemSlotClicked(int32 LocalSlotIndex);
	void PlayPreviewChangeAnimation(EVNHCustomizationSlot CustomizationSlot, bool bRemovingItem);
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
	void HandleItemSlot1Clicked();

	UFUNCTION()
	void HandleItemSlot2Clicked();

	UFUNCTION()
	void HandleItemSlot3Clicked();

	UFUNCTION()
	void HandleItemSlot4Clicked();

	UFUNCTION()
	void HandleItemSlot5Clicked();

	UFUNCTION()
	void HandleItemSlot6Clicked();

	UFUNCTION()
	void HandleItemSlot7Clicked();

	UFUNCTION()
	void HandleItemSlot8Clicked();

	UFUNCTION()
	void HandleItemSlot9Clicked();

	UFUNCTION()
	void HandleItemSlot10Clicked();

	UFUNCTION()
	void HandleItemSlot11Clicked();

	UFUNCTION()
	void HandleItemSlot12Clicked();

	UFUNCTION()
	void HandleItemSlot13Clicked();

	UFUNCTION()
	void HandleItemSlot14Clicked();

	UFUNCTION()
	void HandleItemSlot15Clicked();

	UFUNCTION()
	void HandleItemSlot16Clicked();

	UFUNCTION()
	void HandleItemSlot17Clicked();

	UFUNCTION()
	void HandleItemSlot18Clicked();

	UFUNCTION()
	void HandleItemSlot19Clicked();

	UFUNCTION()
	void HandleItemSlot20Clicked();

	UFUNCTION()
	void HandlePresetOneClicked();

	UFUNCTION()
	void HandlePresetTwoClicked();

	UFUNCTION()
	void HandlePresetThreeClicked();

	UFUNCTION()
	void HandleSavePresetClicked();

	UFUNCTION()
	void HandleLoadPresetClicked();

	UFUNCTION()
	void HandleBlankCanvasClicked();

	UFUNCTION()
	void HandlePreviewFemaleAnimationsChanged(bool bIsChecked);

	bool bLobbyMode = false;
	bool bUsingDesignerWidget = false;
	EVNHCustomizationSlot ActiveSlot = EVNHCustomizationSlot::Body;
	int32 ActivePage = 0;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PreviewText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusText;

	UPROPERTY(Transient, meta = (BindWidgetOptional))
	TObjectPtr<UImage> PreviewRenderImage;

	UPROPERTY(Transient)
	TObjectPtr<UCheckBox> PreviewFemaleAnimationsCheckBox;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PreviewFemaleAnimationsLabel;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetOneButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetTwoButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> PresetThreeButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> SavePresetButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> LoadPresetButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> BlankCanvasButton;

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

	UPROPERTY(Transient)
	TArray<TObjectPtr<UButton>> ItemSlotButtons;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> ItemSlotImages;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> ItemSlotLabels;

	UPROPERTY(Transient)
	TObjectPtr<AVNHCustomizationPreviewActor> PreviewActor;

	bool bDraggingPreview = false;
	bool bUseFemalePreviewAnimations = false;
	FVector2D LastPreviewDragScreenPosition = FVector2D::ZeroVector;
};
