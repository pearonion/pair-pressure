// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/VercelChatProvider.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

FChatProviderCapabilities FVercelChatProvider::GetCapabilities() const
{
	FChatProviderCapabilities Caps = FOpenAICompatProviderBase::GetCapabilities();
	Caps.bSupportsReasoning = true;
	Caps.bSupportsImages = true;
	return Caps;
}

TArray<FChatModelInfo> FVercelChatProvider::GetStaticModels() const
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

	Add(TEXT("openai/gpt-4o"),                   TEXT("GPT-4o"),               TEXT("OpenAI's multimodal flagship via Vercel"), false);
	Add(TEXT("anthropic/claude-sonnet-4.5"),     TEXT("Claude Sonnet 4.5"),    TEXT("Anthropic's balanced model via Vercel"),   true);
	Add(TEXT("google/gemini-2.5-pro"),           TEXT("Gemini 2.5 Pro"),       TEXT("Google's reasoning-capable Gemini via Vercel"), true);
	Add(TEXT("xai/grok-3"),                      TEXT("Grok 3"),               TEXT("xAI's Grok 3 via Vercel"),                 false);

	return Models;
}

bool FVercelChatProvider::ModelSupportsReasoning(const FString& ModelId) const
{
	const FString Lower = ModelId.ToLower();
	return Lower.StartsWith(TEXT("anthropic/"))
		|| Lower.StartsWith(TEXT("openai/o1"))
		|| Lower.StartsWith(TEXT("openai/o3"))
		|| Lower.StartsWith(TEXT("openai/o4"))
		|| Lower.Contains(TEXT("gemini-2.5-pro"))
		|| Lower.Contains(TEXT("gemini-3"));
}

void FVercelChatProvider::DiscoverModelsAsync(TFunction<void(TArray<FChatModelInfo>, FString)> Callback)
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
				const FString Err = FString::Printf(TEXT("Vercel /models fetch failed (HTTP %d)"), Code);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			TArray<FChatModelInfo> Models = ParseDiscoveryResponse(Response->GetContentAsString());
			Callback(MoveTemp(Models), FString());
		});

	if (!Req->ProcessRequest())
	{
		Callback({}, TEXT("Failed to initiate Vercel /models request"));
	}
}
