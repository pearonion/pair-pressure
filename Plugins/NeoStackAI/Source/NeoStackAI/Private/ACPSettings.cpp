// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPSettings.h"
#include "ACPAgentQuirks.h"
#include "AgentInstaller.h"
#include "NeoStackAIModule.h"
#include "MCPServer.h"
#include "UserPreferencesSubsystem.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "ACPSettings"

static bool IsExecutableAvailable(const FString& ExecutablePath, FString& OutResolvedPath)
{
	return FAgentInstaller::Get().ResolveExecutable(ExecutablePath, OutResolvedPath);
}

static FString NormalizeOpenRouterBaseUrl(const FString& InBaseUrl)
{
	FString BaseUrl = InBaseUrl;
	BaseUrl.TrimStartAndEndInline();
	if (BaseUrl.IsEmpty())
	{
		BaseUrl = TEXT("https://openrouter.ai/api/v1");
	}

	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1, EAllowShrinking::No);
	}

	if (BaseUrl.EndsWith(TEXT("/chat/completions")))
	{
		BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/chat/completions")), EAllowShrinking::No);
	}
	else if (BaseUrl.EndsWith(TEXT("/models")))
	{
		BaseUrl.LeftChopInline(FCString::Strlen(TEXT("/models")), EAllowShrinking::No);
	}

	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1, EAllowShrinking::No);
	}

	return BaseUrl;
}

static FString BuildOpenRouterUrl(const FString& BaseUrl, const TCHAR* EndpointPath)
{
	return NormalizeOpenRouterBaseUrl(BaseUrl) + EndpointPath;
}

static const TCHAR* LegacyChatGatewayAgentName = TEXT("OpenRouter");
static const TCHAR* LocalByokChatAgentName = TEXT("Local & BYOK Chat");

static bool IsChatProviderReadyForGateway(const UACPSettings* Settings, const FChatProviderSettings& Row)
{
	if (!Row.bEnabled)
	{
		return false;
	}
	if (!Row.ApiKey.IsEmpty())
	{
		return true;
	}
	if (Row.ProviderId.Equals(TEXT("ollama"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (Settings)
	{
		if (const FUserChatProvider* UserProvider = Settings->FindUserChatProvider(Row.ProviderId))
		{
			return !UserProvider->bRequiresApiKey;
		}
	}
	return false;
}

static bool ResolveClaudeCodeExecutableForAdapter(const FFilePath& PreferredPath, FString& OutResolvedPath)
{
	if (!PreferredPath.FilePath.IsEmpty() && IsExecutableAvailable(PreferredPath.FilePath, OutResolvedPath))
	{
		return true;
	}

	if (FAgentInstaller::Get().ResolveExecutable(TEXT("claude"), OutResolvedPath))
	{
		return true;
	}

	TArray<FString> CandidatePaths;
#if PLATFORM_WINDOWS
	const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (!UserProfile.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude.exe")));
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude.cmd")));
		CandidatePaths.Add(FPaths::Combine(UserProfile, TEXT(".local/bin/claude")));
	}
#else
	const FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (!HomeDir.IsEmpty())
	{
		CandidatePaths.Add(FPaths::Combine(HomeDir, TEXT(".local/bin/claude")));
	}
#endif

	for (FString Candidate : CandidatePaths)
	{
		FPaths::NormalizeFilename(Candidate);
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutResolvedPath = Candidate;
			return true;
		}
	}

	return false;
}

UACPSettings::UACPSettings()
{
	EnsureBuiltInSystemPromptDeliveryDefaults();
}

#if WITH_EDITOR
FText UACPSettings::GetSectionText() const
{
	return LOCTEXT("SectionText", "NeoStack AI");
}

FText UACPSettings::GetSectionDescription() const
{
	return LOCTEXT("SectionDescription", "Configure AI agent connections and API keys for NeoStack AI.");
}

void UACPSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	SaveConfig();

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UACPSettings, MCPServerPort))
	{
		if (FMCPServer::Get().IsRunning())
		{
			const int32 RequestedPort = FMath::Clamp(MCPServerPort, 1, 65535);
			FMCPServer::Get().Stop();
			FMCPServer::Get().Start(RequestedPort);
		}
	}
}
#endif

