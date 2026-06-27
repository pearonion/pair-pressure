// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Extensions/NeoStackExtensionCatalog.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "GenericPlatform/GenericPlatformHttp.h"

// ────────────────────────────────────────────────────────────────
// Module-static cache
// ────────────────────────────────────────────────────────────────
//
// UE HTTP callbacks fire on the game thread, as do WebUI bridge calls. That
// means both read and write to this cache happen on the same thread, so no
// mutex is required. If we ever move HTTP completion off game thread, revisit.

static FNeoStackCatalogResult GCachedCatalog;

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static FString ResolveCatalogBaseUrl()
{
	// Prefer the user's per-provider base URL override (same field used by
	// FNeoStackCloudProvider's AI calls). Fall back to the custom domain.
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
		if (!Override.IsEmpty())
		{
			// The AI provider's base URL ends with /v1 (for chat completions).
			// Strip it so we end up at the root for other /api/* routes.
			FString Root = Override;
			Root.RemoveFromEnd(TEXT("/"));
			Root.RemoveFromEnd(TEXT("/v1"));
			Root.RemoveFromEnd(TEXT("/"));
			return Root;
		}
	}
	return TEXT("https://neostack.dev");
}

static FString ResolveCatalogApiKey()
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->GetChatProviderApiKey(TEXT("neostack"));
	}
	return FString();
}

static const TCHAR* StatusToString(ENeoStackCatalogStatus S)
{
	switch (S)
	{
		case ENeoStackCatalogStatus::Idle:     return TEXT("idle");
		case ENeoStackCatalogStatus::Fetching: return TEXT("fetching");
		case ENeoStackCatalogStatus::Ready:    return TEXT("ready");
		case ENeoStackCatalogStatus::Error:    return TEXT("error");
		default: return TEXT("unknown");
	}
}

// ────────────────────────────────────────────────────────────────
// RefreshAsync
// ────────────────────────────────────────────────────────────────

