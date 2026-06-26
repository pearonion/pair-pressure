// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/Providers/OpenAICompatProviderBase.h"

/**
 * Ollama — local model server. Runs at http://localhost:11434 by default.
 *
 * No API key required. Discovery uses Ollama's native /api/tags endpoint
 * which returns the locally-pulled models, instead of the OpenAI-compatible
 * /v1/models endpoint (which Ollama also exposes but with less metadata).
 */
class FOllamaChatProvider : public FOpenAICompatProviderBase
{
public:
	virtual FString GetId() const override          { return TEXT("ollama"); }
	virtual FString GetDisplayName() const override { return TEXT("Ollama (Local)"); }
	virtual FString GetDescription() const override { return TEXT("Run models locally via Ollama"); }

	virtual bool RequiresApiKey() const override { return false; }
	virtual TArray<FChatModelInfo> GetStaticModels() const override;

	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString)> Callback) override;

protected:
	virtual FString GetDefaultBaseUrl() const override { return TEXT("http://localhost:11434/v1"); }
	virtual FString GetDefaultModel() const override   { return TEXT("llama3.2"); }

	// Ollama doesn't require auth; only set Authorization if user provided
	// a key (e.g. for remote / reverse-proxied Ollama setups).
	virtual void ConfigureHeaders(
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
		const FString& ApiKey) const override;

	// Parse Ollama's native /api/tags response. The default OpenAI-compat
	// {"data":[...]} parser doesn't apply because /api/tags returns
	// {"models":[{"name":"...","modified_at":"...","size":N,...}, ...]}.
	virtual TArray<FChatModelInfo> ParseDiscoveryResponse(const FString& ResponseBody) const override;
};