namespace
{
	// One-shot migrator. The standalone NeoStackApiKey field was removed in
	// favour of GetChatProviderApiKey("neostack"). Installs upgraded from a
	// prior release still have the value sitting in their .ini under the
	// removed key — lift it into the chat-provider row on first access so
	// remote access keeps working without forcing the user to re-paste.
	void MigrateLegacyNeoStackKey(UACPSettings& Settings)
	{
		static bool bMigrated = false;
		if (bMigrated) return;
		bMigrated = true;

		if (!GConfig) return;
		const FString IniName = Settings.GetClass()->GetConfigName();
		const TCHAR* Section = TEXT("/Script/NeoStackAI.ACPSettings");

		FString Legacy;
		if (!GConfig->GetString(Section, TEXT("NeoStackApiKey"), Legacy, IniName)) return;
		Legacy.TrimStartAndEndInline();
		if (Legacy.IsEmpty()) return;

		// Only copy if the canonical row is empty — never clobber a key the
		// user pasted into the new field.
		if (!Settings.GetChatProviderApiKey(TEXT("neostack")).IsEmpty())
		{
			GConfig->RemoveKey(Section, TEXT("NeoStackApiKey"), IniName);
			GConfig->Flush(false, IniName);
			return;
		}

		Settings.SetChatProviderApiKey(TEXT("neostack"), Legacy);
		GConfig->RemoveKey(Section, TEXT("NeoStackApiKey"), IniName);
		GConfig->Flush(false, IniName);
	}
}

UACPSettings* UACPSettings::Get()
{
	if (!UObjectInitialized() || IsEngineExitRequested())
	{
		return nullptr;
	}
	UACPSettings* Settings = GetMutableDefault<UACPSettings>();
	if (Settings)
	{
		MigrateLegacyNeoStackKey(*Settings);
	}
	return Settings;
}

