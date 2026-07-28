#include "VNHSettingsDialogWidget.h"

#include "AudioDevice.h"
#include "Containers/Ticker.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "Input/Events.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "UObject/UnrealType.h"
#include "Blueprint/WidgetTree.h"
#include "VNHInputBindingButton.h"
#include "VNHInputPromptLibrary.h"
#include "VNHLog.h"
#include "VNHPlayerController.h"

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

class FVNHInputBindingCaptureProcessor final : public IInputProcessor
{
public:
	explicit FVNHInputBindingCaptureProcessor(UVNHSettingsDialogWidget* InSettingsWidget)
		: SettingsWidget(InSettingsWidget)
	{
	}

	virtual void Tick(
		const float DeltaTime,
		FSlateApplication& SlateApp,
		TSharedRef<ICursor> Cursor) override
	{
	}

	virtual bool HandleKeyDownEvent(
		FSlateApplication& SlateApp,
		const FKeyEvent& InKeyEvent) override
	{
		return ForwardKey(InKeyEvent.GetKey());
	}

	virtual bool HandleMouseButtonDownEvent(
		FSlateApplication& SlateApp,
		const FPointerEvent& MouseEvent) override
	{
		return ForwardKey(MouseEvent.GetEffectingButton());
	}

	virtual bool HandleAnalogInputEvent(
		FSlateApplication& SlateApp,
		const FAnalogInputEvent& InAnalogInputEvent) override
	{
		return FMath::Abs(InAnalogInputEvent.GetAnalogValue()) >= 0.5f
			&& ForwardKey(InAnalogInputEvent.GetKey());
	}

	virtual const TCHAR* GetDebugName() const override
	{
		return TEXT("VNHInputBindingCapture");
	}

private:
	bool ForwardKey(const FKey Key) const
	{
		return SettingsWidget.IsValid()
			&& SettingsWidget->HandleCapturedInput(Key);
	}

	TWeakObjectPtr<UVNHSettingsDialogWidget> SettingsWidget;
};

