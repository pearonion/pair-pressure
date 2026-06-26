// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/Providers/OpenAICompatProviderBase.h"

/**
 * Vercel AI Gateway — routes through Vercel's unified gateway, which proxies
 * OpenAI / Anthropic / Google / many others under one OpenAI-compatible API.
 *
 * Wire format: standard OpenAI chat completions.
 * Model ids follow the Vercel gateway convention (e.g. "openai/gpt-4o",
 * "anthropic/claude-sonnet-4.5") — same vendor/model shape as OpenRouter.
 * Discovery is supported via /v1/models.
 */
class FVercelChatProvider : public FOpenAICompatProviderBase
{
public:
	virtual FString GetId() const override          { return TEXT("vercel"); }
	virtual FString GetDisplayName() const override { return TEXT("Vercel AI Gateway"); }
	virtual FString GetDescription() const override { return TEXT("Route through Vercel AI Gateway (OpenAI / Anthropic / Google / ...)"); }

	virtual FChatProviderCapabilities GetCapabilities() const override;
	virtual TArray<FChatModelInfo> GetStaticModels() const override;

	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString)> Callback) override;

protected:
	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://ai-gateway.vercel.sh/v1"); }
	virtual FString GetDefaultModel() const override   { return TEXT("openai/gpt-4o"); }

	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;
};
