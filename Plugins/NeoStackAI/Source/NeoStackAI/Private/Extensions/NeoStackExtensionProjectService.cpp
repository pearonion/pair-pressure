// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Extensions/NeoStackExtensionProjectService.h"

#include "NeoStackAIModule.h"
#include "Extensions/NeoStackExtensionRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/Paths.h"
#include "ModuleDescriptor.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "ProjectDescriptor.h"

namespace
{

FString NormalizePath(const FString& InPath)
{
	FString Out = InPath;
	Out.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	return Out;
}

FString BuildLegacyExtensionId(const FString& ModuleName)
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

bool IsNeoStackExtensionPlugin(const IPlugin& Plugin)
{
	const FString PluginName = Plugin.GetName();
	if (PluginName.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase) ||
		PluginName.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase) ||
		PluginName.Equals(TEXT("NeostackAi"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const FPluginDescriptor& Descriptor = Plugin.GetDescriptor();
	if (Descriptor.Category.Equals(TEXT("NeoExtensions"), ESearchCase::IgnoreCase))
	{
		return true;
	}

	const FString LowerBaseDir = NormalizePath(Plugin.GetBaseDir()).ToLower();
	if (LowerBaseDir.Contains(TEXT("/plugins/neoextensions/")))
	{
		return true;
	}

	for (const FPluginReferenceDescriptor& Dependency : Descriptor.Plugins)
	{
		if (Dependency.Name.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase) ||
			Dependency.Name.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

FString InferExtensionId(const IPlugin& Plugin)
{
	for (const FModuleDescriptor& Module : Plugin.GetDescriptor().Modules)
	{
		const FString Id = BuildLegacyExtensionId(Module.Name.ToString());
		if (!Id.IsEmpty() && Id != TEXT("neostack.core"))
		{
			return Id;
		}
	}

	FString Id = Plugin.GetName();
	Id.ReplaceInline(TEXT("NeoStackAI_"), TEXT("neostack."), ESearchCase::CaseSensitive);
	Id.ReplaceInline(TEXT("NSAI_"), TEXT("neostack."), ESearchCase::CaseSensitive);
	Id.ReplaceInline(TEXT("_"), TEXT("."), ESearchCase::CaseSensitive);
	return Id.ToLower();
}

bool FindRuntimeDescriptorForPlugin(
	const IPlugin& Plugin,
	const TArray<FNeoStackExtensionDescriptor>& RuntimeExtensions,
	FNeoStackExtensionDescriptor& OutDescriptor)
{
	for (const FModuleDescriptor& Module : Plugin.GetDescriptor().Modules)
	{
		const FString ModuleNameStr = Module.Name.ToString();
		for (const FNeoStackExtensionDescriptor& Descriptor : RuntimeExtensions)
		{
			if (Descriptor.ModuleName.Equals(ModuleNameStr, ESearchCase::IgnoreCase))
			{
				OutDescriptor = Descriptor;
				return true;
			}
		}
	}

	const FString InferredId = InferExtensionId(Plugin);
	for (const FNeoStackExtensionDescriptor& Descriptor : RuntimeExtensions)
	{
		if (Descriptor.ExtensionId.Equals(InferredId, ESearchCase::IgnoreCase))
		{
			OutDescriptor = Descriptor;
			return true;
		}
	}

	return false;
}

void GetProjectPluginReferenceState(const FString& PluginName, bool& bOutEnabled, bool& bOutHasExplicitEntry)
{
	bOutEnabled = false;
	bOutHasExplicitEntry = false;

	const FProjectDescriptor* CurrentProject = IProjectManager::Get().GetCurrentProject();
	if (!CurrentProject)
	{
		return;
	}

	const int32 PluginRefIndex = CurrentProject->FindPluginReferenceIndex(PluginName);
	if (PluginRefIndex != INDEX_NONE)
	{
		bOutHasExplicitEntry = true;
		bOutEnabled = CurrentProject->Plugins[PluginRefIndex].bEnabled;
	}
}

bool IsPluginLoadedInSession(const IPlugin& Plugin)
{
	for (const FModuleDescriptor& Module : Plugin.GetDescriptor().Modules)
	{
		if (FModuleManager::Get().IsModuleLoaded(Module.Name))
		{
			return true;
		}
	}

	return Plugin.IsEnabled();
}

FString BuildProjectExtensionDescriptorPath(const FString& PluginName)
{
	return FPaths::ProjectPluginsDir() / TEXT("NeoExtensions") / PluginName / FString::Printf(TEXT("%s.uplugin"), *PluginName);
}

bool LoadProjectExtensionDescriptorFromDisk(
	const FString& PluginName,
	FPluginDescriptor& OutDescriptor,
	FString& OutDescriptorPath,
	FString& OutError)
{
	OutDescriptorPath = BuildProjectExtensionDescriptorPath(PluginName);
	if (!FPaths::FileExists(OutDescriptorPath))
	{
		OutError = FString::Printf(TEXT("Plugin '%s' was not found."), *PluginName);
		return false;
	}

	FText LoadFailReason;
	if (!OutDescriptor.Load(OutDescriptorPath, LoadFailReason))
	{
		OutError = FString::Printf(
			TEXT("Plugin '%s' descriptor could not be read: %s"),
			*PluginName,
			*LoadFailReason.ToString());
		return false;
	}

	if (!OutDescriptor.Category.Equals(TEXT("NeoExtensions"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Plugin '%s' is not a NeoStack extension plugin."), *PluginName);
		return false;
	}

	return true;
}

bool SetPluginReferenceEnabled(FProjectDescriptor& Descriptor, const FString& PluginName, bool bEnabled)
{
	const int32 ExistingIndex = Descriptor.FindPluginReferenceIndex(PluginName);
	if (ExistingIndex != INDEX_NONE)
	{
		if (Descriptor.Plugins[ExistingIndex].bEnabled != bEnabled)
		{
			Descriptor.Plugins[ExistingIndex].bEnabled = bEnabled;
			return true;
		}
		return false;
	}

	Descriptor.Plugins.Add(FPluginReferenceDescriptor(PluginName, bEnabled));
	return true;
}

bool AddRequiredDependenciesForEnable(
	const FPluginDescriptor& ExtensionDescriptor,
	TArray<FString>& OutPluginNames,
	FString& OutError)
{
	for (const FPluginReferenceDescriptor& Ref : ExtensionDescriptor.Plugins)
	{
		if (Ref.bOptional ||
			Ref.Name.IsEmpty() ||
			Ref.Name.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		const TSharedPtr<IPlugin> DepPlugin = IPluginManager::Get().FindPlugin(Ref.Name);
		if (!DepPlugin.IsValid())
		{
			OutError = FString::Printf(
				TEXT("Required plugin '%s' is not installed. Install or enable it before enabling this extension."),
				*Ref.Name);
			return false;
		}

		OutPluginNames.AddUnique(Ref.Name);
	}

	return true;
}

} // namespace

bool FNeoStackExtensionProjectService::IsPluginManaged(const FString& PluginName)
{
	return GetManagedExtensions().ContainsByPredicate(
		[&PluginName](const FNeoStackManagedExtension& E) { return E.PluginName == PluginName; });
}

TArray<FNeoStackManagedExtension> FNeoStackExtensionProjectService::GetManagedExtensions()
{
	TArray<FNeoStackManagedExtension> Result;

	const TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
	const TArray<FNeoStackExtensionDescriptor> RuntimeExtensions = FNeoStackExtensionRegistry::Get().GetAllExtensions();

	for (const TSharedRef<IPlugin>& Plugin : Plugins)
	{
		if (!IsNeoStackExtensionPlugin(*Plugin))
		{
			continue;
		}

		FNeoStackManagedExtension Extension;
		Extension.PluginName = Plugin->GetName();
		Extension.DisplayName = Plugin->GetFriendlyName();
		Extension.Description = Plugin->GetDescriptor().Description;
		Extension.Version = Plugin->GetDescriptor().VersionName;
		Extension.Vendor = Plugin->GetDescriptor().CreatedBy;
		Extension.Category = Plugin->GetDescriptor().Category;
		Extension.BaseDir = Plugin->GetBaseDir();
		Extension.ExtensionId = InferExtensionId(*Plugin);
		FNeoStackExtensionUIMetadataLoader::LoadFromPluginDir(Extension.PluginName, Extension.BaseDir, Extension.UIMetadata);
		Extension.bIsProjectPlugin = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project;
		Extension.bIsInstalledOnEngine = Plugin->GetDescriptor().bInstalled;
		Extension.bExplicitlyLoaded = Plugin->GetDescriptor().bExplicitlyLoaded;
		Extension.bEnabledByDefault = Plugin->GetDescriptor().EnabledByDefault == EPluginEnabledByDefault::Enabled;
		Extension.bIsBetaVersion = Plugin->GetDescriptor().bIsBetaVersion;
		Extension.bIsExperimentalVersion = Plugin->GetDescriptor().bIsExperimentalVersion;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		Extension.bMountedInSession = Plugin->IsMounted();
#else
		// Pre-5.7: IPlugin::IsMounted() not exported. Treat enabled+loaded as mounted.
		Extension.bMountedInSession = Plugin->IsEnabled();
#endif
		Extension.bLoadedInSession = IsPluginLoadedInSession(*Plugin);
		Extension.bCanToggle = Extension.bIsProjectPlugin;

		GetProjectPluginReferenceState(Extension.PluginName, Extension.bEnabledInProject, Extension.bHasExplicitProjectEntry);
		if (!Extension.bHasExplicitProjectEntry)
		{
			Extension.bEnabledInProject = Plugin->IsEnabled();
		}

		FNeoStackExtensionDescriptor RuntimeDescriptor;
		if (FindRuntimeDescriptorForPlugin(*Plugin, RuntimeExtensions, RuntimeDescriptor))
		{
			Extension.bHasRuntimeDescriptor = true;
			Extension.ExtensionId = RuntimeDescriptor.ExtensionId.IsEmpty() ? Extension.ExtensionId : RuntimeDescriptor.ExtensionId;
			Extension.DisplayName = RuntimeDescriptor.DisplayName.IsEmpty() ? Extension.DisplayName : RuntimeDescriptor.DisplayName;
			Extension.Description = RuntimeDescriptor.Description.IsEmpty() ? Extension.Description : RuntimeDescriptor.Description;
			// Keep the package/update version sourced from the installed .uplugin
			// descriptor. Runtime descriptors are capability metadata and are
			// often compiled with placeholder versions.
			Extension.Vendor = RuntimeDescriptor.Vendor.IsEmpty() ? Extension.Vendor : RuntimeDescriptor.Vendor;
			Extension.Category = RuntimeDescriptor.Category.IsEmpty() ? Extension.Category : RuntimeDescriptor.Category;
			Extension.RuntimeState = RuntimeDescriptor.State;
			Extension.StatusMessage = RuntimeDescriptor.StatusMessage;
			Extension.bIsBuiltIn = RuntimeDescriptor.bIsBuiltIn;
			Extension.bIsThirdParty = RuntimeDescriptor.bIsThirdParty;
			Extension.bActiveInSession = RuntimeDescriptor.State == ENeoStackExtensionState::Active;
		}

		Extension.bRestartRequired = Extension.bEnabledInProject != Extension.bLoadedInSession;

		// Surface the extension's declared plugin dependencies so the UI can
		// explain why a card is `unavailable` and offer a one-click flip.
		// We skip the NeoStackAI self-dep (always present when these load).
		// Plugin names that mark an extension as third-party — these are not
		// shipped with UE; the user has to install them separately or get them
		// bundled with our extension package. Used so the "Third-party" pill
		// shows even when the extension's own module hasn't loaded yet (the
		// runtime descriptor's bIsThirdParty only kicks in post-load).
		static const TSet<FString> ThirdPartyDepNames = {
			TEXT("AscentCombatFramework"),
			TEXT("BlueprintAssist"),
		};
		for (const FPluginReferenceDescriptor& Ref : Plugin->GetDescriptor().Plugins)
		{
			if (Ref.Name.Equals(TEXT("NeoStackAI"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			FNeoStackExtensionDependency Dep;
			Dep.Name = Ref.Name;
			Dep.bOptional = Ref.bOptional;

			const TSharedPtr<IPlugin> DepPlugin = IPluginManager::Get().FindPlugin(Ref.Name);
			Dep.bInstalled = DepPlugin.IsValid();
			Dep.bEnabled = DepPlugin.IsValid() && DepPlugin->IsEnabled();

			if (ThirdPartyDepNames.Contains(Ref.Name))
			{
				Extension.bIsThirdParty = true;
			}

			Extension.Dependencies.Add(MoveTemp(Dep));
		}

		Result.Add(MoveTemp(Extension));
	}

	Result.Sort([](const FNeoStackManagedExtension& A, const FNeoStackManagedExtension& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	return Result;
}

bool FNeoStackExtensionProjectService::SetProjectExtensionEnabled(const FString& PluginName, bool bEnabled, FString& OutError)
{
	const FString TrimmedPluginName = PluginName.TrimStartAndEnd();
	if (TrimmedPluginName.IsEmpty())
	{
		OutError = TEXT("Plugin name is empty.");
		return false;
	}

	FPluginDescriptor ExtensionDescriptor;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TrimmedPluginName);
	if (!Plugin.IsValid())
	{
		// A freshly installed extension can fail IPluginManager discovery when
		// one of its required plugins is disabled or missing. Fall back to the
		// project NeoExtensions descriptor so we can either enable its deps in
		// the same .uproject edit or return the missing dependency by name.
		FString DescriptorPath;
		if (!LoadProjectExtensionDescriptorFromDisk(TrimmedPluginName, ExtensionDescriptor, DescriptorPath, OutError))
		{
			return false;
		}
	}
	else if (!IsNeoStackExtensionPlugin(*Plugin))
	{
		OutError = FString::Printf(TEXT("Plugin '%s' is not a NeoStack extension plugin."), *TrimmedPluginName);
		return false;
	}
	else if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
	{
		OutError = FString::Printf(TEXT("Plugin '%s' is not project-scoped and cannot be toggled here."), *TrimmedPluginName);
		return false;
	}
	else
	{
		ExtensionDescriptor = Plugin->GetDescriptor();
	}

	const FString ProjectFile = FPaths::GetProjectFilePath();
	if (ProjectFile.IsEmpty())
	{
		OutError = TEXT("Current project file path is not available.");
		return false;
	}

	// Direct read/modify/write of the .uproject descriptor. UE 5.7 made
	// GameProjectUtils::UpdateGameProjectFile private, so we use the public
	// FProjectDescriptor::Load / Save API instead.
	FProjectDescriptor Descriptor;
	FText LoadFailReason;
	if (!Descriptor.Load(ProjectFile, LoadFailReason))
	{
		OutError = LoadFailReason.ToString();
		return false;
	}

	bool bModified = false;
	if (bEnabled)
	{
		TArray<FString> PluginNamesToEnable;
		if (!AddRequiredDependenciesForEnable(ExtensionDescriptor, PluginNamesToEnable, OutError))
		{
			return false;
		}
		PluginNamesToEnable.AddUnique(TrimmedPluginName);

		for (const FString& Name : PluginNamesToEnable)
		{
			bModified |= SetPluginReferenceEnabled(Descriptor, Name, true);
		}
	}
	else
	{
		bModified = SetPluginReferenceEnabled(Descriptor, TrimmedPluginName, false);
	}

	if (bModified)
	{
		FText SaveFailReason;
		if (!Descriptor.Save(ProjectFile, SaveFailReason))
		{
			OutError = SaveFailReason.ToString();
			return false;
		}

		// IProjectManager caches the current project descriptor in memory; our
		// direct-to-disk save doesn't invalidate it, so GetCurrentProject()
		// would keep returning the pre-edit Plugins[] and subsequent
		// GetManagedExtensions() calls would still report the old enabled
		// state. Reload from disk to keep the two in sync.
		IProjectManager::Get().LoadProjectFile(ProjectFile);
	}

	IPluginManager::Get().RefreshPluginsList();

	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Project plugin '%s' marked %s in %s"),
		*TrimmedPluginName,
		bEnabled ? TEXT("enabled") : TEXT("disabled"),
		*ProjectFile);

	return true;
}

bool FNeoStackExtensionProjectService::SetProjectExtensionsEnabled(const TArray<FString>& PluginNames, bool bEnabled, FString& OutError)
{
	if (PluginNames.Num() == 0)
	{
		return true;
	}

	const FString ProjectFile = FPaths::GetProjectFilePath();
	if (ProjectFile.IsEmpty())
	{
		OutError = TEXT("Current project file path is not available.");
		return false;
	}

	FProjectDescriptor Descriptor;
	FText LoadFailReason;
	if (!Descriptor.Load(ProjectFile, LoadFailReason))
	{
		OutError = LoadFailReason.ToString();
		return false;
	}

	bool bModified = false;
	for (const FString& Raw : PluginNames)
	{
		const FString Name = Raw.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			continue;
		}

		const int32 ExistingIndex = Descriptor.FindPluginReferenceIndex(Name);
		if (ExistingIndex != INDEX_NONE)
		{
			if (Descriptor.Plugins[ExistingIndex].bEnabled != bEnabled)
			{
				Descriptor.Plugins[ExistingIndex].bEnabled = bEnabled;
				bModified = true;
			}
		}
		else
		{
			Descriptor.Plugins.Add(FPluginReferenceDescriptor(Name, bEnabled));
			bModified = true;
		}
	}

	if (bModified)
	{
		FText SaveFailReason;
		if (!Descriptor.Save(ProjectFile, SaveFailReason))
		{
			OutError = SaveFailReason.ToString();
			return false;
		}

		// Same in-memory cache invalidation as the single-plugin path.
		IProjectManager::Get().LoadProjectFile(ProjectFile);
	}

	IPluginManager::Get().RefreshPluginsList();

	UE_LOG(LogNeoStackAI, Log,
		TEXT("NeoStackExtension: Bulk-marked %d plugin(s) %s in %s"),
		PluginNames.Num(),
		bEnabled ? TEXT("enabled") : TEXT("disabled"),
		*ProjectFile);

	return true;
}
