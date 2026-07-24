#pragma once

#include "CoreMinimal.h"
#include "Components/InputKeySelector.h"
#include "VNHInputBindingKeySelector.generated.h"

class UImage;
class UVNHSettingsDialogWidget;

/** Runtime key selector carrying the binding identity for one settings cell. */
UCLASS()
class VNHSIMULATOR_API UVNHInputBindingKeySelector : public UInputKeySelector
{
	GENERATED_BODY()

public:
	void InitializeBinding(
		UVNHSettingsDialogWidget* InSettingsOwner,
		FName InMappingName,
		bool bInAxisMapping,
		float InAxisScale,
		bool bInGamepad,
		UImage* InPromptImage);

	void RefreshFromInputSettings();

	FName GetMappingName() const { return MappingName; }
	bool IsAxisMapping() const { return bAxisMapping; }
	float GetAxisScale() const { return AxisScale; }
	bool IsGamepadBinding() const { return bGamepad; }

private:
	UFUNCTION()
	void HandleSelectedKey(FInputChord SelectedChord);

	UPROPERTY(Transient)
	TObjectPtr<UVNHSettingsDialogWidget> SettingsOwner;

	UPROPERTY(Transient)
	TObjectPtr<UImage> PromptImage;

	FName MappingName;
	float AxisScale = 1.0f;
	bool bAxisMapping = false;
	bool bGamepad = false;
};
