// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatModelRegistry.h"
#include "ACPSettings.h"
#include "Chat/Providers/UserDefinedChatProvider.h"
#include "NeoStackAIModule.h"
#include "Async/Async.h"

FChatModelRegistry& FChatModelRegistry::Get()
{
	static FChatModelRegistry Instance;
	return Instance;
}

void FChatModelRegistry::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;

	UE_LOG(LogNeoStackAI, Log, TEXT("ChatModelRegistry: initializing with %d built-in + %d user providers"),
		BuiltInProviders.Num(), UserProviders.Num());

	// Seed each active provider's cache with static models so the picker has
	// something to show before discovery completes.
	for (const TSharedRef<IChatProvider>& Provider : GetActiveProviders())
	{
		SeedStaticFallback(Provider->GetId());
	}

	// Kick off async discovery in the background. Active providers only.
	RefreshAll();
}

void FChatModelRegistry::Shutdown()
{
	FScopeLock ScopeLock(&Lock);
	BuiltInProviders.Empty();
	UserProviders.Empty();
	Caches.Empty();
	bInitialized = false;
}

void FChatModelRegistry::RegisterProvider(TSharedRef<IChatProvider> Provider)
{
	RegisterOwnedProvider(TEXT("neostack.core"), Provider);
}

FNeoStackExtensionHandle FChatModelRegistry::RegisterOwnedProvider(const FString& OwnerExtensionId, TSharedRef<IChatProvider> Provider)
{
	const FString Id = Provider->GetId();
	const FString DisplayName = Provider->GetDisplayName();
	const FString OwnerId = OwnerExtensionId.IsEmpty() ? FString(TEXT("neostack.core")) : OwnerExtensionId;

	FNeoStackExtensionHandle Handle;

	{
		FScopeLock ScopeLock(&Lock);

		for (const FRegisteredProvider& Existing : BuiltInProviders)
		{
			if (Existing.Provider->GetId() == Id)
			{
				const FString DupMsg = FString::Printf(
					TEXT("ChatModelRegistry: duplicate provider registration '%s' - ignored"),
					*Id);
				UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *DupMsg);
				return {};
			}
		}

		const FString RegMsg = FString::Printf(
			TEXT("ChatModelRegistry: registered provider '%s' (%s)"),
			*Id, *DisplayName);
		UE_LOG(LogNeoStackAI, Log, TEXT("%s"), *RegMsg);

		FRegisteredProvider Entry{ Provider, OwnerId, FGuid::NewGuid() };
		BuiltInProviders.Add(MoveTemp(Entry));

		Handle.RegistrationId = BuiltInProviders.Last().RegistrationId;
		Handle.Kind = TEXT("chat_provider");
		Handle.OwnerExtensionId = BuiltInProviders.Last().OwnerExtensionId;
	}

	if (bInitialized)
	{
		SeedStaticFallback(Id);
		RefreshProvider(Id);

		FChatModelRegistryChange Change;
		Change.ProviderId = Id;
		Change.Reason = EChatModelRegistryChange::Refreshed;
		Broadcast(MoveTemp(Change));
	}

	return Handle;
}

bool FChatModelRegistry::UnregisterProvider(const FGuid& RegistrationId)
{
	FString ProviderId;

	{
		FScopeLock ScopeLock(&Lock);
		for (int32 Index = BuiltInProviders.Num() - 1; Index >= 0; --Index)
		{
			if (BuiltInProviders[Index].RegistrationId == RegistrationId)
			{
				ProviderId = BuiltInProviders[Index].Provider->GetId();
				BuiltInProviders.RemoveAt(Index);
				Caches.Remove(ProviderId);
				break;
			}
		}
	}

	if (ProviderId.IsEmpty())
	{
		return false;
	}

	FChatModelRegistryChange Change;
	Change.ProviderId = ProviderId;
	Change.Reason = EChatModelRegistryChange::Refreshed;
	Broadcast(MoveTemp(Change));
	return true;
}

