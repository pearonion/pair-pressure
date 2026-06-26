// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatProviderRegistrar.h"
#include "Chat/ChatModelRegistry.h"
#include "Chat/Providers/NeoStackCloudProvider.h"
#include "Chat/Providers/OllamaChatProvider.h"
#include "Chat/Providers/OpenRouterProvider.h"
#include "Chat/Providers/VercelChatProvider.h"
#include "Extensions/NeoStackExtensionRegistrar.h"

void FChatProviderRegistrar::RegisterBuiltIns()
{
	FNeoStackExtensionRegistrar& Ext = FNeoStackExtensionRegistrar::Get();
	const FString CoreOwner = TEXT("neostack.core");

	Ext.RegisterChatProvider(CoreOwner, MakeShared<FNeoStackCloudProvider>());
	Ext.RegisterChatProvider(CoreOwner, MakeShared<FOpenRouterChatProvider>());
	Ext.RegisterChatProvider(CoreOwner, MakeShared<FVercelChatProvider>());
	Ext.RegisterChatProvider(CoreOwner, MakeShared<FOllamaChatProvider>());

	// Additional built-in providers will be registered here:
	// OpenAI, DeepSeek, Groq, GeminiCompat, AnthropicNative.

	FChatModelRegistry::Get().SyncUserProviders();
}
