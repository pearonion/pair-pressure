#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VNHInputPromptLibrary.generated.h"

class APlayerController;

UENUM(BlueprintType)
enum class EVNHInputPromptFamily : uint8
{
	KeyboardMouse,
	Xbox,
	PlayStation4,
	PlayStation5
};

enum class EVNHInputBindingDevice : uint8
{
	Mouse,
	Keyboard,
	Controller,
	KeyboardMouse
};

/**
 * Shared source of truth for runtime input bindings and prompt artwork.
 * The prompt pack is resolved by convention so newly-supported FKeys do not
 * require a hand-authored DataTable entry.
 */
UCLASS()
class VNHSIMULATOR_API UVNHInputPromptLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static bool IsGamepadConnected(const APlayerController* PlayerController);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static bool ShouldUseGamepadPrompts(const APlayerController* PlayerController);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static EVNHInputPromptFamily GetPromptFamily(const APlayerController* PlayerController);

	/** Controller-only prompt family. Defaults to Xbox until PlayStation hardware is detected. */
	static EVNHInputPromptFamily GetControllerPromptFamily(const APlayerController* PlayerController);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static FKey GetPrimaryActionKey(FName ActionName, bool bGamepad);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static FKey GetPrimaryAxisKey(FName AxisName, float Scale, bool bGamepad);

	static FKey GetPrimaryActionKeyForDevice(
		FName ActionName,
		EVNHInputBindingDevice BindingDevice);
	static FKey GetPrimaryAxisKeyForDevice(
		FName AxisName,
		float Scale,
		EVNHInputBindingDevice BindingDevice);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static UTexture2D* GetKeyIcon(FKey Key, EVNHInputPromptFamily PromptFamily);

	UFUNCTION(BlueprintPure, Category = "VNH|Input Prompts")
	static FText GetKeyDisplayText(FKey Key, EVNHInputPromptFamily PromptFamily);

	UFUNCTION(BlueprintCallable, Category = "VNH|Input Prompts")
	static bool RebindAction(FName ActionName, FKey NewKey, bool bGamepad);

	UFUNCTION(BlueprintCallable, Category = "VNH|Input Prompts")
	static bool RebindAxis(FName AxisName, float Scale, FKey NewKey, bool bGamepad);

	static bool RebindActionForDevice(
		FName ActionName,
		FKey NewKey,
		EVNHInputBindingDevice BindingDevice);
	static bool RebindAxisForDevice(
		FName AxisName,
		float Scale,
		FKey NewKey,
		EVNHInputBindingDevice BindingDevice);
	static bool IsKeyForBindingDevice(FKey Key, EVNHInputBindingDevice BindingDevice);

	static void RefreshPlayerInput(APlayerController* PlayerController);

private:
	static FString GetKeyboardMouseTextureToken(FKey Key);
	static FString GetGamepadTextureToken(FKey Key, EVNHInputPromptFamily PromptFamily);
	static FString GetControllerIdentifier(const APlayerController* PlayerController);
};
