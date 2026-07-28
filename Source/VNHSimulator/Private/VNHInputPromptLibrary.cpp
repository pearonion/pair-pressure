#include "VNHInputPromptLibrary.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/InputDeviceSubsystem.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
constexpr const TCHAR* VNHInputPromptSettingsSection = TEXT("/Script/VNHSimulator.VNHSettings");

bool IsVNHGamepadDevice(
	const FInputDeviceId InputDeviceId,
	const UInputDeviceSubsystem* InputDeviceSubsystem)
{
	if (!InputDeviceSubsystem)
	{
		return false;
	}

	const FHardwareDeviceIdentifier HardwareIdentifier =
		InputDeviceSubsystem->GetInputDeviceHardwareIdentifier(InputDeviceId);
	return HardwareIdentifier.PrimaryDeviceType == EHardwareDevicePrimaryType::Gamepad
		|| HardwareIdentifier == FHardwareDeviceIdentifier::DefaultGamepad
		|| HardwareIdentifier.ToString().Contains(TEXT("Gamepad"), ESearchCase::IgnoreCase)
		|| HardwareIdentifier.ToString().Contains(TEXT("Controller"), ESearchCase::IgnoreCase)
		|| HardwareIdentifier.ToString().Contains(TEXT("DualSense"), ESearchCase::IgnoreCase)
		|| HardwareIdentifier.ToString().Contains(TEXT("DualShock"), ESearchCase::IgnoreCase);
}

FString VNHKeyboardTexturePath(const FString& TextureToken)
{
	return FString::Printf(
		TEXT("/Game/Input_Prompts_Pack/Keyboard_Mouse/White/T_%s_Key_White.T_%s_Key_White"),
		*TextureToken,
		*TextureToken);
}

FString VNHGamepadTexturePath(
	const FString& TextureToken,
	const EVNHInputPromptFamily PromptFamily)
{
	const TCHAR* Folder = TEXT("XGamepad");
	const TCHAR* Prefix = TEXT("X");
	if (PromptFamily == EVNHInputPromptFamily::PlayStation4)
	{
		Folder = TEXT("P4Gamepad");
		Prefix = TEXT("P4");
	}
	else if (PromptFamily == EVNHInputPromptFamily::PlayStation5)
	{
		Folder = TEXT("P5Gamepad");
		Prefix = TEXT("P5");
	}

	const FString AssetName =
		FString::Printf(TEXT("T_%s_%s_Light"), Prefix, *TextureToken);
	return FString::Printf(
		TEXT("/Game/Input_Prompts_Pack/%s/Light/%s.%s"),
		Folder,
		*AssetName,
		*AssetName);
}

FString VNHResolvePlayStationFaceName(const FString& XboxToken)
{
	if (XboxToken == TEXT("A_Color")) return TEXT("Cross_Color");
	if (XboxToken == TEXT("B_Color")) return TEXT("Circle_Color");
	if (XboxToken == TEXT("X_Color")) return TEXT("Square_Color");
	if (XboxToken == TEXT("Y_Color")) return TEXT("Triangle_Color");
	if (XboxToken == TEXT("LB")) return TEXT("L1");
	if (XboxToken == TEXT("RB")) return TEXT("R1");
	if (XboxToken == TEXT("LT")) return TEXT("L2");
	if (XboxToken == TEXT("RT")) return TEXT("R2");
	if (XboxToken == TEXT("Left_Stick_Click")) return TEXT("Left_Stick_Click");
	if (XboxToken == TEXT("Right_Stick_Click")) return TEXT("Right_Stick_Click");
	if (XboxToken == TEXT("Share")) return TEXT("Share");
	if (XboxToken == TEXT("Menu")) return TEXT("Options");
	return XboxToken;
}
}

