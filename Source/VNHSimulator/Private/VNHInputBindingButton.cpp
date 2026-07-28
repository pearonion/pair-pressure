#include "VNHInputBindingButton.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "VNHSettingsDialogWidget.h"

void UVNHInputBindingButton::InitializeBinding(
	UVNHSettingsDialogWidget* InSettingsOwner,
	const FName InMappingName,
	const bool bInAxisMapping,
	const float InAxisScale,
	const EVNHInputBindingDevice InBindingDevice,
	const FKey InDisplayOverrideKey,
	UImage* InPromptImage,
	UTextBlock* InFallbackText,
	UWidget* InWaitingPopup)
{
	SettingsOwner = InSettingsOwner;
	MappingName = InMappingName;
	bAxisMapping = bInAxisMapping;
	AxisScale = InAxisScale;
	BindingDevice = InBindingDevice;
	DisplayOverrideKey = InDisplayOverrideKey;
	PromptImage = InPromptImage;
	FallbackText = InFallbackText;
	WaitingPopup = InWaitingPopup;

	OnClicked.AddUniqueDynamic(this, &UVNHInputBindingButton::HandleClicked);
	SetWaitingForInput(false);
	RefreshFromInputSettings();
}

void UVNHInputBindingButton::RefreshFromInputSettings()
{
	const FKey BoundKey = bAxisMapping
		? UVNHInputPromptLibrary::GetPrimaryAxisKeyForDevice(
			MappingName,
			AxisScale,
			BindingDevice)
		: UVNHInputPromptLibrary::GetPrimaryActionKeyForDevice(
			MappingName,
			BindingDevice);
	const bool bBoundToSharedLeftStickAxis =
		BoundKey == EKeys::Gamepad_LeftX
		|| BoundKey == EKeys::Gamepad_LeftY;
	const FKey DisplayKey =
		DisplayOverrideKey.IsValid() && bBoundToSharedLeftStickAxis
			? DisplayOverrideKey
			: BoundKey;
	const EVNHInputPromptFamily PromptFamily =
		BindingDevice == EVNHInputBindingDevice::Controller
		? UVNHInputPromptLibrary::GetControllerPromptFamily(
			SettingsOwner ? SettingsOwner->GetOwningPlayer() : nullptr)
		: EVNHInputPromptFamily::KeyboardMouse;

	if (UTexture2D* PromptTexture =
		UVNHInputPromptLibrary::GetKeyIcon(DisplayKey, PromptFamily))
	{
		PromptImage->SetBrushFromTexture(PromptTexture, true);
		PromptImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		FallbackText->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	PromptImage->SetVisibility(ESlateVisibility::Collapsed);
	FallbackText->SetText(DisplayKey.IsValid()
		? UVNHInputPromptLibrary::GetKeyDisplayText(DisplayKey, PromptFamily)
		: NSLOCTEXT("VNH", "AddInputBinding", "+"));
	FallbackText->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UVNHInputBindingButton::SetWaitingForInput(const bool bWaitingForInput)
{
	if (WaitingPopup)
	{
		WaitingPopup->SetVisibility(
			bWaitingForInput
				? ESlateVisibility::HitTestInvisible
				: ESlateVisibility::Collapsed);
	}
}

void UVNHInputBindingButton::HandleClicked()
{
	if (SettingsOwner)
	{
		SettingsOwner->BeginInputBindingCapture(this);
	}
}
