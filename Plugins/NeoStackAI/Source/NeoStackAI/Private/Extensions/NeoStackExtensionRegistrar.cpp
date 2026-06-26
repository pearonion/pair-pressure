#include "Extensions/NeoStackExtensionRegistrar.h"

#include "NeoStackAIModule.h"
#include "Chat/ChatModelRegistry.h"
#include "Extensions/NeoStackExtensionRegistry.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "Tools/NeoStackToolRegistry.h"

FNeoStackExtensionRegistrar& FNeoStackExtensionRegistrar::Get()
{
	static FNeoStackExtensionRegistrar Instance;
	return Instance;
}

void FNeoStackExtensionRegistrar::RegisterExtension(const FNeoStackExtensionDescriptor& Descriptor)
{
	FNeoStackExtensionRegistry::Get().RegisterOrUpdateExtension(Descriptor);
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered extension '%s' (%s, state=%d)"),
		*Descriptor.ExtensionId,
		*Descriptor.DisplayName,
		static_cast<int32>(Descriptor.State));
}

bool FNeoStackExtensionRegistrar::UnregisterExtension(const FString& ExtensionIdOrModuleName)
{
	const bool bRemoved = FNeoStackExtensionRegistry::Get().UnregisterExtension(ExtensionIdOrModuleName);
	if (bRemoved)
	{
		UE_LOG(LogNeoStackAI, Log,
			TEXT("NeoStackExtension: Unregistered extension '%s'"),
			*ExtensionIdOrModuleName);
	}
	return bRemoved;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterLuaBinding(
	const FString& OwnerExtensionId,
	const FString& Name,
	TArray<FLuaFunctionDoc> Functions,
	FLuaBindingFunc BindFunc)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FLuaBindingRegistry::Get().RegisterOwned(OwnerExtensionId, Name, MoveTemp(Functions), MoveTemp(BindFunc));
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered Lua domain '%s' for '%s'"),
		*Name,
		*OwnerExtensionId);
	return Handle;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterAssetCapability(
	const FString& OwnerExtensionId,
	const FString& Name,
	const FString& EnrichFunctionName,
	TFunction<bool(UObject* Asset)> OwnsAsset,
	TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)> ResolveGraphs)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FLuaAssetCapabilityRegistry::Get().RegisterOwned(
		OwnerExtensionId,
		Name,
		EnrichFunctionName,
		MoveTemp(OwnsAsset),
		MoveTemp(ResolveGraphs));
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered asset capability '%s' for '%s'"),
		*Name,
		*OwnerExtensionId);
	return Handle;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterGraphCapability(
	const FString& OwnerExtensionId,
	const FString& Name,
	TFunction<bool(UEdGraph* Graph)> OwnsGraph,
	FLuaGraphCapabilityBridges Bridges)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FLuaGraphCapabilityRegistry::Get().RegisterOwned(
		OwnerExtensionId,
		Name,
		MoveTemp(OwnsGraph),
		MoveTemp(Bridges));
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered graph capability '%s' for '%s'"),
		*Name,
		*OwnerExtensionId);
	return Handle;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterTool(
	const FString& OwnerExtensionId,
	TSharedPtr<FNeoStackToolBase> Tool)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FNeoStackToolRegistry::Get().RegisterOwned(OwnerExtensionId, MoveTemp(Tool));
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered tool for '%s'"),
		*OwnerExtensionId);
	return Handle;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterChatProvider(
	const FString& OwnerExtensionId,
	TSharedRef<IChatProvider> Provider)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FChatModelRegistry::Get().RegisterOwnedProvider(OwnerExtensionId, Provider);
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered chat provider for '%s'"),
		*OwnerExtensionId);
	return Handle;
}

FNeoStackExtensionHandle FNeoStackExtensionRegistrar::RegisterGenerativeProvider(
	const FString& OwnerExtensionId,
	TSharedRef<IGenerativeProvider> Provider)
{
	EnsureExtensionPlaceholder(OwnerExtensionId);
	FNeoStackExtensionHandle Handle = FGenerativeProviderRegistry::Get().RegisterOwned(OwnerExtensionId, Provider);
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Registered generative provider for '%s'"),
		*OwnerExtensionId);
	return Handle;
}

bool FNeoStackExtensionRegistrar::Unregister(const FNeoStackExtensionHandle& Handle)
{
	if (!Handle.IsValid())
	{
		return false;
	}

	if (Handle.Kind == TEXT("lua_binding"))
	{
		return FLuaBindingRegistry::Get().Unregister(Handle.RegistrationId);
	}
	if (Handle.Kind == TEXT("lua_asset_capability"))
	{
		return FLuaAssetCapabilityRegistry::Get().Unregister(Handle.RegistrationId);
	}
	if (Handle.Kind == TEXT("lua_graph_capability"))
	{
		return FLuaGraphCapabilityRegistry::Get().Unregister(Handle.RegistrationId);
	}
	if (Handle.Kind == TEXT("tool"))
	{
		return FNeoStackToolRegistry::Get().Unregister(Handle.RegistrationId);
	}
	if (Handle.Kind == TEXT("chat_provider"))
	{
		return FChatModelRegistry::Get().UnregisterProvider(Handle.RegistrationId);
	}
	if (Handle.Kind == TEXT("generative_provider"))
	{
		return FGenerativeProviderRegistry::Get().Unregister(Handle.RegistrationId);
	}

	return false;
}

int32 FNeoStackExtensionRegistrar::UnregisterAllForExtension(const FString& OwnerExtensionId)
{
	int32 Removed = 0;
	Removed += FLuaAssetCapabilityRegistry::Get().UnregisterAllForOwner(OwnerExtensionId);
	Removed += FLuaGraphCapabilityRegistry::Get().UnregisterAllForOwner(OwnerExtensionId);
	Removed += FLuaBindingRegistry::Get().UnregisterAllForOwner(OwnerExtensionId);
	Removed += FNeoStackToolRegistry::Get().UnregisterAllForOwner(OwnerExtensionId);
	Removed += FChatModelRegistry::Get().UnregisterProvidersForOwner(OwnerExtensionId);
	Removed += FGenerativeProviderRegistry::Get().UnregisterAllForOwner(OwnerExtensionId);
	FNeoStackExtensionRegistry::Get().UnregisterExtension(OwnerExtensionId);
	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Unregistered all capabilities for '%s' (%d removed)"),
		*OwnerExtensionId,
		Removed);
	return Removed;
}

void FNeoStackExtensionRegistrar::EnsureExtensionPlaceholder(const FString& OwnerExtensionId)
{
	if (OwnerExtensionId.IsEmpty())
	{
		return;
	}

	FNeoStackExtensionDescriptor Existing;
	if (FNeoStackExtensionRegistry::Get().FindExtension(OwnerExtensionId, Existing))
	{
		return;
	}

	FNeoStackExtensionDescriptor Descriptor;
	Descriptor.ExtensionId = OwnerExtensionId;
	Descriptor.DisplayName = OwnerExtensionId.Equals(TEXT("neostack.core"), ESearchCase::CaseSensitive)
		? TEXT("NeoStack AI Core")
		: OwnerExtensionId;
	Descriptor.Category = TEXT("extension");
	Descriptor.State = OwnerExtensionId.Equals(TEXT("neostack.core"), ESearchCase::CaseSensitive)
		? ENeoStackExtensionState::Active
		: ENeoStackExtensionState::Registered;
	Descriptor.StatusMessage = TEXT("Auto-registered placeholder extension descriptor.");
	FNeoStackExtensionRegistry::Get().RegisterOrUpdateExtension(Descriptor);
}
