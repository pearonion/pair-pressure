#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"
#include "Lua/LuaAssetCapabilityRegistry.h"
#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaGraphCapabilityRegistry.h"

class FNeoStackToolBase;
class IChatProvider;
class IGenerativeProvider;

/**
 * Public registrar facade for extension authors.
 *
 * This sits above the capability-specific registries and makes ownership
 * explicit so extensions can register/unregister themselves cleanly.
 */
class NEOSTACKAI_API FNeoStackExtensionRegistrar
{
public:
	static FNeoStackExtensionRegistrar& Get();

	void RegisterExtension(const FNeoStackExtensionDescriptor& Descriptor);
	bool UnregisterExtension(const FString& ExtensionIdOrModuleName);

	FNeoStackExtensionHandle RegisterLuaBinding(
		const FString& OwnerExtensionId,
		const FString& Name,
		TArray<FLuaFunctionDoc> Functions,
		FLuaBindingFunc BindFunc);

	FNeoStackExtensionHandle RegisterAssetCapability(
		const FString& OwnerExtensionId,
		const FString& Name,
		const FString& EnrichFunctionName,
		TFunction<bool(UObject* Asset)> OwnsAsset,
		TFunction<bool(UObject* Asset, TArray<FResolvedGraphInfo>& OutGraphs)> ResolveGraphs = {});

	FNeoStackExtensionHandle RegisterGraphCapability(
		const FString& OwnerExtensionId,
		const FString& Name,
		TFunction<bool(UEdGraph* Graph)> OwnsGraph,
		FLuaGraphCapabilityBridges Bridges);

	FNeoStackExtensionHandle RegisterTool(
		const FString& OwnerExtensionId,
		TSharedPtr<FNeoStackToolBase> Tool);

	FNeoStackExtensionHandle RegisterChatProvider(
		const FString& OwnerExtensionId,
		TSharedRef<IChatProvider> Provider);

	FNeoStackExtensionHandle RegisterGenerativeProvider(
		const FString& OwnerExtensionId,
		TSharedRef<IGenerativeProvider> Provider);

	bool Unregister(const FNeoStackExtensionHandle& Handle);
	int32 UnregisterAllForExtension(const FString& OwnerExtensionId);

private:
	void EnsureExtensionPlaceholder(const FString& OwnerExtensionId);
};
