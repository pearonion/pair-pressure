// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class IHttpRequest;

// ── Job status (universal across all providers) ──────────────────────

enum class EGenerativeJobStatus : uint8
{
	Pending,
	Running,
	Succeeded,
	Failed,
	Cancelled
};

// ── A job submitted to any provider ──────────────────────────────────

struct NEOSTACKAI_API FGenerativeJob
{
	FString ProviderId;
	FString ActionId;
	FString JobId;
	EGenerativeJobStatus Status = EGenerativeJobStatus::Pending;
	int32 Progress = 0; // 0-100

	// Results (populated on success)
	FString ResultUrl;      // primary download URL (GLB, FBX, PNG, WAV, etc.)
	FString ThumbnailUrl;
	TMap<FString, FString> ExtraUrls; // format→url ("fbx"→url, "glb"→url, "walking_fbx"→url, etc.)
	TArray<FString> ImageUrls;        // for text-to-image/image-to-image (multiple output images)

	FString ErrorMessage;
	TSharedPtr<FJsonObject> RawResponse; // full provider response for advanced access

	bool IsTerminal() const
	{
		return Status == EGenerativeJobStatus::Succeeded
			|| Status == EGenerativeJobStatus::Failed
			|| Status == EGenerativeJobStatus::Cancelled;
	}

	bool IsSuccess() const { return Status == EGenerativeJobStatus::Succeeded; }

	static FGenerativeJob MakePending(const FString& InJobId)
	{
		FGenerativeJob Job;
		Job.JobId = InJobId;
		Job.Status = EGenerativeJobStatus::Pending;
		return Job;
	}

	static FGenerativeJob MakeSuccess(const FString& InJobId, const FString& InResultUrl)
	{
		FGenerativeJob Job;
		Job.JobId = InJobId;
		Job.ResultUrl = InResultUrl;
		Job.Status = EGenerativeJobStatus::Succeeded;
		Job.Progress = 100;
		return Job;
	}

	static FGenerativeJob MakeFail(const FString& InError)
	{
		FGenerativeJob Job;
		Job.ErrorMessage = InError;
		Job.Status = EGenerativeJobStatus::Failed;
		return Job;
	}
};

// ── Describes one action a provider can perform ──────────────────────

struct NEOSTACKAI_API FProviderActionDescriptor
{
	FString ActionId;      // "text_to_3d", "rig", "retexture", "tts"
	FString Description;   // Agent-readable description

	// Loose hints for agent discovery (NOT enforced, just for routing/display)
	TArray<FString> InputHints;   // ["text"], ["image"], ["model","text"], ["job_ref"]
	TArray<FString> OutputHints;  // ["model"], ["animation"], ["image"], ["audio"]

	// Full JSON Schema for this action's parameters
	TSharedPtr<FJsonObject> ParamsSchema;

	// Informational
	FString CreditCost;             // "20 credits" (display only)
	bool bIsSynchronous = false;    // true = no polling needed (e.g., balance check, sync TTS)
};

// ── Async result types ───────────────────────────────────────────────
//
// All HTTP calls and provider methods are async-callback based. The callback
// fires on the game thread (helpers marshal back via UE::NeoStack::RunOnMainThread).
//
// Provider implementations do not block: they kick off an IHttpRequest with an
// OnProcessRequestComplete delegate, then return. The callback runs when the
// HTTP completes (typically a few ms to a few minutes later).
//
// Lua bindings use UE::NeoStack::Lua::AwaitAsync to yield the coroutine while
// waiting for the provider callback, so the editor stays interactive throughout.

/** Result of a JSON HTTP call. */
struct NEOSTACKAI_API FHttpJsonResult
{
	bool bSuccess = false;
	int32 ResponseCode = 0;
	TSharedPtr<FJsonObject> Json;     // parsed body (may be valid even on error if API returned JSON)
	FString Error;                    // populated on failure
};

/** Result of a binary HTTP call (e.g. raw audio bytes from TTS). */
struct NEOSTACKAI_API FHttpRawResult
{
	bool bSuccess = false;
	int32 ResponseCode = 0;
	FString ContentType;
	TArray<uint8> Bytes;
	FString Error;
};

/** Result of a file-download HTTP call. */
struct NEOSTACKAI_API FHttpDownloadResult
{
	bool bSuccess = false;
	int32 ResponseCode = 0;
	FString Error;
};

