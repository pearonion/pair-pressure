// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/ChatTypes.h"
#include "Chat/IChatProvider.h"
#include "Extensions/NeoStackExtensionTypes.h"

/**
 * Reason for a model-registry change broadcast. Consumers use this to decide
 * whether to refresh just one provider's section of the picker or the whole list.
 */
enum class EChatModelRegistryChange : uint8
{
	Refreshed,            // A provider's model list was fetched/updated.
	AuthChanged,          // A provider's API key was added/changed/removed.
	UserProviderEdited,   // A user-defined provider was created/deleted/modified.
	SelectionChanged,     // The currently-selected model changed.
	Cleared               // The whole registry was reset.
};

struct FChatModelRegistryChange
{
	// Which provider triggered the change. Empty if Reason == Cleared or
	// if the change affected multiple providers at once.
	FString ProviderId;
	EChatModelRegistryChange Reason = EChatModelRegistryChange::Refreshed;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatModelRegistryChanged, const FChatModelRegistryChange&);

/**
 * Single source of truth for chat providers and their models.
 *
 * Responsibilities:
 *   - Hold the list of built-in and user-defined providers.
 *   - Cache each provider's model list (with stale-while-revalidate later).
 *   - Expose a flat / grouped view to the picker.
 *   - Track the currently selected model in prefixed "<providerId>:<modelId>"
 *     form, persisting through UACPSettings::SelectedChatModelId.
 *
 * Thread contract: all public methods are called from the game thread.
 * Discovery fires HTTP requests whose callbacks may arrive on any thread;
 * the registry marshals state updates and OnChanged broadcasts back to the
 * game thread before exposing them to consumers.
 */
class NEOSTACKAI_API FChatModelRegistry
{
public:
	static FChatModelRegistry& Get();

	/** Called by module startup after RegisterBuiltIns. Kicks off initial discovery. */
	void Initialize();

	/** Cancels in-flight discovery and clears provider instances. */
	void Shutdown();

	// ── Provider registration ──────────────────────────────────────────

	/** Register a built-in provider. Typically called from FChatProviderRegistrar::RegisterBuiltIns. */
	void RegisterProvider(TSharedRef<IChatProvider> Provider);
	FNeoStackExtensionHandle RegisterOwnedProvider(const FString& OwnerExtensionId, TSharedRef<IChatProvider> Provider);
	bool UnregisterProvider(const FGuid& RegistrationId);
	int32 UnregisterProvidersForOwner(const FString& OwnerExtensionId);

	/** Rebuild FUserDefinedProvider instances from UACPSettings::UserChatProviders. */
	void SyncUserProviders();

	/** Lookup by id (built-in or user). */
	TSharedPtr<IChatProvider> FindProvider(const FString& ProviderId) const;

	/** All known providers (built-in + user), in registration order. */
	TArray<TSharedRef<IChatProvider>> GetAllProviders() const;

	/** Providers that pass ValidateConfig and whose settings row is enabled. */
	TArray<TSharedRef<IChatProvider>> GetActiveProviders() const;

	// ── Model access ───────────────────────────────────────────────────

	/** Models currently cached for a provider. May be the static fallback if no discovery has run. */
	TArray<FChatModelInfo> GetModels(const FString& ProviderId) const;

	/** Flat list of all models across all active providers, tagged with ProviderId / ProviderDisplayName. */
	TArray<FChatModelInfo> GetAllModelsFlat() const;

	/** Grouped view for the picker: map from providerId to its model list. Iteration order matches provider registration. */
	TArray<TPair<TSharedRef<IChatProvider>, TArray<FChatModelInfo>>> GetAllModelsGrouped() const;

	// ── Refresh ────────────────────────────────────────────────────────

	/** Trigger async discovery for every active provider that supports it. */
	void RefreshAll();

	/** Trigger async discovery for one provider. No-op if the provider doesn't support discovery. */
	void RefreshProvider(const FString& ProviderId);

	// ── Selection ──────────────────────────────────────────────────────

	/**
	 * Set the currently selected model by prefixed id ("<providerId>:<modelId>").
	 * Persists to settings. Broadcasts SelectionChanged.
	 */
	void SetSelectedModel(const FString& PrefixedId);

	/** Current prefixed id, or empty if none is set. */
	FString GetSelectedModel() const;

	/**
	 * Resolve the currently selected provider. Falls back to the first active
	 * provider if the selected model points at an unknown provider.
	 */
	TSharedPtr<IChatProvider> GetSelectedProvider() const;

	/**
	 * Bare model id of the current selection (no prefix). Returns the selected
	 * provider's default model if none is set.
	 */
	FString GetSelectedBareModelId() const;

	// ── Change notifications ───────────────────────────────────────────

	FOnChatModelRegistryChanged OnChanged;

private:
	struct FProviderModelCache
	{
		TArray<FChatModelInfo> Models;
		FDateTime LastFetched;
		bool bLastFetchOk = false;
		FString LastError;
		bool bFetchInFlight = false;
	};

	struct FRegisteredProvider
	{
		TSharedRef<IChatProvider> Provider;
		FString OwnerExtensionId;
		FGuid RegistrationId;
	};

	/** Broadcast on the game thread, even if invoked from a worker thread. */
	void Broadcast(FChatModelRegistryChange Change);

	/** Populate a cache entry with the provider's static model list. */
	void SeedStaticFallback(const FString& ProviderId);

	mutable FCriticalSection Lock;
	TArray<FRegisteredProvider> BuiltInProviders;
	TArray<TSharedRef<IChatProvider>> UserProviders;
	TMap<FString, FProviderModelCache> Caches;

	bool bInitialized = false;
};
