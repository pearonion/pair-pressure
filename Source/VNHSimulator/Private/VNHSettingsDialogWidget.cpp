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
#include "Styling/SlateTypes.h"
#include "VNHLog.h"

void UVNHSettingsDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();

	PopulateComboBox(WindowModeCombo, {TEXT("Fullscreen"), TEXT("Windowed Fullscreen"), TEXT("Windowed")}, TEXT("Windowed Fullscreen"));
	PopulateComboBox(QualityPresetCombo, {TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic")}, TEXT("High"));
	PopulateComboBox(InputPresetCombo, {TEXT("Keyboard & Mouse"), TEXT("Controller")}, TEXT("Keyboard & Mouse"));
	PopulateComboBox(KeyboardLayoutCombo, {TEXT("WASD"), TEXT("Arrow Keys"), TEXT("Left-Handed")}, TEXT("WASD"));
	PopulateComboBox(ControllerLayoutCombo, {TEXT("Default"), TEXT("Southpaw"), TEXT("Legacy")}, TEXT("Default"));
	StyleComboBox(WindowModeCombo);
	StyleComboBox(QualityPresetCombo);
	StyleComboBox(InputPresetCombo);
	StyleComboBox(KeyboardLayoutCombo);
	StyleComboBox(ControllerLayoutCombo);

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

void UVNHSettingsDialogWidget::StyleComboBox(UComboBoxString* ComboBox) const
{
	if (!ComboBox)
	{
		return;
	}

	const FLinearColor TextColor(1.0f, 0.98f, 0.94f, 1.0f);
	const FLinearColor MenuBackground(0.018f, 0.030f, 0.038f, 0.99f);
	const FLinearColor RowBackground(0.032f, 0.043f, 0.054f, 1.0f);
	const FLinearColor RowBackgroundAlt(0.025f, 0.036f, 0.047f, 1.0f);
	const FLinearColor RowHover(0.020f, 0.180f, 0.165f, 1.0f);
	const FLinearColor RowSelected(0.000f, 0.260f, 0.225f, 1.0f);
	const FLinearColor Accent(0.000f, 0.920f, 0.780f, 1.0f);
	const FLinearColor Border(0.000f, 0.520f, 0.450f, 0.85f);

	auto MakeBrush = [](const FLinearColor& Color)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Box;
		Brush.TintColor = FSlateColor(Color);
		Brush.Margin = FMargin(0.0f);
		Brush.SetImageSize(FVector2D(8.0f, 8.0f));
		return Brush;
	};

	FComboBoxStyle ComboStyle = ComboBox->GetWidgetStyle();
	FComboButtonStyle ComboButtonStyle = ComboStyle.ComboButtonStyle;
	FButtonStyle ButtonStyle = ComboButtonStyle.ButtonStyle;
	ButtonStyle
		.SetNormal(MakeBrush(FLinearColor(0.030f, 0.037f, 0.048f, 0.96f)))
		.SetHovered(MakeBrush(FLinearColor(0.045f, 0.075f, 0.078f, 1.0f)))
		.SetPressed(MakeBrush(FLinearColor(0.000f, 0.115f, 0.105f, 1.0f)))
		.SetNormalForeground(FSlateColor(TextColor))
		.SetHoveredForeground(FSlateColor(TextColor))
		.SetPressedForeground(FSlateColor(TextColor))
		.SetNormalPadding(FMargin(12.0f, 6.0f))
		.SetPressedPadding(FMargin(12.0f, 7.0f, 12.0f, 5.0f));

	FSlateBrush MenuBorderBrush = MakeBrush(MenuBackground);
	MenuBorderBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
	MenuBorderBrush.OutlineSettings = FSlateBrushOutlineSettings(FSlateColor(Accent), 1.0f);

	FSlateBrush DownArrowBrush = MakeBrush(Accent);
	DownArrowBrush.DrawAs = ESlateBrushDrawType::Image;

	ComboButtonStyle
		.SetButtonStyle(ButtonStyle)
		.SetDownArrowImage(DownArrowBrush)
		.SetMenuBorderBrush(MenuBorderBrush)
		.SetMenuBorderPadding(FMargin(4.0f))
		.SetContentPadding(FMargin(10.0f, 6.0f))
		.SetDownArrowPadding(FMargin(8.0f, 2.0f, 4.0f, 2.0f))
		.SetDownArrowAlignment(VAlign_Center);
	ComboStyle
		.SetComboButtonStyle(ComboButtonStyle)
		.SetContentPadding(FMargin(10.0f, 6.0f))
		.SetMenuRowPadding(FMargin(2.0f));

	FSlateBrush HoverBrush = MakeBrush(RowHover);
	HoverBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
	HoverBrush.OutlineSettings = FSlateBrushOutlineSettings(FSlateColor(Border), 1.0f);

	FSlateBrush SelectedBrush = MakeBrush(RowSelected);
	SelectedBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
	SelectedBrush.OutlineSettings = FSlateBrushOutlineSettings(FSlateColor(Accent), 1.0f);

	FTableRowStyle RowStyle = ComboBox->GetItemStyle();
	RowStyle
		.SetSelectorFocusedBrush(SelectedBrush)
		.SetActiveHoveredBrush(HoverBrush)
		.SetActiveBrush(SelectedBrush)
		.SetInactiveHoveredBrush(HoverBrush)
		.SetInactiveBrush(MakeBrush(RowBackground))
		.SetEvenRowBackgroundBrush(MakeBrush(RowBackground))
		.SetEvenRowBackgroundHoveredBrush(HoverBrush)
		.SetOddRowBackgroundBrush(MakeBrush(RowBackgroundAlt))
		.SetOddRowBackgroundHoveredBrush(HoverBrush)
		.SetTextColor(FSlateColor(TextColor))
		.SetSelectedTextColor(FSlateColor(TextColor));

	ComboBox->SetWidgetStyle(ComboStyle);
	ComboBox->SetItemStyle(RowStyle);
	ComboBox->SetContentPadding(FMargin(10.0f, 6.0f));
	ComboBox->SetMaxListHeight(360.0f);
	ComboBox->SetHasDownArrow(true);
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