void UVNHSettingsDialogWidget::NativeConstruct()
{
	Super::NativeConstruct();
	SetIsFocusable(true);

	PopulateComboBox(WindowModeCombo, {TEXT("Fullscreen"), TEXT("Windowed Fullscreen"), TEXT("Windowed")}, TEXT("Windowed Fullscreen"));
	PopulateComboBox(QualityPresetCombo, {TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic")}, TEXT("High"));
	PopulateComboBox(InputPresetCombo, {TEXT("Auto"), TEXT("Keyboard & Mouse"), TEXT("Controller")}, TEXT("Auto"));
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
	BuildInputBindingRows();
	if (TabGameplayButton && GetOwningPlayer())
	{
		TabGameplayButton->SetUserFocus(GetOwningPlayer());
	}
	SetTab(0, NSLOCTEXT("VNH", "SettingsLoaded", "Settings loaded."));
}

void UVNHSettingsDialogWidget::NativeDestruct()
{
	EndInputBindingCapture();
	Super::NativeDestruct();
}

void UVNHSettingsDialogWidget::NativeTick(const FGeometry& MyGeometry, const float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	InputPromptRefreshAccumulator += InDeltaTime;
	if (InputPromptRefreshAccumulator < 0.35f)
	{
		return;
	}

	InputPromptRefreshAccumulator = 0.0f;
	const EVNHInputPromptFamily CurrentPromptFamily =
		UVNHInputPromptLibrary::GetControllerPromptFamily(GetOwningPlayer());
	if (CurrentPromptFamily != CachedPromptFamily)
	{
		CachedPromptFamily = CurrentPromptFamily;
		RefreshInputBindingRows();
		UpdateControlLabels();
	}
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
	SetSelectedOption(InputPresetCombo, StringValue, TEXT("Auto"));
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
		SetSettingsStringPropertyValue(SaveGame, TEXT("InputPreset"), GetSelectedOption(InputPresetCombo, TEXT("Auto")));
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
	GConfig->SetString(SettingsSection, TEXT("InputPreset"), *GetSelectedOption(InputPresetCombo, TEXT("Auto")), GGameUserSettingsIni);
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
	const FString KeyboardLayout = GetSelectedOption(KeyboardLayoutCombo, TEXT("WASD"));
	const FString ControllerLayout = GetSelectedOption(ControllerLayoutCombo, TEXT("Default"));
	const bool bController = UVNHInputPromptLibrary::ShouldUseGamepadPrompts(GetOwningPlayer());
	auto ResolveTextBlock = [this](TObjectPtr<UTextBlock>& CachedTextBlock, const FName WidgetName) -> UTextBlock*
	{
		if (!CachedTextBlock)
		{
			CachedTextBlock = Cast<UTextBlock>(GetWidgetFromName(WidgetName));
		}
		return CachedTextBlock.Get();
	};

	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_Move, TEXT("RowValue_Move")))
	{
		TextBlock->SetText(FText::FromString(bController ? (ControllerLayout == TEXT("Southpaw") ? TEXT("Right Stick") : TEXT("Left Stick")) : (KeyboardLayout == TEXT("Arrow Keys") ? TEXT("Arrow Keys") : KeyboardLayout == TEXT("Left-Handed") ? TEXT("IJKL") : TEXT("WASD"))));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_Interact, TEXT("RowValue_Interact")))
	{
		TextBlock->SetText(FText::FromString(bController ? TEXT("Xbox A / PS5 Cross") : TEXT("E")));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_QuickChatKey, TEXT("RowValue_QuickChatKey")))
	{
		TextBlock->SetText(FText::FromString(bController ? TEXT("D-Pad") : TEXT("Q / D-Pad UI")));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_HumanActions, TEXT("RowValue_HumanActions")))
	{
		const FString HumanActionsText = bController
			? FString(TEXT("Inspect - L3\nWave - R3\nPoint - Xbox Y / PS5 Triangle\nLaugh - Xbox X / PS5 Square\nFart - Xbox View / PS5 Create"))
			: FString(TEXT("Inspect - 1\nWave - 2\nPoint - 3\nLaugh - 4\nFart - 5"));
		TextBlock->SetText(FText::FromString(HumanActionsText));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_AlienActions, TEXT("RowValue_AlienActions")))
	{
		const FString AlienActionsText = bController
			? FString(TEXT("Inspect - L3\nWave - R3\nPoint - Xbox Y / PS5 Triangle\nLaugh - Xbox X / PS5 Square\nFart - Xbox View / PS5 Create\nPlace Decoy - Xbox Menu / PS5 Options"))
			: FString(TEXT("Inspect - 1\nWave - 2\nPoint - 3\nLaugh - 4\nFart - 5\nPlace Decoy - 6"));
		TextBlock->SetText(FText::FromString(AlienActionsText));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_ActNatural, TEXT("RowValue_ActNatural")))
	{
		TextBlock->SetText(FText::FromString(bController ? TEXT("Xbox RB / PS5 R1") : TEXT("F")));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_Jump, TEXT("RowValue_Jump")))
	{
		TextBlock->SetText(FText::FromString(bController ? FString(TEXT("Xbox A / PS5 Cross")) : FString(TEXT("SPACEBAR"))));
	}
	if (UTextBlock* TextBlock = ResolveTextBlock(RowValue_Crouch, TEXT("RowValue_Crouch")))
	{
		TextBlock->SetText(FText::FromString(bController ? FString(TEXT("Xbox B / PS5 Circle")) : FString(TEXT("CTRL"))));
	}
}

