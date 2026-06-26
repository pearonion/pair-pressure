#include "Lua/LuaAssetCapabilityRegistry.h"

FLuaAssetCapabilityRegistry& FLuaAssetCapabilityRegistry::Get()
{
	static FLuaAssetCapabilityRegistry Instance;
	return Instance;
}

FNeoStackExtensionHandle FLuaAssetCapabilityRegistry::RegisterOwned(
	const FString& OwnerExtensionId,
	const FString& Name,
	const FString& EnrichFunctionName,
	TFunction<bool(UObject* Asset)> OwnsAsset,
	TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)> ResolveGraphs)
{
	FLuaAssetCapability Capability;
	Capability.Name = Name;
	Capability.OwnerExtensionId = OwnerExtensionId.IsEmpty() ? TEXT("neostack.core") : OwnerExtensionId;
	Capability.EnrichFunctionName = EnrichFunctionName;
	Capability.RegistrationId = FGuid::NewGuid();
	Capability.OwnsAsset = MoveTemp(OwnsAsset);
	Capability.ResolveGraphs = MoveTemp(ResolveGraphs);
	Capabilities.Add(MoveTemp(Capability));

	FNeoStackExtensionHandle Handle;
	Handle.RegistrationId = Capabilities.Last().RegistrationId;
	Handle.Kind = TEXT("lua_asset_capability");
	Handle.OwnerExtensionId = Capabilities.Last().OwnerExtensionId;
	return Handle;
}

bool FLuaAssetCapabilityRegistry::Unregister(const FGuid& RegistrationId)
{
	for (int32 Index = Capabilities.Num() - 1; Index >= 0; --Index)
	{
		if (Capabilities[Index].RegistrationId == RegistrationId)
		{
			Capabilities.RemoveAt(Index);
			return true;
		}
	}

	return false;
}

int32 FLuaAssetCapabilityRegistry::UnregisterAllForOwner(const FString& OwnerExtensionId)
{
	int32 Removed = 0;
	for (int32 Index = Capabilities.Num() - 1; Index >= 0; --Index)
	{
		if (Capabilities[Index].OwnerExtensionId.Equals(OwnerExtensionId, ESearchCase::CaseSensitive))
		{
			Capabilities.RemoveAt(Index);
			++Removed;
		}
	}

	return Removed;
}

const FLuaAssetCapability* FLuaAssetCapabilityRegistry::FindOwner(UObject* Asset) const
{
	if (!Asset)
	{
		return nullptr;
	}

	for (int32 Index = Capabilities.Num() - 1; Index >= 0; --Index)
	{
		if (Capabilities[Index].Matches(Asset))
		{
			return &Capabilities[Index];
		}
	}

	return nullptr;
}

bool FLuaAssetCapabilityRegistry::TryResolveGraphs(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs) const
{
	const FLuaAssetCapability* Capability = FindOwner(Asset);
	if (!Capability || !Capability->ResolveGraphs)
	{
		return false;
	}

	return Capability->ResolveGraphs(Asset, OutGraphs);
}