using FHttpJsonCallback     = TFunction<void(const FHttpJsonResult&)>;
using FHttpRawCallback      = TFunction<void(const FHttpRawResult&)>;
using FHttpDownloadCallback = TFunction<void(const FHttpDownloadResult&)>;
using FGenerativeJobCallback = TFunction<void(const FGenerativeJob&)>;

/**
 * Balance result callback.
 *
 *   Balance >= 0, Error empty   → success, the value is the credit balance
 *   Balance == -1, Error empty  → provider doesn't support balance checking (default impl)
 *   Balance == -1, Error set    → call failed (config issue, HTTP error, etc.) — Error explains why
 *
 * Callers should check Error first; if non-empty, surface it to the user.
 */
using FBalanceCallback      = TFunction<void(int32 Balance, const FString& Error)>;
using FBoolCallback         = TFunction<void(bool)>;

// ── The provider interface ───────────────────────────────────────────

class NEOSTACKAI_API IGenerativeProvider : public TSharedFromThis<IGenerativeProvider>
{
public:
	virtual ~IGenerativeProvider() = default;

	// Identity
	virtual FString GetId() const = 0;            // "meshy", "tripo", "elevenlabs"
	virtual FString GetDisplayName() const = 0;   // "Meshy AI"
	virtual FString GetWebsite() const { return TEXT(""); }

	// Auth & routing
	virtual FString GetDirectBaseUrl() const = 0;       // "https://api.meshy.ai"
	virtual FString GetApiKeySettingName() const = 0;   // "MeshyApiKey"

	// What can this provider do?
	virtual TArray<FProviderActionDescriptor> GetActions() const = 0;

	// Check if this provider supports a specific action
	bool SupportsAction(const FString& ActionId) const;

	// Find a specific action descriptor
	const FProviderActionDescriptor* FindAction(const FString& ActionId) const;

	// ── Execute (async) ──────────────────────────────────────────────
	//
	// All execute methods take an OnComplete callback that fires on the game
	// thread when the operation finishes. They return immediately — the actual
	// HTTP call happens in the background. Callers in Lua should use
	// UE::NeoStack::Lua::AwaitAsync to yield while waiting.

	virtual void Submit(const FString& ActionId,
		const TSharedPtr<FJsonObject>& Params,
		FGenerativeJobCallback OnComplete) = 0;

	virtual void CheckStatus(const FString& JobId,
		const FString& ActionId,
		FGenerativeJobCallback OnComplete) = 0;

	// Optional: some providers need a separate result-fetch call (default: same as CheckStatus).
	virtual void GetResult(const FString& JobId,
		const FString& ActionId,
		FGenerativeJobCallback OnComplete)
	{
		CheckStatus(JobId, ActionId, MoveTemp(OnComplete));
	}

	// Optional: cancel a running job (default: not supported, fires false).
	virtual void CancelJob(const FString& JobId, FBoolCallback OnComplete)
	{
		OnComplete(false);
	}

	// Optional: check credit balance (default: not supported, fires -1 with empty error).
	virtual void GetBalance(FBalanceCallback OnComplete)
	{
		OnComplete(-1, FString());
	}

	/**
	 * NeoStack Cloud mode flag.
	 *
	 * When true, the base class routes all HTTP traffic through the
	 * NeoStack proxy at https://neostack.dev/api/v1/{id} instead of the
	 * provider's direct API. The user's per-provider API key (if set in
	 * ACPSettings) is forwarded as an X-Neo-Provider-Key header — BYOK
	 * passthrough — letting power users keep paying the provider directly
	 * while still benefiting from unified analytics and usage caps.
	 *
	 * When false, requests go straight to the provider's API (legacy
	 * behaviour). This is the escape hatch for outage resilience or
	 * studios with strict third-party-routing policies.
	 *
	 * Default is false in the interface; concrete providers override this
	 * to read their per-provider toggle from UACPSettings.
	 */
	virtual bool UseCloudMode() const { return false; }

protected:
	// Cache action list for SupportsAction/FindAction lookups
	mutable TArray<FProviderActionDescriptor> CachedActions;
	mutable bool bActionsCached = false;

	const TArray<FProviderActionDescriptor>& GetCachedActions() const;
};

// ── Base class with HTTP/auth/download helpers (async) ───────────────

class NEOSTACKAI_API FGenerativeProviderBase : public IGenerativeProvider
{
public:
	virtual ~FGenerativeProviderBase() = default;

	// Parse status strings (SUCCEEDED, FAILED, etc.) to EGenerativeJobStatus
	static EGenerativeJobStatus ParseStatus(const FString& StatusStr);

protected:
	// Auth routing: returns the provider-specific key from settings
	virtual FString GetAuthToken() const;

