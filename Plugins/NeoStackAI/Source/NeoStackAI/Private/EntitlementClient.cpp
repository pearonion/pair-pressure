// Copyright 2025 Betide Studio. All Rights Reserved.

#include "EntitlementClient.h"

#include "ACPSettings.h"
#include "BuildVariant.h"
#include "HttpModule.h"
#include "NSAIAnalytics.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "NeoStackAIModule.h"

namespace
{
	// Mirror of the helpers in NeoStackAIModule.cpp. Kept local to avoid
	// changing that file's namespace anonymity.
	FString ResolveBaseUrl()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
			if (!Override.IsEmpty())
			{
				FString Root = Override;
				Root.RemoveFromEnd(TEXT("/"));
				Root.RemoveFromEnd(TEXT("/v1"));
				Root.RemoveFromEnd(TEXT("/"));
				return Root;
			}
		}
		return TEXT("https://neostack.dev");
	}

	FString ResolveApiKey()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			return Settings->GetChatProviderApiKey(TEXT("neostack"));
		}
		return FString();
	}

	constexpr bool ResolveBinaryBuild()
	{
		// Compile-time. The release workflow rewrites the literal in
		// BuildVariant.h before the binary compile pass; the value here
		// is baked into the DLL. Cannot be flipped post-build by editing
		// any on-disk file — see BuildVariant.h for the rationale.
#if NEOSTACK_BUILD_VARIANT_BINARY
		return true;
#else
		return false;
#endif
	}

	EEntitlementResult ParseStatus(const FString& StatusField)
	{
		// Server returns: lifetime / subscription / lifetime+subscription /
		// studio-seat / lifetime+studio-seat / none.
		if (StatusField.Contains(TEXT("lifetime"))) return EEntitlementResult::Lifetime;
		if (StatusField == TEXT("subscription") || StatusField == TEXT("studio-seat"))
		{
			return EEntitlementResult::ActiveSubscription;
		}
		return EEntitlementResult::NotEntitled;
	}

	const TCHAR* EntitlementStatusForAnalytics(EEntitlementResult Result)
	{
		switch (Result)
		{
		case EEntitlementResult::Lifetime:
		case EEntitlementResult::ActiveSubscription:
			return TEXT("valid");
		case EEntitlementResult::NotEntitled:
			return TEXT("not_entitled");
		case EEntitlementResult::NetworkError:
			return TEXT("network_error");
		case EEntitlementResult::Unknown:
		default:
			return TEXT("unknown");
		}
	}

	const TCHAR* EntitlementKindForAnalytics(EEntitlementResult Result)
	{
		switch (Result)
		{
		case EEntitlementResult::Lifetime:
			return TEXT("lifetime");
		case EEntitlementResult::ActiveSubscription:
			return TEXT("subscription");
		case EEntitlementResult::NotEntitled:
			return TEXT("none");
		case EEntitlementResult::NetworkError:
			return TEXT("network_error");
		case EEntitlementResult::Unknown:
		default:
			return TEXT("unknown");
		}
	}

	void RecordCloudAuthState(EEntitlementResult Result, bool bBinaryBuild)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("status"), EntitlementStatusForAnalytics(Result));
		Props->SetStringField(TEXT("entitlement"), EntitlementKindForAnalytics(Result));
		Props->SetBoolField(TEXT("has_key"), !ResolveApiKey().IsEmpty());
		Props->SetBoolField(TEXT("binary_build"), bBinaryBuild);
		FNSAIAnalytics::Get().RecordEvent(TEXT("cloud_auth_state"), Props);
	}
}

FEntitlementClient& FEntitlementClient::Get()
{
	static FEntitlementClient Instance;
	return Instance;
}

bool FEntitlementClient::IsEntitled() const
{
	// Source/full build: always trusted. The point of buying lifetime (or
	// running source from CI) is that it doesn't depend on a network
	// round-trip or a current API key.
	if (!ResolveBinaryBuild()) return true;

	// Binary build: STRICT. Must reach neostack.dev. Until the response
	// lands (or fails synthetically), entitlement is not granted. Callers
	// that need to defer work past this window should use WhenResolved().
	if (!bChecked) return false;

	switch (Result)
	{
	case EEntitlementResult::Lifetime:
	case EEntitlementResult::ActiveSubscription:
		return true;
	case EEntitlementResult::NotEntitled:
	case EEntitlementResult::NetworkError:
	case EEntitlementResult::Unknown:
	default:
		return false;
	}
}

bool FEntitlementClient::IsCheckPending() const
{
	// Source builds skip the network check entirely (Check() resolves
	// synchronously to Lifetime), so this only ever fires for binary
	// builds with the request in flight.
	return bRequestStarted && !bChecked;
}

FDelegateHandle FEntitlementClient::WhenResolved(TFunction<void()> Callback)
{
	if (bChecked)
	{
		// Already resolved — invoke synchronously so latecomers don't miss
		// the one-shot event.
		Callback();
		return FDelegateHandle();
	}
	return OnResolved.AddLambda(MoveTemp(Callback));
}

void FEntitlementClient::RemoveWhenResolved(FDelegateHandle Handle)
{
	if (Handle.IsValid())
	{
		OnResolved.Remove(Handle);
	}
}

