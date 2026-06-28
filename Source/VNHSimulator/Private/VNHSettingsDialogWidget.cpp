#include "VNHSettingsDialogWidget.h"

#include "AudioDevice.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "GameFramework/GameUserSettings.h"
#include "VNHLog.h"

void UVNHSettingsDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();

	PopulateComboBox(WindowModeCombo, {TEXT("Fullscreen"), TEXT("Windowed Fullscreen"), TEXT("Windowed")}, TEXT("Windowed Fullscreen"));
	PopulateComboBox(QualityPresetCombo, {TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic")}, TEXT("High"));
	PopulateComboBox(InputPresetCombo, {TEXT("Keyboard & Mouse"), TEXT("Controller")}, TEXT("Keyboard & Mouse"));
	PopulateComboBox(KeyboardLayoutCombo, {TEXT("WASD"), TEXT("Arrow Keys"), TEXT("Left-Handed")}, TEXT("WASD"));
	PopulateComboBox(ControllerLayoutCombo, {TEXT("Default"), TEXT("Southpaw"), TEXT("Legacy")}, TEXT("Default"));

	if (TabGameplayButton)
	{
		TabGameplayButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleGameplayTabClicked);
	}
	if (TabAudioButton)
	{
		TabAudioButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleAudioTabClicked);
	}
	if (TabVideoButton)
	{
		TabVideoButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleVideoTabClicked);
	}
	if (TabControlsButton)
	{
		TabControlsButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleControlsTabClicked);
	}
	if (TabAccessibilityButton)
	{
		TabAccessibilityButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleAccessibilityTabClicked);
	}
	if (ApplySettingsButton)
	{
		ApplySettingsButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleApplyClicked);
	}
	if (CloseSettingsButton)
	{
		CloseSettingsButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleCloseClicked);
	}
	if (InputPresetCombo)
	{
		InputPresetCombo->OnSelectionChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleInputPresetChanged);
	}
	if (KeyboardLayoutCombo)
	{
		KeyboardLayoutCombo->OnSelectionChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleKeyboardLayoutChanged);
	}
	if (ControllerLayoutCombo)
	{
		ControllerLayoutCombo->OnSelectionChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleControllerLayoutChanged);
	}

	LoadSettings();
	ApplySettings();
	SetTab(0, NSLOCTEXT("VNH", "SettingsLoaded", "Settings loaded."));
}

