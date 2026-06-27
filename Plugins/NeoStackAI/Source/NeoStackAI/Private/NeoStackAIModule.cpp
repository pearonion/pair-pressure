// Copyright 2026 Betide Studio. All Rights Reserved.
// Build marker: refresh core plugin package.
// Force rebuild v1.0.6 — refresh plugin source marker

#include "NeoStackAIModule.h"

DEFINE_LOG_CATEGORY(LogNeoStackAI);
#include "ACPSettingsCustomization.h"
#include "AIDetailPanelExtension.h"
#include "MCPServer.h"
#include "TerminalManager.h"
#include "ACPAttachmentManager.h"
#include "ACPAgentManager.h"
#include "AgentUsageMonitor.h"
#include "ACPRegistryClient.h"
#include "Providers/GenerativeProvider.h"
#include "Chat/ChatProviderRegistrar.h"
#include "Chat/ChatModelRegistry.h"
#include "Chat/ChatStore.h"
#include "AgentService.h"
#include "RelayClient.h"
#include "LocalClient.h"
#include "NSAICrashReporter.h"
#include "EntitlementClient.h"
#include "BuildVariant.h"
#include "Extensions/NeoStackExtensionRegistry.h"
#include "Extensions/NeoStackExtensionInstaller.h"
#include "Extensions/NeoStackExtensionCatalog.h"
#include "Skills/NeoStackSkillRegistry.h"
#include "Skills/NeoStackSkillInstaller.h"

#include "LevelEditor.h"
#include "ToolMenus.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
#include "StatusBarSubsystem.h"
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
#endif
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/Reply.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

// Content Browser
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

// Version check & auto-update
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IPluginManager.h"
#include "ACPSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "Containers/Ticker.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Async/Async.h"

// Blueprint/Graph context
#include "GraphEditorModule.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetData.h"

// WebBrowser for embedded web UI
#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserWindow.h"
#include "WebUIBridge.h"

// Asset drag-drop support
#include "Widgets/SACPAttachmentDropTarget.h"

// Local file server for WebUI (avoids file:// CORS on Windows)
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpResultCallback.h"

// Lua test runner & serializer
#include "Lua/NeoLuaState.h"
#include "LevelSerializerUtils.h"

// Analytics
#include "NSAIAnalytics.h"

#define LOCTEXT_NAMESPACE "FNeoStackAIModule"

// TEMPORARY: Test crash command — remove after testing crash reporter
static FAutoConsoleCommand GTestCrashCmd(
	TEXT("NSAI.TestCrash"),
	TEXT("Force a crash to test the AIK crash reporting pipeline. DO NOT ship with this."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FNSAICrashReporter::SetCrashContext(TEXT("CurrentTool"), TEXT("TestCrash"));
		FNSAICrashReporter::SetCrashContext(TEXT("Status"), TEXT("TestingCrashReporter"));
		UE_LOG(LogNeoStackAI, Warning, TEXT("NSAI.TestCrash: Forcing crash in 1 second..."));

		// Defer so the log line gets flushed
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float) -> bool
			{
				// This will trigger OnHandleSystemError -> breadcrumb write
				UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.TestCrash: Crashing now!"));
				check(false && "NSAI.TestCrash: Intentional crash for testing crash reporter pipeline");
				return false;
			}),
			1.0f);
	})
);

// Console command: NSAI.ExportLevel [OutputDir]
// Exports current level as Lua script + JSON metadata for training data
static FAutoConsoleCommand GExportLevelCmd(
	TEXT("NSAI.ExportLevel"),
	TEXT("Export current level as Lua script + JSON metadata for training data. Usage: NSAI.ExportLevel [OutputDir]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (!GEditor)
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: No editor"));
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: No editor world loaded"));
			return;
		}

		// Output directory: argument or default to Project/Saved/NSAI_TrainingData/
		FString OutputDir;
		if (Args.Num() > 0 && !Args[0].IsEmpty())
		{
			OutputDir = Args[0];
		}
		else
		{
			OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NSAI_TrainingData"));
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*OutputDir);

		FString LevelName = World->GetName();
		// Sanitize level name for filename
		LevelName.ReplaceInline(TEXT("/"), TEXT("_"));
		LevelName.ReplaceInline(TEXT(" "), TEXT("_"));

		// Run the Lua serializer script
		FString SerializerPath = FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("Plugins/NeoStackAI/Scripts/serialize_level.lua"));

		FString SerializerScript;
		if (!FFileHelper::LoadFileToString(SerializerScript, *SerializerPath))
		{
			UE_LOG(LogNeoStackAI, Error,
				TEXT("NSAI.ExportLevel: Could not read serializer at: %s"), *SerializerPath);
			return;
		}

		UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.ExportLevel: Running Lua serializer..."));

		// serialize_level.lua is an internal sync script (no async bindings).
		FScriptResult Result = FNeoLuaState::ExecuteSyncBlocking(SerializerScript);

		// Print trace to log
		for (const FString& Line : Result.Trace)
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("  %s"), *Line);
		}

		if (!Result.bSuccess)
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: Lua error: %s"), *Result.Error);
			return;
		}

		// The serializer returns the Lua script as a string
		FString Script = Result.ReturnValue;
		if (Script.IsEmpty())
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: Serializer returned empty script"));
			return;
		}

		// Save Lua script
		FString ScriptPath = FPaths::Combine(OutputDir, LevelName + TEXT(".lua"));
		if (FFileHelper::SaveStringToFile(Script, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.ExportLevel: Lua script -> %s (%d chars)"),
				*ScriptPath, Script.Len());
		}
		else
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: Failed to write %s"), *ScriptPath);
		}

		// Save JSON metadata (still use C++ for this — it's just counts)
		FString Meta = LevelSerializerUtils::GenerateMetadata(World);
		FString MetaPath = FPaths::Combine(OutputDir, LevelName + TEXT("_meta.json"));
		if (FFileHelper::SaveStringToFile(Meta, *MetaPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.ExportLevel: Metadata -> %s"), *MetaPath);
		}
		else
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.ExportLevel: Failed to write %s"), *MetaPath);
		}

		UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.ExportLevel: Done! Files saved to %s"), *OutputDir);
	})
);

// Console command: NSAI.RunLua <file_or_inline>
// Runs a Lua script from a file path or inline string
static FAutoConsoleCommand GRunLuaCmd(
	TEXT("NSAI.RunLua"),
	TEXT("Run a Lua script. Usage: NSAI.RunLua <path_to_file.lua> OR NSAI.RunLua <inline lua code>"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() == 0)
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.RunLua: No script provided. Usage: NSAI.RunLua <file.lua> or NSAI.RunLua <inline code>"));
			return;
		}

		FString Script;
		FString Input = FString::Join(Args, TEXT(" "));

		// Check if it's a file path (ends with .lua or contains path separators)
		if (Input.EndsWith(TEXT(".lua")))
		{
			// Try as-is first, then relative to Plugins/NeoStackAI/
			FString FilePath = Input;
			if (!FPaths::FileExists(FilePath))
			{
				FilePath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/NeoStackAI"), Input);
			}
			if (!FPaths::FileExists(FilePath))
			{
				FilePath = FPaths::Combine(FPaths::ProjectDir(), Input);
			}
			if (!FFileHelper::LoadFileToString(Script, *FilePath))
			{
				UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.RunLua: Could not read file: %s"), *Input);
				return;
			}
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.RunLua: Running %s"), *FilePath);
		}
		else
		{
			// Inline Lua code
			Script = Input;
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.RunLua: Running inline script (%d chars)"), Script.Len());
		}

		// NSAI.RunLua console command — author runs trusted scripts; if they call
		// async bindings the helper cancels and returns an error.
		FScriptResult Result = FNeoLuaState::ExecuteSyncBlocking(Script);

		for (const FString& Line : Result.Trace)
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("%s"), *Line);
		}

		if (!Result.bSuccess)
		{
			UE_LOG(LogNeoStackAI, Error, TEXT("NSAI.RunLua: Error: %s"), *Result.Error);
		}
		else
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.RunLua: Done."));
		}
	})
);

static FString LuaSingleQuotedString(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return FString::Printf(TEXT("'%s'"), *Escaped);
}

// Console command: NSAI.RunBindingTests [filter ...]
static FAutoConsoleCommand GRunBindingTestsCmd(
	TEXT("NSAI.RunBindingTests"),
	TEXT("Run the NeoStackAI Lua binding test suite. Usage: NSAI.RunBindingTests [file/name filter ...]"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		// Resolve via IPluginManager so the plugin folder can be named anything
		// (Fab installs may keep the legacy AgentIntegrationKit directory name
		// while the module/plugin itself is called NeoStackAI).
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
		if (!Plugin.IsValid())
		{
			UE_LOG(LogNeoStackAI, Error,
				TEXT("NSAI.RunBindingTests: NeoStackAI plugin not found via IPluginManager"));
			return;
		}
		const FString RunnerPath = FPaths::Combine(
			Plugin->GetBaseDir(), TEXT("Tests"), TEXT("test_runner.lua"));
		FString PluginBaseDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		FPaths::NormalizeFilename(PluginBaseDir);

		FString Script;
		if (!FFileHelper::LoadFileToString(Script, *RunnerPath))
		{
			UE_LOG(LogNeoStackAI, Error,
				TEXT("NSAI.RunBindingTests: Could not read test runner at: %s"), *RunnerPath);
			return;
		}

		TArray<FString> FileFilters;
		TArray<FString> CaseFilters;
		bool bParsingCaseFilters = false;
		for (const FString& Arg : Args)
		{
			const FString TrimmedArg = Arg.TrimStartAndEnd();
			if (TrimmedArg.Equals(TEXT("--case"), ESearchCase::IgnoreCase) ||
				TrimmedArg.Equals(TEXT("--test"), ESearchCase::IgnoreCase))
			{
				bParsingCaseFilters = true;
				continue;
			}

			if (!TrimmedArg.IsEmpty())
			{
				(bParsingCaseFilters ? CaseFilters : FileFilters).Add(TrimmedArg);
			}
		}

		auto JoinLuaStringArray = [](const TArray<FString>& Values) -> FString
		{
			FString Result;
			for (const FString& Value : Values)
			{
				if (!Result.IsEmpty())
				{
					Result += TEXT(", ");
				}
				Result += LuaSingleQuotedString(Value);
			}
			return Result;
		};

		const FString Bootstrap = FString::Printf(
			TEXT("_NSAI_BINDING_TEST_PLUGIN_DIR = %s\n_NSAI_BINDING_TEST_FILTERS = { %s }\n_NSAI_BINDING_TEST_CASE_FILTERS = { %s }\n"),
			*LuaSingleQuotedString(PluginBaseDir),
			*JoinLuaStringArray(FileFilters),
			*JoinLuaStringArray(CaseFilters));
		Script = Bootstrap + Script;

		if (FileFilters.Num() > 0 || CaseFilters.Num() > 0)
		{
			if (FileFilters.Num() > 0)
			{
				UE_LOG(LogNeoStackAI, Display,
					TEXT("NSAI.RunBindingTests: File filter(s): %s"),
					*FString::Join(FileFilters, TEXT(", ")));
			}
			if (CaseFilters.Num() > 0)
			{
				UE_LOG(LogNeoStackAI, Display,
					TEXT("NSAI.RunBindingTests: Test case filter(s): %s"),
					*FString::Join(CaseFilters, TEXT(", ")));
			}
		}
		else
		{
			UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.RunBindingTests: Running binding tests..."));
		}

		// Some binding tests intentionally touch coroutine-based APIs such as
		// generate(). Keep the runner alive until async resumes complete.
		TSharedRef<FNeoLuaState> Runner = MakeShared<FNeoLuaState>();
		Runner->Execute(Script,
			[Runner](FScriptResult Result)
			{
				for (const FString& Line : Result.Trace)
				{
					UE_LOG(LogNeoStackAI, Display, TEXT("%s"), *Line);
				}

				if (!Result.bSuccess)
				{
					UE_LOG(LogNeoStackAI, Error,
						TEXT("NSAI.RunBindingTests: Script error: %s"), *Result.Error);
				}
				else
				{
					UE_LOG(LogNeoStackAI, Display, TEXT("NSAI.RunBindingTests: Done."));
				}
			});
	})
);

const FName FNeoStackAIModule::AgentChatTabName(TEXT("AgentChat"));
const FName FNeoStackAIModule::EpicAIAssistantTabName(TEXT("AIAssistant"));
FPluginUpdateInfo FNeoStackAIModule::CachedUpdateInfo;

// Update notification state (file-static, not class members — keeps header clean)
static TSharedPtr<SNotificationItem> GUpdateNotification;
static FTSTicker::FDelegateHandle GProgressTickerHandle;
static FDelegateHandle GUpdateEntitlementResolvedHandle;
static FTSTicker::FDelegateHandle GEntitlementRefreshTickerHandle;

// Forward declarations for static helpers (defined after ShutdownModule)
static void StopProgressTicker();
static bool TickUpdateProgress(float);
static void ShowUpdateNotification();

static bool WriteJsonAtomically(const FString& FinalPath, const FString& Contents)
{
	const FString TempPath = FinalPath + TEXT(".tmp");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FinalPath), true);

	if (!FFileHelper::SaveStringToFile(Contents, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}

	if (!IFileManager::Get().Move(*FinalPath, *TempPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TempPath, false, true);
		return false;
	}

	return true;
}

static bool SerializeCondensedJsonObject(const TSharedRef<FJsonObject>& Object, FString& OutSerialized)
{
	OutSerialized.Reset();
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutSerialized);
	return FJsonSerializer::Serialize(Object, Writer);
}

