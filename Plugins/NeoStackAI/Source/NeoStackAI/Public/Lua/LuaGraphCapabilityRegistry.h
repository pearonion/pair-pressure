#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"

class UEdGraph;

struct NEOSTACKAI_API FLuaGraphCapabilityBridges
{
	FString AddNodeFunctionName;
	FString FindNodesFunctionName;
	FString ConnectFunctionName;
	FString DisconnectFunctionName;
	FString DisconnectAllFunctionName;
	FString DisconnectFromFunctionName;
	FString SplitPinFunctionName;
	FString RecombinePinFunctionName;
	FString SetNodeCommentFunctionName;
	FString DeleteNodeFunctionName;
};

struct NEOSTACKAI_API FLuaGraphCapability
{
	FString Name;
	FString OwnerExtensionId;
	FGuid RegistrationId;
	TFunction<bool(UEdGraph* Graph)> OwnsGraph;
	FLuaGraphCapabilityBridges Bridges;

	bool Matches(UEdGraph* Graph) const
	{
		return OwnsGraph && Graph && OwnsGraph(Graph);
	}
};

class NEOSTACKAI_API FLuaGraphCapabilityRegistry
{
public:
	static FLuaGraphCapabilityRegistry& Get();

	FNeoStackExtensionHandle RegisterOwned(
		const FString& OwnerExtensionId,
		const FString& Name,
		TFunction<bool(UEdGraph* Graph)> OwnsGraph,
		FLuaGraphCapabilityBridges Bridges);

	bool Unregister(const FGuid& RegistrationId);
	int32 UnregisterAllForOwner(const FString& OwnerExtensionId);

	const FLuaGraphCapability* FindOwner(UEdGraph* Graph) const;

private:
	TArray<FLuaGraphCapability> Capabilities;
};
