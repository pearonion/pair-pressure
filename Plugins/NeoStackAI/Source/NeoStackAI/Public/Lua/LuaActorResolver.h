// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// Actor lookup helpers — replaces the per-binding pattern of
//   for (TActorIterator<AActor> It(World); It; ++It) { if (It->GetActorLabel() == Id) ... }
// originally defined inside LuaBinding_LevelDesign.cpp and reused informally.
//
// Usage:
//   AActor* A = NeoLuaActor::FindByLabel(World, TEXT("MyCube"));
//   AActor* B = NeoLuaActor::FindByNameOrLabel(World, IdFromLua);

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

namespace NeoLuaActor
{

// Find an actor by user-visible label (case-insensitive). Returns nullptr if no match.
NEOSTACKAI_API AActor* FindByLabel(UWorld* World, const FString& Label);

// Find an actor by label first, falling back to internal name (both case-insensitive).
// This is what most Lua bindings want — agents pass labels but sometimes pass names.
NEOSTACKAI_API AActor* FindByNameOrLabel(UWorld* World, const FString& Id);

}
