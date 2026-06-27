// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// Editor-action helpers — collapses the inline pattern
//   GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
// (used in 34+ places) into a single call. Also handles the "no GEditor / no
// subsystem yet" guard once.

#pragma once

#include "CoreMinimal.h"

class UObject;
class IAssetEditorInstance;

namespace NeoLuaEditor
{

// Open the asset editor for the given asset. Returns false if no GEditor, no
// AssetEditorSubsystem, or asset is null.
NEOSTACKAI_API bool OpenAssetEditor(UObject* Asset);

// Close all open asset editors for the given asset. Returns the number of editors closed.
NEOSTACKAI_API int32 CloseAssetEditorsFor(UObject* Asset);

// Close every open asset editor (e.g. before destructive bulk operations).
NEOSTACKAI_API bool CloseAllAssetEditors();

// Open the asset editor and pump Slate up to MaxTickSteps until the editor is
// actually initialized. Returns the live IAssetEditorInstance, or nullptr if the
// editor never opened within the tick budget. If the editor is already open,
// returns immediately.
//
// Centralizes the "OpenEditorForAsset -> Tick -> FindEditorForAsset" dance
// duplicated across NodeUtils, LuaBinding_BehaviorTree, and ScreenshotViewportTool.
NEOSTACKAI_API IAssetEditorInstance* OpenAssetEditorAndWait(
    UObject* Asset,
    int32 MaxTickSteps = 40,
    bool bFocusIfOpen = false);

}