void UVNHSettingsDialogWidget::BuildInputBindingRows()
{
	if (!WidgetTree)
	{
		return;
	}

	UVerticalBox* ControlsPanelWidget =
		Cast<UVerticalBox>(GetWidgetFromName(TEXT("ControlsPanel")));
	if (!ControlsPanelWidget)
	{
		UE_LOG(LogVNH, Warning, TEXT("Settings: ControlsPanel is missing; bindable controls were not built."));
		return;
	}

	if (InputBindingButtons.Num() > 0)
	{
		RefreshInputBindingRows();
		return;
	}

	// Keep the old Designer widgets intact, but hide the informational rows that
	// the live binding grid supersedes.
	const FName SupersededWidgetNames[] =
	{
		TEXT("ComboRow_KeyboardLayout"),
		TEXT("ComboRow_ControllerLayout"),
		TEXT("Row_HumanActions"),
		TEXT("Row_AlienActions"),
		TEXT("Row_Move"),
		TEXT("Row_Interact"),
		TEXT("Row_QuickChatKey"),
		TEXT("Row_ActNatural"),
		TEXT("Row_Jump"),
		TEXT("Row_Crouch")
	};
	for (const FName SupersededWidgetName : SupersededWidgetNames)
	{
		if (UWidget* SupersededWidget = GetWidgetFromName(SupersededWidgetName))
		{
			SupersededWidget->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	if (UTextBlock* InputPresetLabel =
		Cast<UTextBlock>(GetWidgetFromName(TEXT("ComboLabel_InputPreset"))))
	{
		InputPresetLabel->SetText(NSLOCTEXT("VNH", "PromptDeviceLabel", "PROMPT DEVICE"));
	}

	UVerticalBox* BindingList =
		Cast<UVerticalBox>(GetWidgetFromName(TEXT("ControlsBindingList")));
	if (!BindingList)
	{
		BindingList = WidgetTree->ConstructWidget<UVerticalBox>(
			UVerticalBox::StaticClass(),
			TEXT("ControlsBindingList"));
		ControlsPanelWidget->AddChildToVerticalBox(BindingList);
	}

	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(
		UHorizontalBox::StaticClass());
	BindingList->AddChildToVerticalBox(HeaderRow);

	auto AddHeaderCell = [this, HeaderRow](const FText& HeaderText, const float Width)
	{
		USizeBox* HeaderSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		HeaderSize->SetWidthOverride(Width);
		UTextBlock* HeaderLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		HeaderLabel->SetText(HeaderText);
		HeaderLabel->SetColorAndOpacity(FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 1.0f)));
		HeaderLabel->SetFont(FSlateFontInfo(
			FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 14)));
		HeaderLabel->SetJustification(ETextJustify::Center);
		HeaderSize->AddChild(HeaderLabel);
		if (UHorizontalBoxSlot* HeaderSlot =
			HeaderRow->AddChildToHorizontalBox(HeaderSize))
		{
			HeaderSlot->SetVerticalAlignment(VAlign_Center);
		}
	};
	AddHeaderCell(NSLOCTEXT("VNH", "ControlHeaderAction", "ACTION"), 300.0f);
	AddHeaderCell(
		NSLOCTEXT("VNH", "ControlHeaderKeyboardMouse", "KEYBOARD + MOUSE"),
		240.0f);
	AddHeaderCell(NSLOCTEXT("VNH", "ControlHeaderController", "CONTROLLER"), 180.0f);

	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindMoveForward", "MOVE FORWARD"), TEXT("VNH_AlienMoveForward"), true, 1.0f, false, true, true, EKeys::Gamepad_LeftStick_Up);
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindMoveBackward", "MOVE BACKWARD"), TEXT("VNH_AlienMoveForward"), true, -1.0f, false, true, true, EKeys::Gamepad_LeftStick_Down);
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindMoveLeft", "MOVE LEFT"), TEXT("VNH_AlienMoveRight"), true, -1.0f, false, true, true, EKeys::Gamepad_LeftStick_Left);
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindMoveRight", "MOVE RIGHT"), TEXT("VNH_AlienMoveRight"), true, 1.0f, false, true, true, EKeys::Gamepad_LeftStick_Right);
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindLookHorizontal", "LOOK HORIZONTAL"), TEXT("Turn Right / Left Gamepad"), true, 1.0f, false, false, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindLookVertical", "LOOK VERTICAL"), TEXT("Look Up / Down Gamepad"), true, 1.0f, false, false, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindJump", "JUMP / CLIMB"), TEXT("Jump"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindDive", "DIVE (JUMP + DIVE IN AIR)"), TEXT("PP_Dive"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindGrab", "GRAB / HOLD / PICK UP"), TEXT("PP_Grab"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindThrow", "THROW (HOLD TO CHARGE)"), TEXT("VNH_AlienActNatural"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindAssist", "ASSIST PARTNER"), TEXT("PP_Assist"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindInteract", "INTERACT"), TEXT("VNH_Interact"), false, 1.0f, true, true, true, FKey());
	AddInputBindingRow(BindingList, NSLOCTEXT("VNH", "BindCrouch", "CROUCH"), TEXT("Crouch"), false, 1.0f, true, true, true, FKey());

	CachedPromptFamily =
		UVNHInputPromptLibrary::GetControllerPromptFamily(GetOwningPlayer());
	RefreshInputBindingRows();
}

