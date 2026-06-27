// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** One entry in the NeoStack Cloud extension catalog. Matches the shape of
 *  the JSON returned by GET /api/extensions/catalog on neostack.dev. */
struct NEOSTACKAI_API FNeoStackCatalogEntry
{
	FString Slug;              // "neo-stack-ai-chaos-fracture"
	FString PluginName;        // "NeoStackAI_ChaosFracture" — matches FNeoStackManagedExtension::PluginName
	FString DisplayName;       // "Chaos Fracture"
	FString Description;
	FString LatestVersion;     // Empty if no release on the requested channel
	FString LatestChannel;     // Channel the latest release lives on
	FString PublishedAt;       // ISO8601
	FString Changelog;
	FString Domain;
	FString DomainLabel;
	int32 SortOrder = 100;
	FString AgentSummary;
	TArray<FString> EnablesAgentTo;
	FString WhenToEnable;
	bool bIsRecommended = false;
	TArray<FString> SupportedEngineVersions; // Engine versions that have artifacts for this release
};

enum class ENeoStackCatalogStatus : uint8
{
	Idle,       // Never fetched
	Fetching,   // Fetch in flight
	Ready,      // Cache has a successful payload
	Error,      // Last fetch failed
};

/** Per-fetch catalog result. */
struct NEOSTACKAI_API FNeoStackCatalogResult
{
	ENeoStackCatalogStatus Status = ENeoStackCatalogStatus::Idle;
	bool bSuccess = false;
	int32 HttpStatus = 0;
	FString ErrorMessage;
	FString Channel;    // Channel used for this fetch
	FString Engine;     // Engine version filter used (empty = "all engines" mode)
	FString VariantHint; // Server-recommended variant for this caller ('full' / 'binary'). Drives installer requests.
	TArray<FNeoStackCatalogEntry> Entries;
	FDateTime FetchedAt;
};

/** HTTP client + cache for the NeoStack Cloud extension catalog.
 *
 *  Usage from the WebUI bridge:
 *    1. RefreshAsync(channel, engineVersion)  — fires the HTTP request
 *    2. GetCachedResult()                      — read the latest cache snapshot
 *
 *  The Svelte panel polls the cache every ~250 ms while Status == Fetching
 *  until it flips to Ready or Error, matching the pattern already used by
 *  the plugin's self-update check. No blocking on the game thread.
 */
class NEOSTACKAI_API FNeoStackExtensionCatalog
{
public:
	/** Fire an async HTTP GET. Result lands in the module-static cache when
	 *  the HTTP callback runs (game thread, via UE's HTTP manager). Safe to
	 *  call repeatedly; later calls overwrite the cache state. */
	static void RefreshAsync(const FString& Channel, const FString& EngineVersion);

	/** Snapshot of the last fetch. Always safe to call from game thread. */
	static FNeoStackCatalogResult GetCachedResult();

	/** Wipe the cache (e.g. if the user rotates their API key or changes
	 *  the provider base URL). */
	static void ClearCache();

	/** JSON shape consumed by the WebUI bridge:
	 *  {
	 *    status: "idle"|"fetching"|"ready"|"error",
	 *    success: bool, httpStatus: int, channel: string, engine: string,
	 *    error?: string, fetchedAt?: "ISO",
	 *    extensions: [{...FNeoStackCatalogEntry...}]
	 *  }
	 */
	static FString ResultToJson(const FNeoStackCatalogResult& Result);
};
