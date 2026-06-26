// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/UserDefinedChatProvider.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "NeoStackAIModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString NormalizeUserProviderBaseUrl(FString BaseUrl)
	{
		BaseUrl.TrimStartAndEndInline();

		while (BaseUrl.EndsWith(TEXT("/")))
		{
			BaseUrl.LeftChopInline(1, EAllowShrinking::No);
		}

		if (BaseUrl.EndsWith(TEXT("/chat/completions")))
		{
			BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/chat/completions")), EAllowShrinking::No);
		}
		else if (BaseUrl.EndsWith(TEXT("/models")))
		{
			BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/models")), EAllowShrinking::No);
		}

		while (BaseUrl.EndsWith(TEXT("/")))
		{
			BaseUrl.LeftChopInline(1, EAllowShrinking::No);
		}

		return BaseUrl;
	}

	FString ResolveUserProviderDisplayName(const FUserChatProvider& Definition)
	{
		const FString DisplayName = Definition.DisplayName.TrimStartAndEnd();
		return DisplayName.IsEmpty() ? FString(TEXT("Custom Provider")) : DisplayName;
	}

	TArray<FChatModelInfo> ParseOpenAIModelsResponse(
		const FString& ResponseBody,
		const FString& ProviderId,
		const FString& ProviderDisplayName)
	{
		TArray<FChatModelInfo> Models;

		TSharedPtr<FJsonObject> JsonRoot;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
		if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
		{
			return Models;
		}

		const TArray<TSharedPtr<FJsonValue>>* DataArray = nullptr;
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

			Info.ProviderId = ProviderId;
			Info.ProviderDisplayName = ProviderDisplayName;
			Info.bSupportsReasoning = false;
			Info.bSupportsImages = false;
			Info.bSupportsTools = true;

			Models.Add(MoveTemp(Info));
		}

		Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
		{
			return A.DisplayName < B.DisplayName;
		});

		return Models;
	}
}

FUserDefinedChatProvider::FUserDefinedChatProvider(const FUserChatProvider& InDefinition)
	: Definition(InDefinition)
	, NormalizedBaseUrl(NormalizeUserProviderBaseUrl(InDefinition.BaseUrl))
{
	Definition.DisplayName = Definition.DisplayName.TrimStartAndEnd();
	Definition.BaseUrl = NormalizedBaseUrl;
}

FString FUserDefinedChatProvider::GetId() const
{
	return Definition.Id;
}

FString FUserDefinedChatProvider::GetDisplayName() const
{
	return ResolveUserProviderDisplayName(Definition);
}

FString FUserDefinedChatProvider::GetDescription() const
{
	return TEXT("User-defined OpenAI-compatible chat endpoint");
}

FChatProviderCapabilities FUserDefinedChatProvider::GetCapabilities() const
{
	FChatProviderCapabilities Caps = FOpenAICompatProviderBase::GetCapabilities();
	Caps.bSupportsReasoning = false;
	Caps.bSupportsImages = false;
	return Caps;
}

bool FUserDefinedChatProvider::RequiresApiKey() const
{
	return Definition.bRequiresApiKey;
}

bool FUserDefinedChatProvider::ValidateConfig(FString& OutError) const
{
	if (NormalizedBaseUrl.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s requires a base URL"), *GetDisplayName());
		return false;
	}

	if (RequiresApiKey() && GetSettingsApiKey().IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s requires an API key"), *GetDisplayName());
		return false;
	}

	if (Definition.StaticModels.Num() == 0 && !Definition.bEnableDiscovery)
	{
		OutError = FString::Printf(
			TEXT("%s needs at least one manual model or model discovery enabled"),
			*GetDisplayName());
		return false;
	}

	return true;
}

TArray<FChatModelInfo> FUserDefinedChatProvider::GetStaticModels() const
{
	TArray<FChatModelInfo> Models;
	Models.Reserve(Definition.StaticModels.Num());

	for (const FChatModelEntry& Entry : Definition.StaticModels)
	{
		FChatModelInfo Model;
		Model.ModelId = Entry.ModelId.TrimStartAndEnd();
		if (Model.ModelId.IsEmpty()) continue;

		Model.DisplayName = Entry.DisplayName.TrimStartAndEnd();
		if (Model.DisplayName.IsEmpty())
		{
			Model.DisplayName = Model.ModelId;
		}
		Model.Description = Entry.Description;
		Model.ProviderId = GetId();
		Model.ProviderDisplayName = GetDisplayName();
		Model.bSupportsReasoning = false;
		Model.bSupportsImages = false;
		Model.bSupportsTools = true;

		Models.Add(MoveTemp(Model));
	}

	return Models;
}

bool FUserDefinedChatProvider::SupportsModelDiscovery() const
{
	return Definition.bEnableDiscovery;
}

void FUserDefinedChatProvider::DiscoverModelsAsync(
	TFunction<void(TArray<FChatModelInfo>, FString)> Callback)
{
	if (!Definition.bEnableDiscovery)
	{
		Callback({}, TEXT("Model discovery is disabled for this provider"));
		return;
	}

	FString Err;
	if (!ValidateConfig(Err))
	{
		Callback({}, Err);
		return;
	}

	const FString ProviderId = GetId();
	const FString ProviderDisplayName = GetDisplayName();
	const FString Url = NormalizedBaseUrl + GetModelsPath();
	const FString ApiKey = GetSettingsApiKey();
	TArray<FChatModelInfo> ManualModels = GetStaticModels();

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(Url);
	Req->SetVerb(TEXT("GET"));
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!ApiKey.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}

	Req->OnProcessRequestComplete().BindLambda(
		[ProviderId, ProviderDisplayName, ManualModels = MoveTemp(ManualModels), Callback](FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bOk) mutable
		{
			if (!bOk || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				const int32 Code = Response.IsValid() ? Response->GetResponseCode() : -1;
				const FString Err = FString::Printf(
					TEXT("%s /models fetch failed (HTTP %d)"),
					*ProviderDisplayName, Code);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *Err);
				Callback({}, Err);
				return;
			}

			TArray<FChatModelInfo> Models = ParseOpenAIModelsResponse(
				Response->GetContentAsString(),
				ProviderId,
				ProviderDisplayName);

			TSet<FString> SeenModelIds;
			for (const FChatModelInfo& Model : Models)
			{
				SeenModelIds.Add(Model.ModelId);
			}
			for (FChatModelInfo& ManualModel : ManualModels)
			{
				if (!SeenModelIds.Contains(ManualModel.ModelId))
				{
					Models.Add(MoveTemp(ManualModel));
				}
			}

			Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
			{
				return A.DisplayName < B.DisplayName;
			});

			Callback(MoveTemp(Models), FString());
		});

	if (!Req->ProcessRequest())
	{
		Callback({}, FString::Printf(TEXT("Failed to initiate %s /models request"), *ProviderDisplayName));
	}
}

FString FUserDefinedChatProvider::GetDefaultBaseUrl() const
{
	return NormalizedBaseUrl;
}

FString FUserDefinedChatProvider::GetDefaultModel() const
{
	for (const FChatModelEntry& Entry : Definition.StaticModels)
	{
		const FString ModelId = Entry.ModelId.TrimStartAndEnd();
		if (!ModelId.IsEmpty())
		{
			return ModelId;
		}
	}
	return FString();
}

void FUserDefinedChatProvider::ConfigureHeaders(
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
	const FString& ApiKey) const
{
	if (!ApiKey.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}
}