TArray<FACPAgentConfig> UACPSettings::GetAgentConfigs() const
{
	TArray<FACPAgentConfig> Configs;
	FString ResolvedPath;

	// Local & BYOK Chat (built-in, native C++ - no external executable required).
	// Provider rows under it include OpenRouter, Ollama, Vercel, and custom
	// OpenAI-compatible endpoints.
	{
		FACPAgentConfig Config;
		Config.AgentName = LocalByokChatAgentName;
		Config.bIsBuiltIn = true;
		Config.IconUrl = TEXT("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none'%3E%3Crect x='4' y='4' width='16' height='12' rx='2' stroke='black' stroke-width='1.8'/%3E%3Cpath d='M8 8h.01M11 8h5M8 12h.01M11 12h5' stroke='black' stroke-width='1.8' stroke-linecap='round'/%3E%3Cpath d='M8 20h8M12 16v4' stroke='black' stroke-width='1.8' stroke-linecap='round'/%3E%3Ccircle cx='17' cy='17' r='2.2' stroke='black' stroke-width='1.8'/%3E%3Cpath d='M18.6 18.6 21 21M20 21l1-1' stroke='black' stroke-width='1.8' stroke-linecap='round'/%3E%3C/svg%3E");
		Config.InstallInstructions = TEXT("Use OpenRouter, Ollama, Vercel, or custom OpenAI-compatible providers.");
		Config.ExecutablePath = TEXT("");
		Config.ApiKey = GetOpenRouterAuthToken();
		Config.ModelId = TEXT("anthropic/claude-sonnet-4");
		Config.WorkingDirectory = FPaths::ProjectDir();

		// New chat provider layer: any enabled provider with valid auth, or an
		// enabled keyless/local provider like Ollama, makes the agent ready.
		bool bAnyChatProviderConfigured = false;
		for (const FChatProviderSettings& Row : ChatProviders)
		{
			if (IsChatProviderReadyForGateway(this, Row))
			{
				bAnyChatProviderConfigured = true;
				break;
			}
		}

		if (HasOpenRouterAuth() || bAnyChatProviderConfigured)
		{
			Config.Status = EACPAgentStatus::Available;
			Config.StatusMessage = TEXT("Ready");
		}
		else
		{
			Config.Status = EACPAgentStatus::MissingApiKey;
			Config.StatusMessage = TEXT("No chat provider configured. Open Settings > Chat & Agents > Chat Providers and add an API key.");
		}
		Configs.Add(Config);
	}

	// NeoStack Cloud (built-in chat gateway scoped to the "neostack" chat provider).
	// Surfaces our own neostack.dev-managed AI gateway as a top-level agent option so
	// users on a NeoStack subscription get a one-click entry instead of having
	// to dig into the model picker under another agent.
	{
		FACPAgentConfig Config;
		Config.AgentName = TEXT("NeoStack Cloud");
		Config.RegistryId = TEXT("neostack");
		Config.GatewayProviderId = TEXT("neostack");
		Config.bIsBuiltIn = true;
		Config.IconUrl = TEXT("https://neostack.dev/logomark-light.svg");
		Config.InstallInstructions = TEXT("Built-in agent. Paste your NeoStack API key (from app.neostack.dev) into the NeoStack Cloud provider row in Settings > Chat & Agents > Chat Providers.");
		Config.ExecutablePath = TEXT("");
		Config.ApiKey = GetChatProviderApiKey(TEXT("neostack"));
		Config.ModelId = TEXT("neostack:MiniMax-M2.7");
		Config.WorkingDirectory = FPaths::ProjectDir();

		if (!Config.ApiKey.IsEmpty())
		{
			Config.Status = EACPAgentStatus::Available;
			Config.StatusMessage = TEXT("Ready");
		}
		else
		{
			Config.Status = EACPAgentStatus::MissingApiKey;
			Config.StatusMessage = TEXT("No NeoStack API key set. Use \"Sign in with NeoStack\" in Settings > Chat & Agents > Chat Providers, or paste a key manually.");
		}
		Configs.Add(Config);
	}

	// Custom agents from settings
	for (const FACPAgentSettingsEntry& Entry : CustomAgents)
	{
		if (Entry.AgentName.IsEmpty() || Entry.ExecutablePath.FilePath.IsEmpty())
		{
			continue;
		}

		FACPAgentConfig Config;
		Config.AgentName = Entry.AgentName;
		Config.ExecutablePath = Entry.ExecutablePath.FilePath;
		Config.Arguments = Entry.Arguments;
		Config.WorkingDirectory = Entry.WorkingDirectory.Path.IsEmpty() ? FPaths::ProjectDir() : Entry.WorkingDirectory.Path;
		Config.EnvironmentVariables = Entry.EnvironmentVariables;
		Config.ApiKey = Entry.ApiKey;
		Config.ModelId = Entry.ModelId;
		Config.bIsBuiltIn = false;
		Config.InstallInstructions = TEXT("Custom agent - check your configuration.");

		if (IsExecutableAvailable(Config.ExecutablePath, ResolvedPath))
		{
			Config.Status = EACPAgentStatus::Available;
			Config.StatusMessage = TEXT("Ready");
		}
		else
		{
			Config.Status = EACPAgentStatus::NotInstalled;
			Config.StatusMessage = FString::Printf(TEXT("Executable not found: %s"), *Entry.ExecutablePath.FilePath);
		}
		Configs.Add(Config);
	}

	return Configs;
}

FString UACPSettings::GetSavedModelForAgent(const FString& AgentName) const
{
	if (const FString* SavedModel = SelectedModelPerAgent.Find(AgentName))
	{
		return *SavedModel;
	}
	if (AgentName == LocalByokChatAgentName)
	{
		if (const FString* LegacySavedModel = SelectedModelPerAgent.Find(LegacyChatGatewayAgentName))
		{
			return *LegacySavedModel;
		}
	}
	return FString();
}

void UACPSettings::SaveModelForAgent(const FString& AgentName, const FString& ModelId)
{
	if (ModelId.IsEmpty())
	{
		SelectedModelPerAgent.Remove(AgentName);
	}
	else
	{
		SelectedModelPerAgent.Add(AgentName, ModelId);
	}
	if (AgentName == LocalByokChatAgentName)
	{
		SelectedModelPerAgent.Remove(LegacyChatGatewayAgentName);
	}

	SavePreferences();
}