bool UVNHInputPromptLibrary::IsGamepadConnected(const APlayerController* PlayerController)
{
	const FPlatformUserId PlatformUserId = PlayerController
		? PlayerController->GetPlatformUserId()
		: IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	TArray<FInputDeviceId> ConnectedInputDevices;
	IPlatformInputDeviceMapper::Get().GetAllConnectedInputDevicesForUser(
		PlatformUserId,
		ConnectedInputDevices);

	const UInputDeviceSubsystem* InputDeviceSubsystem = GEngine
		? GEngine->GetEngineSubsystem<UInputDeviceSubsystem>()
		: nullptr;
	const FInputDeviceId DefaultInputDevice =
		IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
	for (const FInputDeviceId InputDeviceId : ConnectedInputDevices)
	{
		if (IsVNHGamepadDevice(InputDeviceId, InputDeviceSubsystem))
		{
			return true;
		}

		// On Windows the platform layer can report a newly connected XInput
		// device before its hardware descriptor is populated. The default input
		// device is keyboard/mouse; any additional connected device is therefore
		// controller-priority until the descriptor arrives.
		if (InputDeviceId != DefaultInputDevice)
		{
			return true;
		}
	}
	return false;
}

bool UVNHInputPromptLibrary::ShouldUseGamepadPrompts(const APlayerController* PlayerController)
{
	// A connected controller always takes prompt priority, including for older
	// save files that stored the previous Keyboard & Mouse preset.
	if (IsGamepadConnected(PlayerController))
	{
		return true;
	}

	FString InputPreset = TEXT("Auto");
	if (GConfig)
	{
		GConfig->GetString(
			VNHInputPromptSettingsSection,
			TEXT("InputPreset"),
			InputPreset,
			GGameUserSettingsIni);
	}

	if (InputPreset == TEXT("Controller"))
	{
		return true;
	}
	return false;
}

EVNHInputPromptFamily UVNHInputPromptLibrary::GetPromptFamily(const APlayerController* PlayerController)
{
	if (!ShouldUseGamepadPrompts(PlayerController))
	{
		return EVNHInputPromptFamily::KeyboardMouse;
	}

	return GetControllerPromptFamily(PlayerController);
}

EVNHInputPromptFamily UVNHInputPromptLibrary::GetControllerPromptFamily(
	const APlayerController* PlayerController)
{
	const FString ControllerIdentifier = GetControllerIdentifier(PlayerController);
	if (ControllerIdentifier.Contains(TEXT("DualSense"), ESearchCase::IgnoreCase)
		|| ControllerIdentifier.Contains(TEXT("PS5"), ESearchCase::IgnoreCase)
		|| ControllerIdentifier.Contains(TEXT("PlayStation 5"), ESearchCase::IgnoreCase))
	{
		return EVNHInputPromptFamily::PlayStation5;
	}
	if (ControllerIdentifier.Contains(TEXT("DualShock"), ESearchCase::IgnoreCase)
		|| ControllerIdentifier.Contains(TEXT("PS4"), ESearchCase::IgnoreCase)
		|| ControllerIdentifier.Contains(TEXT("PlayStation 4"), ESearchCase::IgnoreCase))
	{
		return EVNHInputPromptFamily::PlayStation4;
	}
	return EVNHInputPromptFamily::Xbox;
}

FKey UVNHInputPromptLibrary::GetPrimaryActionKey(const FName ActionName, const bool bGamepad)
{
	TArray<FInputActionKeyMapping> ActionMappings;
	if (const UInputSettings* InputSettings = UInputSettings::GetInputSettings())
	{
		InputSettings->GetActionMappingByName(ActionName, ActionMappings);
	}
	for (const FInputActionKeyMapping& ActionMapping : ActionMappings)
	{
		if (ActionMapping.Key.IsGamepadKey() == bGamepad)
		{
			return ActionMapping.Key;
		}
	}
	return FKey();
}

