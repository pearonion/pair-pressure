// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaEditorActions.h"
#include "Lua/LuaSubsystem.h"

#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace NeoLuaEditor
{

bool OpenAssetEditor(UObject* Asset)
{
	if (!Asset) return false;
	UAssetEditorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UAssetEditorSubsystem>();
	if (!Sub) return false;
	return Sub->OpenEditorForAsset(Asset);
}

int32 CloseAssetEditorsFor(UObject* Asset)
{
	if (!Asset) return 0;
	UAssetEditorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UAssetEditorSubsystem>();
	if (!Sub) return 0;
	return Sub->CloseAllEditorsForAsset(Asset);
}

bool CloseAllAssetEditors()
{
	UAssetEditorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UAssetEditorSubsystem>();
	return Sub ? Sub->CloseAllAssetEditors() : false;
}

IAssetEditorInstance* OpenAssetEditorAndWait(UObject* Asset, int32 MaxTickSteps, bool bFocusIfOpen)
{
	if (!Asset) return nullptr;
	UAssetEditorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UAssetEditorSubsystem>();
	if (!Sub) return nullptr;

	// Fast path: already open.
	if (IAssetEditorInstance* Existing = Sub->FindEditorForAsset(Asset, bFocusIfOpen))
	{
		return Existing;
	}

	Sub->OpenEditorForAsset(Asset);

	// Pump Slate until the editor reports ready. Sleep would block the game
	// thread and prevent the editor from ever initializing.
	for (int32 i = 0; i < MaxTickSteps; ++i)
	{
		if (IAssetEditorInstance* Instance = Sub->FindEditorForAsset(Asset, bFocusIfOpen))
		{
			return Instance;
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		FlushRenderingCommands();
	}
	return nullptr;
}

}
