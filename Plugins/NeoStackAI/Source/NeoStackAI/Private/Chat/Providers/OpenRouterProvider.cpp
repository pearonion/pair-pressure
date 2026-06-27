// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/OpenRouterProvider.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FChatProviderCapabilities FOpenRouterChatProvider::GetCapabilities() const
{
	FChatProviderCapabilities Caps = FOpenAICompatProviderBase::GetCapabilities();
	Caps.bSupportsReasoning = true;
	Caps.bSupportsImages = true; // depends on model, but OpenRouter routes them
	return Caps;
}

TArray<FChatModelInfo> FOpenRouterChatProvider::GetStaticModels() const
{
	TArray<FChatModelInfo> Models;

	auto Add = [&](const TCHAR* Id, const TCHAR* Name, const TCHAR* Desc, bool bReasoning)
	{
		FChatModelInfo M;
		M.ModelId = Id;
		M.DisplayName = Name;
		M.Description = Desc;
		M.ProviderId = GetId();
		M.ProviderDisplayName = GetDisplayName();
		M.bSupportsReasoning = bReasoning;
		Models.Add(MoveTemp(M));
	};

	Add(TEXT("anthropic/claude-sonnet-4.5"),   TEXT("Claude Sonnet 4.5"),   TEXT("Anthropic's balanced model"), true);
	Add(TEXT("anthropic/claude-opus-4.5"),     TEXT("Claude Opus 4.5"),     TEXT("Anthropic's high-performance model"), true);
	Add(TEXT("openai/gpt-4o"),                 TEXT("GPT-4o"),              TEXT("OpenAI's multimodal flagship"), false);
	Add(TEXT("google/gemini-2.5-pro"),         TEXT("Gemini 2.5 Pro"),      TEXT("Google's reasoning-capable Gemini"), true);
	Add(TEXT("deepseek/deepseek-chat"),        TEXT("DeepSeek Chat"),       TEXT("DeepSeek V3 general-purpose"), false);

	return Models;
}

void FOpenRouterChatProvider::ConfigureHeaders(
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
	const FString& ApiKey) const
{
	HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	HttpRequest->SetHeader(TEXT("HTTP-Referer"), TEXT("https://github.com/betidestudio/NeoStackAI"));
	HttpRequest->SetHeader(TEXT("X-Title"), TEXT("NeoStack AI"));
}

void FOpenRouterChatProvider::FormatReasoningParams(
	TSharedRef<FJsonObject> RequestBody,
	EReasoningEffort Effort) const
{
	TSharedRef<FJsonObject> Reasoning = MakeShared<FJsonObject>();
	Reasoning->SetStringField(TEXT("effort"), ChatTypes::ReasoningEffortToString(Effort));
	RequestBody->SetObjectField(TEXT("reasoning"), Reasoning);
}

bool FOpenRouterChatProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	const FString Lower = ModelId.ToLower();
	return Lower.StartsWith(TEXT("anthropic/"))
		|| Lower.StartsWith(TEXT("openai/o1"))
		|| Lower.StartsWith(TEXT("openai/o3"))
		|| Lower.StartsWith(TEXT("openai/o4"))
		|| Lower.StartsWith(TEXT("deepseek/"))
		|| Lower.Contains(TEXT("gemini-2.5-pro"))
		|| Lower.Contains(TEXT("gemini-3"));
}

TArray<FChatModelInfo> FOpenRouterChatProvider::ParseDiscoveryResponse(const FString& ResponseBody) const
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

		if (!ModelObj->TryGetStringField(TEXT("name"), Info.DisplayName) || Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = Info.ModelId;
		}
		ModelObj->TryGetStringField(TEXT("description"), Info.Description);

		// OpenRouter exposes a supported_parameters array on each model
		bool bReasoningFromParams = false;
		const TArray<TSharedPtr<FJsonValue>>* ParamsArr;
		if (ModelObj->TryGetArrayField(TEXT("supported_parameters"), ParamsArr))
		{
			for (const TSharedPtr<FJsonValue>& Val : *ParamsArr)
			{
				FString ParamStr;
				if (Val->TryGetString(ParamStr)
					&& (ParamStr == TEXT("reasoning") || ParamStr == TEXT("include_reasoning")))
				{
					bReasoningFromParams = true;
					break;
				}
			}
		}

		Info.ProviderId = GetId();
		Info.ProviderDisplayName = GetDisplayName();
		Info.bSupportsReasoning = bReasoningFromParams || ModelSupportsReasoning(Info.ModelId);

		Models.Add(MoveTemp(Info));
	}

	Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	return Models;
}

void FOpenRouterChatProvider::DiscoverModelsAsync(TFunction<void(TArray<FChatModelInfo>, FString)> Callback)
{
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

	// OpenRouter's /models does not require auth, but send it if we have it
	// (some rate limits differ for authenticated callers).
	const FString ApiKey = GetSettingsApiKey();
	if (!ApiKey.IsEmpty())
	{
		ConfigureHeaders(Req, ApiKey);
	}

	Req->OnProcessRequestComplete().BindLambda(
		[this, Callback](FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bOk)
		{
			if (!bOk || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				const int32 Code = Response.IsValid() ? Response->GetResponseCode() : -1;
				const FString Err = FString::Printf(
					TEXT("OpenRouter /models fetch failed (HTTP %d)"), Code);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			TArray<FChatModelInfo> Models = ParseDiscoveryResponse(Response->GetContentAsString());
			UE_LOG(LogNeoStackAI, Log,
				TEXT("OpenRouter: discovered %d models"), Models.Num());
			Callback(MoveTemp(Models), FString());
		});

	if (!Req->ProcessRequest())
	{
		Callback({}, TEXT("Failed to initiate OpenRouter /models request"));
	}
}
