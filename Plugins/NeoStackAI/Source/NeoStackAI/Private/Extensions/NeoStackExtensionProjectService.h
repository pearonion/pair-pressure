// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Extensions/NeoStackExtensionTypes.h"
#include "Extensions/NeoStackExtensionUIMetadata.h"

struct FNeoStackExtensionDependency
{
	FString Name;
	bool bOptional = false;
	bool bEnabled = false;    // Effective enabled state for the current session.
	bool bInstalled = false;  // Discovered on disk (engine, project, or external).
};

struct FNeoStackManagedExtension
{
	FString PluginName;
	FString ExtensionId;
	FString DisplayName;
	FString Description;
	FString Version;
	FString Vendor;
	FString Category;
	FString StatusMessage;
	FString BaseDir;
	ENeoStackExtensionState RuntimeState = ENeoStackExtensionState::Registered;
	bool bHasRuntimeDescriptor = false;
	bool bEnabledInProject = false;
	bool bHasExplicitProjectEntry = false;
	bool bLoadedInSession = false;
	bool bMountedInSession = false;
	bool bActiveInSession = false;
	bool bRestartRequired = false;
	bool bCanToggle = false;
	bool bIsProjectPlugin = false;
	bool bIsInstalledOnEngine = false;
	bool bExplicitlyLoaded = false;
	bool bEnabledByDefault = false;
	bool bIsBetaVersion = false;
	bool bIsExperimentalVersion = false;
	bool bIsBuiltIn = false;
	bool bIsThirdParty = false;
	FNeoStackExtensionUIMetadata UIMetadata;
	TArray<FNeoStackExtensionDependency> Dependencies;
};

class FNeoStackExtensionProjectService
{
public:
	static TArray<FNeoStackManagedExtension> GetManagedExtensions();
	static bool IsPluginManaged(const FString& PluginName);
	static bool SetProjectExtensionEnabled(const FString& PluginName, bool bEnabled, FString& OutError);
	// Bulk variant — flips multiple plugin entries in a single .uproject load/save.
	// Used when enabling an extension together with its required engine/external deps.
	static bool SetProjectExtensionsEnabled(const TArray<FString>& PluginNames, bool bEnabled, FString& OutError);
};