int32 FChatModelRegistry::UnregisterProvidersForOwner(const FString& OwnerExtensionId)
{
	TArray<FString> RemovedProviderIds;

	{
		FScopeLock ScopeLock(&Lock);
		for (int32 Index = BuiltInProviders.Num() - 1; Index >= 0; --Index)
		{
			if (BuiltInProviders[Index].OwnerExtensionId.Equals(OwnerExtensionId, ESearchCase::CaseSensitive))
			{
				const FString ProviderId = BuiltInProviders[Index].Provider->GetId();
				BuiltInProviders.RemoveAt(Index);
				Caches.Remove(ProviderId);
				RemovedProviderIds.Add(ProviderId);
			}
		}
	}

	for (const FString& ProviderId : RemovedProviderIds)
	{
		FChatModelRegistryChange Change;
		Change.ProviderId = ProviderId;
		Change.Reason = EChatModelRegistryChange::Refreshed;
		Broadcast(MoveTemp(Change));
	}

	return RemovedProviderIds.Num();
}

void FChatModelRegistry::SyncUserProviders()
{
	UACPSettings* Settings = UACPSettings::Get();

	TArray<TSharedRef<IChatProvider>> RebuiltProviders;
	TSet<FString> RebuiltIds;
	TMap<FString, TArray<FChatModelInfo>> StaticModelsByProvider;

	if (Settings)
	{
		RebuiltProviders.Reserve(Settings->UserChatProviders.Num());

		for (const FUserChatProvider& Definition : Settings->UserChatProviders)
		{
			if (Definition.Id.IsEmpty())
			{
				continue;
			}

			TSharedRef<IChatProvider> Provider = MakeShared<FUserDefinedChatProvider>(Definition);
			const FString ProviderId = Provider->GetId();
			if (ProviderId.IsEmpty() || RebuiltIds.Contains(ProviderId))
			{
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("ChatModelRegistry: skipping duplicate or empty user provider id '%s'"),
					*ProviderId);
				continue;
			}

			RebuiltIds.Add(ProviderId);
			StaticModelsByProvider.Add(ProviderId, Provider->GetStaticModels());
			RebuiltProviders.Add(Provider);
		}
	}

	{
		FScopeLock ScopeLock(&Lock);

		TArray<FString> ExistingUserIds;
		ExistingUserIds.Reserve(UserProviders.Num());
		for (const TSharedRef<IChatProvider>& Provider : UserProviders)
		{
			ExistingUserIds.Add(Provider->GetId());
		}

		UserProviders = MoveTemp(RebuiltProviders);

		for (const FString& ExistingId : ExistingUserIds)
		{
			if (!RebuiltIds.Contains(ExistingId))
			{
				Caches.Remove(ExistingId);
			}
		}

		for (const auto& Pair : StaticModelsByProvider)
		{
			FProviderModelCache& Cache = Caches.FindOrAdd(Pair.Key);
			Cache.Models = Pair.Value;
			Cache.LastFetched = FDateTime();
			Cache.bLastFetchOk = Pair.Value.Num() > 0;
			Cache.LastError.Empty();
			Cache.bFetchInFlight = false;
		}
	}

	if (!bInitialized)
	{
		return;
	}

	if (Settings)
	{
		FString SelectedProviderId;
		FString SelectedModelId;
		if (ChatTypes::SplitPrefixedModelId(Settings->SelectedChatModelId, SelectedProviderId, SelectedModelId)
			&& SelectedProviderId.StartsWith(TEXT("userprovider_"))
			&& !RebuiltIds.Contains(SelectedProviderId))
		{
			Settings->SelectedChatModelId.Empty();
			Settings->SaveConfig();
		}
	}

	for (const FString& ProviderId : RebuiltIds)
	{
		if (TSharedPtr<IChatProvider> Provider = FindProvider(ProviderId))
		{
			FString Err;
			if (Provider->SupportsModelDiscovery() && Provider->ValidateConfig(Err))
			{
				RefreshProvider(ProviderId);
			}
		}
	}

	FChatModelRegistryChange Change;
	Change.Reason = EChatModelRegistryChange::UserProviderEdited;
	Broadcast(MoveTemp(Change));
}

TSharedPtr<IChatProvider> FChatModelRegistry::FindProvider(const FString& ProviderId) const
{
	FScopeLock ScopeLock(&Lock);

	for (const FRegisteredProvider& Prov : BuiltInProviders)
	{
		if (Prov.Provider->GetId() == ProviderId) return Prov.Provider;
	}
	for (const TSharedRef<IChatProvider>& Prov : UserProviders)
	{
		if (Prov->GetId() == ProviderId) return Prov;
	}
	return nullptr;
}

