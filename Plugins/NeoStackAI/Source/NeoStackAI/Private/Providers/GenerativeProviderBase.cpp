// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProvider.h"
#include "ACPSettings.h"
#include "RunOnMainThread.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"

// ── IGenerativeProvider base methods ─────────────────────────────────

bool IGenerativeProvider::SupportsAction(const FString& ActionId) const
{
	return FindAction(ActionId) != nullptr;
}

const FProviderActionDescriptor* IGenerativeProvider::FindAction(const FString& ActionId) const
{
	const auto& Actions = GetCachedActions();
	for (const auto& Action : Actions)
	{
		if (Action.ActionId == ActionId)
		{
			return &Action;
		}
	}
	return nullptr;
}

const TArray<FProviderActionDescriptor>& IGenerativeProvider::GetCachedActions() const
{
	if (!bActionsCached)
	{
		CachedActions = GetActions();
		bActionsCached = true;
	}
	return CachedActions;
}

// ── Auth & URL routing ───────────────────────────────────────────────

FString FGenerativeProviderBase::GetAuthToken() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	// Look up provider-specific key from settings
	const FString KeyName = GetApiKeySettingName();
	// Use reflection to read the property by name
	FProperty* Prop = UACPSettings::StaticClass()->FindPropertyByName(*KeyName);
	if (Prop)
	{
		FString Value;
		const FStrProperty* StrProp = CastField<FStrProperty>(Prop);
		if (StrProp)
		{
			Value = StrProp->GetPropertyValue_InContainer(Settings);
		}
		return Value;
	}
	return TEXT("");
}

FString FGenerativeProviderBase::GetBaseUrl() const
{
	if (UseCloudMode())
	{
		// NeoStack Cloud proxy. Splat catch-all on neostack.dev forwards
		// `<id>/<rest>` verbatim to the upstream provider through our
		// gateway — so individual action paths (`/v1/text-to-speech/...`,
		// `/openapi/v1/text-to-image`, `/task/{id}`) work without per-
		// provider URL rewriting on the plugin side.
		return FString::Printf(TEXT("https://neostack.dev/api/v1/%s"), *GetId());
	}
	return GetDirectBaseUrl();
}

FString FGenerativeProviderBase::GetNeoStackApiKey() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	return Settings ? Settings->GetChatProviderApiKey(TEXT("neostack")) : FString();
}

// ── Auth header ──────────────────────────────────────────────────────

void FGenerativeProviderBase::SetAuthHeaders(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request) const
{
	if (UseCloudMode())
	{
		// Cloud auth: Bearer <NeoStackApiKey> identifies the caller to our
		// proxy, plus optional X-Neo-Provider-Key carrying the user's own
		// upstream key for BYOK passthrough. The proxy uses the user's key
		// upstream when present; otherwise falls back to NeoStack's master
		// key (billed against the user's rolling caps).
		const FString NeoKey = GetNeoStackApiKey();
		if (NeoKey.IsEmpty())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[%s] NeoStack Cloud is enabled but no NeoStack API key is configured. "
					"Set one in Settings > Chat & Agents > Chat Providers > NeoStack Cloud, "
					"or disable cloud mode for this provider."),
				*GetId());
		}
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *NeoKey));

		const FString ProviderKey = GetAuthToken();
		if (!ProviderKey.IsEmpty())
		{
			Request->SetHeader(TEXT("X-Neo-Provider-Key"), ProviderKey);
		}
		return;
	}
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *GetAuthToken()));
}

// Build a friendly, actionable error message when the provider has no auth token.
// The agent can relay this verbatim to the user; it tells them exactly what to set
// and where.
namespace
{
	FString BuildAuthMissingMessage(const IGenerativeProvider& Provider)
	{
		const FString DisplayName = Provider.GetDisplayName();

		const FString KeyName = Provider.GetApiKeySettingName();
		const FString Website = Provider.GetWebsite();

		FString Msg = FString::Printf(TEXT(
			"%s API key not configured (looking for setting '%s'). "
			"To fix: open Settings > Chat & Agents > Chat Providers, "
			"set '%s' to your %s API key"),
			*DisplayName, *KeyName, *KeyName, *DisplayName);
		if (!Website.IsEmpty())
		{
			Msg += FString::Printf(TEXT(" (get one at %s)"), *Website);
		}
		Msg += TEXT(".");
		return Msg;
	}