FKey UVNHInputPromptLibrary::GetPrimaryAxisKey(
	const FName AxisName,
	const float Scale,
	const bool bGamepad)
{
	TArray<FInputAxisKeyMapping> AxisMappings;
	if (const UInputSettings* InputSettings = UInputSettings::GetInputSettings())
	{
		InputSettings->GetAxisMappingByName(AxisName, AxisMappings);
	}
	for (const FInputAxisKeyMapping& AxisMapping : AxisMappings)
	{
		if (AxisMapping.Key.IsGamepadKey() == bGamepad
			&& FMath::IsNearlyEqual(AxisMapping.Scale, Scale))
		{
			return AxisMapping.Key;
		}
	}
	return FKey();
}

bool UVNHInputPromptLibrary::IsKeyForBindingDevice(
	const FKey Key,
	const EVNHInputBindingDevice BindingDevice)
{
	const bool bMouseKey = Key.IsMouseButton()
		|| Key.GetFName().ToString().StartsWith(TEXT("Mouse"));
	switch (BindingDevice)
	{
	case EVNHInputBindingDevice::Mouse:
		return bMouseKey;
	case EVNHInputBindingDevice::Controller:
		return Key.IsGamepadKey();
	case EVNHInputBindingDevice::KeyboardMouse:
		return !Key.IsGamepadKey();
	default:
		return !Key.IsGamepadKey() && !bMouseKey;
	}
}

FKey UVNHInputPromptLibrary::GetPrimaryActionKeyForDevice(
	const FName ActionName,
	const EVNHInputBindingDevice BindingDevice)
{
	TArray<FInputActionKeyMapping> ActionMappings;
	if (const UInputSettings* InputSettings = UInputSettings::GetInputSettings())
	{
		InputSettings->GetActionMappingByName(ActionName, ActionMappings);
	}
	for (const FInputActionKeyMapping& ActionMapping : ActionMappings)
	{
		if (IsKeyForBindingDevice(ActionMapping.Key, BindingDevice))
		{
			return ActionMapping.Key;
		}
	}
	return FKey();
}

FKey UVNHInputPromptLibrary::GetPrimaryAxisKeyForDevice(
	const FName AxisName,
	const float Scale,
	const EVNHInputBindingDevice BindingDevice)
{
	TArray<FInputAxisKeyMapping> AxisMappings;
	if (const UInputSettings* InputSettings = UInputSettings::GetInputSettings())
	{
		InputSettings->GetAxisMappingByName(AxisName, AxisMappings);
	}
	for (const FInputAxisKeyMapping& AxisMapping : AxisMappings)
	{
		if (IsKeyForBindingDevice(AxisMapping.Key, BindingDevice)
			&& FMath::IsNearlyEqual(AxisMapping.Scale, Scale))
		{
			return AxisMapping.Key;
		}
	}
	return FKey();
}

UTexture2D* UVNHInputPromptLibrary::GetKeyIcon(
	const FKey Key,
	const EVNHInputPromptFamily PromptFamily)
{
	if (!Key.IsValid())
	{
		return nullptr;
	}

	const bool bUseGamepadArt = PromptFamily != EVNHInputPromptFamily::KeyboardMouse
		&& Key.IsGamepadKey();
	FString TexturePath;
	if (bUseGamepadArt)
	{
		const FString TextureToken = GetGamepadTextureToken(Key, PromptFamily);
		if (!TextureToken.IsEmpty())
		{
			TexturePath = VNHGamepadTexturePath(TextureToken, PromptFamily);
		}
	}
	else
	{
		const FString TextureToken = GetKeyboardMouseTextureToken(Key);
		if (!TextureToken.IsEmpty())
		{
			TexturePath = VNHKeyboardTexturePath(TextureToken);
		}
	}

	if (TexturePath.IsEmpty())
	{
		return nullptr;
	}

	return LoadObject<UTexture2D>(nullptr, *TexturePath);
}

