#pragma once

#include "CoreMinimal.h"

/** Public extension API version for the NeoStack AI platform.
 *  Extensions can use this to declare compatibility independent of product/plugin versioning. */
constexpr int32 NEOSTACK_EXTENSION_API_VERSION = 1;

enum class ENeoStackExtensionState : uint8
{
	Registered,   // Known to the registry but not yet activated
	Active,       // Loaded and available
	Unavailable,  // Backing dependency/plugin is not available
	Incompatible, // Present but not compatible with the current core API
	Failed        // Tried to activate but failed
};

struct NEOSTACKAI_API FNeoStackExtensionDescriptor
{
	FString ExtensionId;       // Stable ID, e.g. "neostack.statetree" or "vendor.perforce"
	FString DisplayName;       // User-facing label
	FString ModuleName;        // Current owning UE module, if any
	FString Vendor;            // Vendor or team name
	FString Version;           // Extension version
	FString Category;          // "engine", "tool", "provider", "integration", etc.
	FString Description;       // Optional summary for UI/debugging
	int32 ExtensionApiVersion = NEOSTACK_EXTENSION_API_VERSION;
	int32 MinCoreApiVersion = NEOSTACK_EXTENSION_API_VERSION;
	int32 MaxCoreApiVersion = NEOSTACK_EXTENSION_API_VERSION;
	bool bIsBuiltIn = true;
	bool bIsThirdParty = false;
	ENeoStackExtensionState State = ENeoStackExtensionState::Registered;
	FString StatusMessage;     // Optional runtime status detail
	TArray<FString> Tags;      // Capability tags such as "lua", "tool", "chat"
};

/** Registration handle type reserved for capability registries that need
 *  owner-aware unregister support. This is groundwork for later extensionization. */
struct NEOSTACKAI_API FNeoStackExtensionHandle
{
	FGuid RegistrationId;
	FName Kind;
	FString OwnerExtensionId;

	bool IsValid() const
	{
		return RegistrationId.IsValid();
	}
};

