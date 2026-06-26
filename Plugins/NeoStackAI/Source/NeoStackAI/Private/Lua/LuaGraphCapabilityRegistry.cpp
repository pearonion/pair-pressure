#include "Lua/LuaGraphCapabilityRegistry.h"

FLuaGraphCapabilityRegistry& FLuaGraphCapabilityRegistry::Get()
{
	static FLuaGraphCapabilityRegistry Instance;
	return Instance;
}

FNeoStackExtensionHandle FLuaGraphCapabilityRegistry::RegisterOwned(
	const FString& OwnerExtensionId,
	const FString& Name,
	TFunction<bool(UEdGraph* Graph)> OwnsGraph,
	FLuaGraphCapabilityBridges Bridges)
{
	FLuaGraphCapability Capability;
	Capability.Name = Name;
	Capability.OwnerExtensionId = OwnerExtensionId.IsEmpty() ? TEXT("neostack.core") : OwnerExtensionId;
	Capability.RegistrationId = FGuid::NewGuid();
	Capability.OwnsGraph = MoveTemp(OwnsGraph);
	Capability.Bridges = MoveTemp(Bridges);
	Capabilities.Add(MoveTemp(Capability));

	FNeoStackExtensionHandle Handle;
	Handle.RegistrationId = Capabilities.Last().RegistrationId;
	Handle.Kind = TEXT("lua_graph_capability");
	Handle.OwnerExtensionId = Capabilities.Last().OwnerExtensionId;
	return Handle;
}

bool FLuaGraphCapabilityRegistry::Unregister(const FGuid& RegistrationId)
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

int32 FLuaGraphCapabilityRegistry::UnregisterAllForOwner(const FString& OwnerExtensionId)
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

const FLuaGraphCapability* FLuaGraphCapabilityRegistry::FindOwner(UEdGraph* Graph) const
{
	if (!Graph)
	{
		return nullptr;
	}

	for (int32 Index = Capabilities.Num() - 1; Index >= 0; --Index)
	{
		if (Capabilities[Index].Matches(Graph))
		{
			return &Capabilities[Index];
		}
	}

	return nullptr;
}
