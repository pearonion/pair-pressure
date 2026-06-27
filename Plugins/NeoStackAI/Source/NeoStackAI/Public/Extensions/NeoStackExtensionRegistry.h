#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"

/**
 * Central inventory for NeoStack AI extensions.
 *
 * This registry tracks extension metadata and runtime state independently of
 * the old hardcoded module-status list. Capability-specific registries
 * (Lua/tools/providers/chat) will later register ownership against this.
 */
class NEOSTACKAI_API FNeoStackExtensionRegistry
{
public:
	static FNeoStackExtensionRegistry& Get();

	static int32 GetCoreApiVersion()
	{
		return NEOSTACK_EXTENSION_API_VERSION;
	}

	/** Clears all registered extensions. Used during core startup while the legacy
	 *  module-loading path still owns discovery. */
	void Reset();

	/** Register or update an extension descriptor. ExtensionId is preferred; ModuleName
	 *  is accepted as a temporary fallback key during migration. */
	void RegisterOrUpdateExtension(const FNeoStackExtensionDescriptor& Descriptor);

	/** Remove an extension descriptor by stable ID or module name. */
	bool UnregisterExtension(const FString& ExtensionIdOrModuleName);

	/** Convenience shim for the current NSAI_* module split while the platform migrates
	 *  toward self-registering plugins under Plugins/NeoExtensions. */
	void RegisterLegacyModuleExtension(
		const FString& DisplayName,
		const FString& ModuleName,
		ENeoStackExtensionState State,
		const FString& StatusMessage = FString());

	/** Update only the runtime state/message for an already-known extension. */
	bool SetExtensionState(
		const FString& ExtensionIdOrModuleName,
		ENeoStackExtensionState State,
		const FString& StatusMessage = FString());

	/** Find an extension by stable ID or module name. */
	bool FindExtension(
		const FString& ExtensionIdOrModuleName,
		FNeoStackExtensionDescriptor& OutDescriptor) const;

	/** Snapshot all known extensions in registration order. */
	TArray<FNeoStackExtensionDescriptor> GetAllExtensions() const;

	/** Snapshot all known extensions in the requested state. */
	TArray<FNeoStackExtensionDescriptor> GetExtensionsByState(ENeoStackExtensionState State) const;

private:
	static FString MakeKey(const FString& ExtensionId, const FString& ModuleName);
	static FString NormalizeLookupKey(const FString& ExtensionIdOrModuleName);
	void RebuildIndexMapLocked();

	mutable FCriticalSection Lock;
	TArray<FNeoStackExtensionDescriptor> Extensions;
	TMap<FString, int32> IndexByKey;
};