void UVNHSettingsDialogWidget::AddInputBindingRow(
	UVerticalBox* BindingList,
	const FText& ActionLabel,
	const FName MappingName,
	const bool bAxisMapping,
	const float AxisScale,
	const bool bAllowMouse,
	const bool bAllowKeyboard,
	const bool bAllowGamepad,
	const FKey ControllerDisplayKey)
{
	if (!BindingList || !WidgetTree)
	{
		return;
	}

	UHorizontalBox* BindingRow = WidgetTree->ConstructWidget<UHorizontalBox>(
		UHorizontalBox::StaticClass());
	const int32 BindingRowIndex =
		FMath::Max(0, BindingList->GetChildrenCount() - 1);
	UBorder* RowBackground =
		WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	RowBackground->SetBrushColor(
		BindingRowIndex % 2 == 0
			? FLinearColor(0.015f, 0.025f, 0.03f, 0.12f)
			: FLinearColor(0.04f, 0.065f, 0.07f, 0.32f));
	RowBackground->SetPadding(FMargin(0.0f));
	RowBackground->AddChild(BindingRow);
	if (UVerticalBoxSlot* BindingRowSlot =
		BindingList->AddChildToVerticalBox(RowBackground))
	{
		BindingRowSlot->SetPadding(FMargin(0.0f, 1.0f));
	}

	USizeBox* LabelSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	LabelSize->SetWidthOverride(300.0f);
	LabelSize->SetHeightOverride(54.0f);
	UOverlay* LabelContent =
		WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	LabelSize->AddChild(LabelContent);
	UTextBlock* LabelText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	LabelText->SetText(ActionLabel);
	LabelText->SetColorAndOpacity(FSlateColor(FLinearColor(0.92f, 0.90f, 0.85f, 1.0f)));
	LabelText->SetFont(FSlateFontInfo(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 13)));
	LabelText->SetJustification(ETextJustify::Left);
	if (UOverlaySlot* LabelContentSlot =
		LabelContent->AddChildToOverlay(LabelText))
	{
		LabelContentSlot->SetHorizontalAlignment(HAlign_Left);
		LabelContentSlot->SetVerticalAlignment(VAlign_Center);
		LabelContentSlot->SetPadding(FMargin(10.0f, 0.0f, 14.0f, 0.0f));
	}
	if (UHorizontalBoxSlot* LabelSlot = BindingRow->AddChildToHorizontalBox(LabelSize))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
	}

	auto CreateBindingButton =
		[this, MappingName, bAxisMapping](
			const EVNHInputBindingDevice BindingDevice,
			const float BindingAxisScale,
			const FKey DisplayOverrideKey)
	{
		UVNHInputBindingButton* BindingButton =
			WidgetTree->ConstructWidget<UVNHInputBindingButton>(
				UVNHInputBindingButton::StaticClass());
		FButtonStyle IconButtonStyle;
		FSlateBrush NormalBrush;
		NormalBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
		FSlateBrush HoveredBrush = NormalBrush;
		HoveredBrush.DrawAs = ESlateBrushDrawType::Box;
		HoveredBrush.TintColor =
			FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 0.16f));
		FSlateBrush PressedBrush = NormalBrush;
		PressedBrush.DrawAs = ESlateBrushDrawType::Box;
		PressedBrush.TintColor =
			FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 0.28f));
		IconButtonStyle.SetNormal(NormalBrush);
		IconButtonStyle.SetHovered(HoveredBrush);
		IconButtonStyle.SetPressed(PressedBrush);
		IconButtonStyle.SetDisabled(NormalBrush);
		IconButtonStyle.SetNormalPadding(FMargin(3.0f));
		IconButtonStyle.SetPressedPadding(FMargin(3.0f));
		BindingButton->SetStyle(IconButtonStyle);

		UOverlay* ButtonContent =
			WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
		BindingButton->AddChild(ButtonContent);

		USizeBox* IconFrame =
			WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		IconFrame->SetWidthOverride(64.0f);
		IconFrame->SetHeightOverride(42.0f);
		UScaleBox* IconScaleBox =
			WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
		IconScaleBox->SetStretch(EStretch::ScaleToFit);
		IconScaleBox->SetStretchDirection(EStretchDirection::DownOnly);
		IconFrame->AddChild(IconScaleBox);
		UImage* PromptImage =
			WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		IconScaleBox->AddChild(PromptImage);
		if (UOverlaySlot* PromptSlot =
			ButtonContent->AddChildToOverlay(IconFrame))
		{
			PromptSlot->SetHorizontalAlignment(HAlign_Center);
			PromptSlot->SetVerticalAlignment(VAlign_Center);
		}

		UTextBlock* FallbackText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		FallbackText->SetColorAndOpacity(
			FSlateColor(FLinearColor(0.72f, 0.76f, 0.79f, 1.0f)));
		FallbackText->SetFont(FSlateFontInfo(
			FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 13)));
		FallbackText->SetJustification(ETextJustify::Center);
		if (UOverlaySlot* FallbackSlot = ButtonContent->AddChildToOverlay(FallbackText))
		{
			FallbackSlot->SetHorizontalAlignment(HAlign_Center);
			FallbackSlot->SetVerticalAlignment(VAlign_Center);
		}

		UBorder* WaitingPopup = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		WaitingPopup->SetBrushColor(FLinearColor(0.025f, 0.05f, 0.06f, 0.98f));
		WaitingPopup->SetPadding(FMargin(10.0f, 5.0f));
		WaitingPopup->SetVisibility(ESlateVisibility::Collapsed);
		UTextBlock* WaitingText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		WaitingText->SetText(NSLOCTEXT("VNH", "WaitingForInput", "PRESS INPUT"));
		WaitingText->SetColorAndOpacity(
			FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 1.0f)));
		WaitingText->SetFont(FSlateFontInfo(
			FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 11)));
		WaitingText->SetJustification(ETextJustify::Center);
		WaitingPopup->AddChild(WaitingText);
		if (UOverlaySlot* PopupSlot = ButtonContent->AddChildToOverlay(WaitingPopup))
		{
			PopupSlot->SetHorizontalAlignment(HAlign_Center);
			PopupSlot->SetVerticalAlignment(VAlign_Center);
		}

		BindingButton->InitializeBinding(
			this,
			MappingName,
			bAxisMapping,
			BindingAxisScale,
			BindingDevice,
			DisplayOverrideKey,
			PromptImage,
			FallbackText,
			WaitingPopup);
		InputBindingButtons.Add(BindingButton);
		return BindingButton;
	};

	auto AddBindingColumn =
		[this, BindingRow, &CreateBindingButton](
			const float ColumnWidth,
			const TOptional<EVNHInputBindingDevice> BindingDevice,
			const float BindingAxisScale,
			const FKey DisplayOverrideKey)
	{
		USizeBox* ColumnSize =
			WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		ColumnSize->SetWidthOverride(ColumnWidth);
		ColumnSize->SetHeightOverride(54.0f);
		UOverlay* ColumnContent =
			WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
		ColumnSize->AddChild(ColumnContent);

		if (!BindingDevice.IsSet())
		{
			UTextBlock* NotApplicableText =
				WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			NotApplicableText->SetText(NSLOCTEXT("VNH", "BindingNotApplicable", "-"));
			NotApplicableText->SetColorAndOpacity(
				FSlateColor(FLinearColor(0.30f, 0.33f, 0.36f, 1.0f)));
			NotApplicableText->SetJustification(ETextJustify::Center);
			if (UOverlaySlot* NotApplicableSlot =
				ColumnContent->AddChildToOverlay(NotApplicableText))
			{
				NotApplicableSlot->SetHorizontalAlignment(HAlign_Center);
				NotApplicableSlot->SetVerticalAlignment(VAlign_Center);
			}
		}
		else
		{
			UVNHInputBindingButton* BindingButton =
				CreateBindingButton(
					BindingDevice.GetValue(),
					BindingAxisScale,
					DisplayOverrideKey);
			if (UOverlaySlot* ButtonSlot =
				ColumnContent->AddChildToOverlay(BindingButton))
			{
				ButtonSlot->SetPadding(FMargin(5.0f, 3.0f));
				ButtonSlot->SetHorizontalAlignment(HAlign_Center);
				ButtonSlot->SetVerticalAlignment(VAlign_Center);
			}
		}

		if (UHorizontalBoxSlot* ColumnSlot =
			BindingRow->AddChildToHorizontalBox(ColumnSize))
		{
			ColumnSlot->SetHorizontalAlignment(HAlign_Center);
			ColumnSlot->SetVerticalAlignment(VAlign_Center);
		}
	};

	TOptional<EVNHInputBindingDevice> KeyboardMouseDevice;
	if (bAllowKeyboard || bAllowMouse)
	{
		KeyboardMouseDevice = EVNHInputBindingDevice::KeyboardMouse;
	}
	AddBindingColumn(240.0f, KeyboardMouseDevice, AxisScale, FKey());

	TOptional<EVNHInputBindingDevice> ControllerDevice;
	if (bAllowGamepad)
	{
		ControllerDevice = EVNHInputBindingDevice::Controller;
	}
	const float ControllerAxisScale =
		bAxisMapping && ControllerDisplayKey.IsValid()
			? 1.0f
			: AxisScale;
	AddBindingColumn(
		180.0f,
		ControllerDevice,
		ControllerAxisScale,
		ControllerDisplayKey);
}

