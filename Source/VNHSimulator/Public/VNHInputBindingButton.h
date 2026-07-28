#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "VNHInputPromptLibrary.h"
#include "VNHInputBindingButton.generated.h"

class UImage;
class UTextBlock;
class UWidget;
class UVNHSettingsDialogWidget;

/** Stable icon button carrying the binding identity for one settings cell. */
UCLASS()
class VNHSIMULATOR_API UVNHInputBindingButton : public UButton
{
	GENERATED_BODY()

public:
	void InitializeBinding(
		UVNHSettingsDialogWidget* InSettingsOwner,
		FName InMappingName,
		bool bInAxisMapping,
		float InAxisScale,
		EVNHInputBindingDevice InBindingDevice,
		FKey InDisplayOverrideKey,
		UImage* InPromptImage,
		UTextBlock* InFallbackText,
		UWidget* InWaitingPopup);

	void RefreshFromInputSettings();
	void SetWaitingForInput(bool bWaitingForInput);

	FName GetMappingName() const { return MappingName; }
	bool IsAxisMapping() const { return bAxisMapping; }
	float GetAxisScale() const { return AxisScale; }
	EVNHInputBindingDevice GetBindingDevice() const { return BindingDevice; }

private:
	UFUNCTION()
	void HandleClicked();

	UPROPERTY(Transient)
	TObjectPtr<UVNHSettingsDialogWidget> SettingsOwner;

	UPROPERTY(Transient)
	TObjectPtr<UImage> PromptImage;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> FallbackText;

	UPROPERTY(Transient)
	TObjectPtr<UWidget> WaitingPopup;

	FName MappingName;
	FKey DisplayOverrideKey;
	float AxisScale = 1.0f;
	bool bAxisMapping = false;
	EVNHInputBindingDevice BindingDevice = EVNHInputBindingDevice::Keyboard;
};
