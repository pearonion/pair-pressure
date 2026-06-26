// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Application/SlateApplication.h"
#include "RenderingThread.h"

struct FResolvedGraphInfo
{
	FString Name;
	UEdGraph* Graph;

	FResolvedGraphInfo() : Graph(nullptr) {}
	FResolvedGraphInfo(const FString& InName, UEdGraph* InGraph) : Name(InName), Graph(InGraph) {}
};

namespace LuaGraphResolver
{
inline bool WaitForGraph(TFunctionRef<bool()> Predicate, float TimeoutSeconds = 2.0f, float StepSeconds = 0.05f)
{
	const int32 MaxSteps = FMath::Max(1, FMath::CeilToInt(TimeoutSeconds / StepSeconds));
	for (int32 Step = 0; Step < MaxSteps; ++Step)
	{
		if (Predicate())
		{
			return true;
		}
		FPlatformProcess::Sleep(StepSeconds);
	}
	return Predicate();
}

// Helper: open an asset editor and wait for the graph to become available.
inline UEdGraph* EnsureEditorGraphViaEditorOpen(UObject* Asset, TFunctionRef<UEdGraph*(UObject*)> GraphGetter)
{
	if (UEdGraph* Existing = GraphGetter(Asset))
	{
		return Existing;
	}

	if (!GEditor) return nullptr;

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem) return nullptr;

	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	UEdGraph* Result = nullptr;
	WaitForGraph([&]()
	{
		Result = GraphGetter(Asset);
		return Result != nullptr;
	});

	return Result;
}

// Helper: ensure an asset editor exists for Asset, opening it if needed. Returns the
// IAssetEditorInstance (whose viewport / toolkit can then be queried), or nullptr if the
// asset has no registered editor type. Generic — works for Material, NiagaraSystem,
// Blueprint, anything that registers via UAssetEditorSubsystem.
//
// Use this anywhere a binding needs the editor to be open before reading editor-only
// state (preview material, slate viewport, toolkit's GetObjectsCurrentlyBeingEdited).
inline IAssetEditorInstance* EnsureAssetEditorOpen(UObject* Asset, float TimeoutSeconds = 2.0f)
{
	if (!Asset || !GEditor) return nullptr;

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem) return nullptr;

	if (IAssetEditorInstance* Existing = AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
	{
		return Existing;
	}

	if (!AssetEditorSubsystem->OpenEditorForAsset(Asset))
	{
		return nullptr;
	}

	IAssetEditorInstance* Result = nullptr;
	WaitForGraph([&]()
	{
		Result = AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
		return Result != nullptr;
	}, TimeoutSeconds);

	return Result;
}

} // namespace LuaGraphResolver