static FString GetGlobalDiscoveryFilePath()
{
#if PLATFORM_WINDOWS
	const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
	if (!LocalAppData.IsEmpty())
	{
		return FPaths::Combine(LocalAppData, TEXT("NeoStackAI"), TEXT("runtimes.json"));
	}
#endif

#if PLATFORM_MAC || PLATFORM_LINUX
	const FString HomeDir = FPlatformProcess::UserDir();
	if (!HomeDir.IsEmpty())
	{
		return FPaths::Combine(HomeDir, TEXT(".neostack"), TEXT("runtimes.json"));
	}
#endif

	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("NeoStackAI"), TEXT("runtimes.json"));
}

static bool RuntimeEntryMatchesCurrent(
	const TSharedPtr<FJsonObject>& Entry,
	const FString& CurrentInstanceId,
	const FString& CurrentProjectPath)
{
	if (!Entry.IsValid())
	{
		return false;
	}

	FString EntryInstanceId;
	if (Entry->TryGetStringField(TEXT("instanceId"), EntryInstanceId) && EntryInstanceId == CurrentInstanceId)
	{
		return true;
	}

	FString EntryProjectPath;
	if (Entry->TryGetStringField(TEXT("projectPath"), EntryProjectPath))
	{
		FString NormalizedEntryProjectPath = EntryProjectPath;
		FString NormalizedCurrentProjectPath = CurrentProjectPath;
		FPaths::NormalizeDirectoryName(NormalizedEntryProjectPath);
		FPaths::NormalizeDirectoryName(NormalizedCurrentProjectPath);
		return NormalizedEntryProjectPath == NormalizedCurrentProjectPath;
	}

	return false;
}

static void WriteGlobalDiscoveryFile(const TSharedRef<FJsonObject>& CurrentRuntime)
{
	FString CurrentInstanceId;
	CurrentRuntime->TryGetStringField(TEXT("instanceId"), CurrentInstanceId);

	FString CurrentProjectPath;
	CurrentRuntime->TryGetStringField(TEXT("projectPath"), CurrentProjectPath);

	TArray<TSharedPtr<FJsonValue>> Runtimes;
	const FString RegistryPath = GetGlobalDiscoveryFilePath();

	FString ExistingContents;
	if (FFileHelper::LoadFileToString(ExistingContents, *RegistryPath))
	{
		TSharedPtr<FJsonObject> ExistingRoot;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ExistingContents);
		if (FJsonSerializer::Deserialize(Reader, ExistingRoot) && ExistingRoot.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ExistingRuntimes = nullptr;
			if (ExistingRoot->TryGetArrayField(TEXT("runtimes"), ExistingRuntimes))
			{
				for (const TSharedPtr<FJsonValue>& RuntimeValue : *ExistingRuntimes)
				{
					TSharedPtr<FJsonObject> RuntimeObject = RuntimeValue.IsValid()
						? RuntimeValue->AsObject()
						: nullptr;
					if (RuntimeObject.IsValid() && !RuntimeEntryMatchesCurrent(RuntimeObject, CurrentInstanceId, CurrentProjectPath))
					{
						Runtimes.Add(RuntimeValue);
					}
				}
			}
		}
	}

	Runtimes.Add(MakeShared<FJsonValueObject>(CurrentRuntime));

	TSharedRef<FJsonObject> RegistryRoot = MakeShared<FJsonObject>();
	RegistryRoot->SetNumberField(TEXT("schemaVersion"), 1);
	RegistryRoot->SetStringField(TEXT("updatedAt"), FDateTime::UtcNow().ToIso8601());
	RegistryRoot->SetArrayField(TEXT("runtimes"), Runtimes);

	FString Serialized;
	if (!SerializeCondensedJsonObject(RegistryRoot, Serialized))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("NSAI Discovery: Failed to serialize global runtime registry"));
		return;
	}

	if (!WriteJsonAtomically(RegistryPath, Serialized))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("NSAI Discovery: Failed to write %s"), *RegistryPath);
	}
}

static void RemoveGlobalDiscoveryEntry(const FString& CurrentInstanceId, const FString& CurrentProjectPath)
{
	const FString RegistryPath = GetGlobalDiscoveryFilePath();

	FString ExistingContents;
	if (!FFileHelper::LoadFileToString(ExistingContents, *RegistryPath))
	{
		return;
	}

	TSharedPtr<FJsonObject> ExistingRoot;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ExistingContents);
	if (!FJsonSerializer::Deserialize(Reader, ExistingRoot) || !ExistingRoot.IsValid())
	{
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* ExistingRuntimes = nullptr;
	if (!ExistingRoot->TryGetArrayField(TEXT("runtimes"), ExistingRuntimes))
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Runtimes;
	for (const TSharedPtr<FJsonValue>& RuntimeValue : *ExistingRuntimes)
	{
		TSharedPtr<FJsonObject> RuntimeObject = RuntimeValue.IsValid()
			? RuntimeValue->AsObject()
			: nullptr;
		if (RuntimeObject.IsValid() && !RuntimeEntryMatchesCurrent(RuntimeObject, CurrentInstanceId, CurrentProjectPath))
		{
			Runtimes.Add(RuntimeValue);
		}
	}

	TSharedRef<FJsonObject> RegistryRoot = MakeShared<FJsonObject>();
	RegistryRoot->SetNumberField(TEXT("schemaVersion"), 1);
	RegistryRoot->SetStringField(TEXT("updatedAt"), FDateTime::UtcNow().ToIso8601());
	RegistryRoot->SetArrayField(TEXT("runtimes"), Runtimes);

	FString Serialized;
	if (SerializeCondensedJsonObject(RegistryRoot, Serialized))
	{
		WriteJsonAtomically(RegistryPath, Serialized);
	}
}

void FNeoStackAIModule::StartupModule()
{
	// Guard against running during cook/commandlet (module type is EditorNoCommandlet,
	// but this is defense-in-depth in case the module is loaded unexpectedly)
	if (IsRunningCommandlet())
	{
		return;
	}

	ConfigureEmbeddedBrowserPump();

	// Start MCP server to expose Unreal tools to AI agents
	{
		const UACPSettings* Settings = UACPSettings::Get();
		const int32 MCPPort = Settings ? FMath::Clamp(Settings->MCPServerPort, 1, 65535) : 9315;
		FMCPServer::Get().Start(MCPPort);
	}

	ProjectDiscoveryInstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	ProjectDiscoveryStartedAt = FDateTime::UtcNow();
	WriteProjectDiscoveryFile();
	ProjectDiscoveryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			WriteProjectDiscoveryFile();
			return true;
		}),
		5.0f);

	// Register settings detail customization (adds description text to categories)
	FACPSettingsCustomization::Register();

	// Register AI Assistant section in the Actor Details panel
	FAIDetailPanelExtension::Register();

	// Register the Agent Chat tab spawner (WebUI)
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		AgentChatTabName,
		FOnSpawnTab::CreateRaw(this, &FNeoStackAIModule::SpawnAgentChatTab))
		.SetDisplayName(LOCTEXT("AgentChatTabTitle", "Agent Chat"))
		.SetTooltipText(LOCTEXT("AgentChatTabTooltip", "Open the AI Agent Chat window"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"));

	RegisterStatusBarIntegration();

	// Register menus
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FNeoStackAIModule::RegisterMenus));

	// Register Content Browser context menu extension for Blueprints
	RegisterContentBrowserExtension();

	// Initialize usage monitor (polls Claude Code / Codex rate limit APIs)
	FAgentUsageMonitor::Get().Initialize();

	// Initialize ACP registry client (fetches agent catalog from CDN)
	FACPRegistryClient::Get().Initialize();

	// Reset extension inventory before any capabilities register against it.
	FNeoStackExtensionRegistry::Get().Reset();
	{
		FNeoStackExtensionDescriptor CoreDescriptor;
		CoreDescriptor.ExtensionId = TEXT("neostack.core");
		CoreDescriptor.DisplayName = TEXT("NeoStack AI Core");
		CoreDescriptor.ModuleName = TEXT("NeoStackAI");
		CoreDescriptor.Vendor = TEXT("Betide Studio");
		CoreDescriptor.Category = TEXT("core");
		CoreDescriptor.State = ENeoStackExtensionState::Active;
		CoreDescriptor.Tags = { TEXT("lua"), TEXT("tool"), TEXT("chat"), TEXT("provider") };
		FNeoStackExtensionRegistry::Get().RegisterOrUpdateExtension(CoreDescriptor);
	}

	// Scan Resources/Skills/ for the core plugin + every enabled NeoStack extension,
	// then materialise them into <ProjectDir>/.claude/skills and <ProjectDir>/.agents/skills
	// where Claude Code and Codex CLI pick them up through their own filesystem discovery.
	// User-edited skill files are preserved; upstream updates on top of user edits land
	// as SKILL.new.md siblings, surfaced in the WebUI Skills panel.
	FNeoStackSkillRegistry::Get().Refresh();
	FNeoStackSkillInstaller::SyncProject();

	// Register built-in chat providers and kick off initial model discovery.
	// Must run after settings are available (they load with the module) and
	// before WebUIBridge is used, since the bridge reads from the registry.
	FChatProviderRegistrar::RegisterBuiltIns();
	FChatModelRegistry::Get().Initialize();

	// Execute deferred generative provider registrations (MeshyProvider, FalProvider, etc.)
	// Must happen after module init so all static auto-regs have run.
	FDeferredProviderRegistration::Get().ExecuteAll();

	// Initialize anonymous analytics (opt-out via plugin settings)
	FNSAIAnalytics::Get().Initialize();

	// Initialize crash reporter (hooks crash delegates, detects previous crashes)
	FNSAICrashReporter::Get().Initialize();

	// Fire the per-launch entitlement check immediately. The result drives
	// whether tool dispatch, the relay client, and the update check actually
	// run. One-shot — result is cached for the editor session.
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			FEntitlementClient::Get().Check();
			return false;
		}),
		0.0f);

#if NEOSTACK_BUILD_VARIANT_BINARY
	// Binary subscriptions are revocable while the editor is open. Re-check
	// periodically so a canceled or unpaid subscription doesn't keep running
	// until the next editor restart.
	GEntitlementRefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			FEntitlementClient::Get().Refresh();
			return true;
		}),
		300.0f);
#endif

	// Check for plugin updates 5 seconds after startup (non-blocking, one-shot).
	// Defer through WhenResolved so a slow entitlement HTTP doesn't cause
	// the update check to be silently skipped for the whole session — a
	// one-shot IsEntitled() read at 5s would race with strict-mode binary
	// builds that haven't finished verifying yet.
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (GUpdateEntitlementResolvedHandle.IsValid())
			{
				FEntitlementClient::Get().RemoveWhenResolved(GUpdateEntitlementResolvedHandle);
				GUpdateEntitlementResolvedHandle.Reset();
			}
			GUpdateEntitlementResolvedHandle = FEntitlementClient::Get().WhenResolved([]()
			{
				GUpdateEntitlementResolvedHandle.Reset();
				if (!FEntitlementClient::Get().IsEntitled()) return;

				// Kick off the extension catalog fetch in parallel with the
				// version check. Both use the same neostack_* API key. The
				// catalog populates a module-static cache that
				// FNeoStackExtensionInstaller::ComputePendingUpdates reads
				// later — when CheckForPluginUpdate's resolve step runs (a
				// second HTTP round-trip), the catalog is almost always
				// already populated, so the unified-updater path can fold
				// extension updates into the same notification.
				if (const UACPSettings* Settings = UACPSettings::Get())
				{
					if (Settings->bAlsoUpdateExtensions)
					{
						const FString Channel = Settings->bUseBetaChannel ? TEXT("beta") : TEXT("stable");
						const FString EngineVersion = FString::Printf(
							TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
						FNeoStackExtensionCatalog::RefreshAsync(Channel, EngineVersion);
					}
				}

				CheckForPluginUpdate();
			});
			return false; // One-shot, don't repeat
		}),
		5.0f);

	// Initialize headless agent service (message persistence, agent lifecycle, session discovery)
	FAgentService::Get().Initialize();

	// Initialize relay client for remote access (connects to relay if enabled in settings)
	FRelayClient::Get().Initialize();

	// Initialize local client for IDE connection (discovers IDE via ~/.neostack/server.json)
	FLocalClient::Get().Initialize();
}