TArray<TSharedRef<IChatProvider>> FChatModelRegistry::GetAllProviders() const
{
	FScopeLock ScopeLock(&Lock);
	TArray<TSharedRef<IChatProvider>> Result;
	for (const FRegisteredProvider& Prov : BuiltInProviders)
	{
		Result.Add(Prov.Provider);
	}
	for (const TSharedRef<IChatProvider>& Prov : UserProviders)
	{
		Result.Add(Prov);
	}
	return Result;
}

TArray<TSharedRef<IChatProvider>> FChatModelRegistry::GetActiveProviders() const
{
	const UACPSettings* Settings = UACPSettings::Get();

	TArray<TSharedRef<IChatProvider>> Result;
	{
		FScopeLock ScopeLock(&Lock);
		for (const FRegisteredProvider& Prov : BuiltInProviders)
		{
			Result.Add(Prov.Provider);
		}
		for (const TSharedRef<IChatProvider>& Prov : UserProviders)
		{
			Result.Add(Prov);
		}
	}

	// Filter by enabled + valid config
	Result.RemoveAll([&](const TSharedRef<IChatProvider>& Prov)
	{
		if (!Settings) return false; // keep everything if settings unavailable

		const FString Id = Prov->GetId();
		// Providers with no settings row are treated as enabled-by-default.
		// Once the user touches the settings panel, a row is created.
		const FChatProviderSettings* Row = Settings->FindChatProviderSettings(Id);
		if (Row && !Row->bEnabled) return true;

		FString Err;
		if (!Prov->ValidateConfig(Err)) return true;

		return false;
	});

	return Result;
}

TArray<FChatModelInfo> FChatModelRegistry::GetModels(const FString& ProviderId) const
{
	FScopeLock ScopeLock(&Lock);
	if (const FProviderModelCache* Cache = Caches.Find(ProviderId))
	{
		return Cache->Models;
	}
	return {};
}

TArray<FChatModelInfo> FChatModelRegistry::GetAllModelsFlat() const
{
	TArray<FChatModelInfo> Result;
	for (const TSharedRef<IChatProvider>& Prov : GetActiveProviders())
	{
		Result.Append(GetModels(Prov->GetId()));
	}
	return Result;
}

TArray<TPair<TSharedRef<IChatProvider>, TArray<FChatModelInfo>>> FChatModelRegistry::GetAllModelsGrouped() const
{
	TArray<TPair<TSharedRef<IChatProvider>, TArray<FChatModelInfo>>> Result;
	for (const TSharedRef<IChatProvider>& Prov : GetActiveProviders())
	{
		Result.Add(TPair<TSharedRef<IChatProvider>, TArray<FChatModelInfo>>(Prov, GetModels(Prov->GetId())));
	}
	return Result;
}

void FChatModelRegistry::RefreshAll()
{
	for (const TSharedRef<IChatProvider>& Prov : GetActiveProviders())
	{
		RefreshProvider(Prov->GetId());
	}
}