void FNeoStackExtensionCatalog::RefreshAsync(const FString& Channel, const FString& EngineVersion)
{
	// Mark the cache as in-flight first so pollers see progress even before
	// the HTTP request resolves. Preserve prior Entries so the Svelte panel
	// can render stale-but-ok data while a refresh is running.
	{
		GCachedCatalog.Status = ENeoStackCatalogStatus::Fetching;
		GCachedCatalog.bSuccess = false;
		GCachedCatalog.ErrorMessage = FString();
		GCachedCatalog.HttpStatus = 0;
		GCachedCatalog.Channel = Channel;
		GCachedCatalog.Engine = EngineVersion;
	}

	// Catalog now serves anonymous callers (Fab buyers can browse before
	// signing in). Send the Bearer only if we have one — the server uses it
	// to compute variantHint for lifetime owners.
	const FString ApiKey = ResolveCatalogApiKey();

	const FString BaseUrl = ResolveCatalogBaseUrl();
	FString Url = FString::Printf(TEXT("%s/api/extensions/catalog?channel=%s"),
		*BaseUrl,
		*FGenericPlatformHttp::UrlEncode(Channel));
	if (!EngineVersion.IsEmpty())
	{
		Url += FString::Printf(TEXT("&engine=%s"), *FGenericPlatformHttp::UrlEncode(EngineVersion));
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}

	// Capture the requested channel/engine so the completion callback can
	// decide whether this response is still relevant (user may have changed
	// channel while we were in flight — newer request wins).
	const FString FetchChannel = Channel;
	const FString FetchEngine = EngineVersion;

	Request->OnProcessRequestComplete().BindLambda(
		[FetchChannel, FetchEngine](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			// If another Refresh was kicked off with different params while
			// this one was in flight, drop the stale result on the floor so
			// we don't stomp the newer fetch's state.
			if (GCachedCatalog.Channel != FetchChannel || GCachedCatalog.Engine != FetchEngine)
			{
				return;
			}

			FNeoStackCatalogResult Result;
			Result.Channel = FetchChannel;
			Result.Engine = FetchEngine;
			Result.FetchedAt = FDateTime::UtcNow();

			if (!bConnected || !Response.IsValid())
			{
				Result.Status = ENeoStackCatalogStatus::Error;
				Result.ErrorMessage = TEXT("Could not reach the NeoStack Cloud catalog endpoint.");
				GCachedCatalog = MoveTemp(Result);
				UE_LOG(LogNeoStackAI, Warning, TEXT("Extension catalog: network failure"));
				return;
			}

			Result.HttpStatus = Response->GetResponseCode();
			const FString Body = Response->GetContentAsString();

			if (Result.HttpStatus != 200)
			{
				Result.Status = ENeoStackCatalogStatus::Error;
				TSharedPtr<FJsonObject> Err;
				TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(Body);
				if (FJsonSerializer::Deserialize(ErrReader, Err) && Err.IsValid())
				{
					const TSharedPtr<FJsonObject>* ErrObj = nullptr;
					if (Err->TryGetObjectField(TEXT("error"), ErrObj) && ErrObj && ErrObj->IsValid())
					{
						(*ErrObj)->TryGetStringField(TEXT("message"), Result.ErrorMessage);
					}
				}
				if (Result.ErrorMessage.IsEmpty())
				{
					Result.ErrorMessage = FString::Printf(TEXT("Catalog HTTP %d"), Result.HttpStatus);
				}
				GCachedCatalog = MoveTemp(Result);
				UE_LOG(LogNeoStackAI, Warning, TEXT("Extension catalog: %s"), *GCachedCatalog.ErrorMessage);
				return;
			}

			TSharedPtr<FJsonObject> Root;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Result.Status = ENeoStackCatalogStatus::Error;
				Result.ErrorMessage = TEXT("Catalog response was not valid JSON.");
				GCachedCatalog = MoveTemp(Result);
				UE_LOG(LogNeoStackAI, Warning, TEXT("Extension catalog: invalid JSON"));
				return;
			}

			// Variant hint: tells the installer queue which build to fetch.
			// Lifetime owners get 'full', subscribers/seat-holders get 'binary'.
			// Falls back to 'binary' so older servers still work.
			Root->TryGetStringField(TEXT("variantHint"), Result.VariantHint);
			if (Result.VariantHint.IsEmpty()) Result.VariantHint = TEXT("binary");

			const TArray<TSharedPtr<FJsonValue>>* ExtArr = nullptr;
			if (Root->TryGetArrayField(TEXT("extensions"), ExtArr))
			{
				for (const TSharedPtr<FJsonValue>& V : *ExtArr)
				{
					const TSharedPtr<FJsonObject> Obj = V->AsObject();
					if (!Obj.IsValid()) continue;
					FNeoStackCatalogEntry E;
					Obj->TryGetStringField(TEXT("slug"), E.Slug);
					Obj->TryGetStringField(TEXT("pluginName"), E.PluginName);
					Obj->TryGetStringField(TEXT("name"), E.DisplayName);
					Obj->TryGetStringField(TEXT("description"), E.Description);
					Obj->TryGetStringField(TEXT("latestVersion"), E.LatestVersion);
					Obj->TryGetStringField(TEXT("latestChannel"), E.LatestChannel);
					Obj->TryGetStringField(TEXT("publishedAt"), E.PublishedAt);
					Obj->TryGetStringField(TEXT("changelog"), E.Changelog);
					Obj->TryGetStringField(TEXT("domain"), E.Domain);
					Obj->TryGetStringField(TEXT("domainLabel"), E.DomainLabel);
					double SortOrder = 100.0;
					if (Obj->TryGetNumberField(TEXT("sortOrder"), SortOrder))
					{
						E.SortOrder = static_cast<int32>(SortOrder);
					}
					Obj->TryGetStringField(TEXT("agentSummary"), E.AgentSummary);
					Obj->TryGetStringField(TEXT("whenToEnable"), E.WhenToEnable);
					Obj->TryGetBoolField(TEXT("isRecommended"), E.bIsRecommended);
					const TArray<TSharedPtr<FJsonValue>>* EnablesAgentToArr = nullptr;
					if (Obj->TryGetArrayField(TEXT("enablesAgentTo"), EnablesAgentToArr))
					{
						for (const TSharedPtr<FJsonValue>& CapabilityValue : *EnablesAgentToArr)
						{
							FString Capability;
							if (CapabilityValue->TryGetString(Capability)) E.EnablesAgentTo.Add(Capability);
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* EnginesArr = nullptr;
					if (Obj->TryGetArrayField(TEXT("supportedEngineVersions"), EnginesArr))
					{
						for (const TSharedPtr<FJsonValue>& EV : *EnginesArr)
						{
							FString S;
							if (EV->TryGetString(S)) E.SupportedEngineVersions.Add(S);
						}
					}
					Result.Entries.Add(MoveTemp(E));
				}
			}

			Result.Status = ENeoStackCatalogStatus::Ready;
			Result.bSuccess = true;
			GCachedCatalog = MoveTemp(Result);
			UE_LOG(LogNeoStackAI, Log, TEXT("Extension catalog: %d entries for %s%s"),
				GCachedCatalog.Entries.Num(),
				*FetchChannel,
				FetchEngine.IsEmpty() ? TEXT(" (all engines)") : *FString::Printf(TEXT(" / UE %s"), *FetchEngine));
		});

	Request->ProcessRequest();
}

FNeoStackCatalogResult FNeoStackExtensionCatalog::GetCachedResult()
{
	return GCachedCatalog;
}

void FNeoStackExtensionCatalog::ClearCache()
{
	GCachedCatalog = FNeoStackCatalogResult();
}

FString FNeoStackExtensionCatalog::ResultToJson(const FNeoStackCatalogResult& Result)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("status"), StatusToString(Result.Status));
	Root->SetBoolField(TEXT("success"), Result.bSuccess);
	Root->SetNumberField(TEXT("httpStatus"), Result.HttpStatus);
	Root->SetStringField(TEXT("channel"), Result.Channel);
	Root->SetStringField(TEXT("engine"), Result.Engine);
	// Server-derived hint: 'full' if caller owns lifetime, else 'binary'.
	// The Svelte panel renders tier-aware copy from this; the installer
	// queue also reads it via GetCachedResult().VariantHint.
	if (!Result.VariantHint.IsEmpty())
	{
		Root->SetStringField(TEXT("variantHint"), Result.VariantHint);
	}
	if (!Result.ErrorMessage.IsEmpty())
	{
		Root->SetStringField(TEXT("error"), Result.ErrorMessage);
	}
	if (Result.FetchedAt.GetTicks() != 0)
	{
		Root->SetStringField(TEXT("fetchedAt"), Result.FetchedAt.ToIso8601());
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FNeoStackCatalogEntry& E : Result.Entries)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("slug"), E.Slug);
		O->SetStringField(TEXT("pluginName"), E.PluginName);
		O->SetStringField(TEXT("name"), E.DisplayName);
		O->SetStringField(TEXT("description"), E.Description);
		O->SetStringField(TEXT("latestVersion"), E.LatestVersion);
		O->SetStringField(TEXT("latestChannel"), E.LatestChannel);
		O->SetStringField(TEXT("publishedAt"), E.PublishedAt);
		O->SetStringField(TEXT("changelog"), E.Changelog);
		O->SetStringField(TEXT("domain"), E.Domain);
		O->SetStringField(TEXT("domainLabel"), E.DomainLabel);
		O->SetNumberField(TEXT("sortOrder"), E.SortOrder);
		O->SetStringField(TEXT("agentSummary"), E.AgentSummary);
		O->SetStringField(TEXT("whenToEnable"), E.WhenToEnable);
		O->SetBoolField(TEXT("isRecommended"), E.bIsRecommended);
		TArray<TSharedPtr<FJsonValue>> EnablesAgentToArr;
		for (const FString& Capability : E.EnablesAgentTo)
		{
			EnablesAgentToArr.Add(MakeShared<FJsonValueString>(Capability));
		}
		O->SetArrayField(TEXT("enablesAgentTo"), EnablesAgentToArr);
		TArray<TSharedPtr<FJsonValue>> EngineArr;
		for (const FString& V : E.SupportedEngineVersions)
		{
			EngineArr.Add(MakeShared<FJsonValueString>(V));
		}
		O->SetArrayField(TEXT("supportedEngineVersions"), EngineArr);
		Arr.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("extensions"), Arr);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}