void FNeoStackAIModule::ShutdownModule()
{
	// If startup was skipped (commandlet mode), nothing to tear down
	if (IsRunningCommandlet())
	{
		return;
	}

	// CEF IME: FCEFImeHandler::UnbindCefBrowser() calls DestroyContext() but does not clear
	// TextInputMethodSystem. If BindInputMethodSystem ran and we skip UnbindInputMethodSystem()
	// (e.g. SWebBrowser widget already destroyed so weak ptr fails), OnBrowserClosed still
	// touches Slate IME and can AV during WebBrowser module shutdown (Windows CEF).
	UnbindWebUIInputMethodSystem();

	// Clean up update notification
	if (GUpdateEntitlementResolvedHandle.IsValid())
	{
		FEntitlementClient::Get().RemoveWhenResolved(GUpdateEntitlementResolvedHandle);
		GUpdateEntitlementResolvedHandle.Reset();
	}
	if (GEntitlementRefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GEntitlementRefreshTickerHandle);
		GEntitlementRefreshTickerHandle.Reset();
	}
	StopProgressTicker();
	if (GUpdateNotification.IsValid())
	{
		GUpdateNotification->ExpireAndFadeout();
		GUpdateNotification.Reset();
	}

	// Unregister AI Detail panel extension
	FAIDetailPanelExtension::Unregister();

	// Unregister settings detail customization
	FACPSettingsCustomization::Unregister();

	// Stop WebUI file server
	StopWebUIFileServer();

	if (ProjectDiscoveryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ProjectDiscoveryTickerHandle);
		ProjectDiscoveryTickerHandle.Reset();
	}
	RemoveProjectDiscoveryFile();

	// Shutdown crash reporter (unregisters delegates)
	FNSAICrashReporter::Get().Shutdown();

	// Shutdown analytics (flushes remaining events)
	FNSAIAnalytics::Get().Shutdown();

	// Shutdown usage monitor and registry client
	FAgentUsageMonitor::Get().Shutdown();
	FACPRegistryClient::Get().Shutdown();

	// Shutdown chat provider registry (clears provider instances and cancels discovery)
	FChatModelRegistry::Get().Shutdown();

	// Close all terminal sessions
	FTerminalManager::Get().CloseAll();

	// Shutdown local client (IDE connection)
	FLocalClient::Get().Shutdown();

	// Shutdown relay client (remote access)
	FRelayClient::Get().Shutdown();

	// Shutdown agent service
	FAgentService::Get().Shutdown();

	// Disconnect ACP subprocesses during module shutdown while the module and logging
	// are still alive. Leaving this to FACPAgentManager's static destructor can run
	// after LogExit and hang editor teardown on a blocked ACP reader thread.
	FACPAgentManager::Get().DisconnectAll();

	// Stop MCP server
	FMCPServer::Get().Stop();

	// Unregister Content Browser extender
	if (ContentBrowserExtenderHandle.IsValid())
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuExtenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
		CBMenuExtenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
		{
			return Delegate.GetHandle() == ContentBrowserExtenderHandle;
		});
		ContentBrowserExtenderHandle.Reset();
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	UnregisterStatusBarIntegration();

	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AgentChatTabName);

	if (InputMethodSystemSlatePreShutdownDelegateHandle.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreShutdown().Remove(InputMethodSystemSlatePreShutdownDelegateHandle);
		}
		InputMethodSystemSlatePreShutdownDelegateHandle.Reset();
	}

	// Clean up bridge — during editor exit, GC may have already torn down the UObject
	// subsystem, so IsValid()/RemoveFromRoot() can crash in FUObjectArray::IndexToObject.
	// Only touch the pointer if GC is still operational.
	// Clean up browser window — unlink from dock tab before destroying
	if (WebUIBrowserWindow.IsValid())
	{
		WebUIBrowserWindow->OnUnhandledKeyDown().Unbind();
		WebUIBrowserWindow->OnUnhandledKeyUp().Unbind();
#if ENGINE_MINOR_VERSION == 7
		WebUIBrowserWindow->SetParentDockTab(nullptr);
#endif
		WebUIBrowserWindow.Reset();
	}
	WebUIBrowserWidget.Reset();

	if (WebUIBridgeInstance && !GExitPurge)
	{
		if (IsValid(WebUIBridgeInstance))
		{
			WebUIBridgeInstance->RemoveFromRoot();
		}
		WebUIBridgeInstance = nullptr;
	}

	// Close the chat history SQLite handle while SQLiteCore is still loaded.
	// Must run after WebUIBridge teardown — the bridge's JS handlers are the last
	// path that can write to the store. The Meyers singleton's own destructor
	// runs at C++ static teardown (after SQLiteCore is gone), and FSQLiteDatabase
	// asserts in its destructor if Close() was never called.
	FNeoStackChatStore::Get().Shutdown();
}

FString FNeoStackAIModule::GetProjectDiscoveryFilePath()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("NeoStackAI"),
		TEXT("runtime.json"));
}

void FNeoStackAIModule::WriteProjectDiscoveryFile()
{
	const FString DiscoveryPath = GetProjectDiscoveryFilePath();
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	const FString ProjectName = FApp::GetProjectName();
	const int32 EditorPid = FPlatformProcess::GetCurrentProcessId();
	const FDateTime HeartbeatAt = FDateTime::UtcNow();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 2);
	Root->SetStringField(TEXT("instanceId"), ProjectDiscoveryInstanceId);
	Root->SetNumberField(TEXT("editorPid"), EditorPid);
	Root->SetStringField(TEXT("projectName"), ProjectName);
	Root->SetStringField(TEXT("projectPath"), ProjectDir);
	Root->SetStringField(TEXT("uprojectPath"), ProjectFilePath);
	Root->SetStringField(TEXT("pluginVersion"), TEXT("1.0.0"));
	Root->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Root->SetStringField(TEXT("startedAt"), ProjectDiscoveryStartedAt.ToIso8601());
	Root->SetStringField(TEXT("lastHeartbeatAt"), HeartbeatAt.ToIso8601());
	Root->SetBoolField(TEXT("mcpRunning"), FMCPServer::Get().IsRunning());

	TArray<TSharedPtr<FJsonValue>> McpServers;
	if (FMCPServer::Get().IsRunning())
	{
		TSharedPtr<FJsonObject> UnrealMcp = MakeShared<FJsonObject>();
		UnrealMcp->SetStringField(TEXT("name"), TEXT("unreal-editor"));
		UnrealMcp->SetStringField(TEXT("type"), TEXT("http"));
		UnrealMcp->SetStringField(
			TEXT("url"),
			FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), FMCPServer::Get().GetPort()));

		TArray<TSharedPtr<FJsonValue>> EmptyHeaders;
		UnrealMcp->SetArrayField(TEXT("headers"), EmptyHeaders);
		McpServers.Add(MakeShared<FJsonValueObject>(UnrealMcp));
	}
	Root->SetArrayField(TEXT("mcpServers"), McpServers);

	// IDE connection state
	Root->SetBoolField(TEXT("ideConnected"), FLocalClient::Get().IsConnected());

	FString Serialized;
	if (!SerializeCondensedJsonObject(Root.ToSharedRef(), Serialized))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("NSAI Discovery: Failed to serialize runtime file"));
		return;
	}

	if (!WriteJsonAtomically(DiscoveryPath, Serialized))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("NSAI Discovery: Failed to write %s"), *DiscoveryPath);
		return;
	}

	WriteGlobalDiscoveryFile(Root.ToSharedRef());

	UE_LOG(LogNeoStackAI, Verbose, TEXT("NSAI Discovery: Updated %s"), *DiscoveryPath);
}

void FNeoStackAIModule::RemoveProjectDiscoveryFile()
{
	const FString DiscoveryPath = GetProjectDiscoveryFilePath();
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	RemoveGlobalDiscoveryEntry(ProjectDiscoveryInstanceId, ProjectDir);
	IFileManager::Get().Delete(*DiscoveryPath, false, true);
}

void FNeoStackAIModule::RegisterStatusBarIntegration()
{
	FModuleManager::Get().LoadModuleChecked(TEXT("StatusBar"));

	const TSharedPtr<IPlugin> AIAssistantPlugin = IPluginManager::Get().FindPlugin(TEXT("AIAssistant"));
	const bool bAIAssistantEnabled = AIAssistantPlugin.IsValid() && AIAssistantPlugin->IsEnabled();

	if (bAIAssistantEnabled)
	{
		// Epic's status bar contributes the "Ask AI" button for this tab id. Keep
		// that button, but route the tab itself to NeoStack's embedded WebUI.
		FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("AIAssistant"));
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(EpicAIAssistantTabName);
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			EpicAIAssistantTabName,
			FOnSpawnTab::CreateRaw(this, &FNeoStackAIModule::SpawnAgentChatTab))
			.SetDisplayName(LOCTEXT("NeoStackAskAITabTitle", "AI Assistant"))
			.SetTooltipText(LOCTEXT("NeoStackAskAITabTooltip", "Open NeoStack AI Agent Chat"))
			.SetMenuType(ETabSpawnerMenuType::Hidden)
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"));

		bRegisteredEpicAIAssistantTabOverride = true;
		UE_LOG(LogNeoStackAI, Log, TEXT("NeoStackAI: routed Epic AI Assistant status-bar entry to NeoStack WebUI"));
		return;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (GEditor)
	{
		if (UStatusBarSubsystem* StatusBarSubsystem = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
		{
			PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
			StatusBarPanelDrawerSummonHandle = StatusBarSubsystem->RegisterPanelDrawerSummon(
				UStatusBarSubsystem::FRegisterPanelDrawerSummonDelegate::FDelegate::CreateLambda(
					[](TArray<UStatusBarSubsystem::FTabIdAndButtonLabel>& OutTabIdsAndLabels, const TSharedRef<SDockTab>&)
					{
						OutTabIdsAndLabels.Emplace(FNeoStackAIModule::AgentChatTabName, LOCTEXT("StatusBarSummonNeoStack", "NeoStack"));
					}));
			PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
		}
	}
#endif
}

void FNeoStackAIModule::UnregisterStatusBarIntegration()
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (StatusBarPanelDrawerSummonHandle.IsValid() && GEditor)
	{
		if (UStatusBarSubsystem* StatusBarSubsystem = GEditor->GetEditorSubsystem<UStatusBarSubsystem>())
		{
			PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
			StatusBarSubsystem->UnregisterPanelDrawerSummon(StatusBarPanelDrawerSummonHandle);
			PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
		}
		StatusBarPanelDrawerSummonHandle.Reset();
	}
#endif

	if (bRegisteredEpicAIAssistantTabOverride)
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(EpicAIAssistantTabName);
		bRegisteredEpicAIAssistantTabOverride = false;
	}
}

void FNeoStackAIModule::ConfigureEmbeddedBrowserPump()
{
	if (!GConfig)
	{
		return;
	}

	bool bForceMessageLoop = false;
	GConfig->GetBool(TEXT("Browser"), TEXT("bForceMessageLoop"), bForceMessageLoop, GEngineIni);
	if (!bForceMessageLoop)
	{
		GConfig->SetBool(TEXT("Browser"), TEXT("bForceMessageLoop"), true, GEngineIni);
	}

	int32 MinMessageLoopHertz = 1;
	GConfig->GetInt(TEXT("Browser"), TEXT("MinMessageLoopHertz"), MinMessageLoopHertz, GEngineIni);
	if (MinMessageLoopHertz < 30)
	{
		GConfig->SetInt(TEXT("Browser"), TEXT("MinMessageLoopHertz"), 30, GEngineIni);
	}

	int32 MaxForcedMessageLoopHertz = 15;
	GConfig->GetInt(TEXT("Browser"), TEXT("MaxForcedMessageLoopHertz"), MaxForcedMessageLoopHertz, GEngineIni);
	if (MaxForcedMessageLoopHertz < 60)
	{
		GConfig->SetInt(TEXT("Browser"), TEXT("MaxForcedMessageLoopHertz"), 60, GEngineIni);
	}
}

void FNeoStackAIModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	// Add to Window menu
	UToolMenu* WindowMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& WindowSection = WindowMenu->FindOrAddSection("AI");
	WindowSection.AddMenuEntry(
		"OpenAgentChat",
		LOCTEXT("OpenAgentChatMenuLabel", "Agent Chat"),
		LOCTEXT("OpenAgentChatMenuTooltip", "Open the AI Agent Chat window"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"),
		FUIAction(FExecuteAction::CreateRaw(this, &FNeoStackAIModule::OpenAgentChatWindow))
	);

	// Register node context menu extension
	RegisterNodeContextMenuExtension();
}

void FNeoStackAIModule::RegisterNodeContextMenuExtension()
{
	// Extend the base EdGraphNode context menu
	// This will add our "Select as Context" option to ALL graph nodes
	UToolMenu* NodeMenu = UToolMenus::Get()->ExtendMenu("GraphEditor.GraphNodeContextMenu.EdGraphNode");
	if (NodeMenu)
	{
		NodeMenu->AddDynamicSection(
			"NeoStackAI",
			FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				UGraphNodeContextMenuContext* Context = InMenu->FindContext<UGraphNodeContextMenuContext>();
				if (!Context || !Context->Node)
				{
					return;
				}

				FToolMenuSection& AISection = InMenu->FindOrAddSection("NeoStackAI");
				AISection.Label = LOCTEXT("AIContextSection", "AI Context");

				// Capture node pointer for the action
				const UEdGraphNode* Node = Context->Node;

				AISection.AddMenuEntry(
					"SelectAsContext",
					LOCTEXT("SelectAsContext", "Select as Context"),
					LOCTEXT("SelectAsContextTooltip", "Attach this node's data as context for the AI agent"),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"),
					FUIAction(
						FExecuteAction::CreateLambda([Node]()
						{
							if (Node)
							{
								FACPAttachmentManager::Get().AddNodeFromGraph(Node);
							}
						})
					)
				);
			})
		);
	}
}

void FNeoStackAIModule::RegisterContentBrowserExtension()
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	TArray<FContentBrowserMenuExtender_SelectedAssets>& CBMenuExtenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();

	CBMenuExtenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(
		this, &FNeoStackAIModule::OnExtendContentBrowserAssetMenu));

	ContentBrowserExtenderHandle = CBMenuExtenders.Last().GetHandle();
}

TSharedRef<FExtender> FNeoStackAIModule::OnExtendContentBrowserAssetMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	// Check if any selected assets are Blueprints
	bool bHasBlueprint = false;
	for (const FAssetData& Asset : SelectedAssets)
	{
		if (Asset.AssetClassPath.GetAssetName() == UBlueprint::StaticClass()->GetFName() ||
			Asset.AssetClassPath.GetAssetName().ToString().Contains(TEXT("Blueprint")))
		{
			bHasBlueprint = true;
			break;
		}
	}

	if (bHasBlueprint)
	{
		Extender->AddMenuExtension(
			"CommonAssetActions",
			EExtensionHook::After,
			nullptr,
			FMenuExtensionDelegate::CreateLambda([SelectedAssets](FMenuBuilder& MenuBuilder)
			{
				MenuBuilder.BeginSection("AIContext", LOCTEXT("AIContextSection", "AI Context"));
				MenuBuilder.AddMenuEntry(
					LOCTEXT("SelectBPAsContext", "Select as AI Context"),
					LOCTEXT("SelectBPAsContextTooltip", "Attach this blueprint's structure as context for the AI agent"),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"),
					FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
					{
						for (const FAssetData& Asset : SelectedAssets)
						{
							UObject* LoadedAsset = Asset.GetAsset();
							if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
							{
								FACPAttachmentManager::Get().AddBlueprintAsset(Blueprint);
							}
						}
					}))
				);
				MenuBuilder.EndSection();
			})
		);
	}

	return Extender;
}