FString UACPSettings::GetSavedModeForAgent(const FString& AgentName) const
{
	if (const FString* SavedMode = SelectedModePerAgent.Find(AgentName))
	{
		return *SavedMode;
	}
	return FString();
}

void UACPSettings::SaveModeForAgent(const FString& AgentName, const FString& ModeId)
{
	if (ModeId.IsEmpty())
	{
		SelectedModePerAgent.Remove(AgentName);
	}
	else
	{
		SelectedModePerAgent.Add(AgentName, ModeId);
	}

	SavePreferences();
}

FString UACPSettings::GetSavedReasoningForAgent(const FString& AgentName) const
{
	if (const FString* SavedReasoning = SelectedReasoningPerAgent.Find(AgentName))
	{
		return *SavedReasoning;
	}
	return FString();
}

void UACPSettings::SaveReasoningForAgent(const FString& AgentName, const FString& ReasoningLevel)
{
	if (ReasoningLevel.IsEmpty())
	{
		SelectedReasoningPerAgent.Remove(AgentName);
	}
	else
	{
		SelectedReasoningPerAgent.Add(AgentName, ReasoningLevel);
	}

	SavePreferences();
}

bool UACPSettings::HasOpenRouterAuth() const
{
	return !GetChatProviderApiKey(TEXT("openrouter")).IsEmpty();
}

bool UACPSettings::HasMeshyAuth() const
{
	return !MeshyApiKey.TrimStartAndEnd().IsEmpty();
}

bool UACPSettings::HasFalAuth() const
{
	return !FalApiKey.TrimStartAndEnd().IsEmpty();
}

FString UACPSettings::GetOpenRouterAuthToken() const
{
	return GetChatProviderApiKey(TEXT("openrouter"));
}

FString UACPSettings::GetMeshyAuthToken() const
{
	return MeshyApiKey.TrimStartAndEnd();
}

FString UACPSettings::GetFalAuthToken() const
{
	return FalApiKey.TrimStartAndEnd();
}

FString UACPSettings::GetOpenRouterChatCompletionsUrl() const
{
	return BuildOpenRouterUrl(GetChatProviderBaseUrlOverride(TEXT("openrouter")), TEXT("/chat/completions"));
}

FString UACPSettings::GetOpenRouterImageGenerationUrl() const
{
	return BuildOpenRouterUrl(GetChatProviderBaseUrlOverride(TEXT("openrouter")), TEXT("/chat/completions"));
}

FString UACPSettings::GetOpenRouterModelsUrl() const
{
	return BuildOpenRouterUrl(GetChatProviderBaseUrlOverride(TEXT("openrouter")), TEXT("/models"));
}

FString UACPSettings::GetMeshyBaseUrl() const
{
	return TEXT("https://api.meshy.ai");
}

FString UACPSettings::GetFalSubmitUrl() const
{
	return TEXT("https://queue.fal.run");
}

FString UACPSettings::GetProfileSystemPromptAppend() const
{
	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	return Prefs ? Prefs->ACPSystemPromptAppend : FString();
}

// Forward decl — implementation lower in this TU.
static ESystemPromptDelivery StringToDelivery(const FString& S);

ESystemPromptDelivery UACPSettings::GetSystemPromptDeliveryForAgent(const FString& AgentName) const
{
	if (const ESystemPromptDelivery* Found = SystemPromptDeliveryPerAgent.Find(AgentName))
	{
		return *Found;
	}
	return ESystemPromptDelivery::SessionMeta;
}

