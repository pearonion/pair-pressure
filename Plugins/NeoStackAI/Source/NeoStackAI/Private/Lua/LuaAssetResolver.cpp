// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaAssetResolver.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace NeoLuaAsset
{

UObject* ResolveWithRegistry(const FString& Path)
{
	const FString Normalized = NeoLuaAsset::NormalizePath(Path);
	if (Normalized.IsEmpty()) return nullptr;

	if (UObject* Direct = LoadObject<UObject>(nullptr, *Normalized))
	{
		return Direct;
	}
	if (UObject* Direct = LoadObject<UObject>(nullptr, *NeoLuaAsset::ToObjectPath(Normalized)))
	{
		return Direct;
	}

	// Fallback: include dirty assets created earlier in the same Lua/editor run.
	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	TArray<FAssetData> Assets;
	ARM.Get().GetAssetsByPackageName(FName(*NeoLuaAsset::ToPackagePath(Normalized)), Assets, false);
	return Assets.Num() > 0 ? Assets[0].GetAsset() : nullptr;
}

}
