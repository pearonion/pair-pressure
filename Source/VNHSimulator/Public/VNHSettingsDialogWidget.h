#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VNHSettingsDialogWidget.generated.h"

class UButton;
class UCheckBox;
class UComboBoxString;
class USlider;
class UTextBlock;
class UWidgetSwitcher;

UCLASS()
class VNHSIMULATOR_API UVNHSettingsDialogWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeConstruct() override;

	UFUNCTION(BlueprintCallable, Category = "VNH|Settings")
	void LoadSettings();

	UFUNCTION(BlueprintCallable, Category = "VNH|Settings")
	void ApplySettings();

	UFUNCTION(BlueprintCallable, Category = "VNH|Settings")
	void SaveSettings();

protected:
	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UWidgetSwitcher> SettingsSwitcher;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SettingsStatusText;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> TabGameplayButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> TabAudioButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> TabVideoButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> TabControlsButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> TabAccessibilityButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> ApplySettingsButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UButton> CloseSettingsButton;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> InvertLookCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> MouseSensitivitySlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> HoldActNaturalCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> MasterVolumeSlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> MusicVolumeSlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> SfxVolumeSlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> MuteWhenUnfocusedCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> WindowModeCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> QualityPresetCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> VSyncCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> BrightnessSlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> InputPresetCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> KeyboardLayoutCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UComboBoxString> ControllerLayoutCombo;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RowValue_Move;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RowValue_Interact;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RowValue_QuickChatKey;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RowValue_ActNatural;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> SubtitlesCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> HighContrastCheckBox;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<USlider> UIScaleSlider;

	UPROPERTY(BlueprintReadOnly, meta = (BindWidgetOptional))
	TObjectPtr<UCheckBox> ReduceCameraShakeCheckBox;

private:
	static constexpr const TCHAR* SettingsSection = TEXT("/Script/VNHSimulator.VNHSettings");

	void PopulateComboBox(UComboBoxString* ComboBox, std::initializer_list<const TCHAR*> Options, const FString& DefaultOption);
	void SetSelectedOption(UComboBoxString* ComboBox, const FString& Value, const FString& DefaultValue);
	FString GetSelectedOption(const UComboBoxString* ComboBox, const FString& DefaultValue) const;
	bool GetCheckBoxValue(const UCheckBox* CheckBox, bool DefaultValue) const;
	float GetSliderValue(const USlider* Slider, float DefaultValue) const;
	void SetStatus(const FText& StatusText);
	void SetTab(int32 TabIndex, const FText& StatusText);
	void UpdateControlLabels();
	void ApplyAudioSettings();
	void ApplyVideoSettings();

	UFUNCTION()
	void HandleGameplayTabClicked();

	UFUNCTION()
	void HandleAudioTabClicked();

	UFUNCTION()
	void HandleVideoTabClicked();

	UFUNCTION()
	void HandleControlsTabClicked();

	UFUNCTION()
	void HandleAccessibilityTabClicked();

	UFUNCTION()
	void HandleApplyClicked();

	UFUNCTION()
	void HandleCloseClicked();

	UFUNCTION()
	void HandleInputPresetChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleKeyboardLayoutChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleControllerLayoutChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
};
