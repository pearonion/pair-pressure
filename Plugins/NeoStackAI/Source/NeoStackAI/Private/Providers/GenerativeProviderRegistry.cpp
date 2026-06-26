// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Providers/GenerativeProviderRegistry.h"

// ── Singleton ────────────────────────────────────────────────────────

FGenerativeProviderRegistry& FGenerativeProviderRegistry::Get()
{
	static FGenerativeProviderRegistry Instance;
	return Instance;
}

// ── Registration ─────────────────────────────────────────────────────

void FGenerativeProviderRegistry::Register(TSharedRef<IGenerativeProvider> Provider)
{
	RegisterOwned(TEXT("neostack.core"), Provider);
}

FNeoStackExtensionHandle FGenerativeProviderRegistry::RegisterOwned(const FString& OwnerExtensionId, TSharedRef<IGenerativeProvider> Provider)
{
	const FString Id = Provider->GetId();
	const FString Key = Id.ToLower();
	const FGuid RegistrationId = FGuid::NewGuid();
	if (ProviderIdIndex.Contains(Key))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GenerativeProviderRegistry] Overwriting provider '%s'"), *Id);
		int32 Idx = ProviderIdIndex[Key];
		Providers[Idx] = Provider;
	}
	else
	{
		Providers.Add(Provider);
		UE_LOG(LogTemp, Log, TEXT("[GenerativeProviderRegistry] Registered provider '%s' (%s) with %d actions"),
			*Id, *Provider->GetDisplayName(), Provider->GetActions().Num());
	}

	ProviderOwnerIds.Add(Key, OwnerExtensionId.IsEmpty() ? TEXT("neostack.core") : OwnerExtensionId);
	ProviderRegistrationIds.Add(Key, RegistrationId);
	RebuildIndexMap();

	FNeoStackExtensionHandle Handle;
	Handle.RegistrationId = RegistrationId;
	Handle.Kind = TEXT("generative_provider");
	Handle.OwnerExtensionId = ProviderOwnerIds[Key];
	return Handle;
}

bool FGenerativeProviderRegistry::Unregister(const FGuid& RegistrationId)
{
	for (const TPair<FString, FGuid>& Pair : ProviderRegistrationIds)
	{
		if (Pair.Value == RegistrationId)
		{
			const int32* Idx = ProviderIdIndex.Find(Pair.Key);
			if (!Idx)
			{
				return false;
			}

			Providers.RemoveAt(*Idx);
			ProviderOwnerIds.Remove(Pair.Key);
			ProviderRegistrationIds.Remove(Pair.Key);
			RebuildIndexMap();
			return true;
		}
	}

	return false;
}

int32 FGenerativeProviderRegistry::UnregisterAllForOwner(const FString& OwnerExtensionId)
{
	TArray<FString> KeysToRemove;
	for (const TPair<FString, FString>& Pair : ProviderOwnerIds)
	{
		if (Pair.Value.Equals(OwnerExtensionId, ESearchCase::CaseSensitive))
		{
			KeysToRemove.Add(Pair.Key);
		}
	}

	KeysToRemove.StableSort([this](const FString& A, const FString& B)
	{
		const int32 IndexA = ProviderIdIndex.FindRef(A);
		const int32 IndexB = ProviderIdIndex.FindRef(B);
		return IndexA > IndexB;
	});

	for (const FString& Key : KeysToRemove)
	{
		if (const int32* Idx = ProviderIdIndex.Find(Key))
		{
			Providers.RemoveAt(*Idx);
			ProviderOwnerIds.Remove(Key);
			ProviderRegistrationIds.Remove(Key);
			RebuildIndexMap();
		}
	}

	return KeysToRemove.Num();
}

// ── Lookup ───────────────────────────────────────────────────────────

TSharedPtr<IGenerativeProvider> FGenerativeProviderRegistry::Find(const FString& ProviderId) const
{
	const int32* Idx = ProviderIdIndex.Find(ProviderId.ToLower());
	if (Idx)
	{
		return Providers[*Idx];
	}
	return nullptr;
}

TArray<TSharedRef<IGenerativeProvider>> FGenerativeProviderRegistry::FindByAction(const FString& ActionId) const
{
	TArray<TSharedRef<IGenerativeProvider>> Result;
	for (const auto& Provider : Providers)
	{
		if (Provider->SupportsAction(ActionId))
		{
			Result.Add(Provider);
		}
	}
	return Result;
}

void FGenerativeProviderRegistry::RebuildIndexMap()
{
	ProviderIdIndex.Empty();
	for (int32 Index = 0; Index < Providers.Num(); ++Index)
	{
		ProviderIdIndex.Add(Providers[Index]->GetId().ToLower(), Index);
	}
}

TArray<TSharedRef<IGenerativeProvider>> FGenerativeProviderRegistry::FindByOutputHint(const FString& OutputHint) const
{
	TArray<TSharedRef<IGenerativeProvider>> Result;
	const FString HintLower = OutputHint.ToLower();
	for (const auto& Provider : Providers)
	{
		for (const auto& Action : Provider->GetActions())
		{
			for (const auto& Hint : Action.OutputHints)
			{
				if (Hint.ToLower() == HintLower)
				{
					Result.AddUnique(Provider);
					break;
				}
			}
		}
	}
	return Result;
}

TArray<TPair<FString, FProviderActionDescriptor>> FGenerativeProviderRegistry::GetAllActions(
	const FString& OutputHintFilter) const
{
	TArray<TPair<FString, FProviderActionDescriptor>> Result;
	const FString FilterLower = OutputHintFilter.ToLower();

	for (const auto& Provider : Providers)
	{
		for (const auto& Action : Provider->GetActions())
		{
			if (FilterLower.IsEmpty())
			{
				Result.Add(TPair<FString, FProviderActionDescriptor>(Provider->GetId(), Action));
			}
			else
			{
				for (const auto& Hint : Action.OutputHints)
				{
					if (Hint.ToLower() == FilterLower)
					{
						Result.Add(TPair<FString, FProviderActionDescriptor>(Provider->GetId(), Action));
						break;
					}
				}
			}
		}
	}
	return Result;
}

// ── Deferred registration ────────────────────────────────────────────

FDeferredProviderRegistration& FDeferredProviderRegistration::Get()
{
	static FDeferredProviderRegistration Instance;
	return Instance;
}

void FDeferredProviderRegistration::Add(TFunction<void()> Func)
{
	if (bExecuted)
	{
		// Module already initialized, register immediately
		Func();
	}
	else
	{
		PendingRegistrations.Add(MoveTemp(Func));
	}
}

void FDeferredProviderRegistration::ExecuteAll()
{
	for (const auto& Func : PendingRegistrations)
	{
		Func();
	}
	PendingRegistrations.Empty();
	bExecuted = true;
}
