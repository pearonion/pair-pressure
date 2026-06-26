// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/Providers/OpenAICompatProviderBase.h"

/**
 * OpenRouter — 400+ models routed through a single endpoint.
 *
 * Wire format: standard OpenAI chat completions.
 * Model ids are "vendor/model" strings (e.g. "anthropic/claude-sonnet-4.5").
 * Reasoning is configured via a nested {reasoning: {effort: "..."}} object.
 * Tracking headers HTTP-Referer and X-Title identify this plugin to OpenRouter
 * analytics (optional but encouraged by their docs).
 */
class FOpenRouterChatProvider : public FOpenAICompatProviderBase
{
public:
	// IChatProvider
	virtual FString GetId() const override          { return TEXT("openrouter"); }
	virtual FString GetDisplayName() const override { return TEXT("OpenRouter"); }
	virtual FString GetDescription() const override { return TEXT("400+ models from all major providers via OpenRouter"); }

	virtual FChatProviderCapabilities GetCapabilities() const override;
	virtual TArray<FChatModelInfo> GetStaticModels() const override;

	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString)> Callback) override;

protected:
	// FOpenAICompatProviderBase
	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://openrouter.ai/api/v1"); }
	virtual FString GetDefaultModel() const override   { return TEXT("anthropic/claude-sonnet-4.5"); }

	virtual void ConfigureHeaders(
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
		const FString& ApiKey) const override;

	virtual void FormatReasoningParams(
		TSharedRef<FJsonObject> RequestBody,
		EReasoningEffort Effort) const override;

	virtual bool ModelSupportsReasoning(const FString& ModelId) const override;

	virtual TArray<FChatModelInfo> ParseDiscoveryResponse(const FString& ResponseBody) const override;
};
