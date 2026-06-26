// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Human-readable card metadata owned by each extension.
 *
 * Each first-party NeoExtension should ship:
 *   Plugins/NeoExtensions/<PluginName>/Resources/ExtensionUI.json
 *
 * The file is intentionally co-located with the extension so new extensions
 * can ship readable UI copy without editing a central list in the core plugin.
 * Core owns the v1 schema in Source/NeoStackAI/Resources/ExtensionUI.schema.json.
 */
struct NEOSTACKAI_API FNeoStackExtensionUIMetadata
{
	int32 SchemaVersion = 1;
	FString Domain;       // Stable grouping key: recommended, animation, ai, vfx, world, input, pipeline.
	FString DomainLabel;  // User-facing group label rendered in the Extensions panel.
	int32 SortOrder = 100;
	FString AgentSummary;
	TArray<FString> EnablesAgentTo;
	FString WhenToEnable;
	bool bIsRecommended = false;
	bool bHasMetadata = false;
};

class NEOSTACKAI_API FNeoStackExtensionUIMetadataLoader
{
public:
	/** Load Resources/ExtensionUI.json from the plugin base directory.
	 *  Returns false and leaves OutMetadata.bHasMetadata false when missing
	 *  or invalid, allowing callers to fall back to .uplugin Description.
	 */
	static bool LoadFromPluginDir(const FString& PluginName, const FString& BaseDir, FNeoStackExtensionUIMetadata& OutMetadata);
};