// ============================================
// Update notification helpers
// ============================================

static FString GetCurrentPluginVersion()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown");
}

static void StopProgressTicker()
{
	if (GProgressTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GProgressTickerHandle);
		GProgressTickerHandle.Reset();
	}
}

static bool TickUpdateProgress(float)
{
	if (!GUpdateNotification.IsValid())
	{
		StopProgressTicker();
		return false;
	}

	auto& Info = FNeoStackAIModule::GetUpdateInfoMutable();

	switch (Info.State)
	{
	case EPluginUpdateState::Downloading:
	{
		int32 Pct = FMath::RoundToInt(Info.DownloadProgress * 100.0f);
		GUpdateNotification->SetSubText(FText::FromString(
			FString::Printf(TEXT("Downloading v%s... %d%%"), *Info.LatestVersion, Pct)));
		return true; // Keep ticking
	}
	case EPluginUpdateState::Downloaded:
		GUpdateNotification->SetText(FText::FromString(TEXT("NeoStack AI Update")));
		GUpdateNotification->SetSubText(FText::FromString(
			FString::Printf(TEXT("v%s ready to install | Requires editor restart"), *Info.LatestVersion)));
		GUpdateNotification->SetCompletionState(SNotificationItem::CS_Success);
		return false; // Stop ticking
	case EPluginUpdateState::Failed:
		GUpdateNotification->SetText(FText::FromString(TEXT("NeoStack AI Update")));
		GUpdateNotification->SetSubText(FText::FromString(Info.ErrorMessage));
		GUpdateNotification->SetCompletionState(SNotificationItem::CS_Fail);
		return false;
	default:
		return false;
	}
}

// ────────────────────────────────────────────────────────────────
// NeoStack Cloud resolution helpers
//
// Mirrors the pattern used by FNeoStackExtensionCatalog / installer so
// the core update check reads from the same credential source and
// honours any user-set base-URL override (Settings > Chat & Agents > Chat Providers >
// NeoStack Cloud). The update worker expects the same neostack_* key
// used by the NeoStack Cloud provider row.
//
// Must be declared before ShowUpdateNotification / CheckForPluginUpdate
// since both resolve their API key through these helpers.
// ────────────────────────────────────────────────────────────────
namespace
{
	FString ResolveCloudBaseUrl()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
			if (!Override.IsEmpty())
			{
				// The AI provider's base URL ends with /v1; strip it so we're
				// at the root for other /api/* routes.
				FString Root = Override;
				Root.RemoveFromEnd(TEXT("/"));
				Root.RemoveFromEnd(TEXT("/v1"));
				Root.RemoveFromEnd(TEXT("/"));
				return Root;
			}
		}
		return TEXT("https://neostack.dev");
	}

	FString ResolveCloudApiKey()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			return Settings->GetChatProviderApiKey(TEXT("neostack"));
		}
		return FString();
	}

	// Core's slug on neostack.dev. Extensions derive theirs via slugify()
	// from pluginName; core's is stable.
	const TCHAR* CorePluginSlug = TEXT("neo-stack-ai");

	FString BuildExtensionUpdateSummary(const TArray<FNeoStackExtensionUpdateCandidate>& Pending)
	{
		if (Pending.Num() == 0)
		{
			return FString();
		}

		FString Summary = FString::Printf(TEXT("%d extension update%s"),
			Pending.Num(), Pending.Num() == 1 ? TEXT("") : TEXT("s"));

		// Keep the native Slate notification small. Long extension lists made
		// the toast grow into an editor-blocking overlay on UE 5.6.
		const int32 NamesToShow = FMath::Min(Pending.Num(), 3);
		if (NamesToShow > 0)
		{
			Summary += TEXT(": ");
			for (int32 i = 0; i < NamesToShow; ++i)
			{
				if (i > 0)
				{
					Summary += TEXT(", ");
				}
				Summary += Pending[i].DisplayName;
			}
			if (Pending.Num() > NamesToShow)
			{
				Summary += FString::Printf(TEXT(" + %d more"), Pending.Num() - NamesToShow);
			}
		}

		return Summary;
	}

	FString BuildUpdateDetailsText()
	{
		const FPluginUpdateInfo& Info = FNeoStackAIModule::GetUpdateInfo();
		const FString CurrentVersion = GetCurrentPluginVersion();
		const bool bCoreUpdate = Info.bUpdateAvailable && !Info.LatestVersion.IsEmpty();

		FString Details = TEXT("NeoStack AI Update\n\n");
		if (bCoreUpdate)
		{
			Details += FString::Printf(TEXT("Core: v%s -> v%s\n"),
				*CurrentVersion, *Info.LatestVersion);
		}
		else
		{
			Details += FString::Printf(TEXT("Core: v%s is current\n"), *CurrentVersion);
		}

		if (Info.PendingExtensionUpdates.Num() > 0)
		{
			Details += FString::Printf(TEXT("\nExtension updates (%d):\n"),
				Info.PendingExtensionUpdates.Num());
			for (const FNeoStackExtensionUpdateCandidate& Candidate : Info.PendingExtensionUpdates)
			{
				Details += FString::Printf(TEXT("- %s"), *Candidate.DisplayName);
				if (!Candidate.InstalledVersion.IsEmpty() || !Candidate.LatestVersion.IsEmpty())
				{
					Details += FString::Printf(TEXT(" (%s -> %s)"),
						Candidate.InstalledVersion.IsEmpty() ? TEXT("?") : *Candidate.InstalledVersion,
						Candidate.LatestVersion.IsEmpty() ? TEXT("?") : *Candidate.LatestVersion);
				}
				Details += TEXT("\n");
			}
		}

		if (!Info.Changelog.IsEmpty())
		{
			Details += TEXT("\nRelease notes:\n");
			Details += Info.Changelog;
		}

		if (Info.Changelog.IsEmpty() && Info.PendingExtensionUpdates.Num() == 0)
		{
			Details += TEXT("\nNo release notes were provided by the update service.");
		}

		return Details;
	}

	void ShowUpdateDetailsDialog()
	{
		FMessageDialog::Open(
			EAppMsgType::Ok,
			FText::FromString(BuildUpdateDetailsText()),
			FText::FromString(TEXT("NeoStack AI Update Details")));
	}
}