ESystemPromptDelivery UACPSettings::GetSystemPromptDeliveryForAgent(const FACPAgentConfig& Config) const
{
	// 1. Explicit per-agent user override (highest priority).
	if (const ESystemPromptDelivery* Found = SystemPromptDeliveryPerAgent.Find(Config.AgentName))
	{
		return *Found;
	}

	// 2. Hard agent constraint declared in quirks (e.g. codex-acp doesn't read _meta.systemPrompt).
	// Keyed by stable RegistryId — survives display-name renames in the registry.
	if (!Config.RegistryId.IsEmpty())
	{
		const FACPAgentQuirks& Quirks = FACPAgentQuirksMap::GetQuirks(Config.RegistryId);
		if (!Quirks.SystemPromptDeliveryOverride.IsEmpty())
		{
			return StringToDelivery(Quirks.SystemPromptDeliveryOverride);
		}
	}

	// 3. Default.
	return ESystemPromptDelivery::SessionMeta;
}

void UACPSettings::EnsureBuiltInSystemPromptDeliveryDefaults()
{
	// Pre-populate defaults for agents known to not support _meta.systemPrompt.
	// These show up in the UI so users can see and override them.
	// NOTE: Do NOT call SaveConfig() here — this runs from the constructor before
	// UE has loaded the config file, so SaveConfig() would overwrite saved user
	// settings (API keys, checkboxes, etc.) with default/empty values.

	if (!SystemPromptDeliveryPerAgent.Contains(TEXT("OpenCode")))
	{
		SystemPromptDeliveryPerAgent.Add(TEXT("OpenCode"), ESystemPromptDelivery::FirstUserMessage);
	}
}

// ── Chat Provider Helpers ────────────────────────────────────────────

FChatProviderSettings& UACPSettings::GetOrCreateChatProviderSettings(const FString& ProviderId)
{
	for (FChatProviderSettings& Row : ChatProviders)
	{
		if (Row.ProviderId == ProviderId)
		{
			return Row;
		}
	}
	FChatProviderSettings NewRow;
	NewRow.ProviderId = ProviderId;
	return ChatProviders.Add_GetRef(MoveTemp(NewRow));
}

const FChatProviderSettings* UACPSettings::FindChatProviderSettings(const FString& ProviderId) const
{
	for (const FChatProviderSettings& Row : ChatProviders)
	{
		if (Row.ProviderId == ProviderId)
		{
			return &Row;
		}
	}
	return nullptr;
}

FString UACPSettings::GetChatProviderApiKey(const FString& ProviderId) const
{
	const FChatProviderSettings* Row = FindChatProviderSettings(ProviderId);
	return Row ? Row->ApiKey : FString();
}

FString UACPSettings::GetChatProviderBaseUrlOverride(const FString& ProviderId) const
{
	const FChatProviderSettings* Row = FindChatProviderSettings(ProviderId);
	return Row ? Row->BaseUrlOverride : FString();
}

bool UACPSettings::IsChatProviderConfigured(const FString& ProviderId, bool bRequiresApiKey) const
{
	const FChatProviderSettings* Row = FindChatProviderSettings(ProviderId);
	if (!Row || !Row->bEnabled)
	{
		return false;
	}
	if (bRequiresApiKey && Row->ApiKey.IsEmpty())
	{
		return false;
	}
	return true;
}

void UACPSettings::SetChatProviderApiKey(const FString& ProviderId, const FString& ApiKey)
{
	FChatProviderSettings& Row = GetOrCreateChatProviderSettings(ProviderId);
	Row.ApiKey = ApiKey.TrimStartAndEnd();
	SaveConfig();
}

void UACPSettings::SetChatProviderBaseUrlOverride(const FString& ProviderId, const FString& BaseUrl)
{
	FChatProviderSettings& Row = GetOrCreateChatProviderSettings(ProviderId);
	Row.BaseUrlOverride = BaseUrl.TrimStartAndEnd();
	SaveConfig();
}

void UACPSettings::SetChatProviderEnabled(const FString& ProviderId, bool bEnabled)
{
	FChatProviderSettings& Row = GetOrCreateChatProviderSettings(ProviderId);
	Row.bEnabled = bEnabled;
	SaveConfig();
}

FUserChatProvider* UACPSettings::FindUserChatProvider(const FString& ProviderId)
{
	for (FUserChatProvider& Prov : UserChatProviders)
	{
		if (Prov.Id == ProviderId)
		{
			return &Prov;
		}
	}
	return nullptr;
}

