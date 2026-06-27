#include "Blueprint/AssetTypeAliasRegistry.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace
{
	FCriticalSection GAssetAliasMutex;

	TMap<FString, FString>& GetRegistry()
	{
		static TMap<FString, FString> Registry;
		return Registry;
	}

	FString NormalizeAlias(const FString& Alias)
	{
		return Alias.TrimStartAndEnd().ToLower();
	}
}

namespace NeoAssetTypeAliases
{
	void RegisterAlias(const FString& Alias, const FString& ClassPath)
	{
		const FString NormalizedAlias = NormalizeAlias(Alias);
		const FString NormalizedClassPath = ClassPath.TrimStartAndEnd();
		if (NormalizedAlias.IsEmpty() || NormalizedClassPath.IsEmpty())
		{
			return;
		}

		FScopeLock Lock(&GAssetAliasMutex);
		GetRegistry().Add(NormalizedAlias, NormalizedClassPath);
	}

	void RegisterAliases(const TMap<FString, FString>& Aliases)
	{
		FScopeLock Lock(&GAssetAliasMutex);
		TMap<FString, FString>& Registry = GetRegistry();
		for (const auto& Pair : Aliases)
		{
			const FString NormalizedAlias = NormalizeAlias(Pair.Key);
			const FString NormalizedClassPath = Pair.Value.TrimStartAndEnd();
			if (!NormalizedAlias.IsEmpty() && !NormalizedClassPath.IsEmpty())
			{
				Registry.Add(NormalizedAlias, NormalizedClassPath);
			}
		}
	}

	void UnregisterAlias(const FString& Alias)
	{
		const FString NormalizedAlias = NormalizeAlias(Alias);
		if (NormalizedAlias.IsEmpty())
		{
			return;
		}

		FScopeLock Lock(&GAssetAliasMutex);
		GetRegistry().Remove(NormalizedAlias);
	}

	void UnregisterAliases(const TArray<FString>& Aliases)
	{
		FScopeLock Lock(&GAssetAliasMutex);
		TMap<FString, FString>& Registry = GetRegistry();
		for (const FString& Alias : Aliases)
		{
			const FString NormalizedAlias = NormalizeAlias(Alias);
			if (!NormalizedAlias.IsEmpty())
			{
				Registry.Remove(NormalizedAlias);
			}
		}
	}

	bool ResolveAlias(const FString& Alias, FString& OutClassPath)
	{
		const FString NormalizedAlias = NormalizeAlias(Alias);
		if (NormalizedAlias.IsEmpty())
		{
			return false;
		}

		FScopeLock Lock(&GAssetAliasMutex);
		if (const FString* Found = GetRegistry().Find(NormalizedAlias))
		{
			OutClassPath = *Found;
			return true;
		}
		return false;
	}

	void ListAliases(TArray<TPair<FString, FString>>& OutTypes)
	{
		FScopeLock Lock(&GAssetAliasMutex);
		for (const auto& Pair : GetRegistry())
		{
			OutTypes.Add(TPair<FString, FString>(Pair.Key, Pair.Value));
		}
	}
}
