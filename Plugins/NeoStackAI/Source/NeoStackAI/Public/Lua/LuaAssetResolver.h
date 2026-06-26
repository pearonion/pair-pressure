// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Lua-facing asset path resolver. Replaces the scattered
//   `if (!Path.StartsWith("/")) Path = TEXT("/Game/") + Path;
//    LoadObject<T>(nullptr, *Path);`
// pattern used by every binding that accepts asset paths from Lua.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Package.h"

namespace NeoLuaAsset
{

// Prepend /Game/ if the path is not already package-rooted. Empty input returns empty.
inline FString NormalizePath(const FString& Path)
{
	if (Path.IsEmpty() || Path.StartsWith(TEXT("/")))
	{
		return Path;
	}
	return TEXT("/Game/") + Path;
}

// Convert a Lua-friendly package path (/Game/Foo/Bar) to an object path
// (/Game/Foo/Bar.Bar). Existing object paths pass through unchanged.
inline FString ToObjectPath(const FString& Path)
{
	const FString Normalized = NeoLuaAsset::NormalizePath(Path);
	if (Normalized.IsEmpty())
	{
		return Normalized;
	}

	int32 SlashIndex = INDEX_NONE;
	int32 DotIndex = INDEX_NONE;
	Normalized.FindLastChar(TEXT('/'), SlashIndex);
	Normalized.FindLastChar(TEXT('.'), DotIndex);
	if (DotIndex > SlashIndex)
	{
		return Normalized;
	}

	const FString AssetName = (SlashIndex != INDEX_NONE) ? Normalized.Mid(SlashIndex + 1) : Normalized;
	return AssetName.IsEmpty() ? Normalized : FString::Printf(TEXT("%s.%s"), *Normalized, *AssetName);
}

// Convert an object path back to its package path for Asset Registry lookups.
inline FString ToPackagePath(const FString& Path)
{
	const FString Normalized = NeoLuaAsset::NormalizePath(Path);
	int32 SlashIndex = INDEX_NONE;
	int32 DotIndex = INDEX_NONE;
	Normalized.FindLastChar(TEXT('/'), SlashIndex);
	Normalized.FindLastChar(TEXT('.'), DotIndex);
	return (DotIndex > SlashIndex) ? Normalized.Left(DotIndex) : Normalized;
}

// Resolve a Lua-provided asset path to a loaded UObject*. Returns nullptr on empty
// input or when the asset fails to load. Caller is responsible for logging failure.
template <typename T>
T* Resolve(const FString& Path)
{
	const FString Normalized = NeoLuaAsset::NormalizePath(Path);
	if (Normalized.IsEmpty())
	{
		return nullptr;
	}
	if (T* Direct = LoadObject<T>(nullptr, *Normalized))
	{
		return Direct;
	}
	return LoadObject<T>(nullptr, *NeoLuaAsset::ToObjectPath(Normalized));
}

// Resolve with Asset Registry fallback for paths that LoadObject misses
// (e.g. PCG assets that don't resolve from short package paths).
//
// Mirrors the open_asset behavior: try LoadObject first, fall back to
// AssetRegistry::GetAssetsByPackageName(path) including in-memory dirty assets,
// return the first hit.
//
// Defined in the .cpp to avoid leaking AssetRegistry include into every TU.
NEOSTACKAI_API UObject* ResolveWithRegistry(const FString& Path);

template <typename T>
T* ResolveWithRegistry(const FString& Path)
{
	return Cast<T>(ResolveWithRegistry(Path));
}

}
