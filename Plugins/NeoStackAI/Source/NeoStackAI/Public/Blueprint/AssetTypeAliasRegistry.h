#pragma once

#include "CoreMinimal.h"

namespace NeoAssetTypeAliases
{
	NEOSTACKAI_API void RegisterAlias(const FString& Alias, const FString& ClassPath);
	NEOSTACKAI_API void RegisterAliases(const TMap<FString, FString>& Aliases);
	NEOSTACKAI_API void UnregisterAlias(const FString& Alias);
	NEOSTACKAI_API void UnregisterAliases(const TArray<FString>& Aliases);
	NEOSTACKAI_API bool ResolveAlias(const FString& Alias, FString& OutClassPath);
	NEOSTACKAI_API void ListAliases(TArray<TPair<FString, FString>>& OutTypes);
}
