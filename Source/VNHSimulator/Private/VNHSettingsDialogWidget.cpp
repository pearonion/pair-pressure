#include "VNHSettingsDialogWidget.h"

#include "AudioDevice.h"
#include "Containers/Ticker.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Styling/SlateTypes.h"
#include "UObject/UnrealType.h"
#include "VNHLog.h"

namespace
{
constexpr const TCHAR* SettingsSaveSlot = TEXT("PlayerSettings");
constexpr const TCHAR* SettingsSaveGameClassPath = TEXT("/Game/UI/BP_SettingsSaveGame.BP_SettingsSaveGame_C");

bool bCachedMuteWhenUnfocused = true;
FTSTicker::FDelegateHandle AudioFocusTickerHandle;

USoundMix* GetVNHSettingsSoundMix()
{
	static USoundMix* RuntimeSoundMix = nullptr;
	if (!RuntimeSoundMix)
	{
		RuntimeSoundMix = NewObject<USoundMix>(GetTransientPackage(), TEXT("VNHSettingsRuntimeSoundMix"));
		RuntimeSoundMix->AddToRoot();
	}
	return RuntimeSoundMix;
}

UClass* LoadSettingsSaveGameClass()
{
	return LoadClass<USaveGame>(nullptr, SettingsSaveGameClassPath);
}

USaveGame* LoadSettingsSaveGame()
{
	return UGameplayStatics::DoesSaveGameExist(SettingsSaveSlot, 0)
		? UGameplayStatics::LoadGameFromSlot(SettingsSaveSlot, 0)
		: nullptr;
}

USaveGame* CreateSettingsSaveGame()
{
	UClass* SaveClass = LoadSettingsSaveGameClass();
	return SaveClass ? UGameplayStatics::CreateSaveGameObject(SaveClass) : nullptr;
}

float GetSettingsFloatPropertyValue(const UObject* Object, const FName PropertyName, float DefaultValue)
{
	if (const FFloatProperty* Property = Object ? FindFProperty<FFloatProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		return Property->GetPropertyValue_InContainer(Object);
	}
	return DefaultValue;
}

bool GetSettingsBoolPropertyValue(const UObject* Object, const FName PropertyName, bool DefaultValue)
{
	if (const FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		return Property->GetPropertyValue_InContainer(Object);
	}
	return DefaultValue;
}

FString GetSettingsStringPropertyValue(const UObject* Object, const FName PropertyName, const FString& DefaultValue)
{
	if (const FStrProperty* Property = Object ? FindFProperty<FStrProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		return Property->GetPropertyValue_InContainer(Object);
	}
	return DefaultValue;
}

void SetSettingsFloatPropertyValue(UObject* Object, const FName PropertyName, float Value)
{
	if (FFloatProperty* Property = Object ? FindFProperty<FFloatProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		Property->SetPropertyValue_InContainer(Object, Value);
	}
}

void SetSettingsBoolPropertyValue(UObject* Object, const FName PropertyName, bool bValue)
{
	if (FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		Property->SetPropertyValue_InContainer(Object, bValue);
	}
}

void SetSettingsStringPropertyValue(UObject* Object, const FName PropertyName, const FString& Value)
{
	if (FStrProperty* Property = Object ? FindFProperty<FStrProperty>(Object->GetClass(), PropertyName) : nullptr)
	{
		Property->SetPropertyValue_InContainer(Object, Value);
	}
}

USoundClass* LoadSoundClass(const TCHAR* Path)
{
	return LoadObject<USoundClass>(nullptr, Path);
}

bool IsVNHApplicationActive()
{
	return !FSlateApplication::IsInitialized() || FSlateApplication::Get().IsActive();
}

void ApplyPrimaryVolumeForFocus()
{
	if (!GEngine)
	{
		return;
	}

	if (FAudioDeviceHandle AudioDevice = GEngine->GetMainAudioDevice())
	{
		const bool bShouldMute = bCachedMuteWhenUnfocused && !IsVNHApplicationActive();
		AudioDevice->SetTransientPrimaryVolume(bShouldMute ? 0.0f : 1.0f);
	}
}

bool TickAudioFocusSettings(float DeltaTime)
{
	ApplyPrimaryVolumeForFocus();
	return true;
}

void EnsureAudioFocusTicker()
{
	if (!AudioFocusTickerHandle.IsValid())
	{
		AudioFocusTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickAudioFocusSettings), 0.25f);
	}
}

