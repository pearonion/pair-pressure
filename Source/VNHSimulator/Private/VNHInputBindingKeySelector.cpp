#include "VNHInputBindingKeySelector.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "VNHInputPromptLibrary.h"
#include "VNHSettingsDialogWidget.h"

void UVNHInputBindingKeySelector::InitializeBinding(
	UVNHSettingsDialogWidget* InSettingsOwner,
	const FName InMappingName,
	const bool bInAxisMapping,
	const float InAxisScale,
	const EVNHInputBindingDevice InBindingDevice,
	UImage* InPromptImage,
	UTextBlock* InFallbackText,
	UWidget* InWaitingPopup)
{
	SettingsOwner = InSettingsOwner;
	MappingName = InMappingName;
	bAxisMapping = bInAxisMapping;
	AxisScale = InAxisScale;
	BindingDevice = InBindingDevice;
	PromptImage = InPromptImage;
	FallbackText = InFallbackText;
	WaitingPopup = InWaitingPopup;

	SetAllowModifierKeys(false);
	SetAllowGamepadKeys(BindingDevice == EVNHInputBindingDevice::Controller);
	SetKeySelectionText(FText::GetEmpty());
	SetNoKeySpecifiedText(NSLOCTEXT("VNH", "UnboundBinding", "UNBOUND"));
	SetTextBlockVisibility(ESlateVisibility::Collapsed);
	SetMargin(FMargin(0.0f));

	FButtonStyle IconButtonStyle;
	FSlateBrush NormalBrush;
	NormalBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
	FSlateBrush HoveredBrush;
	HoveredBrush.DrawAs = ESlateBrushDrawType::Box;
	HoveredBrush.TintColor = FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 0.10f));
	FSlateBrush PressedBrush = HoveredBrush;
	PressedBrush.TintColor = FSlateColor(FLinearColor(0.0f, 0.92f, 0.78f, 0.20f));
	IconButtonStyle.SetNormal(NormalBrush);
	IconButtonStyle.SetHovered(HoveredBrush);
	IconButtonStyle.SetPressed(PressedBrush);
	IconButtonStyle.SetDisabled(NormalBrush);
	IconButtonStyle.SetNormalPadding(FMargin(0.0f));
	IconButtonStyle.SetPressedPadding(FMargin(0.0f));
	SetButtonStyle(IconButtonStyle);

	const TArray<FKey> EscapeBindingKeys = {EKeys::Escape, EKeys::Gamepad_Special_Right};
	SetEscapeKeys(EscapeBindingKeys);
	OnKeySelected.AddUniqueDynamic(this, &UVNHInputBindingKeySelector::HandleSelectedKey);
	OnIsSelectingKeyChanged.AddUniqueDynamic(
		this,
		&UVNHInputBindingKeySelector::HandleIsSelectingKeyChanged);
	RefreshFromInputSettings();
}

void UVNHInputBindingKeySelector::RefreshFromInputSettings()
{
	const FKey BoundKey = bAxisMapping
		? UVNHInputPromptLibrary::GetPrimaryAxisKeyForDevice(
			MappingName,
			AxisScale,
			BindingDevice)
		: UVNHInputPromptLibrary::GetPrimaryActionKeyForDevice(
			MappingName,
			BindingDevice);
	SetSelectedKey(FInputChord(BoundKey));

	if (PromptImage)
	{
		const EVNHInputPromptFamily PromptFamily =
			BindingDevice == EVNHInputBindingDevice::Controller
			? UVNHInputPromptLibrary::GetPromptFamily(SettingsOwner ? SettingsOwner->GetOwningPlayer() : nullptr)
			: EVNHInputPromptFamily::KeyboardMouse;
		if (UTexture2D* PromptTexture = UVNHInputPromptLibrary::GetKeyIcon(BoundKey, PromptFamily))
		{
			PromptImage->SetBrushFromTexture(PromptTexture, true);
			PromptImage->SetVisibility(ESlateVisibility::HitTestInvisible);
			if (FallbackText)
			{
				FallbackText->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		else
		{
			PromptImage->SetVisibility(ESlateVisibility::Collapsed);
			if (FallbackText)
			{
				FallbackText->SetText(BoundKey.IsValid()
					? UVNHInputPromptLibrary::GetKeyDisplayText(BoundKey, PromptFamily)
					: NSLOCTEXT("VNH", "AddInputBinding", "+"));
				FallbackText->SetVisibility(ESlateVisibility::HitTestInvisible);
			}
		}
	}
}

void UVNHInputBindingKeySelector::HandleSelectedKey(const FInputChord SelectedChord)
{
	if (SettingsOwner)
	{
		// This selector is retained only for serialized-class compatibility.
		// The settings grid now uses UVNHInputBindingButton and its global
		// next-input capture path.
		SettingsOwner->HandleCapturedInput(SelectedChord.Key);
	}
}

void UVNHInputBindingKeySelector::HandleIsSelectingKeyChanged()
{
	if (WaitingPopup)
	{
		WaitingPopup->SetVisibility(
			GetIsSelectingKey()
				? ESlateVisibility::HitTestInvisible
				: ESlateVisibility::Collapsed);
	}
}