const FUserChatProvider* UACPSettings::FindUserChatProvider(const FString& ProviderId) const
{
	for (const FUserChatProvider& Prov : UserChatProviders)
	{
		if (Prov.Id == ProviderId)
		{
			return &Prov;
		}
	}
	return nullptr;
}

FString UACPSettings::CreateUserChatProvider(const FString& DisplayName, const FString& BaseUrl)
{
	FUserChatProvider Prov;
	Prov.Id = TEXT("userprovider_") + FGuid::NewGuid().ToString(EGuidFormats::Short).ToLower();
	Prov.DisplayName = DisplayName.TrimStartAndEnd();
	Prov.BaseUrl = BaseUrl.TrimStartAndEnd();
	UserChatProviders.Add(Prov);

	// Seed a settings row so the provider is enabled by default.
	FChatProviderSettings& Row = GetOrCreateChatProviderSettings(Prov.Id);
	Row.bEnabled = true;

	SaveConfig();
	return Prov.Id;
}

void UACPSettings::DeleteUserChatProvider(const FString& ProviderId)
{
	UserChatProviders.RemoveAll([&](const FUserChatProvider& P) { return P.Id == ProviderId; });
	ChatProviders.RemoveAll([&](const FChatProviderSettings& R) { return R.ProviderId == ProviderId; });
	SaveConfig();
}

void UACPSettings::UpdateUserChatProvider(const FString& ProviderId, TFunctionRef<void(FUserChatProvider&)> Mutator)
{
	if (FUserChatProvider* Prov = FindUserChatProvider(ProviderId))
	{
		Mutator(*Prov);
		SaveConfig();
	}
}

void UACPSettings::AddUserChatProviderModel(const FString& ProviderId, const FChatModelEntry& Entry)
{
	if (FUserChatProvider* Prov = FindUserChatProvider(ProviderId))
	{
		for (const FChatModelEntry& Existing : Prov->StaticModels)
		{
			if (Existing.ModelId == Entry.ModelId) return; // no duplicates
		}
		Prov->StaticModels.Add(Entry);
		SaveConfig();
	}
}

void UACPSettings::RemoveUserChatProviderModel(const FString& ProviderId, const FString& ModelId)
{
	if (FUserChatProvider* Prov = FindUserChatProvider(ProviderId))
	{
		Prov->StaticModels.RemoveAll([&](const FChatModelEntry& E) { return E.ModelId == ModelId; });
		SaveConfig();
	}
}

void UACPSettings::RefreshAgentStatus()
{
	FScopeLock Lock(&StatusCacheLock);
	CachedAgentStatus.Empty();
	LastStatusRefresh = FDateTime::UtcNow();

	TArray<FACPAgentConfig> Configs = GetAgentConfigs();
	for (const FACPAgentConfig& Config : Configs)
	{
		CachedAgentStatus.Add(Config.AgentName, Config.Status);
	}
}

void UACPSettings::InvalidateAgentStatusCache()
{
	FScopeLock Lock(&StatusCacheLock);
	CachedAgentStatus.Empty();
	LastStatusRefresh = FDateTime::MinValue();
}

bool UACPSettings::IsAgentStatusStale() const
{
	FScopeLock Lock(&StatusCacheLock);
	if (CachedAgentStatus.Num() == 0)
	{
		return true;
	}
	FDateTime Now = FDateTime::UtcNow();
	return (Now - LastStatusRefresh).GetTotalSeconds() > StatusCacheTTLSeconds;
}

// ============================================================================
// Unified Preferences Persistence (~/.agentintegrationkit/preferences.json)
// UE's SaveConfig() writes to Config/DefaultXxx.ini but the Saved config
// at Saved/Config/<Platform>/Xxx.ini takes precedence and can override with
// stale values. This JSON file is the single source of truth for all
// WebUI-mutated settings.
// ============================================================================

static FString GetPreferencesDir()
{
	FString HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (HomeDir.IsEmpty())
	{
		HomeDir = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	}
	return FPaths::Combine(HomeDir, TEXT(".agentintegrationkit"));
}

