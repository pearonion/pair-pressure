// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Module-level helper that constructs the built-in chat providers and hands
 * them to FChatModelRegistry. Called exactly once from StartupModule, after
 * settings have been loaded and before the registry's Initialize().
 *
 * Adding a new built-in provider:
 *   1. Implement IChatProvider (or inherit from FOpenAICompatProviderBase).
 *   2. Include the header in ChatProviderRegistrar.cpp.
 *   3. Construct and register it inside RegisterBuiltIns.
 */
class NEOSTACKAI_API FChatProviderRegistrar
{
public:
	static void RegisterBuiltIns();
};