FText UVNHInputPromptLibrary::GetKeyDisplayText(
	const FKey Key,
	const EVNHInputPromptFamily PromptFamily)
{
	if (!Key.IsValid())
	{
		return NSLOCTEXT("VNH", "InputUnbound", "UNBOUND");
	}

	const FString XboxToken = GetGamepadTextureToken(Key, EVNHInputPromptFamily::Xbox);
	if (Key.IsGamepadKey() && PromptFamily != EVNHInputPromptFamily::KeyboardMouse)
	{
		const FString DisplayToken = PromptFamily == EVNHInputPromptFamily::Xbox
			? XboxToken
			: VNHResolvePlayStationFaceName(XboxToken);
		return FText::FromString(DisplayToken.Replace(TEXT("_Color"), TEXT("")).Replace(TEXT("_"), TEXT(" ")));
	}
	return Key.GetDisplayName();
}

bool UVNHInputPromptLibrary::RebindAction(
	const FName ActionName,
	const FKey NewKey,
	const bool bGamepad)
{
	if (!NewKey.IsValid() || NewKey.IsGamepadKey() != bGamepad)
	{
		return false;
	}

	UInputSettings* InputSettings = UInputSettings::GetInputSettings();
	if (!InputSettings)
	{
		return false;
	}

	TArray<FInputActionKeyMapping> ExistingMappings;
	InputSettings->GetActionMappingByName(ActionName, ExistingMappings);
	for (const FInputActionKeyMapping& ExistingMapping : ExistingMappings)
	{
		if (ExistingMapping.Key.IsGamepadKey() == bGamepad)
		{
			InputSettings->RemoveActionMapping(ExistingMapping, false);
		}
	}
	InputSettings->AddActionMapping(FInputActionKeyMapping(ActionName, NewKey), false);
	InputSettings->SaveKeyMappings();
	return true;
}

bool UVNHInputPromptLibrary::RebindAxis(
	const FName AxisName,
	const float Scale,
	const FKey NewKey,
	const bool bGamepad)
{
	if (!NewKey.IsValid() || NewKey.IsGamepadKey() != bGamepad)
	{
		return false;
	}

	UInputSettings* InputSettings = UInputSettings::GetInputSettings();
	if (!InputSettings)
	{
		return false;
	}

	TArray<FInputAxisKeyMapping> ExistingMappings;
	InputSettings->GetAxisMappingByName(AxisName, ExistingMappings);
	for (const FInputAxisKeyMapping& ExistingMapping : ExistingMappings)
	{
		if (ExistingMapping.Key.IsGamepadKey() == bGamepad
			&& FMath::IsNearlyEqual(ExistingMapping.Scale, Scale))
		{
			InputSettings->RemoveAxisMapping(ExistingMapping, false);
		}
	}
	InputSettings->AddAxisMapping(FInputAxisKeyMapping(AxisName, NewKey, Scale), false);
	InputSettings->SaveKeyMappings();
	return true;
}

bool UVNHInputPromptLibrary::RebindActionForDevice(
	const FName ActionName,
	const FKey NewKey,
	const EVNHInputBindingDevice BindingDevice)
{
	if (!NewKey.IsValid() || !IsKeyForBindingDevice(NewKey, BindingDevice))
	{
		return false;
	}

	UInputSettings* InputSettings = UInputSettings::GetInputSettings();
	if (!InputSettings)
	{
		return false;
	}

	TArray<FInputActionKeyMapping> ExistingMappings;
	InputSettings->GetActionMappingByName(ActionName, ExistingMappings);
	for (const FInputActionKeyMapping& ExistingMapping : ExistingMappings)
	{
		if (IsKeyForBindingDevice(ExistingMapping.Key, BindingDevice))
		{
			InputSettings->RemoveActionMapping(ExistingMapping, false);
		}
	}
	InputSettings->AddActionMapping(FInputActionKeyMapping(ActionName, NewKey), false);
	InputSettings->SaveKeyMappings();
	return true;
}