static void ShowUpdateNotification()
{
	// Expire any existing notification
	if (GUpdateNotification.IsValid())
	{
		GUpdateNotification->ExpireAndFadeout();
		GUpdateNotification.Reset();
	}
	StopProgressTicker();

	const auto& Info = FNeoStackAIModule::GetUpdateInfo();
	const FString CurrentVersion = GetCurrentPluginVersion();
	const int32 ExtCount = Info.PendingExtensionUpdates.Num();

	// Three notification shapes:
	//   - core only                 → "v0.5.5 → v0.5.6"
	//   - core + extensions         → "v0.5.5 → v0.5.6 + N extension updates"
	//   - extensions only (no core) → "N extension updates available"
	const bool bCoreUpdate = Info.bUpdateAvailable && !Info.LatestVersion.IsEmpty();

	FNotificationInfo NotifInfo(FText::FromString(TEXT("NeoStack AI Update")));
	NotifInfo.bFireAndForget = false;
	NotifInfo.bUseSuccessFailIcons = true;
	NotifInfo.bUseLargeFont = true;
	NotifInfo.WidthOverride = 420.0f;

	FString SummaryText;
	if (bCoreUpdate)
	{
		SummaryText = FString::Printf(TEXT("Core update: v%s -> v%s"), *CurrentVersion, *Info.LatestVersion);
		if (ExtCount > 0)
		{
			SummaryText += FString::Printf(TEXT(" plus %d extension%s"),
				ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"));
		}
	}
	else if (ExtCount > 0)
	{
		SummaryText = BuildExtensionUpdateSummary(Info.PendingExtensionUpdates);
	}
	else
	{
		SummaryText = FString::Printf(TEXT("Current version: v%s"), *CurrentVersion);
	}

	if (!Info.Changelog.IsEmpty() || ExtCount > 0)
	{
		SummaryText += TEXT("\nUse Details to view release notes.");
	}
	NotifInfo.SubText = FText::FromString(SummaryText);

	if (!Info.Changelog.IsEmpty() || ExtCount > 0)
	{
		FNotificationButtonInfo DetailsBtn(
			FText::FromString(TEXT("Details")),
			FText::FromString(TEXT("View release notes and extension update details")),
			FSimpleDelegate::CreateStatic(&ShowUpdateDetailsDialog),
			SNotificationItem::CS_None);
		DetailsBtn.VisibilityOnSuccess = EVisibility::Visible;
		DetailsBtn.VisibilityOnFail = EVisibility::Visible;
		NotifInfo.ButtonDetails.Add(DetailsBtn);
	}

	// Read the same neostack_* key the update check + download flow uses.
	const FString NeoStackKey = ResolveCloudApiKey();

	if (Info.bDownloadAvailable && !NeoStackKey.IsEmpty())
	{
		// Download button — visible in initial state (CS_None).
		// When extensions are also pending, the button label hints at the
		// bundle so the user isn't surprised when extension updates also
		// land after they click.
		FNotificationButtonInfo DownloadBtn(
			FText::FromString(TEXT("Download")),
			FText::FromString(ExtCount > 0
				? FString::Printf(TEXT("Download v%s and %d extension update%s"),
					*Info.LatestVersion, ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"))
				: FString::Printf(TEXT("Download v%s"), *Info.LatestVersion)),
			FSimpleDelegate::CreateLambda([LatestVersion = Info.LatestVersion]()
			{
				FNeoStackAIModule::DownloadUpdate();
				if (GUpdateNotification.IsValid())
				{
					GUpdateNotification->SetCompletionState(SNotificationItem::CS_Pending);
					GUpdateNotification->SetSubText(FText::FromString(
						FString::Printf(TEXT("Downloading v%s... 0%%"), *LatestVersion)));
					// Start progress ticker (update every 0.25s)
					StopProgressTicker();
					GProgressTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateStatic(&TickUpdateProgress), 0.25f);
				}
			}),
			SNotificationItem::CS_None);
		NotifInfo.ButtonDetails.Add(DownloadBtn);

		// Install button — visible after download completes (CS_Success).
		// Route through the unified path so any pending extension updates
		// piggyback on the same editor-restart cycle as the core swap.
		// InstallUpdateUnified itself falls back to the core-only path
		// when no extensions are pending.
		FNotificationButtonInfo InstallBtn(
			FText::FromString(TEXT("Install")),
			FText::FromString(ExtCount > 0
				? FString::Printf(TEXT("Install v%s and %d extension update%s (requires editor restart)"),
					*Info.LatestVersion, ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"))
				: FString::Printf(TEXT("Install v%s (requires editor restart)"), *Info.LatestVersion)),
			FSimpleDelegate::CreateStatic(&FNeoStackAIModule::InstallUpdateUnified),
			SNotificationItem::CS_Success);
		NotifInfo.ButtonDetails.Add(InstallBtn);
	}
	else if (Info.bDownloadAvailable)
	{
		// File exists for this platform but no API key configured — direct
		// users to the Settings surface where the neostack_* key is pasted.
		NotifInfo.SubText = FText::FromString(FString::Printf(
			TEXT("Core update: v%s -> v%s\nAdd a NeoStack Cloud API key to enable download."),
			*CurrentVersion, *Info.LatestVersion));
		NotifInfo.HyperlinkText = FText::FromString(TEXT("Open NeoStack.dev"));
		NotifInfo.Hyperlink = FSimpleDelegate::CreateLambda([]()
		{
			FPlatformProcess::LaunchURL(TEXT("https://neostack.dev/dashboard/tokens"), nullptr, nullptr);
		});
	}
	else if (ExtCount > 0 && !NeoStackKey.IsEmpty())
	{
		// Extensions-only path: core is current but at least one extension
		// is out of date. The unified install path handles this — no core
		// download required, just queue Update ops on the extension
		// installer and launch the combined script with no core payload.
		FNotificationButtonInfo UpdateBtn(
			FText::FromString(TEXT("Update")),
			FText::FromString(FString::Printf(TEXT("Install %d extension update%s (requires editor restart)"),
				ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"))),
			FSimpleDelegate::CreateStatic(&FNeoStackAIModule::InstallUpdateUnified),
			SNotificationItem::CS_None);
		NotifInfo.ButtonDetails.Add(UpdateBtn);
	}
	else
	{
		// No downloadable file for this platform
		NotifInfo.SubText = FText::FromString(FString::Printf(
			TEXT("Core update: v%s -> v%s\nNo auto-update package is available for this platform yet."),
			*CurrentVersion, *Info.LatestVersion));
		NotifInfo.HyperlinkText = FText::FromString(TEXT("Open NeoStack.dev"));
		NotifInfo.Hyperlink = FSimpleDelegate::CreateLambda([]()
		{
			FPlatformProcess::LaunchURL(TEXT("https://neostack.dev"), nullptr, nullptr);
		});
	}

	// Dismiss button — visible in initial, success, and fail states. Keep the
	// label short so it remains reachable in narrow editor layouts.
	FNotificationButtonInfo DismissBtn(
		FText::FromString(TEXT("Dismiss")),
		FText::GetEmpty(),
		FSimpleDelegate::CreateLambda([]()
		{
			FNeoStackAIModule::GetUpdateInfoMutable().bDismissed = true;
			if (GUpdateNotification.IsValid())
			{
				GUpdateNotification->ExpireAndFadeout();
				GUpdateNotification.Reset();
			}
			StopProgressTicker();
		}),
		SNotificationItem::CS_None);
	DismissBtn.VisibilityOnFail = EVisibility::Visible;
	DismissBtn.VisibilityOnSuccess = EVisibility::Visible;
	NotifInfo.ButtonDetails.Add(DismissBtn);

	GUpdateNotification = FSlateNotificationManager::Get().AddNotification(NotifInfo);
	if (GUpdateNotification.IsValid())
	{
		GUpdateNotification->SetCompletionState(SNotificationItem::CS_None);
	}
}

// ============================================
// Update check / download / install
// ============================================

// Second leg of the update check: with an available build confirmed by
// /version, mint a signed download URL via /api/external/download. The
// response carries the (short-lived, HMAC-signed) direct-to-R2 URL plus
// checksum/filename/size that DownloadUpdate() will stream from.
static void ResolveDownloadAndNotify(FPluginUpdateInfo InInfo, const FString& EngineVersion, const FString& Platform, const FString& Channel)
{
	const FString ApiKey = ResolveCloudApiKey();
	// Caller already checked this path requires a key; defensive early-out.
	// GetUpdateInfoMutable() is the accessor for the private static on
	// FNeoStackAIModule — this function is a free function so it can't
	// touch the member directly.
	if (ApiKey.IsEmpty())
	{
		AsyncTask(ENamedThreads::GameThread, [InInfo]()
		{
			FNeoStackAIModule::GetUpdateInfoMutable() = InInfo;
			ShowUpdateNotification();
		});
		return;
	}

	const FString BaseUrl = ResolveCloudBaseUrl();
	// Match the variant the user is currently running. The compile-time
	// macro in BuildVariant.h is baked into the DLL by the release
	// workflow's binary pass, so this answer is authoritative and not
	// editable post-build. Hardcoding `binary` here would silently
	// migrate lifetime owners onto the source-stripped build on the next
	// update.
#if NEOSTACK_BUILD_VARIANT_BINARY
	const TCHAR* SelfVariant = TEXT("binary");
#else
	const TCHAR* SelfVariant = TEXT("full");
#endif
	const FString Url = FString::Printf(
		TEXT("%s/api/external/download?slug=%s&engine=%s&platform=%s&variant=%s&channel=%s"),
		*BaseUrl,
		CorePluginSlug,
		*FGenericPlatformHttp::UrlEncode(EngineVersion),
		*FGenericPlatformHttp::UrlEncode(Platform),
		SelfVariant,
		*FGenericPlatformHttp::UrlEncode(Channel));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	Request->OnProcessRequestComplete().BindLambda(
		[InInfo, Url, EngineVersion, Channel](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
		{
			FPluginUpdateInfo Info = InInfo;

			const int32 Code = (bConnected && Response.IsValid()) ? Response->GetResponseCode() : 0;
			if (!bConnected || !Response.IsValid() || Code != 200)
			{
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("[NSAI] Update download resolve failed (HTTP %d): %s"),
					Code, *Url);
				// Update exists but we can't hand the user a download URL right now
				// (token rejected, build missing for platform, worker down). Fall
				// through to the notification so they at least see version info.
				Info.bDownloadAvailable = false;
				AsyncTask(ENamedThreads::GameThread, [Info]()
				{
					FNeoStackAIModule::GetUpdateInfoMutable() = Info;
					ShowUpdateNotification();
				});
				return;
			}

			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("[NSAI] Update download resolve: malformed JSON from %s"), *Url);
				Info.bDownloadAvailable = false;
				AsyncTask(ENamedThreads::GameThread, [Info]()
				{
					FNeoStackAIModule::GetUpdateInfoMutable() = Info;
					ShowUpdateNotification();
				});
				return;
			}

			Json->TryGetStringField(TEXT("url"), Info.DownloadUrl);
			Json->TryGetStringField(TEXT("fileName"), Info.FileName);
			Json->TryGetStringField(TEXT("checksum"), Info.Checksum);
			double SizeD = 0;
			if (Json->TryGetNumberField(TEXT("fileSize"), SizeD))
			{
				Info.FileSize = static_cast<int64>(SizeD);
			}
			Info.bDownloadAvailable = !Info.DownloadUrl.IsEmpty();

			AsyncTask(ENamedThreads::GameThread, [Info, EngineVersion, Channel]() mutable
			{
				// Fold any out-of-date extensions into the same notification.
				// Honors bAlsoUpdateExtensions: when off, the user gets the
				// historical core-only notification copy. ComputePendingUpdates
				// returns empty if the catalog isn't Ready yet (e.g. its
				// parallel fetch lost the race), so the unified flow falls
				// back to core-only without blocking.
				const UACPSettings* SettingsLocal = UACPSettings::Get();
				if (SettingsLocal && SettingsLocal->bAlsoUpdateExtensions)
				{
					Info.PendingExtensionUpdates =
						FNeoStackExtensionInstaller::ComputePendingUpdates(Channel, EngineVersion);
				}
				Info.CheckedChannel = Channel;

				FNeoStackAIModule::GetUpdateInfoMutable() = Info;
				UE_LOG(LogNeoStackAI, Log,
					TEXT("[NSAI] Update ready: v%s (%lld bytes) — %s (extensions: %d)"),
					*Info.LatestVersion, Info.FileSize, *Info.FileName,
					Info.PendingExtensionUpdates.Num());
				ShowUpdateNotification();

				// Auto-download if the user has opted in.
				const UACPSettings* Settings = UACPSettings::Get();
				if (Settings && Settings->bAutoDownloadUpdates && Info.bDownloadAvailable)
				{
					FNeoStackAIModule::DownloadUpdate();
					if (GUpdateNotification.IsValid())
					{
						GUpdateNotification->SetCompletionState(SNotificationItem::CS_Pending);
						GUpdateNotification->SetSubText(FText::FromString(
							FString::Printf(TEXT("Downloading v%s... 0%%"), *Info.LatestVersion)));
						StopProgressTicker();
						GProgressTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
							FTickerDelegate::CreateStatic(&TickUpdateProgress), 0.25f);
					}
				}
			});
		});

	Request->ProcessRequest();
}

void FNeoStackAIModule::CheckForPluginUpdate()
{
	// Reset so any stale notification / banner hides while we re-check.
	CachedUpdateInfo = FPluginUpdateInfo();
	CachedUpdateInfo.State = EPluginUpdateState::Checking;

	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings->bCheckForUpdates)
	{
		CachedUpdateInfo.State = EPluginUpdateState::None;
		return;
	}

	// Version from .uplugin descriptor — server compares against this.
	FString CurrentVersion = TEXT("0.1");
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	if (Plugin.IsValid())
	{
		CurrentVersion = Plugin->GetDescriptor().VersionName;
	}

	const FString EngineVersion = FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	FString Platform = TEXT("Win64");
#if PLATFORM_MAC
	Platform = TEXT("Mac");
#endif
	// bUseBetaChannel remains a bool in settings today. Map to the server's
	// 4-channel hierarchy: opting-in upgrades to 'beta' (which also includes
	// stable). Full channel choice (dev/alpha) is a future settings addition.
	const FString Channel = Settings->bUseBetaChannel ? TEXT("beta") : TEXT("stable");

	const FString BaseUrl = ResolveCloudBaseUrl();
	const FString Url = FString::Printf(
		TEXT("%s/api/plugins/%s/version?current=%s&engine=%s&platform=%s&channel=%s"),
		*BaseUrl,
		CorePluginSlug,
		*FGenericPlatformHttp::UrlEncode(CurrentVersion),
		*FGenericPlatformHttp::UrlEncode(EngineVersion),
		*FGenericPlatformHttp::UrlEncode(Platform),
		*FGenericPlatformHttp::UrlEncode(Channel));

	// Step 1 — public /version endpoint. No auth needed; just version metadata.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->OnProcessRequestComplete().BindLambda(
		[CurrentVersion, EngineVersion, Platform, Channel, Url]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			const int32 Code = (bConnectedSuccessfully && Response.IsValid()) ? Response->GetResponseCode() : 0;
			if (!bConnectedSuccessfully || !Response.IsValid() || Code != 200)
			{
				// Log the failure so a broken update endpoint doesn't stay
				// invisible — prior revision silently set state=None and
				// returned on 404, which was the whole bug we're fixing.
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("[NSAI] Update check failed (HTTP %d): %s"),
					Code, *Url);
				CachedUpdateInfo.State = EPluginUpdateState::None;
				CachedUpdateInfo.bChecked = true;
				return;
			}

			TSharedPtr<FJsonObject> Json;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("[NSAI] Update check: malformed JSON from %s"), *Url);
				CachedUpdateInfo.State = EPluginUpdateState::None;
				CachedUpdateInfo.bChecked = true;
				return;
			}

			FPluginUpdateInfo Info;
			Info.bChecked = true;
			Json->TryGetStringField(TEXT("latestVersion"), Info.LatestVersion);
			Json->TryGetStringField(TEXT("changelog"), Info.Changelog);
			// Server computes updateAvailable based on `current` query param.
			Json->TryGetBoolField(TEXT("updateAvailable"), Info.bUpdateAvailable);
			Json->TryGetBoolField(TEXT("downloadAvailable"), Info.bDownloadAvailable);
			Info.State = Info.bUpdateAvailable ? EPluginUpdateState::UpdateAvailable : EPluginUpdateState::None;

			if (!Info.bUpdateAvailable)
			{
				AsyncTask(ENamedThreads::GameThread, [Info, Channel]()
				{
					CachedUpdateInfo = Info;
					// Distinguish "up-to-date" from "nothing published on this channel"
					// — both return updateAvailable=false but mean very different
					// things when diagnosing a broken-looking release pipeline.
					if (Info.LatestVersion.IsEmpty())
					{
						UE_LOG(LogNeoStackAI, Log,
							TEXT("[NSAI] No release published on channel '%s' yet."),
							*Channel);
					}
					else
					{
						UE_LOG(LogNeoStackAI, Log,
							TEXT("[NSAI] No update available (latest: v%s on %s)"),
							*Info.LatestVersion, *Channel);
					}
					// Core is current — but extensions may have drifted (user
					// updated core through some other route, or skipped the
					// last bundle). Surface a notification if any installed
					// extension is behind the catalog's latest.
					FNeoStackAIModule::MaybeNotifyExtensionsOnlyUpdate();
				});
				return;
			}

			UE_LOG(LogNeoStackAI, Log, TEXT("[NSAI] Update available: v%s"), *Info.LatestVersion);

			// If a build exists for this platform and the user has a key,
			// resolve a signed download URL in step 2. Otherwise show the
			// notification as-is — ShowUpdateNotification() branches on
			// (bDownloadAvailable, key-present) to pick the right body.
			const bool bHasKey = !ResolveCloudApiKey().IsEmpty();
			if (Info.bDownloadAvailable && bHasKey)
			{
				ResolveDownloadAndNotify(Info, EngineVersion, Platform, Channel);
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, [Info]()
				{
					CachedUpdateInfo = Info;
					ShowUpdateNotification();
				});
			}
		});

	Request->ProcessRequest();
}

FString FNeoStackAIModule::GetUpdateCacheDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NeoStackAI-Updates"));
}

