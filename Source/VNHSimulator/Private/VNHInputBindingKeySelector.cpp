#include "VNHInputBindingKeySelector.h"

#include "Components/Image.h"
#include "VNHInputPromptLibrary.h"
#include "VNHSettingsDialogWidget.h"

void UVNHInputBindingKeySelector::InitializeBinding(
	UVNHSettingsDialogWidget* InSettingsOwner,
	const FName InMappingName,
	const bool bInAxisMapping,
	const float InAxisScale,
	const bool bInGamepad,
	UImage* InPromptImage)
{
	SettingsOwner = InSettingsOwner;
	MappingName = InMappingName;
	bAxisMapping = bInAxisMapping;
	AxisScale = InAxisScale;
	bGamepad = bInGamepad;
	PromptImage = InPromptImage;

	SetAllowModifierKeys(false);
	SetAllowGamepadKeys(bGamepad);
	SetKeySelectionText(NSLOCTEXT("VNH", "PressNewBinding", "PRESS A KEY"));
	SetNoKeySpecifiedText(NSLOCTEXT("VNH", "UnboundBinding", "UNBOUND"));
	const TArray<FKey> EscapeBindingKeys = {EKeys::Escape, EKeys::Gamepad_Special_Right};
	SetEscapeKeys(EscapeBindingKeys);
	OnKeySelected.AddUniqueDynamic(this, &UVNHInputBindingKeySelector::HandleSelectedKey);
	RefreshFromInputSettings();
}

void UVNHInputBindingKeySelector::RefreshFromInputSettings()
{
	const FKey BoundKey = bAxisMapping
		? UVNHInputPromptLibrary::GetPrimaryAxisKey(MappingName, AxisScale, bGamepad)
		: UVNHInputPromptLibrary::GetPrimaryActionKey(MappingName, bGamepad);
	SetSelectedKey(FInputChord(BoundKey));

	if (PromptImage)
	{
		const EVNHInputPromptFamily PromptFamily = bGamepad
			? UVNHInputPromptLibrary::GetPromptFamily(SettingsOwner ? SettingsOwner->GetOwningPlayer() : nullptr)
			: EVNHInputPromptFamily::KeyboardMouse;
		if (UTexture2D* PromptTexture = UVNHInputPromptLibrary::GetKeyIcon(BoundKey, PromptFamily))
		{
			PromptImage->SetBrushFromTexture(PromptTexture, true);
			PromptImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			PromptImage->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
}

void UVNHInputBindingKeySelector::HandleSelectedKey(const FInputChord SelectedChord)
{
	if (SettingsOwner)
	{
		SettingsOwner->HandleInputBindingSelected(this, SelectedChord.Key);
	}
}
