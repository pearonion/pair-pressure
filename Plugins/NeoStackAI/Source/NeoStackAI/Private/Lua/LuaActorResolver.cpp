// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaActorResolver.h"

#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace NeoLuaActor
{

AActor* FindByLabel(UWorld* World, const FString& Label)
{
	if (!World) return nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && A->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			return A;
		}
	}
	return nullptr;
}

AActor* FindByNameOrLabel(UWorld* World, const FString& Id)
{
	if (!World) return nullptr;

	// Label first (user-visible name)
	if (AActor* A = FindByLabel(World, Id)) return A;

	// Internal name fallback
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A && A->GetName().Equals(Id, ESearchCase::IgnoreCase))
		{
			return A;
		}
	}
	return nullptr;
}

}