void FNeoStackAIModule::DownloadUpdate()
{
	// CheckForPluginUpdate's step 2 populates DownloadUrl with a 10-minute
	// HMAC-signed URL to /api/plugin/download — the URL itself is the auth,
	// so this path no longer sets an x-api-key or Authorization header.
	if (CachedUpdateInfo.DownloadUrl.IsEmpty())
	{
		CachedUpdateInfo.State = EPluginUpdateState::Failed;
		CachedUpdateInfo.ErrorMessage = TEXT("No signed download URL available. Re-run the update check after setting a NeoStack API key in Settings > Chat & Agents > Chat Providers > NeoStack Cloud.");
		return;
	}

	// Don't re-download if already downloading or downloaded
	if (CachedUpdateInfo.State == EPluginUpdateState::Downloading ||
		CachedUpdateInfo.State == EPluginUpdateState::Downloaded ||
		CachedUpdateInfo.State == EPluginUpdateState::Installing)
	{
		return;
	}

	CachedUpdateInfo.State = EPluginUpdateState::Downloading;
	CachedUpdateInfo.DownloadProgress = 0.0f;
	CachedUpdateInfo.ErrorMessage.Empty();

	const FString EngineVersion = FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	FString Platform = TEXT("Win64");
#if PLATFORM_MAC
	Platform = TEXT("Mac");
#endif
	const FString Url = CachedUpdateInfo.DownloadUrl;

	// Ensure cache directory exists and clean up old downloads
	const FString CacheDir = GetUpdateCacheDir();
	IFileManager::Get().MakeDirectory(*CacheDir, true);

	TArray<FString> OldFiles;
	IFileManager::Get().FindFiles(OldFiles, *(CacheDir / TEXT("*.zip")), true, false);
	for (const FString& OldFile : OldFiles)
	{
		IFileManager::Get().Delete(*(CacheDir / OldFile));
	}

	const FString DownloadFileName = !CachedUpdateInfo.FileName.IsEmpty()
		? CachedUpdateInfo.FileName
		: FString::Printf(TEXT("NeoStackAI-UE%s-%s-v%s_Binary.zip"), *EngineVersion, *Platform, *CachedUpdateInfo.LatestVersion);
	const FString SavePath = FPaths::ConvertRelativePathToFull(CacheDir / DownloadFileName);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> DownloadRequest = FHttpModule::Get().CreateRequest();
	DownloadRequest->SetURL(Url);
	DownloadRequest->SetVerb(TEXT("GET"));
	DownloadRequest->SetHeader(TEXT("Accept"), TEXT("application/zip"));

	if (CachedUpdateInfo.FileSize > 0)
	{
		const int64 ExpectedFileSize = CachedUpdateInfo.FileSize;
		DownloadRequest->OnRequestProgress64().BindLambda(
			[ExpectedFileSize](FHttpRequestPtr, uint64 /*BytesSent*/, uint64 BytesReceived)
			{
				CachedUpdateInfo.DownloadProgress = FMath::Clamp(
					static_cast<float>(BytesReceived) / static_cast<float>(ExpectedFileSize), 0.0f, 1.0f);
			});
	}

	DownloadRequest->OnProcessRequestComplete().BindLambda(
		[SavePath](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				CachedUpdateInfo.State = EPluginUpdateState::Failed;
				CachedUpdateInfo.ErrorMessage = TEXT("Failed to connect to neostack.dev");
				return;
			}

			const int32 Code = Response->GetResponseCode();
			if (Code != 200)
			{
				TSharedPtr<FJsonObject> ErrJson;
				TSharedRef<TJsonReader<>> ErrReader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
				FString ErrMsg;
				if (FJsonSerializer::Deserialize(ErrReader, ErrJson) && ErrJson.IsValid())
				{
					ErrJson->TryGetStringField(TEXT("error"), ErrMsg);
				}

				if (Code == 401)
				{
					CachedUpdateInfo.ErrorMessage = ErrMsg.IsEmpty() ? TEXT("Invalid NeoStack API token") : ErrMsg;
				}
				else if (Code == 403)
				{
					CachedUpdateInfo.ErrorMessage = ErrMsg.IsEmpty() ? TEXT("Plugin access denied for this account") : ErrMsg;
				}
				else if (Code == 404)
				{
					CachedUpdateInfo.ErrorMessage = ErrMsg.IsEmpty() ? TEXT("No compatible plugin build found") : ErrMsg;
				}
				else
				{
					CachedUpdateInfo.ErrorMessage = FString::Printf(TEXT("Server error (%d): %s"), Code, *ErrMsg);
				}
				CachedUpdateInfo.State = EPluginUpdateState::Failed;
				return;
			}

			const FString HeaderFileName = Response->GetHeader(TEXT("x-neostack-plugin-version"));
			if (!HeaderFileName.IsEmpty())
			{
				CachedUpdateInfo.DownloadedVersion = HeaderFileName;
			}
			else
			{
				CachedUpdateInfo.DownloadedVersion = CachedUpdateInfo.LatestVersion;
			}

			const FString HeaderChecksum = Response->GetHeader(TEXT("x-neostack-plugin-sha256"));
			if (!HeaderChecksum.IsEmpty())
			{
				CachedUpdateInfo.Checksum = HeaderChecksum;
			}

			const TArray<uint8>& Content = Response->GetContent();
			if (Content.Num() == 0)
			{
				CachedUpdateInfo.State = EPluginUpdateState::Failed;
				CachedUpdateInfo.ErrorMessage = TEXT("Downloaded file is empty");
				return;
			}

			if (!FFileHelper::SaveArrayToFile(Content, *SavePath))
			{
				CachedUpdateInfo.State = EPluginUpdateState::Failed;
				CachedUpdateInfo.ErrorMessage = FString::Printf(TEXT("Failed to save update to: %s"), *SavePath);
				return;
			}

			CachedUpdateInfo.DownloadedZipPath = SavePath;
			CachedUpdateInfo.DownloadProgress = 1.0f;
			CachedUpdateInfo.State = EPluginUpdateState::Downloaded;
			UE_LOG(LogNeoStackAI, Log, TEXT("[NSAI] Update v%s downloaded to: %s (%lld bytes)"),
				*CachedUpdateInfo.DownloadedVersion, *SavePath, Content.Num());
		});

	DownloadRequest->ProcessRequest();
}

void FNeoStackAIModule::InstallUpdate()
{
	if (CachedUpdateInfo.State != EPluginUpdateState::Downloaded)
	{
		return;
	}

	const FString ZipPath = FPaths::ConvertRelativePathToFull(CachedUpdateInfo.DownloadedZipPath);
	if (!FPaths::FileExists(ZipPath))
	{
		CachedUpdateInfo.State = EPluginUpdateState::Failed;
		CachedUpdateInfo.ErrorMessage = TEXT("Downloaded file no longer exists");
		return;
	}

	// Get plugin install directory
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	if (!Plugin.IsValid())
	{
		CachedUpdateInfo.State = EPluginUpdateState::Failed;
		CachedUpdateInfo.ErrorMessage = TEXT("Could not locate plugin directory");
		return;
	}

	const FString PluginDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	const FString PluginParentDir = FPaths::GetPath(PluginDir);
	const FString BackupDir = PluginDir + TEXT("_backup");
	const FString ScriptDir = FPaths::ConvertRelativePathToFull(GetUpdateCacheDir());

	// Confirm with user before closing editor
	EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::OkCancel,
		FText::FromString(FString::Printf(
			TEXT("NeoStack AI v%s is ready to install.\n\n"
				"The editor will close automatically. The updater will:\n"
				"  1. Back up the current plugin\n"
				"  2. Extract the new version\n"
				"  3. Clean up\n\n"
				"You can relaunch the editor after the update completes.\n\n"
				"Press OK to proceed, or Cancel to install later."),
			*CachedUpdateInfo.LatestVersion)));

	if (Result != EAppReturnType::Ok)
	{
		return;
	}

	// Generate platform-specific updater script
	FString ScriptContent;
	FString ScriptPath;
	FString LaunchExe;
	FString LaunchArgs;

#if PLATFORM_WINDOWS
	// Normalize paths to backslashes for Windows batch script
	FString WinZipPath = ZipPath;
	FString WinPluginDir = PluginDir;
	FString WinPluginParentDir = PluginParentDir;
	FString WinBackupDir = BackupDir;
	FPaths::MakePlatformFilename(WinZipPath);
	FPaths::MakePlatformFilename(WinPluginDir);
	FPaths::MakePlatformFilename(WinPluginParentDir);
	FPaths::MakePlatformFilename(WinBackupDir);

	ScriptPath = ScriptDir / TEXT("updater.bat");
	FPaths::MakePlatformFilename(ScriptPath);

	ScriptContent = FString::Printf(TEXT(
		"@echo off\r\n"
		"echo.\r\n"
		"echo ============================================\r\n"
		"echo   NeoStack AI - Auto Updater\r\n"
		"echo ============================================\r\n"
		"echo.\r\n"
		"echo Waiting for Unreal Editor to close...\r\n"
		":WAIT_EDITOR\r\n"
		"tasklist /FI \"IMAGENAME eq UnrealEditor.exe\" 2>NUL | find /I \"UnrealEditor.exe\" >NUL\r\n"
		"if %%ERRORLEVEL%% == 0 (\r\n"
		"    timeout /t 2 /nobreak >NUL\r\n"
		"    goto WAIT_EDITOR\r\n"
		")\r\n"
		"echo Editor closed.\r\n"
		"echo Waiting for file handles to release...\r\n"
		"timeout /t 5 /nobreak >NUL\r\n"
		"echo.\r\n"
		"\r\n"
		"echo Backing up current plugin...\r\n"
		"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
		"\r\n"
		":RETRY_MOVE\r\n"
		"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
		"if %%ERRORLEVEL%% == 0 goto MOVE_OK\r\n"
		"echo.\r\n"
		"echo Plugin files are still locked by another process.\r\n"
		"echo.\r\n"
		"echo Checking for processes that may be locking files...\r\n"
		"set FOUND_BLOCKER=0\r\n"
		"tasklist /FI \"IMAGENAME eq devenv.exe\" 2>NUL | find /I \"devenv.exe\" >NUL && (\r\n"
		"    echo   [!] Visual Studio ^(devenv.exe^) is running\r\n"
		"    set FOUND_BLOCKER=1\r\n"
		")\r\n"
		"tasklist /FI \"IMAGENAME eq rider64.exe\" 2>NUL | find /I \"rider64.exe\" >NUL && (\r\n"
		"    echo   [!] JetBrains Rider ^(rider64.exe^) is running\r\n"
		"    set FOUND_BLOCKER=1\r\n"
		")\r\n"
		"tasklist /FI \"IMAGENAME eq Code.exe\" 2>NUL | find /I \"Code.exe\" >NUL && (\r\n"
		"    echo   [!] VS Code ^(Code.exe^) is running\r\n"
		"    set FOUND_BLOCKER=1\r\n"
		")\r\n"
		"tasklist /FI \"IMAGENAME eq ShaderCompileWorker.exe\" 2>NUL | find /I \"ShaderCompileWorker.exe\" >NUL && (\r\n"
		"    echo   [!] Shader Compile Worker is still running\r\n"
		"    set FOUND_BLOCKER=1\r\n"
		")\r\n"
		"tasklist /FI \"IMAGENAME eq UnrealBuildAccelerator.exe\" 2>NUL | find /I \"UnrealBuildAccelerator.exe\" >NUL && (\r\n"
		"    echo   [!] Unreal Build Accelerator is still running\r\n"
		"    set FOUND_BLOCKER=1\r\n"
		")\r\n"
		"if %%FOUND_BLOCKER%% == 0 (\r\n"
		"    echo   No known blockers found. Files may still be releasing...\r\n"
		")\r\n"
		"echo.\r\n"
		"echo Close the above program^(s^), then press any key to retry.\r\n"
		"echo Or close this window to cancel the update.\r\n"
		"pause >NUL\r\n"
		"goto RETRY_MOVE\r\n"
		"\r\n"
		":MOVE_OK\r\n"
		"echo Backup created successfully.\r\n"
		"echo Extracting update...\r\n"
		"powershell -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"\r\n"
		"if %%ERRORLEVEL%% == 0 (\r\n"
		"    echo.\r\n"
		"    echo ============================================\r\n"
		"    echo   Update installed successfully!\r\n"
		"    echo ============================================\r\n"
		"    rmdir /S /Q \"%s\"\r\n"
		"    del /F /Q \"%s\"\r\n"
		"    echo.\r\n"
		"    echo You can now restart the Unreal Editor.\r\n"
		") else (\r\n"
		"    echo.\r\n"
		"    echo ERROR: Extraction failed! Restoring backup...\r\n"
		"    if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
		"    move /Y \"%s\" \"%s\"\r\n"
		"    echo Backup restored. Your plugin is unchanged.\r\n"
		")\r\n"
		"echo.\r\n"
		"pause\r\n"),
		*WinBackupDir, *WinBackupDir,                 // rmdir backup if leftover
		*WinPluginDir, *WinBackupDir,                 // move plugin -> backup (retry loop)
		*WinZipPath, *WinPluginParentDir,             // extract zip
		*WinBackupDir,                                // remove backup on success
		*WinZipPath,                                  // remove zip on success
		*WinPluginDir, *WinPluginDir,                 // remove failed extract on fail
		*WinBackupDir, *WinPluginDir);                // restore backup on fail
	LaunchExe = TEXT("cmd.exe");
	LaunchArgs = FString::Printf(TEXT("/c start \"AIK Updater\" /wait cmd.exe /c \"\"%s\"\""), *ScriptPath);
#else
	ScriptPath = FPaths::ConvertRelativePathToFull(ScriptDir / TEXT("updater.sh"));
	ScriptContent = FString::Printf(TEXT(
		"#!/bin/bash\n"
		"echo\n"
		"echo '============================================'\n"
		"echo '  NeoStack AI - Auto Updater'\n"
		"echo '============================================'\n"
		"echo\n"
		"echo 'Waiting for Unreal Editor to close...'\n"
		"while pgrep -x 'UnrealEditor' > /dev/null 2>&1; do\n"
		"    sleep 2\n"
		"done\n"
		"echo 'Editor closed. Installing update...'\n"
		"echo\n"
		"echo 'Backing up current plugin...'\n"
		"rm -rf '%s'\n"
		"mv '%s' '%s'\n"
		"if [ $? -ne 0 ]; then\n"
		"    echo 'ERROR: Failed to back up plugin directory.'\n"
		"    read -p 'Press Enter to exit...'\n"
		"    exit 1\n"
		"fi\n"
		"echo 'Extracting update...'\n"
		"unzip -o '%s' -d '%s'\n"
		"if [ $? -eq 0 ]; then\n"
		"    echo\n"
		"    echo 'Update installed successfully!'\n"
		"    rm -rf '%s'\n"
		"    rm -f '%s'\n"
		"    echo 'You can now restart the Unreal Editor.'\n"
		"else\n"
		"    echo\n"
		"    echo 'ERROR: Update failed! Restoring backup...'\n"
		"    rm -rf '%s'\n"
		"    mv '%s' '%s'\n"
		"    echo 'Backup restored. Your plugin is unchanged.'\n"
		"fi\n"
		"echo\n"
		"read -p 'Press Enter to exit...'\n"),
		*BackupDir,                       // rm backup if leftover
		*PluginDir, *BackupDir,           // mv plugin -> backup
		*ZipPath, *PluginParentDir,       // unzip
		*BackupDir,                       // remove backup on success
		*ZipPath,                         // remove zip on success
		*PluginDir,                       // remove failed extract on fail
		*BackupDir, *PluginDir);          // restore backup on fail
	LaunchExe = TEXT("/usr/bin/open");
	LaunchArgs = FString::Printf(TEXT("-a Terminal \"%s\""), *ScriptPath);
#endif

	// Write script
	if (!FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath))
	{
		CachedUpdateInfo.State = EPluginUpdateState::Failed;
		CachedUpdateInfo.ErrorMessage = TEXT("Failed to write updater script");
		return;
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] Updater script written to: %s"), *ScriptPath);

#if !PLATFORM_WINDOWS
	// Make script executable on macOS/Linux
	FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *ScriptPath), nullptr, nullptr, nullptr);
