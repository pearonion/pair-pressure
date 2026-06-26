#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"
#include "Lua/LuaGraphResolverExtension.h"

struct NEOSTACKAI_API FLuaAssetCapability
{
	FString Name;
	FString OwnerExtensionId;
	FString EnrichFunctionName;
	FGuid RegistrationId;
	TFunction<bool(UObject* Asset)> OwnsAsset;
	TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)> ResolveGraphs;

	bool Matches(UObject* Asset) const
	{
		return OwnsAsset && Asset && OwnsAsset(Asset);
	}
};

class NEOSTACKAI_API FLuaAssetCapabilityRegistry
{
public:
	static FLuaAssetCapabilityRegistry& Get();

	FNeoStackExtensionHandle RegisterOwned(
		const FString& OwnerExtensionId,
		const FString& Name,
		const FString& EnrichFunctionName,
		TFunction<bool(UObject* Asset)> OwnsAsset,
		TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)> ResolveGraphs = {});

	bool Unregister(const FGuid& RegistrationId);
	int32 UnregisterAllForOwner(const FString& OwnerExtensionId);

	const FLuaAssetCapability* FindOwner(UObject* Asset) const;
	bool TryResolveGraphs(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs) const;

private:
	TArray<FLuaAssetCapability> Capabilities;
};
