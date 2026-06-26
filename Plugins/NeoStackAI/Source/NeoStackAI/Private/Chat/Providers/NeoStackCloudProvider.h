// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/Providers/OpenAICompatProviderBase.h"

/**
 * NeoStack Cloud — managed AI gateway at neostack.dev.
 *
 * Wire format: standard OpenAI chat completions, served by the NeoStack
 * proxy at neostack.dev which forwards to upstream providers.
 * This is the path users take when they want managed inference billed against
 * their NeoStack subscription instead of paying upstream providers directly.
 *
 * Auth: API keys minted on neostack.dev (prefix `neostack_`), org-scoped.
 * The user pastes one into the per-provider settings row keyed by GetId().
 */
class FNeoStackCloudProvider : public FOpenAICompatProviderBase
{
public:
	// IChatProvider
	virtual FString GetId() const override          { return TEXT("neostack"); }
	virtual FString GetDisplayName() const override { return TEXT("NeoStack Cloud"); }
	virtual FString GetDescription() const override { return TEXT("Managed AI billed to your NeoStack subscription"); }

	virtual FChatProviderCapabilities GetCapabilities() const override;
	virtual TArray<FChatModelInfo> GetStaticModels() const override;

	virtual bool SupportsModelDiscovery() const override { return true; }
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString)> Callback) override;

protected:
	// FOpenAICompatProviderBase
	virtual FString GetDefaultBaseUrl() const override { return TEXT("https://neostack.dev/api/v1"); }
	virtual FString GetDefaultModel() const override   { return TEXT("MiniMax-M2.7"); }

	virtual TArray<FChatModelInfo> ParseDiscoveryResponse(const FString& ResponseBody) const override;
};
