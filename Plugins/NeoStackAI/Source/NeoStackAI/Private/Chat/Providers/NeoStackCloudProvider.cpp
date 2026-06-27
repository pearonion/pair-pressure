// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/NeoStackCloudProvider.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FChatProviderCapabilities FNeoStackCloudProvider::GetCapabilities() const
{
	FChatProviderCapabilities Caps = FOpenAICompatProviderBase::GetCapabilities();
	// MiniMax-M2.7 emits chain-of-thought wrapped in <think>...</think> blocks
	// inside the regular content stream — there is no API toggle for it, so
	// we do NOT advertise reasoning support (no effort selector in the UI).
	Caps.bSupportsReasoning = false;
	Caps.bSupportsImages = false;
	return Caps;
}

TArray<FChatModelInfo> FNeoStackCloudProvider::GetStaticModels() const
{
	TArray<FChatModelInfo> Models;

	auto Add = [&](const TCHAR* Id, const TCHAR* Name, const TCHAR* Desc)
	{
		FChatModelInfo M;
		M.ModelId = Id;
		M.DisplayName = Name;
		M.Description = Desc;
		M.ProviderId = GetId();
		M.ProviderDisplayName = GetDisplayName();
		M.bSupportsReasoning = false;
		Models.Add(MoveTemp(M));
	};

	Add(TEXT("MiniMax-M2.7"), TEXT("MiniMax M2.7"),
		TEXT("Long-context reasoning model proxied via NeoStack Cloud"));

	return Models;
}

TArray<FChatModelInfo> FNeoStackCloudProvider::ParseDiscoveryResponse(const FString& ResponseBody) const
{
	TArray<FChatModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		const TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FChatModelInfo Info;
		ModelObj->TryGetStringField(TEXT("id"), Info.ModelId);
		if (Info.ModelId.IsEmpty()) continue;

		// Our /v1/models returns id + description but no separate display name.
		if (!ModelObj->TryGetStringField(TEXT("name"), Info.DisplayName) || Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = Info.ModelId;
		}
		ModelObj->TryGetStringField(TEXT("description"), Info.Description);

		Info.ProviderId = GetId();
		Info.ProviderDisplayName = GetDisplayName();
		Info.bSupportsReasoning = false;

		Models.Add(MoveTemp(Info));
	}

	Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	return Models;
}

void FNeoStackCloudProvider::DiscoverModelsAsync(TFunction<void(TArray<FChatModelInfo>, FString)> Callback)
{
	const FString ApiKey = GetSettingsApiKey();
	if (ApiKey.IsEmpty())
	{
		// /v1/models requires auth. Surface a clean message instead of a 401
		// log line so the settings UI can prompt the user to paste a key.
		Callback({}, TEXT("Paste your NeoStack API key to discover models."));
		return;
	}

	FString BaseUrl = GetSettingsBaseUrlOverride();
	if (BaseUrl.IsEmpty())
	{
		BaseUrl = GetDefaultBaseUrl();
	}
	const FString Url = BaseUrl + GetModelsPath();

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	ConfigureHeaders(Req, ApiKey);

	Req->OnProcessRequestComplete().BindLambda(
		[this, Callback](FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bOk)
		{
			if (!bOk || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				const int32 Code = Response.IsValid() ? Response->GetResponseCode() : -1;
				const FString Err = FString::Printf(
					TEXT("NeoStack Cloud /models fetch failed (HTTP %d)"), Code);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			TArray<FChatModelInfo> Models = ParseDiscoveryResponse(Response->GetContentAsString());
			UE_LOG(LogNeoStackAI, Log,
				TEXT("NeoStack Cloud: discovered %d models"), Models.Num());
			Callback(MoveTemp(Models), FString());
		});

	if (!Req->ProcessRequest())
	{
		Callback({}, TEXT("Failed to initiate NeoStack Cloud /models request"));
	}
}