void FChatModelRegistry::RefreshProvider(const FString& ProviderId)
{
	TSharedPtr<IChatProvider> Prov = FindProvider(ProviderId);
	if (!Prov.IsValid() || !Prov->SupportsModelDiscovery())
	{
		return;
	}

	{
		FScopeLock ScopeLock(&Lock);
		FProviderModelCache& Cache = Caches.FindOrAdd(ProviderId);
		if (Cache.bFetchInFlight)
		{
			return;
		}
		Cache.bFetchInFlight = true;
	}

	const FString CapturedId = ProviderId;
	const FString ProviderDisplay = Prov->GetDisplayName();

	Prov->DiscoverModelsAsync(
		[this, CapturedId, ProviderDisplay](TArray<FChatModelInfo> Models, FString Error)
		{
			{
				FScopeLock ScopeLock(&Lock);

				bool bProviderStillRegistered = false;
				for (const FRegisteredProvider& BuiltIn : BuiltInProviders)
				{
					if (BuiltIn.Provider->GetId() == CapturedId)
					{
						bProviderStillRegistered = true;
						break;
					}
				}
				if (!bProviderStillRegistered)
				{
					for (const TSharedRef<IChatProvider>& UserProvider : UserProviders)
					{
						if (UserProvider->GetId() == CapturedId)
						{
							bProviderStillRegistered = true;
							break;
						}
					}
				}
				if (!bProviderStillRegistered)
				{
					return;
				}

				FProviderModelCache& Cache = Caches.FindOrAdd(CapturedId);
				Cache.bFetchInFlight = false;
				Cache.LastFetched = FDateTime::UtcNow();

				if (!Error.IsEmpty() || Models.Num() == 0)
				{
					Cache.bLastFetchOk = false;
					Cache.LastError = Error;
					// Keep whatever static fallback was already there.
					UE_LOG(LogNeoStackAI, Warning,
						TEXT("ChatModelRegistry: %s discovery failed: %s"),
						*ProviderDisplay, *Error);
				}
				else
				{
					Cache.bLastFetchOk = true;
					Cache.LastError.Empty();
					Cache.Models = MoveTemp(Models);
					UE_LOG(LogNeoStackAI, Log,
						TEXT("ChatModelRegistry: %s discovered %d models"),
						*ProviderDisplay, Cache.Models.Num());
				}
			}

			FChatModelRegistryChange Change;
			Change.ProviderId = CapturedId;
			Change.Reason = EChatModelRegistryChange::Refreshed;
			Broadcast(MoveTemp(Change));
		});
}

void FChatModelRegistry::SetSelectedModel(const FString& PrefixedId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SelectedChatModelId = PrefixedId;
		Settings->SaveConfig();
	}

	FChatModelRegistryChange Change;
	Change.Reason = EChatModelRegistryChange::SelectionChanged;
	Broadcast(MoveTemp(Change));
}

FString FChatModelRegistry::GetSelectedModel() const
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->SelectedChatModelId;
	}
	return FString();
}

TSharedPtr<IChatProvider> FChatModelRegistry::GetSelectedProvider() const
{
	const FString Selected = GetSelectedModel();
	if (!Selected.IsEmpty())
	{
		FString ProvId, ModelId;
		if (ChatTypes::SplitPrefixedModelId(Selected, ProvId, ModelId))
		{
			if (TSharedPtr<IChatProvider> Prov = FindProvider(ProvId))
			{
				return Prov;
			}
		}
	}

	// Fallback: first active provider
	const TArray<TSharedRef<IChatProvider>> Active = GetActiveProviders();
	if (Active.Num() > 0)
	{
		return Active[0];
	}
	return nullptr;
}

FString FChatModelRegistry::GetSelectedBareModelId() const
{
	const FString Selected = GetSelectedModel();
	if (!Selected.IsEmpty())
	{
		FString ProvId, ModelId;
		if (ChatTypes::SplitPrefixedModelId(Selected, ProvId, ModelId))
		{
			if (FindProvider(ProvId).IsValid())
			{
				return ModelId;
			}
		}
	}

	if (TSharedPtr<IChatProvider> Prov = GetSelectedProvider())
	{
		TArray<FChatModelInfo> Models = GetModels(Prov->GetId());
		if (Models.Num() > 0)
		{
			return Models[0].ModelId;
		}
	}
	return FString();
}

void FChatModelRegistry::SeedStaticFallback(const FString& ProviderId)
{
	TSharedPtr<IChatProvider> Prov = FindProvider(ProviderId);
	if (!Prov.IsValid()) return;

	FScopeLock ScopeLock(&Lock);
	FProviderModelCache& Cache = Caches.FindOrAdd(ProviderId);
	if (Cache.Models.Num() == 0)
	{
		Cache.Models = Prov->GetStaticModels();
	}
}

void FChatModelRegistry::Broadcast(FChatModelRegistryChange Change)
{
	// Marshal to the game thread so delegate consumers can touch UObjects.
	if (IsInGameThread())
	{
		OnChanged.Broadcast(Change);
	}
	else
	{
		AsyncTask(ENamedThreads::GameThread, [this, Change = MoveTemp(Change)]()
		{
			OnChanged.Broadcast(Change);
		});
	}
}