void UVNHSettingsDialogWidget::RefreshInputBindingRows()
{
	for (UVNHInputBindingButton* BindingButton : InputBindingButtons)
	{
		if (BindingButton)
		{
			BindingButton->RefreshFromInputSettings();
		}
	}
}

void UVNHSettingsDialogWidget::BeginInputBindingCapture(
	UVNHInputBindingButton* BindingButton)
{
	EndInputBindingCapture();
	if (!BindingButton || !FSlateApplication::IsInitialized())
	{
		return;
	}

	ActiveInputBindingButton = BindingButton;
	ActiveInputBindingButton->SetWaitingForInput(true);
	InputBindingCaptureProcessor =
		MakeShared<FVNHInputBindingCaptureProcessor>(this);
	FSlateApplication::Get().RegisterInputPreProcessor(
		InputBindingCaptureProcessor,
		0);
	SetStatus(NSLOCTEXT(
		"VNH",
		"WaitingForInputStatus",
		"Press the new input. Escape cancels."));
}

void UVNHSettingsDialogWidget::EndInputBindingCapture()
{
	if (ActiveInputBindingButton)
	{
		ActiveInputBindingButton->SetWaitingForInput(false);
	}
	ActiveInputBindingButton = nullptr;

	if (InputBindingCaptureProcessor.IsValid()
		&& FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(
			InputBindingCaptureProcessor);
	}
	InputBindingCaptureProcessor.Reset();
}