	/**
	 * In Cloud mode the provider key is optional — the proxy uses our master
	 * key if the user hasn't set their own. But the NeoStack key is required
	 * to identify the user; without it the proxy returns 401. Surface a
	 * dedicated, actionable error so the agent doesn't blame the wrong
	 * setting.
	 */
	FString BuildCloudAuthMissingMessage(const IGenerativeProvider& Provider)
	{
		const FString DisplayName = Provider.GetDisplayName();
		return FString::Printf(TEXT(
			"NeoStack Cloud is enabled for %s but no NeoStack API key is configured. "
			"Set a NeoStack Cloud key in Settings > Chat & Agents > Chat Providers "
			"(generate one at https://neostack.dev), or disable "
			"'Route through NeoStack Cloud' for %s."),
			*DisplayName, *DisplayName);
	}
}

// ── Async HTTP helpers ───────────────────────────────────────────────
//
// All requests are dispatched via IHttpRequest::OnProcessRequestComplete delegate.
// The completion callback marshals back to the game thread via RunOnMainThreadDeferred
// (the engine's HTTP module already runs callbacks on the game thread tick, but the
// marshal makes it explicit and safe across UE versions).

namespace
{
	// Parse a JSON-shaped error message out of a response body. Best-effort.
	FString ExtractApiErrorMessage(const TSharedPtr<FJsonObject>& Json, const FString& FallbackBody)
	{
		FString ApiMsg;
		if (Json.IsValid())
		{
			if (Json->HasField(TEXT("message")))
			{
				ApiMsg = Json->GetStringField(TEXT("message"));
			}
			else if (Json->HasTypedField<EJson::Object>(TEXT("error")))
			{
				ApiMsg = Json->GetObjectField(TEXT("error"))->GetStringField(TEXT("message"));
			}
			else if (Json->HasTypedField<EJson::Object>(TEXT("detail")))
			{
				ApiMsg = Json->GetObjectField(TEXT("detail"))->GetStringField(TEXT("message"));
			}
		}
		return ApiMsg.IsEmpty() ? FallbackBody.Left(500) : ApiMsg;
	}
}

void FGenerativeProviderBase::DispatchJsonRequest(
	const FString& Verb,
	const FString& FullUrl,
	const TSharedPtr<FJsonObject>& Body,
	FHttpJsonCallback OnComplete,
	float TimeoutSeconds) const
{
	// Pre-flight auth check. Fail fast with an actionable message instead of
	// letting the API return a generic 401/403.
	//   - Cloud mode: NeoStack API key is required; provider key is optional
	//     (BYOK passthrough — empty just means "use NeoStack caps").
	//   - Direct mode: provider key is required.
	if (UseCloudMode())
	{
		if (GetNeoStackApiKey().IsEmpty())
		{
			FHttpJsonResult Result;
			Result.bSuccess = false;
			Result.Error = BuildCloudAuthMissingMessage(*this);
			UE::NeoStack::RunOnMainThreadDeferred(
				[OnComplete = MoveTemp(OnComplete), Result]()
				{
					OnComplete(Result);
				});
			return;
		}
	}
	else if (GetAuthToken().IsEmpty())
	{
		FHttpJsonResult Result;
		Result.bSuccess = false;
		Result.Error = BuildAuthMissingMessage(*this);
		UE::NeoStack::RunOnMainThreadDeferred(
			[OnComplete = MoveTemp(OnComplete), Result]()
			{
				OnComplete(Result);
			});
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FullUrl);
	Request->SetVerb(Verb);
	SetAuthHeaders(Request);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(TimeoutSeconds);

	if (Body.IsValid())
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyStr);
	}

	const FString ProviderId = GetId();
	const FString CapturedVerb = Verb;
	const FString CapturedUrl = FullUrl;

	Request->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), ProviderId, CapturedVerb, CapturedUrl]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bSucceeded)
		{
			UE::NeoStack::RunOnMainThreadDeferred(
				[OnComplete, ProviderId, CapturedVerb, CapturedUrl, Resp, bSucceeded]()
				{
					FHttpJsonResult Result;

					if (!bSucceeded || !Resp.IsValid())
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("HTTP request failed (no response) for %s %s"),
							*CapturedVerb, *CapturedUrl);
						UE_LOG(LogTemp, Error, TEXT("[%s] %s %s failed: %s"),
							*ProviderId, *CapturedVerb, *CapturedUrl, *Result.Error);
						OnComplete(Result);
						return;
					}

					Result.ResponseCode = Resp->GetResponseCode();
					const FString ResponseBody = Resp->GetContentAsString();

					// Try to parse as JSON regardless of status code; APIs often return
					// JSON error bodies with useful detail.
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
					FJsonSerializer::Deserialize(Reader, Result.Json);

					if (Result.ResponseCode < 200 || Result.ResponseCode >= 300)
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("HTTP %d: %s"), Result.ResponseCode,
							*ExtractApiErrorMessage(Result.Json, ResponseBody));
						UE_LOG(LogTemp, Error, TEXT("[%s] %s %s -> %s"),
							*ProviderId, *CapturedVerb, *CapturedUrl, *Result.Error);
						OnComplete(Result);
						return;
					}

					Result.bSuccess = true;
					OnComplete(Result);
				});
		});

	Request->ProcessRequest();
}