static FString GetPreferencesFilePath()
{
	return FPaths::Combine(GetPreferencesDir(), TEXT("preferences.json"));
}

// Helpers: serialize TMap<FString,FString> ↔ FJsonObject
static TSharedRef<FJsonObject> MapToJson(const TMap<FString, FString>& Map)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	for (const auto& Pair : Map) { Obj->SetStringField(Pair.Key, Pair.Value); }
	return Obj;
}

static void JsonToMap(const TSharedPtr<FJsonObject>& Obj, TMap<FString, FString>& OutMap)
{
	OutMap.Empty();
	if (!Obj.IsValid()) return;
	for (const auto& Pair : Obj->Values)
	{
		FString Val;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
		{
			OutMap.Add(FString(*Pair.Key), Val);
		}
	}
}

// Helpers: serialize TArray/TSet<FString> ↔ JSON array
static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Arr)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : Arr) { Out.Add(MakeShared<FJsonValueString>(S)); }
	return Out;
}

static TArray<TSharedPtr<FJsonValue>> StringSetToJson(const TSet<FString>& Set)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : Set) { Out.Add(MakeShared<FJsonValueString>(S)); }
	return Out;
}

static void JsonToStringArray(const TArray<TSharedPtr<FJsonValue>>* Arr, TArray<FString>& Out)
{
	Out.Empty();
	if (!Arr) return;
	for (const auto& Val : *Arr)
	{
		FString S;
		if (Val.IsValid() && Val->TryGetString(S) && !S.IsEmpty()) { Out.Add(S); }
	}
}

static void JsonToStringSet(const TArray<TSharedPtr<FJsonValue>>* Arr, TSet<FString>& Out)
{
	Out.Empty();
	if (!Arr) return;
	for (const auto& Val : *Arr)
	{
		FString S;
		if (Val.IsValid() && Val->TryGetString(S) && !S.IsEmpty()) { Out.Add(S); }
	}
}

// ESystemPromptDelivery ↔ string
static FString DeliveryToString(ESystemPromptDelivery D)
{
	switch (D)
	{
	case ESystemPromptDelivery::FirstUserMessage:  return TEXT("FirstUserMessage");
	case ESystemPromptDelivery::EveryUserMessage:  return TEXT("EveryUserMessage");
	default:                                       return TEXT("SessionMeta");
	}
}

static ESystemPromptDelivery StringToDelivery(const FString& S)
{
	if (S == TEXT("FirstUserMessage"))  return ESystemPromptDelivery::FirstUserMessage;
	if (S == TEXT("EveryUserMessage"))  return ESystemPromptDelivery::EveryUserMessage;
	return ESystemPromptDelivery::SessionMeta;
}

void UACPSettings::SavePreferences()
{
	const FString FilePath = GetPreferencesFilePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	// Installed agents
	Root->SetArrayField(TEXT("installedAgentIds"), StringArrayToJson(InstalledAgentIds));

	// Onboarding
	Root->SetBoolField(TEXT("onboardingCompleted"), bOnboardingCompleted);
	Root->SetBoolField(TEXT("languageOnboardingCompleted"), bLanguageOnboardingCompleted);

	// Last used agent
	Root->SetStringField(TEXT("lastUsedAgent"), LastUsedAgentName);

	// Per-agent selections
	Root->SetObjectField(TEXT("selectedModels"), MapToJson(SelectedModelPerAgent));
	Root->SetObjectField(TEXT("selectedModes"), MapToJson(SelectedModePerAgent));
	Root->SetObjectField(TEXT("selectedReasoning"), MapToJson(SelectedReasoningPerAgent));

	// System prompt delivery per agent
	TSharedRef<FJsonObject> DeliveryObj = MakeShared<FJsonObject>();
	for (const auto& Pair : SystemPromptDeliveryPerAgent)
	{
		DeliveryObj->SetStringField(Pair.Key, DeliveryToString(Pair.Value));
	}
	Root->SetObjectField(TEXT("systemPromptDelivery"), DeliveryObj);

	// Serialize
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Root, Writer);

	if (FFileHelper::SaveStringToFile(JsonString, *FilePath))
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("ACPSettings: Saved preferences to %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("ACPSettings: Failed to save preferences to %s"), *FilePath);
	}
}