float BrightnessToDisplayGamma(float Brightness)
{
	return FMath::Lerp(1.4f, 3.0f, FMath::Clamp(Brightness, 0.0f, 1.0f));
}
}

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
	if (ResetBrightnessButton)
	{
		ResetBrightnessButton->OnClicked.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleResetBrightnessClicked);
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
	if (MasterVolumeSlider)
	{
		MasterVolumeSlider->OnValueChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleAudioSliderChanged);
	}
	if (MusicVolumeSlider)
	{
		MusicVolumeSlider->OnValueChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleAudioSliderChanged);
	}
	if (SfxVolumeSlider)
	{
		SfxVolumeSlider->OnValueChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleAudioSliderChanged);
	}
	if (BrightnessSlider)
	{
		BrightnessSlider->OnValueChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleBrightnessSliderChanged);
	}
	if (MuteWhenUnfocusedCheckBox)
	{
		MuteWhenUnfocusedCheckBox->OnCheckStateChanged.AddUniqueDynamic(this, &UVNHSettingsDialogWidget::HandleMuteWhenUnfocusedChanged);
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
	USaveGame* SaveGame = LoadSettingsSaveGame();

	if (InvertLookCheckBox)
	{
		GConfig->GetBool(SettingsSection, TEXT("bInvertLook"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("InvertLook"), bBoolValue);
		InvertLookCheckBox->SetIsChecked(bBoolValue);
	}
	if (MouseSensitivitySlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("MouseSensitivity"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("MouseSensitivity"), FloatValue);
		MouseSensitivitySlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (HoldActNaturalCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bHoldActNatural"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("HoldActNatural"), bBoolValue);
		HoldActNaturalCheckBox->SetIsChecked(bBoolValue);
	}
	if (MasterVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("MasterVolume"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("MasterVolume"), FloatValue);
		MasterVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (MusicVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("MusicVolume"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("MusicVolume"), FloatValue);
		MusicVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (SfxVolumeSlider)
	{
		FloatValue = 0.8f;
		GConfig->GetFloat(SettingsSection, TEXT("SfxVolume"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("SfxVolume"), FloatValue);
		SfxVolumeSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (MuteWhenUnfocusedCheckBox)
	{
		bBoolValue = true;
		GConfig->GetBool(SettingsSection, TEXT("bMuteWhenUnfocused"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("MuteWhenUnfocused"), bBoolValue);
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
	StringValue = GetSettingsStringPropertyValue(SaveGame, TEXT("WindowMode"), FString());
	if (!StringValue.IsEmpty())
	{
		SetSelectedOption(WindowModeCombo, StringValue, TEXT("Windowed Fullscreen"));
	}
	StringValue = GetSettingsStringPropertyValue(SaveGame, TEXT("QualityPreset"), FString());
	if (!StringValue.IsEmpty())
	{
		SetSelectedOption(QualityPresetCombo, StringValue, TEXT("High"));
	}
	if (VSyncCheckBox && SaveGame)
	{
		VSyncCheckBox->SetIsChecked(GetSettingsBoolPropertyValue(SaveGame, TEXT("VSync"), VSyncCheckBox->IsChecked()));
	}

	if (BrightnessSlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("Brightness"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("Brightness"), FloatValue);
		BrightnessSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}

	GConfig->GetString(SettingsSection, TEXT("InputPreset"), StringValue, GGameUserSettingsIni);
	StringValue = GetSettingsStringPropertyValue(SaveGame, TEXT("InputPreset"), StringValue);
	SetSelectedOption(InputPresetCombo, StringValue, TEXT("Keyboard & Mouse"));
	GConfig->GetString(SettingsSection, TEXT("KeyboardLayout"), StringValue, GGameUserSettingsIni);
	StringValue = GetSettingsStringPropertyValue(SaveGame, TEXT("KeyboardLayout"), StringValue);
	SetSelectedOption(KeyboardLayoutCombo, StringValue, TEXT("WASD"));
	GConfig->GetString(SettingsSection, TEXT("ControllerLayout"), StringValue, GGameUserSettingsIni);
	StringValue = GetSettingsStringPropertyValue(SaveGame, TEXT("ControllerLayout"), StringValue);
	SetSelectedOption(ControllerLayoutCombo, StringValue, TEXT("Default"));

	if (SubtitlesCheckBox)
	{
		bBoolValue = true;
		GConfig->GetBool(SettingsSection, TEXT("bSubtitles"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("Subtitles"), bBoolValue);
		SubtitlesCheckBox->SetIsChecked(bBoolValue);
	}
	if (HighContrastCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bHighContrast"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("HighContrast"), bBoolValue);
		HighContrastCheckBox->SetIsChecked(bBoolValue);
	}
	if (UIScaleSlider)
	{
		FloatValue = 0.5f;
		GConfig->GetFloat(SettingsSection, TEXT("UIScale"), FloatValue, GGameUserSettingsIni);
		FloatValue = GetSettingsFloatPropertyValue(SaveGame, TEXT("UIScale"), FloatValue);
		UIScaleSlider->SetValue(FMath::Clamp(FloatValue, 0.0f, 1.0f));
	}
	if (ReduceCameraShakeCheckBox)
	{
		bBoolValue = false;
		GConfig->GetBool(SettingsSection, TEXT("bReduceCameraShake"), bBoolValue, GGameUserSettingsIni);
		bBoolValue = GetSettingsBoolPropertyValue(SaveGame, TEXT("ReduceCameraShake"), bBoolValue);
		ReduceCameraShakeCheckBox->SetIsChecked(bBoolValue);
	}

	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::ApplySettings()
{
	ApplyVideoSettings();
	ApplyBrightnessSettings();
	ApplyAudioSettings();
	ApplyMuteWhenUnfocusedSettings();
	UpdateControlLabels();
}

void UVNHSettingsDialogWidget::SaveSettings()
{
	USaveGame* SaveGame = LoadSettingsSaveGame();
	if (!SaveGame)
	{
		SaveGame = CreateSettingsSaveGame();
	}

	if (SaveGame)
	{
		SetSettingsBoolPropertyValue(SaveGame, TEXT("InvertLook"), GetCheckBoxValue(InvertLookCheckBox, false));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("MouseSensitivity"), GetSliderValue(MouseSensitivitySlider, 0.5f));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("HoldActNatural"), GetCheckBoxValue(HoldActNaturalCheckBox, false));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("MasterVolume"), GetSliderValue(MasterVolumeSlider, 0.8f));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("MusicVolume"), GetSliderValue(MusicVolumeSlider, 0.8f));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("SfxVolume"), GetSliderValue(SfxVolumeSlider, 0.8f));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("MuteWhenUnfocused"), GetCheckBoxValue(MuteWhenUnfocusedCheckBox, true));
		SetSettingsStringPropertyValue(SaveGame, TEXT("WindowMode"), GetSelectedOption(WindowModeCombo, TEXT("Windowed Fullscreen")));
		SetSettingsStringPropertyValue(SaveGame, TEXT("QualityPreset"), GetSelectedOption(QualityPresetCombo, TEXT("High")));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("VSync"), GetCheckBoxValue(VSyncCheckBox, false));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("Brightness"), GetSliderValue(BrightnessSlider, 0.5f));
		SetSettingsStringPropertyValue(SaveGame, TEXT("InputPreset"), GetSelectedOption(InputPresetCombo, TEXT("Keyboard & Mouse")));
		SetSettingsStringPropertyValue(SaveGame, TEXT("KeyboardLayout"), GetSelectedOption(KeyboardLayoutCombo, TEXT("WASD")));
		SetSettingsStringPropertyValue(SaveGame, TEXT("ControllerLayout"), GetSelectedOption(ControllerLayoutCombo, TEXT("Default")));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("Subtitles"), GetCheckBoxValue(SubtitlesCheckBox, true));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("HighContrast"), GetCheckBoxValue(HighContrastCheckBox, false));
		SetSettingsFloatPropertyValue(SaveGame, TEXT("UIScale"), GetSliderValue(UIScaleSlider, 0.5f));
		SetSettingsBoolPropertyValue(SaveGame, TEXT("ReduceCameraShake"), GetCheckBoxValue(ReduceCameraShakeCheckBox, false));
		UGameplayStatics::SaveGameToSlot(SaveGame, SettingsSaveSlot, 0);
	}

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
	const FLinearColor ComboAccent(0.000f, 0.920f, 0.780f, 1.0f);
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
	MenuBorderBrush.OutlineSettings = FSlateBrushOutlineSettings(FSlateColor(ComboAccent), 1.0f);

	FSlateBrush DownArrowBrush = MakeBrush(ComboAccent);
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
	SelectedBrush.OutlineSettings = FSlateBrushOutlineSettings(FSlateColor(ComboAccent), 1.0f);

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
	const float MasterVolume = FMath::Clamp(GetSliderValue(MasterVolumeSlider, 0.8f), 0.0f, 1.0f);
	const float MusicVolume = FMath::Clamp(GetSliderValue(MusicVolumeSlider, 0.8f), 0.0f, 1.0f);
	const float SfxVolume = FMath::Clamp(GetSliderValue(SfxVolumeSlider, 0.8f), 0.0f, 1.0f);
	const float EffectiveMusicVolume = MasterVolume * MusicVolume;
	const float EffectiveSfxVolume = MasterVolume * SfxVolume;

	EnsureAudioFocusTicker();
	ApplyPrimaryVolumeForFocus();

	UWorld* World = GetWorld();
	USoundMix* RuntimeSoundMix = GetVNHSettingsSoundMix();
	if (!World || !RuntimeSoundMix)
	{
		return;
	}

	if (USoundClass* MasterClass = LoadSoundClass(TEXT("/Engine/EngineSounds/Master.Master")))
	{
		UGameplayStatics::SetSoundMixClassOverride(World, RuntimeSoundMix, MasterClass, 1.0f, 1.0f, 0.0f, true);
	}
	if (USoundClass* MusicClass = LoadSoundClass(TEXT("/Engine/EngineSounds/Music.Music")))
	{
		UGameplayStatics::SetSoundMixClassOverride(World, RuntimeSoundMix, MusicClass, EffectiveMusicVolume, 1.0f, 0.0f, true);
	}
	if (USoundClass* SfxClass = LoadSoundClass(TEXT("/Engine/EngineSounds/SFX.SFX")))
	{
		UGameplayStatics::SetSoundMixClassOverride(World, RuntimeSoundMix, SfxClass, EffectiveSfxVolume, 1.0f, 0.0f, true);
	}
	UGameplayStatics::PushSoundMixModifier(World, RuntimeSoundMix);

	UE_LOG(LogVNH, Display, TEXT("SettingsAudio: Master=%.2f Music=%.2f SFX=%.2f EffectiveMusic=%.2f EffectiveSFX=%.2f"),
		MasterVolume,
		MusicVolume,
		SfxVolume,
		EffectiveMusicVolume,
		EffectiveSfxVolume);
}