void FGenerativeProviderBase::HttpPost(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Body,
	FHttpJsonCallback OnComplete,
	float TimeoutSeconds) const
{
	DispatchJsonRequest(TEXT("POST"), GetBaseUrl() + Path, Body, MoveTemp(OnComplete), TimeoutSeconds);
}

void FGenerativeProviderBase::HttpGet(
	const FString& Path,
	FHttpJsonCallback OnComplete,
	float TimeoutSeconds) const
{
	DispatchJsonRequest(TEXT("GET"), GetBaseUrl() + Path, nullptr, MoveTemp(OnComplete), TimeoutSeconds);
}

void FGenerativeProviderBase::HttpDelete(
	const FString& Path,
	FHttpJsonCallback OnComplete,
	float TimeoutSeconds) const
{
	DispatchJsonRequest(TEXT("DELETE"), GetBaseUrl() + Path, nullptr, MoveTemp(OnComplete), TimeoutSeconds);
}

void FGenerativeProviderBase::HttpDownload(
	const FString& Url,
	const FString& OutputPath,
	FHttpDownloadCallback OnComplete,
	float TimeoutSeconds) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(TimeoutSeconds);

	const FString CapturedOutputPath = OutputPath;

	Request->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), CapturedOutputPath]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bSucceeded)
		{
			UE::NeoStack::RunOnMainThreadDeferred(
				[OnComplete, CapturedOutputPath, Resp, bSucceeded]()
				{
					FHttpDownloadResult Result;

					if (!bSucceeded || !Resp.IsValid())
					{
						Result.bSuccess = false;
						Result.Error = TEXT("Download failed: no response");
						OnComplete(Result);
						return;
					}

					Result.ResponseCode = Resp->GetResponseCode();
					if (Result.ResponseCode < 200 || Result.ResponseCode >= 300)
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("Download failed: HTTP %d"), Result.ResponseCode);
						OnComplete(Result);
						return;
					}

					if (!FFileHelper::SaveArrayToFile(Resp->GetContent(), *CapturedOutputPath))
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("Failed to save to %s"), *CapturedOutputPath);
						OnComplete(Result);
						return;
					}

					Result.bSuccess = true;
					OnComplete(Result);
				});
		});

	Request->ProcessRequest();
}

void FGenerativeProviderBase::HttpPostRaw(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Body,
	FHttpRawCallback OnComplete,
	float TimeoutSeconds) const
{
	// Pre-flight auth check (same as DispatchJsonRequest).
	if (UseCloudMode())
	{
		if (GetNeoStackApiKey().IsEmpty())
		{
			FHttpRawResult Result;
			Result.bSuccess = false;
			Result.Error = BuildCloudAuthMissingMessage(*this);
			UE::NeoStack::RunOnMainThreadDeferred(
				[OnComplete = MoveTemp(OnComplete), Result]()
				{
					OnComplete(Result);
				});
			return;
		}
	}
	else if (GetAuthToken().IsEmpty())
	{
		FHttpRawResult Result;
		Result.bSuccess = false;
		Result.Error = BuildAuthMissingMessage(*this);
		UE::NeoStack::RunOnMainThreadDeferred(
			[OnComplete = MoveTemp(OnComplete), Result]()
			{
				OnComplete(Result);
			});
		return;
	}

	const FString FullUrl = GetBaseUrl() + Path;

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(FullUrl);
	Request->SetVerb(TEXT("POST"));
	SetAuthHeaders(Request);
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(TimeoutSeconds);

	if (Body.IsValid())
	{
		FString BodyStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyStr);
	}

	const FString CapturedPath = Path;

	Request->OnProcessRequestComplete().BindLambda(
		[OnComplete = MoveTemp(OnComplete), CapturedPath]
		(FHttpRequestPtr /*Req*/, FHttpResponsePtr Resp, bool bSucceeded)
		{
			UE::NeoStack::RunOnMainThreadDeferred(
				[OnComplete, CapturedPath, Resp, bSucceeded]()
				{
					FHttpRawResult Result;

					if (!bSucceeded || !Resp.IsValid())
					{
						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("HTTP request failed (no response) for POST %s"), *CapturedPath);
						OnComplete(Result);
						return;
					}

					Result.ResponseCode = Resp->GetResponseCode();
					if (Result.ResponseCode < 200 || Result.ResponseCode >= 300)
					{
						const FString ResponseBody = Resp->GetContentAsString();
						TSharedPtr<FJsonObject> Json;
						TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
						FJsonSerializer::Deserialize(Reader, Json);

						Result.bSuccess = false;
						Result.Error = FString::Printf(TEXT("HTTP %d: %s"), Result.ResponseCode,
							*ExtractApiErrorMessage(Json, ResponseBody));
						OnComplete(Result);
						return;
					}

					Result.bSuccess = true;
					Result.ContentType = Resp->GetContentType();
					Result.Bytes = TArray<uint8>(Resp->GetContent());
					OnComplete(Result);
				});
		});

	Request->ProcessRequest();
}