void UACPSettings::LoadPreferences()
{
	const FString FilePath = GetPreferencesFilePath();

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		// Try migrating from legacy files
		FString LegacyDir = GetPreferencesDir();

		// Migrate installed_agents.json
		FString LegacyInstalled;
		if (FFileHelper::LoadFileToString(LegacyInstalled, *FPaths::Combine(LegacyDir, TEXT("installed_agents.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(LegacyInstalled);
			if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Obj->TryGetArrayField(TEXT("installed"), Arr))
				{
					JsonToStringArray(Arr, InstalledAgentIds);
				}
			}
		}

		// Migrate onboarding.json
		FString LegacyOnboarding;
		if (FFileHelper::LoadFileToString(LegacyOnboarding, *FPaths::Combine(LegacyDir, TEXT("onboarding.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(LegacyOnboarding);
			if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
			{
				Obj->TryGetBoolField(TEXT("completed"), bOnboardingCompleted);
			}
		}

		// Save the migrated data as preferences.json
		if (InstalledAgentIds.Num() > 0 || bOnboardingCompleted)
		{
			SavePreferences();
			UE_LOG(LogNeoStackAI, Log, TEXT("ACPSettings: Migrated legacy JSON files to preferences.json"));
		}
		else
		{
			UE_LOG(LogNeoStackAI, Log, TEXT("ACPSettings: No preferences.json found"));
		}
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("ACPSettings: Failed to parse preferences.json"));
		return;
	}

	// Installed agents
	const TArray<TSharedPtr<FJsonValue>>* InstalledArr = nullptr;
	if (Root->TryGetArrayField(TEXT("installedAgentIds"), InstalledArr))
	{
		JsonToStringArray(InstalledArr, InstalledAgentIds);
	}

	// Onboarding
	Root->TryGetBoolField(TEXT("onboardingCompleted"), bOnboardingCompleted);
	Root->TryGetBoolField(TEXT("languageOnboardingCompleted"), bLanguageOnboardingCompleted);

	// Last used agent
	Root->TryGetStringField(TEXT("lastUsedAgent"), LastUsedAgentName);
	if (LastUsedAgentName == LegacyChatGatewayAgentName)
	{
		LastUsedAgentName = LocalByokChatAgentName;
	}

	// Per-agent selections (use TryGetObjectField — GetObjectField asserts if missing)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Root->TryGetObjectField(TEXT("selectedModels"), Obj))    JsonToMap(*Obj, SelectedModelPerAgent);
		if (Root->TryGetObjectField(TEXT("selectedModes"), Obj))     JsonToMap(*Obj, SelectedModePerAgent);
		if (Root->TryGetObjectField(TEXT("selectedReasoning"), Obj)) JsonToMap(*Obj, SelectedReasoningPerAgent);
	}

	// System prompt delivery per agent
	{
		const TSharedPtr<FJsonObject>* DeliveryPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("systemPromptDelivery"), DeliveryPtr))
		{
			SystemPromptDeliveryPerAgent.Empty();
			for (const auto& Pair : (*DeliveryPtr)->Values)
			{
				FString Val;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Val))
				{
					SystemPromptDeliveryPerAgent.Add(FString(*Pair.Key), StringToDelivery(Val));
				}
			}
		}
	}

	const FString LoadedMsg = FString::Printf(
		TEXT("ACPSettings: Loaded preferences (%d installed agents, %d user chat providers, lastUsed=%s, onboarding=%s)"),
		InstalledAgentIds.Num(), UserChatProviders.Num(), *LastUsedAgentName,
		bOnboardingCompleted ? TEXT("done") : TEXT("pending"));
	UE_LOG(LogNeoStackAI, Log, TEXT("%s"), *LoadedMsg);
}

// Legacy wrappers — delegate to unified system
void UACPSettings::SaveInstalledAgentIds() { SavePreferences(); }
void UACPSettings::LoadInstalledAgentIds() { LoadPreferences(); }

#undef LOCTEXT_NAMESPACE
