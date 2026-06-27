// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPSettings.h"
#include "Chat/Providers/OpenAICompatProviderBase.h"

/**
 * User-defined OpenAI-compatible chat provider.
 *
 * Instances are immutable snapshots of FUserChatProvider. The registry rebuilds
 * them from settings whenever custom provider CRUD changes.
 */
class FUserDefinedChatProvider : public FOpenAICompatProviderBase
{
public:
	explicit FUserDefinedChatProvider(const FUserChatProvider& InDefinition);

	virtual FString GetId() const override;
	virtual FString GetDisplayName() const override;
	virtual FString GetDescription() const override;

	virtual FChatProviderCapabilities GetCapabilities() const override;
	virtual bool RequiresApiKey() const override;
	virtual bool ValidateConfig(FString& OutError) const override;
	virtual TArray<FChatModelInfo> GetStaticModels() const override;

	virtual bool SupportsModelDiscovery() const override;
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString)> Callback) override;

protected:
	virtual FString GetDefaultBaseUrl() const override;
	virtual FString GetDefaultModel() const override;

	virtual void ConfigureHeaders(
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
		const FString& ApiKey) const override;

private:
	FUserChatProvider Definition;
	FString NormalizedBaseUrl;
};