void FEntitlementClient::Check()
{
	if (bRequestStarted) return;
	bRequestStarted = true;
	bRequestInFlight = true;

	const bool bBinaryBuild = ResolveBinaryBuild();
	const FString ApiKey = ResolveApiKey();

	// No key set:
	//   - Source/full build → mark checked with Lifetime; IsEntitled() is
	//     already true via the compile-time !ResolveBinaryBuild()
	//     short-circuit, but this keeps GetResult() informative for the
	//     WebUI banner.
	//   - Binary build → hard fail; this build REQUIRES a key.
	if (ApiKey.IsEmpty())
	{
		if (bBinaryBuild)
		{
			HandleResponse(EEntitlementResult::NotEntitled);
			UE_LOG(LogNeoStackAI, Warning,
				TEXT("[NSAI] Binary build requires a NeoStack API key. ")
				TEXT("Set one in Settings > Chat & Agents > Chat Providers > NeoStack Cloud."));
		}
		else
		{
			HandleResponse(EEntitlementResult::Lifetime);
		}
		return;
	}

	const FString Url = FString::Printf(
		TEXT("%s/api/plugin/entitlement/status"),
		*ResolveBaseUrl());

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(10.0f);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));

	Request->OnProcessRequestComplete().BindLambda(
		[bBinaryBuild](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			const int32 Code = (bConnectedSuccessfully && Response.IsValid()) ? Response->GetResponseCode() : 0;
			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("[NSAI] Entitlement check: network error (binary=%s)."),
					bBinaryBuild ? TEXT("true") : TEXT("false"));
				FEntitlementClient::Get().HandleResponse(EEntitlementResult::NetworkError, nullptr);
				return;
			}
			if (Code == 401 || Code == 403)
			{
				FEntitlementClient::Get().HandleResponse(EEntitlementResult::NotEntitled, nullptr);
				return;
			}
			if (Code != 200)
			{
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("[NSAI] Entitlement check failed (HTTP %d)."), Code);
				FEntitlementClient::Get().HandleResponse(EEntitlementResult::NetworkError, nullptr);
				return;
			}

			const FString Body = Response->GetContentAsString();
			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				FEntitlementClient::Get().HandleResponse(EEntitlementResult::NetworkError, nullptr);
				return;
			}

			FString Status;
			Json->TryGetStringField(TEXT("status"), Status);
			FEntitlementClient::Get().HandleResponse(ParseStatus(Status), &Body);
		});

	// ProcessRequest can return false if the HTTP module rejects dispatch
	// (e.g. malformed URL, module shutting down). Without this synthesis
	// the OnProcessRequestComplete lambda never fires and bChecked stays
	// false forever — a binary build would sit in "verifying" indefinitely
	// and a strict-mode IsEntitled() would never return true. Synthesise a
	// NetworkError so deferred callbacks fire and the WebUI banner appears.
	if (!Request->ProcessRequest())
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("[NSAI] Entitlement check failed to dispatch HTTP request."));
		HandleResponse(EEntitlementResult::NetworkError);
	}
}

void FEntitlementClient::Refresh()
{
	if (bRequestInFlight) return;
	bRequestStarted = false;
	Check();
}

void FEntitlementClient::BroadcastAccountChanged()
{
	AccountChanged.Broadcast();
}

void FEntitlementClient::HandleResponse(EEntitlementResult InResult, const FString* HttpBody)
{
	bRequestInFlight = false;
	Result = InResult;
	bChecked = true;

	if (HttpBody && !HttpBody->IsEmpty())
	{
		CachedAccountJson = *HttpBody;
	}
	else if (InResult == EEntitlementResult::NotEntitled || ResolveApiKey().IsEmpty())
	{
		CachedAccountJson.Empty();
	}

	const bool bBinaryBuild = ResolveBinaryBuild();
	// Only surface log lines on a state transition. The 300s refresh ticker
	// would otherwise re-log the same warning every 5 minutes for the
	// whole session — the editor output log fills with "No active
	// entitlement" repeats and the plugin reads as nagware. Positive
	// transitions (e.g. user pasted a working key) still log so the user
	// gets confirmation.
	const bool bChanged = (InResult != LastLoggedResult);

	switch (InResult)
	{
	case EEntitlementResult::Lifetime:
	case EEntitlementResult::ActiveSubscription:
		if (bChanged)
		{
			UE_LOG(LogNeoStackAI, Log, TEXT("[NSAI] Entitlement verified: %s."),
				InResult == EEntitlementResult::Lifetime ? TEXT("lifetime") : TEXT("subscription"));
		}
		break;

	case EEntitlementResult::NotEntitled:
		// Source builds shrug this off — IsEntitled() short-circuits when
		// the compile-time variant macro is 0. Binary builds get locked
		// down by the same rule.
		if (bChanged)
		{
			UE_LOG(LogNeoStackAI, Warning,
				TEXT("[NSAI] No active entitlement (binary=%s). %s"),
				bBinaryBuild ? TEXT("true") : TEXT("false"),
				bBinaryBuild
					? TEXT("Renew at https://neostack.dev/account.")
					: TEXT("Source build — continuing without restrictions."));
		}
		break;

	case EEntitlementResult::NetworkError:
		if (bChanged)
		{
			UE_LOG(LogNeoStackAI, Warning,
				TEXT("[NSAI] Couldn't verify entitlement (network, binary=%s). Running %s."),
				bBinaryBuild ? TEXT("true") : TEXT("false"),
				bBinaryBuild ? TEXT("in limited mode") : TEXT("normally (source build)"));
		}
		break;

	case EEntitlementResult::Unknown:
		// Defensive: don't fire OnResolved on Unknown — keep things in a
		// pending state. Caller should always pass a definitive value.
		bChecked = false;
		return;
	}

	LastLoggedResult = InResult;
	RecordCloudAuthState(InResult, bBinaryBuild);

	// Fire one-shot resolution callbacks (RelayClient::Initialize defers
	// onto this; ditto any future feature that needs to wait). Move-clear
	// so subsequent registrations go through the synchronous path in
	// WhenResolved().
	FOnEntitlementResolved Local = MoveTemp(OnResolved);
	OnResolved.Clear();
	Local.Broadcast();
	BroadcastAccountChanged();
}