	/**
	 * URL routing. In Direct mode, returns `GetDirectBaseUrl()`. In Cloud
	 * mode (UseCloudMode() == true), returns `https://neostack.dev/api/v1/{id}`
	 * so the NeoStack proxy can forward to the real provider.
	 */
	virtual FString GetBaseUrl() const;

	/**
	 * Default auth header application.
	 *   - Cloud mode: `Authorization: Bearer <NeoStackApiKey>` plus an
	 *     optional `X-Neo-Provider-Key` header carrying the provider key
	 *     (BYOK passthrough), so the proxy can use the user's own
	 *     upstream credentials when present.
	 *   - Direct mode: `Authorization: Bearer <providerKey>`.
	 *
	 * ElevenLabs overrides this to use `xi-api-key` in Direct mode but
	 * defers to the base in Cloud mode.
	 */
	virtual void SetAuthHeaders(const TSharedRef<class IHttpRequest, ESPMode::ThreadSafe>& Request) const;

	/** Read the user's NeoStack Cloud API key from settings (`NeoStackApiKey`).
	 *  Used by Cloud mode to authenticate against neostack.dev. */
	FString GetNeoStackApiKey() const;

	// ── Async HTTP helpers ───────────────────────────────────────────
	//
	// All return immediately. OnComplete fires on the game thread when the
	// HTTP request finishes (success, error, or timeout). The provider
	// implementation captures whatever state it needs in the callback's lambda.

	void HttpPost(const FString& Path,
		const TSharedPtr<FJsonObject>& Body,
		FHttpJsonCallback OnComplete,
		float TimeoutSeconds = 60.0f) const;

	void HttpGet(const FString& Path,
		FHttpJsonCallback OnComplete,
		float TimeoutSeconds = 60.0f) const;

	void HttpDelete(const FString& Path,
		FHttpJsonCallback OnComplete,
		float TimeoutSeconds = 30.0f) const;

	/** Download a URL to a file path. */
	void HttpDownload(const FString& Url,
		const FString& OutputPath,
		FHttpDownloadCallback OnComplete,
		float TimeoutSeconds = 300.0f) const;

	/** POST with JSON body, receive raw binary response (e.g. audio bytes from TTS). */
	void HttpPostRaw(const FString& Path,
		const TSharedPtr<FJsonObject>& Body,
		FHttpRawCallback OnComplete,
		float TimeoutSeconds = 120.0f) const;

	// Build a JSON Schema object for a single property
	static TSharedPtr<FJsonObject> SchemaString(const FString& Desc,
		const TArray<FString>& Enum = {}, const FString& Default = TEXT(""));
	static TSharedPtr<FJsonObject> SchemaInt(const FString& Desc,
		int32 Min = 0, int32 Max = 0, int32 Default = 0);
	static TSharedPtr<FJsonObject> SchemaBool(const FString& Desc, bool Default = false);
	static TSharedPtr<FJsonObject> SchemaStringArray(const FString& Desc);

	// Build a complete JSON Schema with properties map and required list
	static TSharedPtr<FJsonObject> BuildSchema(
		const TMap<FString, TSharedPtr<FJsonObject>>& Properties,
		const TArray<FString>& Required = {});

private:
	// Internal HTTP dispatch — builds, sends, marshals callback to game thread.
	void DispatchJsonRequest(const FString& Verb,
		const FString& FullUrl,
		const TSharedPtr<FJsonObject>& Body,
		FHttpJsonCallback OnComplete,
		float TimeoutSeconds) const;
};

// ── Provider auto-registration macro ─────────────────────────────────
// Uses the same pattern as REGISTER_LUA_BINDING — static constructor calls into registry directly.

#define REGISTER_GENERATIVE_PROVIDER(ProviderClass) \
	static struct FAutoReg_Provider_##ProviderClass { \
		FAutoReg_Provider_##ProviderClass() { \
			FDeferredProviderRegistration::Get().Add([]() { \
				FGenerativeProviderRegistry::Get().Register(MakeShared<ProviderClass>()); \
			}); \
		} \
	} GAutoReg_Provider_##ProviderClass;

// Deferred registration helper — collects provider registrations during static init,
// executes them once during module startup (after UACPSettings is available).
class NEOSTACKAI_API FDeferredProviderRegistration
{
public:
	static FDeferredProviderRegistration& Get();
	void Add(TFunction<void()> Func);
	void ExecuteAll();

private:
	TArray<TFunction<void()>> PendingRegistrations;
	bool bExecuted = false;
};