bool UVNHInputPromptLibrary::RebindAxisForDevice(
	const FName AxisName,
	const float Scale,
	const FKey NewKey,
	const EVNHInputBindingDevice BindingDevice)
{
	if (!NewKey.IsValid() || !IsKeyForBindingDevice(NewKey, BindingDevice))
	{
		return false;
	}

	UInputSettings* InputSettings = UInputSettings::GetInputSettings();
	if (!InputSettings)
	{
		return false;
	}

	TArray<FInputAxisKeyMapping> ExistingMappings;
	InputSettings->GetAxisMappingByName(AxisName, ExistingMappings);
	for (const FInputAxisKeyMapping& ExistingMapping : ExistingMappings)
	{
		if (IsKeyForBindingDevice(ExistingMapping.Key, BindingDevice)
			&& FMath::IsNearlyEqual(ExistingMapping.Scale, Scale))
		{
			InputSettings->RemoveAxisMapping(ExistingMapping, false);
		}
	}
	InputSettings->AddAxisMapping(FInputAxisKeyMapping(AxisName, NewKey, Scale), false);
	InputSettings->SaveKeyMappings();
	return true;
}

void UVNHInputPromptLibrary::RefreshPlayerInput(APlayerController* PlayerController)
{
	if (PlayerController && PlayerController->PlayerInput)
	{
		PlayerController->PlayerInput->ForceRebuildingKeyMaps(false);
	}
}

FString UVNHInputPromptLibrary::GetKeyboardMouseTextureToken(const FKey Key)
{
	const FString KeyName = Key.GetFName().ToString();
	if (KeyName.Len() == 1 && FChar::IsAlnum(KeyName[0]))
	{
		return KeyName.ToUpper();
	}

	static const TMap<FName, FString> TextureTokens =
	{
		{EKeys::SpaceBar.GetFName(), TEXT("Space")},
		{EKeys::Tab.GetFName(), TEXT("Tab")},
		{EKeys::Enter.GetFName(), TEXT("Enter")},
		{EKeys::Escape.GetFName(), TEXT("Esc")},
		{EKeys::BackSpace.GetFName(), TEXT("BackSpace")},
		{EKeys::LeftShift.GetFName(), TEXT("Shift")},
		{EKeys::RightShift.GetFName(), TEXT("Shift")},
		{EKeys::LeftControl.GetFName(), TEXT("Crtl")},
		{EKeys::RightControl.GetFName(), TEXT("Crtl")},
		{EKeys::LeftAlt.GetFName(), TEXT("Alt")},
		{EKeys::RightAlt.GetFName(), TEXT("Alt")},
		{EKeys::CapsLock.GetFName(), TEXT("CapsLock")},
		{EKeys::Insert.GetFName(), TEXT("Ins")},
		{EKeys::Delete.GetFName(), TEXT("Del")},
		{EKeys::Home.GetFName(), TEXT("Home")},
		{EKeys::End.GetFName(), TEXT("End")},
		{EKeys::PageUp.GetFName(), TEXT("PageUp")},
		{EKeys::PageDown.GetFName(), TEXT("PageDown")},
		{FName(TEXT("PrintScreen")), TEXT("PrtScrn")},
		{EKeys::NumLock.GetFName(), TEXT("NumLock")},
		{EKeys::NumPadZero.GetFName(), TEXT("0")},
		{EKeys::NumPadOne.GetFName(), TEXT("1")},
		{EKeys::NumPadTwo.GetFName(), TEXT("2")},
		{EKeys::NumPadThree.GetFName(), TEXT("3")},
		{EKeys::NumPadFour.GetFName(), TEXT("4")},
		{EKeys::NumPadFive.GetFName(), TEXT("5")},
		{EKeys::NumPadSix.GetFName(), TEXT("6")},
		{EKeys::NumPadSeven.GetFName(), TEXT("7")},
		{EKeys::NumPadEight.GetFName(), TEXT("8")},
		{EKeys::NumPadNine.GetFName(), TEXT("9")},
		{EKeys::Semicolon.GetFName(), TEXT("Semicolon")},
		{EKeys::Slash.GetFName(), TEXT("Slash")},
		{EKeys::LeftBracket.GetFName(), TEXT("Brackets_L")},
		{EKeys::RightBracket.GetFName(), TEXT("Brackets_R")},
		{EKeys::Apostrophe.GetFName(), TEXT("Quotation")},
		{EKeys::Hyphen.GetFName(), TEXT("Minus")},
		{EKeys::Subtract.GetFName(), TEXT("Minus")},
		{EKeys::Add.GetFName(), TEXT("Plus")},
		{EKeys::Multiply.GetFName(), TEXT("Asterisk")},
		{EKeys::Asterix.GetFName(), TEXT("Asterisk")},
		{EKeys::Divide.GetFName(), TEXT("Slash")},
		{EKeys::Up.GetFName(), TEXT("Up")},
		{EKeys::Down.GetFName(), TEXT("Down")},
		{EKeys::Left.GetFName(), TEXT("Left")},
		{EKeys::Right.GetFName(), TEXT("Right")},
		{EKeys::LeftMouseButton.GetFName(), TEXT("Mouse_Left")},
		{EKeys::RightMouseButton.GetFName(), TEXT("Mouse_Right")},
		{EKeys::MiddleMouseButton.GetFName(), TEXT("Mouse_Middle")},
		{EKeys::MouseX.GetFName(), TEXT("Mouse_X")},
		{EKeys::MouseY.GetFName(), TEXT("Mouse_Y")},
		{EKeys::Mouse2D.GetFName(), TEXT("Mouse_XY")},
		{EKeys::MouseWheelAxis.GetFName(), TEXT("Mouse_Scroll_Key_Dark")},
		{EKeys::MouseScrollUp.GetFName(), TEXT("Mouse_Scroll_Up_Key_Dark")},
		{EKeys::MouseScrollDown.GetFName(), TEXT("Mouse_Scroll_Down_Key_Dark")},
		{EKeys::Tilde.GetFName(), TEXT("Tilde")}
	};
	if (const FString* TextureToken = TextureTokens.Find(Key.GetFName()))
	{
		return *TextureToken;
	}

	if (KeyName.StartsWith(TEXT("F")) && KeyName.Len() <= 3)
	{
		return KeyName;
	}
	return FString();
}

