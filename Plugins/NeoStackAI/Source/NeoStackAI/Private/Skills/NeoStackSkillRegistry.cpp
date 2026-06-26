// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Skills/NeoStackSkillRegistry.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "NeoStackAIModule.h"

namespace
{

// Mirrors BuildLegacyExtensionId in NeoStackExtensionRegistry.cpp — derives the stable
// `neostack.<domain>` identifier from a plugin's module/folder name so skills and
// capability registries agree on ownership.
FString PluginNameToSourceId(const FString& PluginName)
{
	if (PluginName.Equals(TEXT("NeoStackAI"), ESearchCase::CaseSensitive))
	{
		return TEXT("neostack.core");
	}

	FString Id = PluginName;
	Id.ReplaceInline(TEXT("NeoStackAI_"), TEXT("neostack."), ESearchCase::CaseSensitive);
	Id.ReplaceInline(TEXT("_"), TEXT("."), ESearchCase::CaseSensitive);
	return Id.ToLower();
}

bool IsNeoStackPlugin(const FString& PluginName)
{
	return PluginName.Equals(TEXT("NeoStackAI"), ESearchCase::CaseSensitive)
		|| PluginName.StartsWith(TEXT("NeoStackAI_"), ESearchCase::CaseSensitive);
}

void CollectSkillsFromPlugin(TSharedRef<IPlugin> Plugin, TArray<FNeoStackSkill>& InOut)
{
	const FString SkillsRoot = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Skills"));
	IFileManager& Files = IFileManager::Get();
	if (!Files.DirectoryExists(*SkillsRoot))
	{
		return;
	}

	const FString SourceId = PluginNameToSourceId(Plugin->GetName());
	const FString Version = Plugin->GetDescriptor().VersionName;
	const FString DisplayName = Plugin->GetDescriptor().FriendlyName.IsEmpty()
		? Plugin->GetName()
		: Plugin->GetDescriptor().FriendlyName;

	// Enumerate immediate subdirectories; each is a candidate skill package.
	TArray<FString> SkillDirs;
	Files.FindFiles(SkillDirs, *(SkillsRoot / TEXT("*")), /*Files*/ false, /*Directories*/ true);

	for (const FString& DirName : SkillDirs)
	{
		const FString SkillMdPath = FPaths::Combine(SkillsRoot, DirName, TEXT("SKILL.md"));
		if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*SkillMdPath))
		{
			continue;
		}

		FNeoStackSkill Skill;
		if (!ParseSkillMdFile(SkillMdPath, Skill))
		{
			continue;
		}

		Skill.SourceId = SourceId;
		Skill.SourceDisplayName = DisplayName;
		Skill.SourceVersion = Version;
		InOut.Add(MoveTemp(Skill));
	}
}

} // namespace

FNeoStackSkillRegistry& FNeoStackSkillRegistry::Get()
{
	static FNeoStackSkillRegistry Instance;
	return Instance;
}

void FNeoStackSkillRegistry::Refresh()
{
	TArray<FNeoStackSkill> Found;

	for (TSharedRef<IPlugin> Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		if (!IsNeoStackPlugin(Plugin->GetName()))
		{
			continue;
		}
		CollectSkillsFromPlugin(Plugin, Found);
	}

	Found.StableSort([](const FNeoStackSkill& A, const FNeoStackSkill& B)
	{
		if (A.SourceId != B.SourceId) { return A.SourceId < B.SourceId; }
		return A.Name < B.Name;
	});

	{
		FScopeLock ScopeLock(&Lock);
		Skills = MoveTemp(Found);
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("SkillRegistry: refreshed, %d skill(s) discovered"), Skills.Num());
	for (const FNeoStackSkill& S : Skills)
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("  • %s  (source=%s  digest=%s)"),
			*S.Name, *S.SourceId, *S.ShippedDigest.Left(8));
	}
}

TArray<FNeoStackSkill> FNeoStackSkillRegistry::GetAll() const
{
	FScopeLock ScopeLock(&Lock);
	return Skills;
}

TArray<FNeoStackSkill> FNeoStackSkillRegistry::GetBySource(const FString& SourceId) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FNeoStackSkill> Out;
	for (const FNeoStackSkill& S : Skills)
	{
		if (S.SourceId.Equals(SourceId, ESearchCase::IgnoreCase))
		{
			Out.Add(S);
		}
	}
	return Out;
}

bool FNeoStackSkillRegistry::FindByName(const FString& SkillName, FNeoStackSkill& OutSkill) const
{
	FScopeLock ScopeLock(&Lock);
	for (const FNeoStackSkill& S : Skills)
	{
		if (S.Name.Equals(SkillName, ESearchCase::IgnoreCase))
		{
			OutSkill = S;
			return true;
		}
	}
	return false;
}
