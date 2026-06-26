// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/OllamaChatProvider.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/**
	 * Convert an Ollama base URL like "http://localhost:11434/v1" into the
	 * native API root "http://localhost:11434" so we can hit /api/tags directly.
	 */
	FString StripOpenAICompatSuffix(const FString& BaseUrl)
	{
		FString Out = BaseUrl;
		if (Out.EndsWith(TEXT("/v1")))
		{
			Out.LeftChopInline(3);
		}
		else if (Out.EndsWith(TEXT("/v1/")))
		{
			Out.LeftChopInline(4);
		}
		return Out;
	}
}

TArray<FChatModelInfo> FOllamaChatProvider::GetStaticModels() const
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
		Models.Add(MoveTemp(M));
	};

	// Curated suggestions only — discovery returns the actual locally-pulled
	// models. These show up before the user has installed anything.
	Add(TEXT("llama3.2"),       TEXT("Llama 3.2"),       TEXT("Meta's compact model (run: ollama pull llama3.2)"));
	Add(TEXT("qwen2.5-coder"),  TEXT("Qwen 2.5 Coder"),  TEXT("Alibaba's coding model"));
	Add(TEXT("deepseek-r1"),    TEXT("DeepSeek R1"),     TEXT("Reasoning model"));
	Add(TEXT("mistral"),        TEXT("Mistral"),         TEXT("Mistral 7B"));

	return Models;
}

void FOllamaChatProvider::ConfigureHeaders(
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
	const FString& ApiKey) const
{
	if (!ApiKey.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}
}

TArray<FChatModelInfo> FOllamaChatProvider::ParseDiscoveryResponse(const FString& ResponseBody) const
{
	TArray<FChatModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	// Native Ollama format: {"models": [{"name": "...", "size": N, "modified_at": "...", ...}]}
	const TArray<TSharedPtr<FJsonValue>>* ModelsArray;
	if (JsonRoot->TryGetArrayField(TEXT("models"), ModelsArray))
	{
		for (const TSharedPtr<FJsonValue>& ModelVal : *ModelsArray)
		{
			const TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
			if (!ModelObj.IsValid()) continue;

			FChatModelInfo Info;
			ModelObj->TryGetStringField(TEXT("name"), Info.ModelId);
			if (Info.ModelId.IsEmpty()) continue;

			Info.DisplayName = Info.ModelId;

			// Build a description from size + family if available
			int64 Size = 0;
			ModelObj->TryGetNumberField(TEXT("size"), Size);
			if (Size > 0)
			{
				const double SizeGiB = static_cast<double>(Size) / (1024.0 * 1024.0 * 1024.0);
				Info.Description = FString::Printf(TEXT("Local model (%.1f GB)"), SizeGiB);
			}
			else
			{
				Info.Description = TEXT("Local model");
			}

			Info.ProviderId = GetId();
			Info.ProviderDisplayName = GetDisplayName();
			Models.Add(MoveTemp(Info));
		}

		Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
		{
			return A.DisplayName < B.DisplayName;
		});
		return Models;
	}

	// Fallback: OpenAI-compat shape ({"data": [{"id": "..."}]}) if the user
	// somehow points us at the /v1/models endpoint via a base URL override.
	return FOpenAICompatProviderBase::ParseDiscoveryResponse(ResponseBody);
}

void FOllamaChatProvider::DiscoverModelsAsync(TFunction<void(TArray<FChatModelInfo>, FString)> Callback)
{
	FString BaseUrl = GetSettingsBaseUrlOverride();
	if (BaseUrl.IsEmpty())
	{
		BaseUrl = GetDefaultBaseUrl();
	}

	// Hit Ollama's native /api/tags endpoint, not the OpenAI-compat /v1/models.
	const FString NativeRoot = StripOpenAICompatSuffix(BaseUrl);
	const FString Url = NativeRoot + TEXT("/api/tags");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	const FString ApiKey = GetSettingsApiKey();
	if (!ApiKey.IsEmpty())
	{
		ConfigureHeaders(Req, ApiKey);
	}

	Req->OnProcessRequestComplete().BindLambda(
		[this, Callback](FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bOk)
		{
			if (!bOk || !Response.IsValid())
			{
				const FString Err = TEXT("Ollama discovery failed (no response). Is the local server running?");
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			const int32 Code = Response->GetResponseCode();
			if (Code != 200)
			{
				const FString Err = FString::Printf(TEXT("Ollama /api/tags returned HTTP %d"), Code);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			TArray<FChatModelInfo> Models = ParseDiscoveryResponse(Response->GetContentAsString());
			Callback(MoveTemp(Models), FString());
		});

	if (!Req->ProcessRequest())
	{
		Callback({}, TEXT("Failed to initiate Ollama /api/tags request"));
	}
}
