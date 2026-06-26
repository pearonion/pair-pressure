// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// Subsystem accessor helpers — replaces the per-binding pattern of:
//   static UMySubsystem* GetMyThing()
//   {
//       return GEditor ? GEditor->GetEditorSubsystem<UMySubsystem>() : nullptr;
//   }
// (~18 such helpers exist scattered across binding files.)
//
// Usage:
//   UEditorActorSubsystem* Sub = NeoLuaSubsystem::GetEditor<UEditorActorSubsystem>();
//   UReplaySubsystem*      Rep = NeoLuaSubsystem::GetGameInstance<UReplaySubsystem>(World);
//   UWaterSubsystem*       Wtr = NeoLuaSubsystem::GetWorld<UWaterSubsystem>(World);
//   UEngineSubsystem*      Eng = NeoLuaSubsystem::GetEngine<UMyEngineSubsystem>();

#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/WorldSubsystem.h"

namespace NeoLuaSubsystem
{

// Editor subsystem (UEditorSubsystem). Returns nullptr if no GEditor or class not registered.
template <typename T>
T* GetEditor()
{
	return GEditor ? GEditor->GetEditorSubsystem<T>() : nullptr;
}

// Engine subsystem (UEngineSubsystem). Returns nullptr if no GEngine.
template <typename T>
T* GetEngine()
{
	return GEngine ? GEngine->GetEngineSubsystem<T>() : nullptr;
}

// World subsystem (UWorldSubsystem). World must be valid.
template <typename T>
T* GetWorld(UWorld* World)
{
	return World ? World->GetSubsystem<T>() : nullptr;
}

// GameInstance subsystem. Resolves via the world's owning game instance.
template <typename T>
T* GetGameInstance(UWorld* World)
{
	if (!World) return nullptr;
	UGameInstance* GI = World->GetGameInstance();
	return GI ? GI->GetSubsystem<T>() : nullptr;
}

// LocalPlayer subsystem. Picks the world's first local player.
template <typename T>
T* GetLocalPlayer(UWorld* World)
{
	if (!World) return nullptr;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return nullptr;
	if (ULocalPlayer* LP = GI->GetFirstGamePlayer()) return LP->GetSubsystem<T>();
	return nullptr;
}

}
