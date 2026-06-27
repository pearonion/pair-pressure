// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

/**
 * Per-launch entitlement check against neostack.dev. Fired once from
 * StartupModule (after the existing update-check ticker). Result is cached
 * for the editor session and read by NeoStackToolRegistry::Execute,
 * CheckForPluginUpdate, FRelayClient, and the WebUI bridge to gate features
 * for users without an active subscription.
 *
 * The fail-open / fail-closed rule turns on the .uplugin Installed flag (set
 * to true on the binary variant the workflow ships to subscribers; false on
 * the source/full variant lifetime owners receive). Source builds are
 * trusted offline; binary builds must phone home.
 */
enum class EEntitlementResult : uint8
{
	Unknown,           // Pre-check or in-flight
	Lifetime,          // user_plugin_entitlement row exists
	ActiveSubscription,// individual or studio seat
	NotEntitled,       // server says no
	NetworkError       // couldn't reach neostack.dev
};

DECLARE_MULTICAST_DELEGATE(FOnEntitlementResolved);
DECLARE_MULTICAST_DELEGATE(FOnEntitlementAccountChanged);

class NEOSTACKAI_API FEntitlementClient
{
public:
	static FEntitlementClient& Get();

	/** Fire the async HTTP request. One-shot per session. */
	void Check();

	/** Re-run the entitlement check after the startup one-shot has resolved. */
	void Refresh();

	/**
	 * True if the plugin should currently be allowed to operate. STRICT for
	 * binary builds — they must reach neostack.dev on every editor launch
	 * (the user-confirmed design choice) — so this returns false until the
	 * HTTP response (or a synthetic NetworkError on failure to dispatch)
	 * lands. Source/full builds (compile-time macro = 0, baked into the
	 * DLL by the release workflow) are always trusted: a stale key,
	 * network failure, or "not entitled" response must not disable the
	 * plugin for someone with the source on disk.
	 *
	 * Callers that need to defer work past the in-flight window should
	 * register on OnResolved() instead of polling — see FRelayClient.
	 */
	bool IsEntitled() const;

	/** True while we're still waiting for the first response (binary builds
	 *  only — source builds skip the check and resolve synchronously). Lets
	 *  the tool registry distinguish "verifying" from "denied". */
	bool IsCheckPending() const;

	/** Last raw classification from the server (or Unknown / NetworkError). */
	EEntitlementResult GetResult() const { return Result; }

	/** True once the HTTP call has resolved one way or another. */
	bool HasChecked() const { return bChecked; }

	/**
	 * Fires exactly once when bChecked flips true. Subscribers added after
	 * resolution are invoked synchronously before this returns so deferred work
	 * can register at any point without missing the event. Returns a removable
	 * handle when queued; returns an invalid handle when invoked synchronously.
	 */
	FDelegateHandle WhenResolved(TFunction<void()> Callback);

	/** Remove a queued WhenResolved callback. Safe to call with an invalid handle. */
	void RemoveWhenResolved(FDelegateHandle Handle);

	/** Last successful /api/plugin/entitlement/status body (empty when never fetched). */
	const FString& GetCachedAccountJson() const { return CachedAccountJson; }

	/** Fires whenever account cache or entitlement resolution changes (including refresh). */
	FOnEntitlementAccountChanged& OnAccountChanged() { return AccountChanged; }

private:
	FEntitlementClient() = default;

	void HandleResponse(EEntitlementResult InResult, const FString* HttpBody = nullptr);
	void BroadcastAccountChanged();

	bool bRequestStarted = false; // one-shot guard; flips true when Check() fires
	bool bRequestInFlight = false; // true while an HTTP request is outstanding
	bool bChecked = false;        // flips true once a response (or its synthetic equivalent) arrives
	EEntitlementResult Result = EEntitlementResult::Unknown;
	// Last result we surfaced via UE_LOG. The binary refresh ticker fires
	// every 300s; without this guard, a user with a missing or expired key
	// would see the "No active entitlement" warning every 5 minutes for
	// the whole editor session, which reads to lifetime owners (who landed
	// on the binary track by accident) as "the plugin keeps nagging me".
	// Compare against this before logging so we only emit on state change.
	EEntitlementResult LastLoggedResult = EEntitlementResult::Unknown;
	FOnEntitlementResolved OnResolved;
	FOnEntitlementAccountChanged AccountChanged;
	FString CachedAccountJson;
};