bool UVNHSettingsDialogWidget::HandleCapturedInput(const FKey NewKey)
{
	UVNHInputBindingButton* BindingButton = ActiveInputBindingButton;
	if (!BindingButton || !NewKey.IsValid())
	{
		return false;
	}
	if (NewKey == EKeys::Escape)
	{
		EndInputBindingCapture();
		SetStatus(NSLOCTEXT("VNH", "BindingCancelled", "Input binding cancelled."));
		return true;
	}

	const EVNHInputBindingDevice BindingDevice =
		BindingButton->GetBindingDevice();
	if (!UVNHInputPromptLibrary::IsKeyForBindingDevice(NewKey, BindingDevice))
	{
		switch (BindingDevice)
		{
		case EVNHInputBindingDevice::Mouse:
			SetStatus(NSLOCTEXT("VNH", "MouseBindingRequired", "Choose a mouse input for the mouse column."));
			break;
		case EVNHInputBindingDevice::Controller:
			SetStatus(NSLOCTEXT("VNH", "ControllerBindingRequired", "Choose a controller input for the controller column."));
			break;
		case EVNHInputBindingDevice::KeyboardMouse:
			SetStatus(NSLOCTEXT("VNH", "KeyboardMouseBindingRequired", "Choose a keyboard or mouse input for the keyboard + mouse column."));
			break;
		default:
			SetStatus(NSLOCTEXT("VNH", "KeyboardBindingRequired", "Choose a keyboard input for the keyboard column."));
			break;
		}
		return true;
	}

	EndInputBindingCapture();
	const bool bBindingUpdated = BindingButton->IsAxisMapping()
		? UVNHInputPromptLibrary::RebindAxisForDevice(
			BindingButton->GetMappingName(),
			BindingButton->GetAxisScale(),
			NewKey,
			BindingDevice)
		: UVNHInputPromptLibrary::RebindActionForDevice(
			BindingButton->GetMappingName(),
			NewKey,
			BindingDevice);
	if (!bBindingUpdated)
	{
		SetStatus(NSLOCTEXT("VNH", "BindingFailed", "That input could not be rebound."));
		BindingButton->RefreshFromInputSettings();
		return true;
	}

	UVNHInputPromptLibrary::RefreshPlayerInput(GetOwningPlayer());
	if (AVNHPlayerController* ResolvedPlayerController =
		Cast<AVNHPlayerController>(GetOwningPlayer()))
	{
		ResolvedPlayerController->RefreshRuntimeInputMappings();
	}
	RefreshInputBindingRows();
	SetStatus(FText::Format(
		NSLOCTEXT("VNH", "BindingUpdated", "{0} updated."),
		FText::FromName(BindingButton->GetMappingName())));
	return true;
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