// ── Status parsing ───────────────────────────────────────────────────

EGenerativeJobStatus FGenerativeProviderBase::ParseStatus(const FString& StatusStr)
{
	const FString Upper = StatusStr.ToUpper();
	if (Upper == TEXT("SUCCEEDED") || Upper == TEXT("SUCCESS") || Upper == TEXT("COMPLETED"))
		return EGenerativeJobStatus::Succeeded;
	if (Upper == TEXT("FAILED") || Upper == TEXT("ERROR"))
		return EGenerativeJobStatus::Failed;
	if (Upper == TEXT("CANCELED") || Upper == TEXT("CANCELLED"))
		return EGenerativeJobStatus::Cancelled;
	if (Upper == TEXT("IN_PROGRESS") || Upper == TEXT("RUNNING") || Upper == TEXT("PROCESSING"))
		return EGenerativeJobStatus::Running;
	return EGenerativeJobStatus::Pending; // PENDING, QUEUED, or unknown
}

// ── Schema builders ──────────────────────────────────────────────────

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaString(
	const FString& Desc, const TArray<FString>& Enum, const FString& Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("string"));
	Obj->SetStringField(TEXT("description"), Desc);
	if (Enum.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> EnumArr;
		for (const auto& E : Enum) EnumArr.Add(MakeShared<FJsonValueString>(E));
		Obj->SetArrayField(TEXT("enum"), EnumArr);
	}
	if (!Default.IsEmpty())
	{
		Obj->SetStringField(TEXT("default"), Default);
	}
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaInt(
	const FString& Desc, int32 Min, int32 Max, int32 Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("integer"));
	Obj->SetStringField(TEXT("description"), Desc);
	if (Min != 0 || Max != 0)
	{
		if (Min != 0) Obj->SetNumberField(TEXT("minimum"), Min);
		if (Max != 0) Obj->SetNumberField(TEXT("maximum"), Max);
	}
	if (Default != 0) Obj->SetNumberField(TEXT("default"), Default);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaBool(const FString& Desc, bool Default)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("boolean"));
	Obj->SetStringField(TEXT("description"), Desc);
	Obj->SetBoolField(TEXT("default"), Default);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::SchemaStringArray(const FString& Desc)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("type"), TEXT("array"));
	Obj->SetStringField(TEXT("description"), Desc);
	TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
	Items->SetStringField(TEXT("type"), TEXT("string"));
	Obj->SetObjectField(TEXT("items"), Items);
	return Obj;
}

TSharedPtr<FJsonObject> FGenerativeProviderBase::BuildSchema(
	const TMap<FString, TSharedPtr<FJsonObject>>& Properties,
	const TArray<FString>& Required)
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	for (const auto& Pair : Properties)
	{
		Props->SetObjectField(Pair.Key, Pair.Value);
	}
	Schema->SetObjectField(TEXT("properties"), Props);

	if (Required.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ReqArr;
		for (const auto& R : Required) ReqArr.Add(MakeShared<FJsonValueString>(R));
		Schema->SetArrayField(TEXT("required"), ReqArr);
	}

	return Schema;
}