#endif

	// Launch the updater script detached with a visible console window
	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*LaunchExe, *LaunchArgs,
		true,   // bLaunchDetached
		false,  // bLaunchHidden
		false,  // bLaunchReallyHidden
		nullptr, 0, nullptr, nullptr);

	if (!ProcHandle.IsValid())
	{
		CachedUpdateInfo.State = EPluginUpdateState::Failed;
		CachedUpdateInfo.ErrorMessage = TEXT("Failed to launch updater script");
		UE_LOG(LogNeoStackAI, Error, TEXT("[AIK] Failed to launch updater: %s %s"), *LaunchExe, *LaunchArgs);
		return;
	}

	CachedUpdateInfo.State = EPluginUpdateState::Installing;
	UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] Updater launched. Requesting editor shutdown."));

	// Request a clean editor shutdown so the updater script can proceed
	GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
}

// ============================================
// Unified updater (core + extensions in one editor-restart)
// ============================================
//
// Two arrival paths:
//   1. Core-update notification's Install button — CachedUpdateInfo has a
//      downloaded ZIP plus zero or more PendingExtensionUpdates.
//   2. Extensions-only notification (MaybeNotifyExtensionsOnlyUpdate) —
//      CachedUpdateInfo has no DownloadedZipPath but PendingExtensionUpdates
//      is populated.
//
// In both cases we:
//   a. Confirm with the user (single dialog, extended copy when extensions
//      are involved).
//   b. Queue Update ops on FNeoStackExtensionInstaller for each pending
//      extension (by Slug + PluginName) on the cached channel.
//   c. Subscribe once to OnAllOpsStaged.
//   d. Call ApplyDeferred so the installer state machine stages every
//      extension to PendingRestart without launching its own script.
//   e. When the delegate fires, call WriteAndLaunchUnifiedUpdater with
//      either a populated FCoreUpdatePayload (path 1) or empty (path 2).
//   f. Quit the editor — the unified script takes over.

namespace
{
	FDelegateHandle GUnifiedAllStagedHandle;
	bool GUnifiedInstallInFlight = false;

	void LaunchUnifiedScriptAndQuit()
	{
		using FCorePayload = FNeoStackExtensionInstaller::FCoreUpdatePayload;

		FPluginUpdateInfo& Info = FNeoStackAIModule::GetUpdateInfoMutable();

		// Build the optional core payload only when there's a downloaded ZIP
		// to swap in. The extensions-only path leaves DownloadedZipPath empty.
		TOptional<FCorePayload> CorePayload;
		if (!Info.DownloadedZipPath.IsEmpty() && Info.State == EPluginUpdateState::Downloaded)
		{
			TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
			if (Plugin.IsValid())
			{
				FCorePayload P;
				P.TargetPluginDir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
				P.PluginParentDir = FPaths::GetPath(P.TargetPluginDir);
				P.ZipPath = FPaths::ConvertRelativePathToFull(Info.DownloadedZipPath);
				P.LatestVersion = Info.LatestVersion;
				CorePayload = MoveTemp(P);
			}
			else
			{
				UE_LOG(LogNeoStackAI, Error,
					TEXT("[AIK] Unified install: NeoStackAI plugin not found via IPluginManager"));
				Info.State = EPluginUpdateState::Failed;
				Info.ErrorMessage = TEXT("Could not locate plugin directory.");
				GUnifiedInstallInFlight = false;
				return;
			}
		}

		const bool bLaunched = FNeoStackExtensionInstaller::WriteAndLaunchUnifiedUpdater(CorePayload);
		if (!bLaunched)
		{
			Info.State = EPluginUpdateState::Failed;
			Info.ErrorMessage = TEXT("Failed to launch updater script.");
			GUnifiedInstallInFlight = false;
			UE_LOG(LogNeoStackAI, Error, TEXT("[AIK] Unified updater failed to launch."));
			return;
		}

		Info.State = EPluginUpdateState::Installing;
		GUnifiedInstallInFlight = false;

		UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] Unified updater launched. Requesting editor shutdown."));

		// Request a clean editor shutdown so the updater script can proceed.
		// Same pattern as the legacy InstallUpdate path.
		if (GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

void FNeoStackAIModule::InstallUpdateUnified()
{
	if (GUnifiedInstallInFlight)
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] InstallUpdateUnified: already in progress"));
		return;
	}

	FPluginUpdateInfo& Info = GetUpdateInfoMutable();
	const int32 ExtCount = Info.PendingExtensionUpdates.Num();
	const bool bHasCoreDownload = !Info.DownloadedZipPath.IsEmpty()
		&& Info.State == EPluginUpdateState::Downloaded;

	// If neither core nor extensions need to move, fall back to the
	// historical behavior (probably unreachable from the notification UI but
	// guards against direct callers).
	if (!bHasCoreDownload && ExtCount == 0)
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("[AIK] InstallUpdateUnified: nothing to do."));
		return;
	}

	// Compose the confirmation dialog text. The core-only path keeps copy
	// close to the legacy InstallUpdate dialog so muscle-memory still works.
	FString Body;
	if (bHasCoreDownload && ExtCount > 0)
	{
		Body = FString::Printf(
			TEXT("NeoStack AI v%s and %d extension update%s are ready to install.\n\n"
				 "The editor will close automatically. The updater will:\n"
				 "  1. Back up each plugin\n"
				 "  2. Swap in the new core and extensions\n"
				 "  3. Roll back any individual swap that fails\n\n"
				 "Relaunch the editor when the updater says it's done.\n\n"
				 "Press OK to proceed, or Cancel to install later."),
			*Info.LatestVersion,
			ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"));
	}
	else if (bHasCoreDownload)
	{
		Body = FString::Printf(
			TEXT("NeoStack AI v%s is ready to install.\n\n"
				 "The editor will close automatically. The updater will:\n"
				 "  1. Back up the current plugin\n"
				 "  2. Extract the new version\n"
				 "  3. Clean up\n\n"
				 "You can relaunch the editor after the update completes.\n\n"
				 "Press OK to proceed, or Cancel to install later."),
			*Info.LatestVersion);
	}
	else
	{
		Body = FString::Printf(
			TEXT("%d NeoStack AI extension update%s ready.\n\n"
				 "The editor will close automatically. The updater swaps each "
				 "extension folder, rolling back any individual failure.\n\n"
				 "Relaunch the editor when the updater says it's done.\n\n"
				 "Press OK to proceed, or Cancel to install later."),
			ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"));
	}

	const EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::OkCancel, FText::FromString(Body));
	if (Result != EAppReturnType::Ok)
	{
		return;
	}

	GUnifiedInstallInFlight = true;

	// Path A: no extensions to move. Defer to the legacy InstallUpdate flow
	// which already handles the core-only case (writes updater.bat, closes
	// the editor). Saves the cost of a second script writer for the trivial
	// case and keeps that battle-tested path live.
	if (ExtCount == 0)
	{
		GUnifiedInstallInFlight = false;
		InstallUpdate();
		return;
	}

	// Path B: queue extension Update ops on the deferred installer. The
	// state machine runs each op through ResolveDownload → Download →
	// Verify → Extract → AtomicInstall, ending in PendingRestart for any
	// op whose target dir already exists (the standard update case). When
	// every op reaches a terminal phase, OnAllOpsStaged fires and we
	// launch the combined script.
	const FString Channel = !Info.CheckedChannel.IsEmpty() ? Info.CheckedChannel : TEXT("stable");

	for (const FNeoStackExtensionUpdateCandidate& C : Info.PendingExtensionUpdates)
	{
		FNeoStackExtensionInstaller::Queue(C.Slug, C.PluginName, ENeoStackOpKind::Update, Channel);
	}

	// One-shot subscription. Capture the channel by value so even a
	// concurrent CheckForPluginUpdate can't shift the closure's view.
	if (GUnifiedAllStagedHandle.IsValid())
	{
		FNeoStackExtensionInstaller::OnAllOpsStaged().Remove(GUnifiedAllStagedHandle);
		GUnifiedAllStagedHandle.Reset();
	}
	GUnifiedAllStagedHandle = FNeoStackExtensionInstaller::OnAllOpsStaged().AddLambda([]()
	{
		if (GUnifiedAllStagedHandle.IsValid())
		{
			FNeoStackExtensionInstaller::OnAllOpsStaged().Remove(GUnifiedAllStagedHandle);
			GUnifiedAllStagedHandle.Reset();
		}
		LaunchUnifiedScriptAndQuit();
	});

	FNeoStackExtensionInstaller::ApplyDeferred();

	// Update the notification subtext so the user sees something is happening
	// — extension downloads typically take a few seconds each.
	if (GUpdateNotification.IsValid())
	{
		GUpdateNotification->SetCompletionState(SNotificationItem::CS_Pending);
		GUpdateNotification->SetSubText(FText::FromString(FString::Printf(
			TEXT("Preparing %d extension update%s…"),
			ExtCount, ExtCount == 1 ? TEXT("") : TEXT("s"))));
	}
}

void FNeoStackAIModule::MaybeNotifyExtensionsOnlyUpdate()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || !Settings->bAlsoUpdateExtensions || !Settings->bCheckForUpdates)
	{
		return;
	}

	const FString Channel = Settings->bUseBetaChannel ? TEXT("beta") : TEXT("stable");
	const FString EngineVersion = FString::Printf(TEXT("%d.%d"),
		ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);

	// The catalog refresh is fired in parallel with CheckForPluginUpdate,
	// so when this function is called at the "no core update" branch the
	// catalog may still be in flight. Retry up to ~5 seconds before giving
	// up — if it never resolves, the notification simply doesn't appear
	// (next session catches up).
	const FNeoStackCatalogResult Catalog = FNeoStackExtensionCatalog::GetCachedResult();
	if (Catalog.Status == ENeoStackCatalogStatus::Fetching
		|| Catalog.Status == ENeoStackCatalogStatus::Idle)
	{
		static int32 RetryAttempts = 0;
		if (RetryAttempts++ < 5)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[](float) -> bool
				{
					FNeoStackAIModule::MaybeNotifyExtensionsOnlyUpdate();
					return false; // one-shot
				}), 1.0f);
		}
		else
		{
			RetryAttempts = 0;
			UE_LOG(LogNeoStackAI, Verbose,
				TEXT("[NSAI] Extensions-only check: catalog never reached Ready; skipping."));
		}
		return;
	}

	const TArray<FNeoStackExtensionUpdateCandidate> Pending =
		FNeoStackExtensionInstaller::ComputePendingUpdates(Channel, EngineVersion);
	if (Pending.Num() == 0) return;

	// Park the candidate list on the same notification slot as the core flow.
	// Reusing the slot keeps the user's mental model simple — they always
	// look at one banner. When the user clicks Install we route through
	// InstallUpdateUnified, which sees no core download and runs the
	// extensions-only path.
	FPluginUpdateInfo& Info = GetUpdateInfoMutable();
	Info.PendingExtensionUpdates = Pending;
	Info.CheckedChannel = Channel;
	Info.bChecked = true;
	// Leave bUpdateAvailable / DownloadUrl unset — the notification renders
	// the "extensions-only" copy when bUpdateAvailable is false but the
	// pending list is non-empty.
	Info.bUpdateAvailable = false;
	Info.bDownloadAvailable = false;

	UE_LOG(LogNeoStackAI, Log,
		TEXT("[AIK] %d extension update(s) available (core is current); showing notification."),
		Pending.Num());

	ShowUpdateNotification();
}

void FNeoStackAIModule::OpenAgentChatWindow()
{
	FGlobalTabmanager::Get()->TryInvokeTab(
		bRegisteredEpicAIAssistantTabOverride ? EpicAIAssistantTabName : AgentChatTabName);
}

void FNeoStackAIModule::BindWebUIInputMethodSystem(const TSharedPtr<SWebBrowser>& Browser)
{
	WebUIInputMethodBrowserWidget = Browser;

	if (!Browser.IsValid() || !FSlateApplication::IsInitialized())
	{
		return;
	}

	ITextInputMethodSystem* InputMethodSystem = FSlateApplication::Get().GetTextInputMethodSystem();
	if (!InputMethodSystem)
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("WebUI: No Input Method System available; non-ASCII input may not work in chat."));
		return;
	}

	Browser->BindInputMethodSystem(InputMethodSystem);

	if (!InputMethodSystemSlatePreShutdownDelegateHandle.IsValid())
	{
		InputMethodSystemSlatePreShutdownDelegateHandle =
			FSlateApplication::Get().OnPreShutdown().AddRaw(this, &FNeoStackAIModule::UnbindWebUIInputMethodSystem);
	}
}

void FNeoStackAIModule::UnbindWebUIInputMethodSystem()
{
	// Unbind on IWebBrowserWindow first: the Slate SWebBrowser may already be destroyed
	// during editor exit while this shared ptr still keeps CEF alive.
	if (WebUIBrowserWindow.IsValid())
	{
		WebUIBrowserWindow->UnbindInputMethodSystem();
	}

	if (FSlateApplication::IsInitialized())
	{
		if (TSharedPtr<SWebBrowser> Browser = WebUIInputMethodBrowserWidget.Pin())
		{
			Browser->UnbindInputMethodSystem();
		}
	}

	WebUIInputMethodBrowserWidget.Reset();
}

// ── WebUI local file server ─────────────────────────────────────────
// Serves the SvelteKit static build over HTTP so we avoid file:// CORS
// issues on Windows (Chromium blocks ES module imports from file:// origins).

