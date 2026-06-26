// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Extensions/NeoStackExtensionUIMetadata.h"

#include "NeoStackAIModule.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ReadRequiredString(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, FString& OutValue)
{
	return Root.IsValid() && Root->TryGetStringField(Field, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty();
}

bool ReadRequiredStringArray(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, TArray<FString>& OutValues)
{
	if (!Root.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Root->TryGetArrayField(Field, Values) || !Values)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text) && !Text.TrimStartAndEnd().IsEmpty())
		{
			OutValues.Add(Text);
		}
	}

	return OutValues.Num() > 0;
}

bool IsKnownDomain(const FString& Domain)
{
	static const TSet<FString> KnownDomains = {
		TEXT("recommended"),
		TEXT("animation"),
		TEXT("ai"),
		TEXT("vfx"),
		TEXT("world"),
		TEXT("input"),
		TEXT("pipeline"),
	};
	return KnownDomains.Contains(Domain);
}
}

bool FNeoStackExtensionUIMetadataLoader::LoadFromPluginDir(
	const FString& PluginName,
	const FString& BaseDir,
	FNeoStackExtensionUIMetadata& OutMetadata)
{
	OutMetadata = FNeoStackExtensionUIMetadata();

	const FString MetadataPath = FPaths::Combine(BaseDir, TEXT("Resources"), TEXT("ExtensionUI.json"));
	if (!FPaths::FileExists(MetadataPath))
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("Extension UI metadata missing for %s at %s"),
			*PluginName,
			*MetadataPath);
		return false;
	}

	FString RawJson;
	if (!FFileHelper::LoadFileToString(RawJson, *MetadataPath))
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("Extension UI metadata could not be read for %s at %s"),
			*PluginName,
			*MetadataPath);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("Extension UI metadata is not valid JSON for %s at %s"),
			*PluginName,
			*MetadataPath);
		return false;
	}

	double SchemaVersion = 0.0;
	if (!Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) || static_cast<int32>(SchemaVersion) != 1)
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("Extension UI metadata for %s uses unsupported schemaVersion at %s"),
			*PluginName,
			*MetadataPath);
		return false;
	}

	FNeoStackExtensionUIMetadata Parsed;
	Parsed.SchemaVersion = 1;
	double SortOrder = 100.0;
	Root->TryGetNumberField(TEXT("sortOrder"), SortOrder);
	Parsed.SortOrder = static_cast<int32>(SortOrder);
	Root->TryGetBoolField(TEXT("isRecommended"), Parsed.bIsRecommended);

	const bool bHasRequiredFields =
		ReadRequiredString(Root, TEXT("domain"), Parsed.Domain) &&
		ReadRequiredString(Root, TEXT("domainLabel"), Parsed.DomainLabel) &&
		ReadRequiredString(Root, TEXT("agentSummary"), Parsed.AgentSummary) &&
		ReadRequiredString(Root, TEXT("whenToEnable"), Parsed.WhenToEnable) &&
		ReadRequiredStringArray(Root, TEXT("enablesAgentTo"), Parsed.EnablesAgentTo);

	if (!bHasRequiredFields || !IsKnownDomain(Parsed.Domain))
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("Extension UI metadata missing required v1 fields for %s at %s"),
			*PluginName,
			*MetadataPath);
		return false;
	}

	Parsed.bHasMetadata = true;
	OutMetadata = MoveTemp(Parsed);
	return true;
}