FString UVNHInputPromptLibrary::GetGamepadTextureToken(
	const FKey Key,
	const EVNHInputPromptFamily PromptFamily)
{
	static const TMap<FName, FString> XboxTextureTokens =
	{
		{EKeys::Gamepad_FaceButton_Bottom.GetFName(), TEXT("A_Color")},
		{EKeys::Gamepad_FaceButton_Right.GetFName(), TEXT("B_Color")},
		{EKeys::Gamepad_FaceButton_Left.GetFName(), TEXT("X_Color")},
		{EKeys::Gamepad_FaceButton_Top.GetFName(), TEXT("Y_Color")},
		{EKeys::Gamepad_LeftShoulder.GetFName(), TEXT("LB")},
		{EKeys::Gamepad_RightShoulder.GetFName(), TEXT("RB")},
		{EKeys::Gamepad_LeftTrigger.GetFName(), TEXT("LT")},
		{EKeys::Gamepad_RightTrigger.GetFName(), TEXT("RT")},
		{EKeys::Gamepad_LeftTriggerAxis.GetFName(), TEXT("LT")},
		{EKeys::Gamepad_RightTriggerAxis.GetFName(), TEXT("RT")},
		{EKeys::Gamepad_LeftThumbstick.GetFName(), TEXT("Left_Stick_Click")},
		{EKeys::Gamepad_RightThumbstick.GetFName(), TEXT("Right_Stick_Click")},
		{EKeys::Gamepad_Special_Left.GetFName(), TEXT("Share")},
		{EKeys::Gamepad_Special_Right.GetFName(), TEXT("Share")},
		{EKeys::Gamepad_DPad_Up.GetFName(), TEXT("Dpad_Up")},
		{EKeys::Gamepad_DPad_Down.GetFName(), TEXT("Dpad_Down")},
		{EKeys::Gamepad_DPad_Left.GetFName(), TEXT("Dpad_Left")},
		{EKeys::Gamepad_DPad_Right.GetFName(), TEXT("Dpad_Right")},
		{EKeys::Gamepad_LeftX.GetFName(), TEXT("L_X")},
		{EKeys::Gamepad_LeftY.GetFName(), TEXT("L_Y")},
		{EKeys::Gamepad_Left2D.GetFName(), TEXT("L_2D")},
		{EKeys::Gamepad_LeftStick_Up.GetFName(), TEXT("L_UP")},
		{EKeys::Gamepad_LeftStick_Down.GetFName(), TEXT("L_Down")},
		{EKeys::Gamepad_LeftStick_Left.GetFName(), TEXT("L_Left")},
		{EKeys::Gamepad_LeftStick_Right.GetFName(), TEXT("L_Right")},
		{EKeys::Gamepad_RightX.GetFName(), TEXT("R_X")},
		{EKeys::Gamepad_RightY.GetFName(), TEXT("R_Y")},
		{EKeys::Gamepad_Right2D.GetFName(), TEXT("R_2D")},
		{EKeys::Gamepad_RightStick_Up.GetFName(), TEXT("R_UP")},
		{EKeys::Gamepad_RightStick_Down.GetFName(), TEXT("R_Down")},
		{EKeys::Gamepad_RightStick_Left.GetFName(), TEXT("R_Left")},
		{EKeys::Gamepad_RightStick_Right.GetFName(), TEXT("R_Right")}
	};

	const FString* XboxToken = XboxTextureTokens.Find(Key.GetFName());
	if (!XboxToken)
	{
		return FString();
	}
	if (PromptFamily == EVNHInputPromptFamily::Xbox)
	{
		return *XboxToken;
	}

	FString PlayStationToken = VNHResolvePlayStationFaceName(*XboxToken);
	if (PromptFamily == EVNHInputPromptFamily::PlayStation5
		&& (PlayStationToken == TEXT("Left_Stick_Click")
			|| PlayStationToken == TEXT("Right_Stick_Click")))
	{
		PlayStationToken += TEXT("_Alt");
	}
	return PlayStationToken;
}