static FString GetMimeType(const FString& FilePath)
{
	if (FilePath.EndsWith(TEXT(".html"))) return TEXT("text/html; charset=utf-8");
	if (FilePath.EndsWith(TEXT(".js")))   return TEXT("application/javascript; charset=utf-8");
	if (FilePath.EndsWith(TEXT(".css")))  return TEXT("text/css; charset=utf-8");
	if (FilePath.EndsWith(TEXT(".json"))) return TEXT("application/json; charset=utf-8");
	if (FilePath.EndsWith(TEXT(".svg")))  return TEXT("image/svg+xml");
	if (FilePath.EndsWith(TEXT(".png")))  return TEXT("image/png");
	if (FilePath.EndsWith(TEXT(".jpg")) || FilePath.EndsWith(TEXT(".jpeg"))) return TEXT("image/jpeg");
	if (FilePath.EndsWith(TEXT(".woff2"))) return TEXT("font/woff2");
	if (FilePath.EndsWith(TEXT(".woff")))  return TEXT("font/woff");
	if (FilePath.EndsWith(TEXT(".ico")))   return TEXT("image/x-icon");
	return TEXT("application/octet-stream");
}

bool FNeoStackAIModule::StartWebUIFileServer(const FString& BuildDir)
{
	if (WebUIFileRouter.IsValid())
	{
		return true; // Already running
	}

	WebUIBuildDir = BuildDir;
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	HttpServerModule.StartAllListeners();

	// Try ports 19800-19810
	constexpr int32 BasePort = 19800;
	constexpr int32 MaxAttempts = 10;

	for (int32 i = 0; i < MaxAttempts; ++i)
	{
		const int32 Port = BasePort + i;
		TSharedPtr<IHttpRouter> Router = HttpServerModule.GetHttpRouter(Port, /* bFailOnBindFailure */ true);
		if (Router.IsValid())
		{
			WebUIFileRouter = Router;
			WebUIFileServerPort = Port;
			break;
		}
	}

	if (!WebUIFileRouter.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUI FileServer: Failed to bind any port in range %d-%d"), BasePort, BasePort + MaxAttempts - 1);
		return false;
	}

	// Use a request preprocessor to serve ALL requests from the build directory.
	// We can't use BindRoute("/") because UE's FHttpPath::IsValidPath() returns false for root.
	FString CapturedBuildDir = WebUIBuildDir;
	WebUIFileServerPreprocessorHandle = WebUIFileRouter->RegisterRequestPreprocessor(
		FHttpRequestHandler::CreateLambda([CapturedBuildDir](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			FString RelPath = Request.RelativePath.GetPath();

			// Security: prevent path traversal
			RelPath.ReplaceInline(TEXT(".."), TEXT(""));
			while (RelPath.StartsWith(TEXT("/")))
			{
				RelPath.RemoveAt(0);
			}

			// Root request → serve index.html
			FString FilePath;
			if (RelPath.IsEmpty())
			{
				FilePath = FPaths::Combine(CapturedBuildDir, TEXT("index.html"));
			}
			else
			{
				FilePath = FPaths::Combine(CapturedBuildDir, RelPath);
			}

			TArray<uint8> FileData;
			if (FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				auto Response = FHttpServerResponse::Create(MoveTemp(FileData), GetMimeType(FilePath));
				OnComplete(MoveTemp(Response));
			}
			else
			{
				// SvelteKit SPA fallback — serve index.html for non-file paths
				FString FallbackPath = FPaths::Combine(CapturedBuildDir, TEXT("index.html"));
				TArray<uint8> FallbackData;
				if (FFileHelper::LoadFileToArray(FallbackData, *FallbackPath))
				{
					auto Response = FHttpServerResponse::Create(MoveTemp(FallbackData), TEXT("text/html; charset=utf-8"));
					OnComplete(MoveTemp(Response));
				}
				else
				{
					OnComplete(FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound, TEXT(""), TEXT("Not Found")));
				}
			}
			return true;
		}));

	UE_LOG(LogNeoStackAI, Log, TEXT("WebUI FileServer: Serving %s on http://localhost:%d"), *WebUIBuildDir, WebUIFileServerPort);
	return true;
}

void FNeoStackAIModule::StopWebUIFileServer()
{
	if (WebUIFileRouter.IsValid())
	{
		if (WebUIFileServerPreprocessorHandle.IsValid())
		{
			WebUIFileRouter->UnregisterRequestPreprocessor(WebUIFileServerPreprocessorHandle);
			WebUIFileServerPreprocessorHandle.Reset();
		}
		UE_LOG(LogNeoStackAI, Log, TEXT("WebUI FileServer: Stopping on port %d"), WebUIFileServerPort);
		WebUIFileRouter.Reset();
		WebUIFileServerPort = 0;
	}
}

TSharedRef<SDockTab> FNeoStackAIModule::SpawnAgentChatTab(const FSpawnTabArgs& SpawnTabArgs)
{
	// Create the bridge UObject (once, reused across tab reopens)
	if (!WebUIBridgeInstance)
	{
		WebUIBridgeInstance = NewObject<UWebUIBridge>();
		WebUIBridgeInstance->AddToRoot(); // Prevent GC
	}

	// Resolve URL: always serve the local build via the embedded HTTP server
	// (avoids file:// CORS on Windows). Falls back to the Vite default port
	// `http://localhost:5173` if no build is present, so running `bun run dev`
	// in Source/WebUI continues to work for local UI development.
	FString URL;

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	if (Plugin.IsValid())
	{
		FString BuildDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/WebUI/build"));
		FPaths::NormalizeFilename(BuildDir);
		BuildDir = FPaths::ConvertRelativePathToFull(BuildDir);
		FString IndexPath = FPaths::Combine(BuildDir, TEXT("index.html"));

		if (FPaths::FileExists(IndexPath))
		{
			if (StartWebUIFileServer(BuildDir))
			{
				URL = FString::Printf(TEXT("http://localhost:%d/"), WebUIFileServerPort);
				UE_LOG(LogNeoStackAI, Log, TEXT("WebUI: Serving local build via HTTP at %s"), *URL);
			}
			else
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("WebUI: Failed to start local file server, falling back to file:// URL"));
				FString FilePath = FPaths::Combine(BuildDir, TEXT("index.html"));
				FilePath.ReplaceInline(TEXT("\\"), TEXT("/"));
				FilePath.ReplaceInline(TEXT(" "), TEXT("%20"));
				URL = FString::Printf(TEXT("file:///%s"), *FilePath);
			}
		}
	}

	if (URL.IsEmpty())
	{
		URL = TEXT("http://localhost:5173");
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUI: Local build not found — falling back to Vite dev server at %s (run `bun run build` in Source/WebUI)"), *URL);
	}

	// Create browser window via IWebBrowserSingleton (matches Bridge/Fab plugin pattern).
	// On macOS, this creates the native WKWebView. Without SetParentDockTab below,
	// the native view stays always-visible and covers other editor panels/popups.
	FCreateBrowserWindowSettings WindowSettings;
	WindowSettings.InitialURL = URL;
	WindowSettings.BrowserFrameRate = 60;
	WindowSettings.bShowErrorMessage = true;

	IWebBrowserSingleton* WebBrowserSingleton = IWebBrowserModule::Get().GetSingleton();
	WebUIBrowserWindow = WebBrowserSingleton ? WebBrowserSingleton->CreateBrowserWindow(WindowSettings) : nullptr;
	if (WebUIBrowserWindow.IsValid())
	{
		WebUIBrowserWindow->OnUnhandledKeyDown().BindLambda([this](const FKeyEvent&)
		{
			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}

			const TSharedPtr<SWidget> BrowserWidget = WebUIBrowserWidget.Pin();
			return BrowserWidget.IsValid() && FSlateApplication::Get().HasFocusedDescendants(BrowserWidget.ToSharedRef());
		});
		WebUIBrowserWindow->OnUnhandledKeyUp().BindLambda([this](const FKeyEvent&)
		{
			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}

			const TSharedPtr<SWidget> BrowserWidget = WebUIBrowserWidget.Pin();
			return BrowserWidget.IsValid() && FSlateApplication::Get().HasFocusedDescendants(BrowserWidget.ToSharedRef());
		});
	}

	TSharedPtr<SWebBrowser> Browser;

	TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		.OnTabClosed_Lambda([this](TSharedRef<SDockTab>)
		{
			UnbindWebUIInputMethodSystem();

			// Unlink browser from dock tab so native view is hidden
			if (WebUIBrowserWindow.IsValid())
			{
				WebUIBrowserWindow->OnUnhandledKeyDown().Unbind();
				WebUIBrowserWindow->OnUnhandledKeyUp().Unbind();
			}
#if ENGINE_MINOR_VERSION == 7
			if (WebUIBrowserWindow.IsValid())
			{
				WebUIBrowserWindow->SetParentDockTab(nullptr);
			}
#endif
		})
		[
			SNew(SACPAttachmentDropTarget)
			[
				SAssignNew(Browser, SWebBrowser, WebUIBrowserWindow)
					.ShowControls(false)
					.ShowAddressBar(false)
					.ShowErrorMessage(true)
					.SupportsTransparency(false)
					.OnBeforePopup(FOnBeforePopupDelegate::CreateLambda(
						[](FString PopupUrl, FString)
						{
							if (PopupUrl.IsEmpty())
							{
								return true;
							}

							FString ErrorString;
							FPlatformProcess::LaunchURL(*PopupUrl, TEXT(""), &ErrorString);
							if (!ErrorString.IsEmpty())
							{
								UE_LOG(LogNeoStackAI, Warning,
									TEXT("WebUI: Failed to open popup URL '%s': %s"), *PopupUrl, *ErrorString);
							}

							// Block opening a second embedded browser window; use system browser instead.
							return true;
						}))
					.OnLoadCompleted(FSimpleDelegate::CreateLambda([this]()
						{
							const TWeakPtr<SWidget> WeakBrowser = WebUIBrowserWidget;
							AsyncTask(ENamedThreads::GameThread, [WeakBrowser]()
							{
								if (!FSlateApplication::IsInitialized())
								{
									return;
								}

								if (const TSharedPtr<SWidget> BrowserWidget = WeakBrowser.Pin())
								{
									FSlateApplication::Get().SetKeyboardFocus(BrowserWidget, EFocusCause::SetDirectly);
								}
							});
						}))
					.OnSuppressContextMenu(FOnSuppressContextMenu::CreateLambda([]() { return true; }))
					.BrowserFrameRate(60)
					.OnConsoleMessage(FOnConsoleMessageDelegate::CreateLambda(
						[](const FString& Message, const FString& Source, int32 Line, EWebBrowserConsoleLogSeverity Severity)
						{
							const UACPSettings* Settings = UACPSettings::Get();
							const bool bLogAllBrowserConsole = Settings && Settings->bVerboseLogging;
							const TCHAR* SeverityText = TEXT("default");

							switch (Severity)
							{
							case EWebBrowserConsoleLogSeverity::Verbose: SeverityText = TEXT("verbose"); break;
							case EWebBrowserConsoleLogSeverity::Debug:   SeverityText = TEXT("debug"); break;
							case EWebBrowserConsoleLogSeverity::Info:    SeverityText = TEXT("info"); break;
							case EWebBrowserConsoleLogSeverity::Warning: SeverityText = TEXT("warning"); break;
							case EWebBrowserConsoleLogSeverity::Error:   SeverityText = TEXT("error"); break;
							case EWebBrowserConsoleLogSeverity::Fatal:   SeverityText = TEXT("fatal"); break;
							default: break;
							}

							const FString SafeSource = Source.IsEmpty() ? TEXT("<unknown>") : Source;
							const FString ConsoleLine = FString::Printf(
								TEXT("WebUI Console [%s] %s:%d %s"),
								SeverityText,
								*SafeSource,
								Line,
								*Message);

							switch (Severity)
							{
							case EWebBrowserConsoleLogSeverity::Warning:
								UE_LOG(LogNeoStackAI, Warning, TEXT("%s"), *ConsoleLine);
								break;
							case EWebBrowserConsoleLogSeverity::Error:
							case EWebBrowserConsoleLogSeverity::Fatal:
								UE_LOG(LogNeoStackAI, Error, TEXT("%s"), *ConsoleLine);
								break;
							default:
								if (bLogAllBrowserConsole)
								{
									UE_LOG(LogNeoStackAI, Log, TEXT("%s"), *ConsoleLine);
								}
								break;
							}
						}))
				]
		];

	// CRITICAL: Link browser to dock tab — on macOS this subscribes to tab
	// foregrounding events and toggles native WKWebView visibility so the
	// browser only shows when its tab is active (fixes z-ordering over other panels)
#if ENGINE_MINOR_VERSION == 7
	if (WebUIBrowserWindow.IsValid())
	{
		WebUIBrowserWindow->SetParentDockTab(Tab);
	}
#endif

	// Fix z-ordering on tab drag/relocation (matches Bridge/Fab plugin workaround)
	WebUIBrowserWidget = Browser;
	BindWebUIInputMethodSystem(Browser);
	Tab->SetOnTabDraggedOverDockArea(
		FSimpleDelegate::CreateLambda([WeakBrowser = TWeakPtr<SWidget>(Browser)]()
		{
			if (TSharedPtr<SWidget> Pinned = WeakBrowser.Pin())
			{
				Pinned->Invalidate(EInvalidateWidgetReason::Layout);
			}
		})
	);
	Tab->SetOnTabRelocated(
		FSimpleDelegate::CreateLambda([WeakBrowser = TWeakPtr<SWidget>(Browser)]()
		{
			if (TSharedPtr<SWidget> Pinned = WeakBrowser.Pin())
			{
				Pinned->Invalidate(EInvalidateWidgetReason::Layout);
			}
		})
	);

	// Bind the bridge so JS can access it as window.ue.bridge.*
	if (Browser.IsValid() && WebUIBridgeInstance)
	{
		Browser->BindUObject(TEXT("bridge"), WebUIBridgeInstance, true);
		UE_LOG(LogNeoStackAI, Log, TEXT("WebUI: Bridge bound to browser as window.ue.bridge"));
	}

	return Tab;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNeoStackAIModule, NeoStackAI)
