#include "Extensions/NeoStackExtensionRegistry.h"
#include "NeoStackAIModule.h"

namespace
{

FString BuildRegistryLegacyExtensionId(const FString& ModuleName)
{
	FString Id = ModuleName;
	if (Id.IsEmpty())
	{
		return FString();
	}

	Id.ReplaceInline(TEXT("NSAI_"), TEXT("neostack."), ESearchCase::CaseSensitive);
	Id.ReplaceInline(TEXT("NeoStackAI"), TEXT("neostack.core"), ESearchCase::CaseSensitive);
	Id.ReplaceInline(TEXT("_"), TEXT("."), ESearchCase::CaseSensitive);
	return Id.ToLower();
}

} // namespace

FNeoStackExtensionRegistry& FNeoStackExtensionRegistry::Get()
{
	static FNeoStackExtensionRegistry Instance;
	return Instance;
}

void FNeoStackExtensionRegistry::Reset()
{
	FScopeLock ScopeLock(&Lock);
	Extensions.Empty();
	IndexByKey.Empty();
}

void FNeoStackExtensionRegistry::RegisterOrUpdateExtension(const FNeoStackExtensionDescriptor& Descriptor)
{
	const FString Key = MakeKey(Descriptor.ExtensionId, Descriptor.ModuleName);
	if (Key.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("NeoStackExtensionRegistry: ignoring extension with no ExtensionId/ModuleName (%s)"),
			*Descriptor.DisplayName);
		return;
	}

	FScopeLock ScopeLock(&Lock);
	int32 ExistingIndex = INDEX_NONE;
	if (const int32* Found = IndexByKey.Find(Key))
	{
		ExistingIndex = *Found;
	}
	else if (!Descriptor.ModuleName.IsEmpty())
	{
		const FString ModuleKey = Descriptor.ModuleName.ToLower();
		if (const int32* FoundByModule = IndexByKey.Find(ModuleKey))
		{
			ExistingIndex = *FoundByModule;
		}
	}

	if (ExistingIndex != INDEX_NONE)
	{
		Extensions[ExistingIndex] = Descriptor;
	}
	else
	{
		Extensions.Add(Descriptor);
	}

	RebuildIndexMapLocked();
}

bool FNeoStackExtensionRegistry::UnregisterExtension(const FString& ExtensionIdOrModuleName)
{
	const FString LookupKey = NormalizeLookupKey(ExtensionIdOrModuleName);
	if (LookupKey.IsEmpty())
	{
		return false;
	}

	FScopeLock ScopeLock(&Lock);
	const int32* ExistingIndex = IndexByKey.Find(LookupKey);
	if (!ExistingIndex)
	{
		return false;
	}

	Extensions.RemoveAt(*ExistingIndex);
	RebuildIndexMapLocked();
	return true;
}

void FNeoStackExtensionRegistry::RegisterLegacyModuleExtension(
	const FString& DisplayName,
	const FString& ModuleName,
	ENeoStackExtensionState State,
	const FString& StatusMessage)
{
	FNeoStackExtensionDescriptor Existing;
	if (FindExtension(ModuleName, Existing) || FindExtension(BuildRegistryLegacyExtensionId(ModuleName), Existing))
	{
		if (Existing.DisplayName.IsEmpty())
		{
			Existing.DisplayName = DisplayName;
		}
		if (Existing.ModuleName.IsEmpty())
		{
			Existing.ModuleName = ModuleName;
		}
		if (Existing.ExtensionId.IsEmpty())
		{
			Existing.ExtensionId = BuildRegistryLegacyExtensionId(ModuleName);
		}
		Existing.State = State;
		if (!StatusMessage.IsEmpty())
		{
			Existing.StatusMessage = StatusMessage;
		}
		RegisterOrUpdateExtension(Existing);
		return;
	}

	FNeoStackExtensionDescriptor Descriptor;
	Descriptor.ExtensionId = BuildRegistryLegacyExtensionId(ModuleName);
	Descriptor.DisplayName = DisplayName;
	Descriptor.ModuleName = ModuleName;
	Descriptor.Vendor = TEXT("Betide Studio");
	Descriptor.Category = TEXT("engine");
	Descriptor.Description = TEXT("Legacy in-plugin extension module pending migration to NeoExtensions.");
	Descriptor.State = State;
	Descriptor.StatusMessage = StatusMessage;
	Descriptor.Tags = { TEXT("lua") };
	RegisterOrUpdateExtension(Descriptor);
}

bool FNeoStackExtensionRegistry::SetExtensionState(
	const FString& ExtensionIdOrModuleName,
	ENeoStackExtensionState State,
	const FString& StatusMessage)
{
	const FString LookupKey = NormalizeLookupKey(ExtensionIdOrModuleName);
	if (LookupKey.IsEmpty())
	{
		return false;
	}

	FScopeLock ScopeLock(&Lock);
	if (const int32* ExistingIndex = IndexByKey.Find(LookupKey))
	{
		FNeoStackExtensionDescriptor& Descriptor = Extensions[*ExistingIndex];
		Descriptor.State = State;
		Descriptor.StatusMessage = StatusMessage;
		return true;
	}

	return false;
}

bool FNeoStackExtensionRegistry::FindExtension(
	const FString& ExtensionIdOrModuleName,
	FNeoStackExtensionDescriptor& OutDescriptor) const
{
	const FString LookupKey = NormalizeLookupKey(ExtensionIdOrModuleName);
	if (LookupKey.IsEmpty())
	{
		return false;
	}

	FScopeLock ScopeLock(&Lock);
	if (const int32* ExistingIndex = IndexByKey.Find(LookupKey))
	{
		OutDescriptor = Extensions[*ExistingIndex];
		return true;
	}

	return false;
}

TArray<FNeoStackExtensionDescriptor> FNeoStackExtensionRegistry::GetAllExtensions() const
{
	FScopeLock ScopeLock(&Lock);
	return Extensions;
}

TArray<FNeoStackExtensionDescriptor> FNeoStackExtensionRegistry::GetExtensionsByState(ENeoStackExtensionState State) const
{
	TArray<FNeoStackExtensionDescriptor> Result;

	FScopeLock ScopeLock(&Lock);
	for (const FNeoStackExtensionDescriptor& Descriptor : Extensions)
	{
		if (Descriptor.State == State)
		{
			Result.Add(Descriptor);
		}
	}

	return Result;
}

FString FNeoStackExtensionRegistry::MakeKey(const FString& ExtensionId, const FString& ModuleName)
{
	if (!ExtensionId.IsEmpty())
	{
		return ExtensionId.ToLower();
	}
	if (!ModuleName.IsEmpty())
	{
		return ModuleName.ToLower();
	}
	return FString();
}

FString FNeoStackExtensionRegistry::NormalizeLookupKey(const FString& ExtensionIdOrModuleName)
{
	return ExtensionIdOrModuleName.ToLower();
}

void FNeoStackExtensionRegistry::RebuildIndexMapLocked()
{
	IndexByKey.Empty();

	for (int32 Index = 0; Index < Extensions.Num(); ++Index)
	{
		const FNeoStackExtensionDescriptor& Descriptor = Extensions[Index];
		if (!Descriptor.ExtensionId.IsEmpty())
		{
			IndexByKey.FindOrAdd(Descriptor.ExtensionId.ToLower(), Index);
		}
		if (!Descriptor.ModuleName.IsEmpty())
		{
			IndexByKey.FindOrAdd(Descriptor.ModuleName.ToLower(), Index);
		}
	}
}