void UVNHSettingsDialogWidget::ApplyBrightnessSettings()
{
	const float Brightness = FMath::Clamp(GetSliderValue(BrightnessSlider, 0.5f), 0.0f, 1.0f);
	const float DisplayGamma = BrightnessToDisplayGamma(Brightness);

	if (GEngine)
	{
		GEngine->DisplayGamma = DisplayGamma;
	}

	if (APlayerController* PlayerController = GetOwningPlayer())
	{
		PlayerController->ConsoleCommand(FString::Printf(TEXT("Gamma %.3f"), DisplayGamma), true);
	}
	else if (UWorld* World = GetWorld())
	{
		if (APlayerController* FirstPlayerController = World->GetFirstPlayerController())
		{
			FirstPlayerController->ConsoleCommand(FString::Printf(TEXT("Gamma %.3f"), DisplayGamma), true);
		}
	}
}

void UVNHSettingsDialogWidget::ApplyMuteWhenUnfocusedSettings()
{
	bCachedMuteWhenUnfocused = GetCheckBoxValue(MuteWhenUnfocusedCheckBox, true);
	EnsureAudioFocusTicker();
	ApplyPrimaryVolumeForFocus();
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

void UVNHSettingsDialogWidget::HandleAudioSliderChanged(float Value)
{
	ApplyAudioSettings();
}

void UVNHSettingsDialogWidget::HandleBrightnessSliderChanged(float Value)
{
	ApplyBrightnessSettings();
}

void UVNHSettingsDialogWidget::HandleResetBrightnessClicked()
{
	if (BrightnessSlider)
	{
		BrightnessSlider->SetValue(0.5f);
	}
	ApplyBrightnessSettings();
}

void UVNHSettingsDialogWidget::HandleMuteWhenUnfocusedChanged(bool bIsChecked)
{
	ApplyMuteWhenUnfocusedSettings();
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