FString UVNHInputPromptLibrary::GetControllerIdentifier(
	const APlayerController* PlayerController)
{
	const UInputDeviceSubsystem* InputDeviceSubsystem = GEngine
		? GEngine->GetEngineSubsystem<UInputDeviceSubsystem>()
		: nullptr;
	if (!InputDeviceSubsystem)
	{
		return FString();
	}

	const FPlatformUserId PlatformUserId = PlayerController
		? PlayerController->GetPlatformUserId()
		: IPlatformInputDeviceMapper::Get().GetPrimaryPlatformUser();
	TArray<FInputDeviceId> ConnectedInputDevices;
	IPlatformInputDeviceMapper::Get().GetAllConnectedInputDevicesForUser(
		PlatformUserId,
		ConnectedInputDevices);
	for (const FInputDeviceId InputDeviceId : ConnectedInputDevices)
	{
		const FHardwareDeviceIdentifier HardwareIdentifier =
			InputDeviceSubsystem->GetInputDeviceHardwareIdentifier(InputDeviceId);
		if (IsVNHGamepadDevice(InputDeviceId, InputDeviceSubsystem))
		{
			return HardwareIdentifier.ToString();
		}
	}

	return InputDeviceSubsystem->GetMostRecentlyUsedHardwareDevice(PlatformUserId).ToString();
}