void UVNHSettingsDialogWidget::LoadSettings()
{
	bool bBoolValue = false;
	float FloatValue = 0.0f;
	FString StringValue;

	if (InvertLookCheckBox)
	{
		GConfig->GetBool(SettingsSection, TEXT("bInvertLook"), bBoolValue, GGameUserSettingsIni);
		InvertLookCheckBox->SetIsChecked(bBoolValue);
	}
	if (MouseSensitivitySlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("MouseSensitivity"), FloatValue, GGameUserSettingsIni);
		MouseSensitivitySlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (HoldActNaturalCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bHoldActNatural"), bBoolValue, GGameUserSettingsIni);
		HoldActNaturalCheckBox->SetIsChecked(bBoolValue);
	}
	if (MasterVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("MasterVolume"), FloatValue, GGameUserSettingsIni);
		MasterVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (MusicVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("MusicVolume"), FloatValue, GGameUserSettingsIni);
		MusicVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (SfxVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("SfxVolume"), FloatValue, GGameUserSettingsIni);
		SfxVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (MuteWhenUnfocusedCheckBox)
	{
		bBoolValue = true;
		GConfig->GetBool(SettingsSection, TEXT("bMuteWhenUnfocused"), bBoolValue, GGameUserSettingsIni);
		MuteWhenUnfocusedCheckBox->SetIsChecked(bBoolValue);
	}

	if (UGameUserSettings* UserSettings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		switch (UserSettings->GetFullscreenMode())
		{
		case EWindowMode::Fullscreen:
			SetSelectedOption(WindowModeCombo, TEXT("Fullscreen"), TEXT("Windowed Fullscreen"));
			break;
		case EWindowMode::Windowed:
			SetSelectedOption(WindowModeCombo, TEXT("Windowed"), TEXT("Windowed Fullscreen"));
			break;
		default:
			SetSelectedOption(WindowModeCombo, TEXT("Windowed Fullscreen"), TEXT("Windowed Fullscreen"));
			break;
		}

		const int32 QualityLevel = UserSettings->GetOverallScalabilityLevel();
		const TCHAR* QualityName = QualityLevel <= 0 ? TEXT("Low") : QualityLevel == 1 ? TEXT("Medium") : QualityLevel == 2 ? TEXT("High") : TEXT("Epic");
		SetSelectedOption(QualityPresetCombo, QualityName, TEXT("High"));
		if (VSyncCheckBox)
		{
			VSyncCheckBox->SetIsChecked(UserSettings->IsVSyncEnabled());
		}
	}

	if (BrightnessSlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("Brightness"), FloatValue, GGameUserSettingsIni);
		BrightnessSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}

	GConfig->GetString(SettingsSection, TEXT("InputPreset"), StringValue, GGameUserSettingsIni);
	SetSelectedOption(InputPresetCombo, StringValue, TEXT("Keyboard & Mouse"));
	GConfig->GetString(SettingsSection, TEXT("KeyboardLayout"), StringValue, GGameUserSettingsIni);
	SetSelectedOption(KeyboardLayoutCombo, StringValue, TEXT("WASD"));
	GConfig->GetString(SettingsSection, TEXT("ControllerLayout"), StringValue, GGameUserSettingsIni);
	SetSelectedOption(ControllerLayoutCombo, StringValue, TEXT("Default"));

	if (SubtitlesCheckBox)
	{
		bBoolValue = true;
		GConfig->GetBool(SettingsSection, TEXT("bSubtitles"), bBoolValue, GGameUserSettingsIni);
		SubtitlesCheckBox->SetIsChecked(bBoolValue);
	}
	if (HighContrastCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bHighContrast"), bBoolValue, GGameUserSettingsIni);
		HighContrastCheckBox->SetIsChecked(bBoolValue);
	}
	if (UIScaleSlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("UIScale"), FloatValue, GGameUserSettingsIni);
		UIScaleSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (ReduceCameraShakeCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bReduceCameraShake"), bBoolValue, GGameUserSettingsIni);
		ReduceCameraShakeCheckBox->SetIsChecked(bBoolValue);
	}

	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::ApplySettings()
{
	ApplyVideoSettings();
	ApplyAudioSettings();
	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::SaveSettings()
{
	GConfig->SetBool(SettingsSection, TEXT("bInvertLook"), GetCheckBoxValue(InvertLookCheckBox, false), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("MouseSensitivity"), GetSliderValue(MouseSensitivitySlider, 0.5f), GGameUserSettingsIni);
	GConfig->SetBool(SettingsSection, TEXT("bHoldActNatural"), GetCheckBoxValue(HoldActNaturalCheckBox, false), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("MasterVolume"), GetSliderValue(MasterVolumeSlider, 0.8f), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("MusicVolume"), GetSliderValue(MusicVolumeSlider, 0.8f), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("SfxVolume"), GetSliderValue(SfxVolumeSlider, 0.8f), GGameUserSettingsIni);
	GConfig->SetBool(SettingsSection, TEXT("bMuteWhenUnfocused"), GetCheckBoxValue(MuteWhenUnfocusedCheckBox, true), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("Brightness"), GetSliderValue(BrightnessSlider, 0.5f), GGameUserSettingsIni);
	GConfig->SetString(SettingsSection, TEXT("InputPreset"), *GetSelectedOption(InputPresetCombo, TEXT("Keyboard & Mouse")), GGameUserSettingsIni);
	GConfig->SetString(SettingsSection, TEXT("KeyboardLayout"), *GetSelectedOption(KeyboardLayoutCombo, TEXT("WASD")), GGameUserSettingsIni);
	GConfig->SetString(SettingsSection, TEXT("ControllerLayout"), *GetSelectedOption(ControllerLayoutCombo, TEXT("Default")), GGameUserSettingsIni);
	GConfig->SetBool(SettingsSection, TEXT("bSubtitles"), GetCheckBoxValue(SubtitlesCheckBox, true), GGameUserSettingsIni);
	GConfig->SetBool(SettingsSection, TEXT("bHighContrast"), GetCheckBoxValue(HighContrastCheckBox, false), GGameUserSettingsIni);
	GConfig->SetFloat(SettingsSection, TEXT("UIScale"), GetSliderValue(UIScaleSlider, 0.5f), GGameUserSettingsIni);
	GConfig->SetBool(SettingsSection, TEXT("bReduceCameraShake"), GetCheckBoxValue(ReduceCameraShakeCheckBox, false), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);

	if (UGameUserSettings* UserSettings = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		UserSettings->SaveSettings();
	}

	UE_LOG(LogVNH, Display, TEXT("Settings saved to %s."), *GGameUserSettingsIni);
}

void UVNHSettingsDialogWidget::PopulateComboBox(UComboBoxString* ComboBox, std::initializer_list<const TCHAR*> Options, const FString& DefaultOption)
{
	if (!ComboBox)
	{
		return;
	}

	ComboBox->ClearOptions();
	for (const TCHAR* Option : Options)
	{
		ComboBox->AddOption(Option);
	}
	ComboBox->SetSelectedOption(DefaultOption);
}

void UVNHSettingsDialogWidget::SetSelectedOption(UComboBoxString* ComboBox, const FString& Value, const FString& DefaultValue)
{
	if (!ComboBox)
	{
		return;
	}

	const FString SelectedValue = !Value.IsEmpty() && ComboBox->FindOptionIndex(Value) != INDEX_NONE ? Value : DefaultValue;
	ComboBox->SetSelectedOption(SelectedValue);
}

FString UVNHSettingsDialogWidget::GetSelectedOption(const UComboBoxString* ComboBox, const FString& DefaultValue) const
{
	if (!ComboBox)
	{
		return DefaultValue;
	}

	const FString SelectedValue = ComboBox->GetSelectedOption();
	return SelectedValue.IsEmpty() ? DefaultValue : SelectedValue;
}

bool UVNHSettingsDialogWidget::GetCheckBoxValue(const UCheckBox* CheckBox, bool DefaultValue) const
{
	return CheckBox ? CheckBox->IsChecked() : DefaultValue;
}

float UVNHSettingsDialogWidget::GetSliderValue(const USlider* Slider, float DefaultValue) const
{
	return Slider ? Slider->GetValue() : DefaultValue;
}

void UVNHSettingsDialogWidget::SetStatus(const FText& StatusText)
{
	if (SettingsStatusText)
	{
		SettingsStatusText->SetText(StatusText);
	}
}

void UVNHSettingsDialogWidget::SetTab(int32 TabIndex, const FText& StatusText)
{
	if (SettingsSwitcher)
	{
		SettingsSwitcher->SetActiveWidgetIndex(TabIndex);
	}
	SetStatus(StatusText);
}

void UVNHSettingsDialogWidget::UpdateControlLabels()
{
	const FString InputPreset = GetSelectedOption(InputPresetCombo, TEXT("Keyboard & Mouse"));
	const FString KeyboardLayout = GetSelectedOption(KeyboardLayoutCombo, TEXT("WASD"));
	const FString ControllerLayout = GetSelectedOption(ControllerLayoutCombo, TEXT("Default"));
	const bool bController = InputPreset == TEXT("Controller");

	if (RowValue_Move)
	{
		RowValue_Move->SetText(FText::FromString(bController ? (ControllerLayout == TEXT("Southpaw") ? TEXT("Right Stick") : TEXT("Left Stick")) : (KeyboardLayout == TEXT("Arrow Keys") ? TEXT("Arrow Keys") : KeyboardLayout == TEXT("Left-Handed") ? TEXT("IJKL") : TEXT("WASD"))));
	}
	if (RowValue_Interact)
	{
		RowValue_Interact->SetText(FText::FromString(bController ? TEXT("Face Button Bottom") : TEXT("E")));
	}
	if (RowValue_QuickChatKey)
	{
		RowValue_QuickChatKey->SetText(FText::FromString(bController ? TEXT("D-Pad") : TEXT("Q / D-Pad UI")));
	}
	if (RowValue_ActNatural)
	{
		RowValue_ActNatural->SetText(FText::FromString(bController ? TEXT("Right Shoulder") : TEXT("F")));
	}
}

void UVNHSettingsDialogWidget::ApplyAudioSettings()
{
	if (GEngine)
	{
		if (FAudioDeviceHandle AudioDevice = GEngine->GetMainAudioDevice())
		{
			AudioDevice->SetTransientPrimaryVolume(GetSliderValue(MasterVolumeSlider, 0.8f));
		}
	}
}

void UVNHSettingsDialogWidget::ApplyVideoSettings()
{
	UGameUserSettings* UserSettings = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (!UserSettings)
	{
		return;
	}

	const FString WindowMode = GetSelectedOption(WindowModeCombo, TEXT("Windowed Fullscreen"));
	if (WindowMode == TEXT("Fullscreen"))
	{
		UserSettings->SetFullscreenMode(EWindowMode::Fullscreen);
	}
	else if (WindowMode == TEXT("Windowed"))
	{
		UserSettings->SetFullscreenMode(EWindowMode::Windowed);
	}
	else
	{
		UserSettings->SetFullscreenMode(EWindowMode::WindowedFullscreen);
	}

	const FString QualityPreset = GetSelectedOption(QualityPresetCombo, TEXT("High"));
	const int32 QualityLevel = QualityPreset == TEXT("Low") ? 0 : QualityPreset == TEXT("Medium") ? 1 : QualityPreset == TEXT("High") ? 2 : 3;
	UserSettings->SetOverallScalabilityLevel(QualityLevel);
	UserSettings->SetVSyncEnabled(GetCheckBoxValue(VSyncCheckBox, false));
	UserSettings->ApplySettings(false);
}

void UVNHSettingsDialogWidget::HandleGameplayTabClicked()
{
	SetTab(0, NSLOCTEXT("VNH", "SettingsGameplayTab", "Gameplay settings."));
}

void UVNHSettingsDialogWidget::HandleAudioTabClicked()
{
	SetTab(1, NSLOCTEXT("VNH", "SettingsAudioTab", "Audio settings."));
}

void UVNHSettingsDialogWidget::HandleVideoTabClicked()
{
	SetTab(2, NSLOCTEXT("VNH", "SettingsVideoTab", "Video settings."));
}

void UVNHSettingsDialogWidget::HandleControlsTabClicked()
{
	SetTab(3, NSLOCTEXT("VNH", "SettingsControlsTab", "Controls settings."));
}

void UVNHSettingsDialogWidget::HandleAccessibilityTabClicked()
{
	SetTab(4, NSLOCTEXT("VNH", "SettingsAccessibilityTab", "Accessibility settings."));
}

void UVNHSettingsDialogWidget::HandleApplyClicked()
{
	ApplySettings();
	SaveSettings();
	SetStatus(NSLOCTEXT("VNH", "SettingsSavedStatus", "Settings applied and saved."));
}

void UVNHSettingsDialogWidget::HandleCloseClicked()
{
	RemoveFromParent();
}

void UVNHSettingsDialogWidget::HandleInputPresetChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::HandleKeyboardLayoutChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::HandleControllerLayoutChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	UpdateControlLabels();
}
