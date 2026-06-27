// Copyright 2026 Betide Studio. All Rights Reserved.

#include "WebUIBridge.h"
#include "HAL/PlatformMisc.h"
#include "TerminalManager.h"
#include "NeoStackAIModule.h"
#include "EntitlementClient.h"
#include "NeoStackDeviceAuth.h"
#include "BuildVariant.h"
#include "Utils/NeoTypeResolver.h"
#include "ACPAgentManager.h"
#include "ACPClient.h"
#include "ACPSessionManager.h"
#include "ACPClaudeCodeHistoryReader.h"
#include "ACPCodexHistoryReader.h"
#include "ACPCopilotHistoryReader.h"
#include "ACPGeminiHistoryReader.h"
#include "ACPSettings.h"
#include "ACPTypes.h"
#include "UserPreferencesSubsystem.h"
#include "ACPTerminalResumeCommand.h"
#include "AgentInstaller.h"
#include "AgentUsageMonitor.h"
#include "ACPRegistryClient.h"
#include "ACPAttachmentManager.h"
#include "ProjectIndexManager.h"
#include "ACPClipboardImageReader.h"
#include "AgentRuntime/AgentRuntimeService.h"
#include "Chat/ChatModelRegistry.h"
#include "Chat/ChatStore.h"
#include "Chat/ChatSessionManager.h"
#include "Chat/IChatProvider.h"
#include "Extensions/NeoStackExtensionCatalog.h"
#include "Extensions/NeoStackExtensionInstaller.h"
#include "Extensions/NeoStackExtensionProjectService.h"
#include "Extensions/NeoStackExtensionRegistry.h"
#include "Skills/NeoStackSkill.h"
#include "Skills/NeoStackSkillInstaller.h"
#include "Skills/NeoStackSkillRegistry.h"
#include "MCPServer.h"
#include "MCPTypes.h"
#include "NSAIAnalytics.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "ISettingsModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "SourceCodeNavigation.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Lua/NeoLuaState.h"
#include "Lua/LuaEditorActions.h"
#include "Providers/GenerativeProvider.h"
#include "Providers/GenerativeProviderRegistry.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlWindowsModule.h"
#include "SourceControlWindows.h"
#include "UnrealEdMisc.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/CoreStyle.h"
#include "Editor.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"

#if PLATFORM_MAC
#include <CoreText/CoreText.h>
#endif

namespace
{
constexpr float WebUIStreamFlushIntervalSeconds = 1.0f / 30.0f;

FString ExtensionRuntimeStateToString(const FNeoStackManagedExtension& Extension)
{
	if (!Extension.bEnabledInProject)
	{
		return TEXT("disabled");
	}

	switch (Extension.RuntimeState)
	{
	case ENeoStackExtensionState::Registered:
		return TEXT("registered");
	case ENeoStackExtensionState::Active:
		return TEXT("active");
	case ENeoStackExtensionState::Unavailable:
		return TEXT("unavailable");
	case ENeoStackExtensionState::Incompatible:
		return TEXT("incompatible");
	case ENeoStackExtensionState::Failed:
		return TEXT("failed");
	default:
		return TEXT("unknown");
	}
}

} // namespace

// Helper: serialize a FJsonObject to compact JSON string
static FString JsonToString(const TSharedRef<FJsonObject>& Obj)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Obj, Writer);
	return Out;
}

// Helper: serialize a JSON array to compact string
static FString JsonArrayToString(const TArray<TSharedPtr<FJsonValue>>& Arr)
{
	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}

static TArray<FACPRemoteSessionEntry> GetLocalHistorySessionsForAgent(const FString& AgentName, const FString& WorkingDirectory)
{
	if (AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Gemini"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPGeminiHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogNeoStackAI, Log, TEXT("LocalHistory: Found %d Gemini sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	if (AgentName == TEXT("Copilot CLI") || AgentName == TEXT("GitHub Copilot"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPCopilotHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogNeoStackAI, Log, TEXT("LocalHistory: Found %d Copilot sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		FString AbsWorkDir = FPaths::ConvertRelativePathToFull(WorkingDirectory);
		TArray<FACPRemoteSessionEntry> Sessions = FACPCodexHistoryReader::ListSessions(AbsWorkDir);
		UE_LOG(LogNeoStackAI, Log, TEXT("LocalHistory: Found %d Codex sessions for '%s'"), Sessions.Num(), *AbsWorkDir);
		return Sessions;
	}

	return TArray<FACPRemoteSessionEntry>();
}

static bool IsLikelyMonospaceFontName(const FString& Family)
{
	const FString Lower = Family.ToLower();
	return Lower.Contains(TEXT("mono")) ||
		Lower.Contains(TEXT("code")) ||
		Lower.Contains(TEXT("console")) ||
		Lower.Contains(TEXT("consolas")) ||
		Lower.Contains(TEXT("courier")) ||
		Lower.Contains(TEXT("menlo")) ||
		Lower.Contains(TEXT("monaco")) ||
		Lower.Contains(TEXT("cascadia")) ||
		Lower.Contains(TEXT("meslo")) ||
		Lower.Contains(TEXT("hack")) ||
		Lower.Contains(TEXT("source code"));
}

static void AddSystemFontFamily(const FString& RawFamily, TSet<FString>& Seen, TArray<FString>& OutFamilies)
{
	FString Family = RawFamily;
	Family.TrimStartAndEndInline();
	if (Family.Len() >= 2 && Family.StartsWith(TEXT("\"")) && Family.EndsWith(TEXT("\"")))
	{
		Family = Family.Mid(1, Family.Len() - 2);
	}

	if (Family.IsEmpty() || Family.StartsWith(TEXT(".")) || Family.Len() > 128)
	{
		return;
	}

	if (!Seen.Contains(Family))
	{
		Seen.Add(Family);
		OutFamilies.Add(Family);
	}
}

static void AddCommaSeparatedFontFamilies(const FString& RawFamilies, TSet<FString>& Seen, TArray<FString>& OutFamilies)
{
	TArray<FString> Parts;
	RawFamilies.ParseIntoArray(Parts, TEXT(","), true);
	for (const FString& Part : Parts)
	{
		AddSystemFontFamily(Part, Seen, OutFamilies);
	}
}

static void AddFallbackSystemFonts(TSet<FString>& Seen, TArray<FString>& OutFamilies)
{
#if PLATFORM_MAC
	const TCHAR* Defaults[] = {
		TEXT("SF Pro Text"), TEXT("SF Pro Display"), TEXT("SF Mono"), TEXT("Menlo"),
		TEXT("Monaco"), TEXT("Helvetica Neue"), TEXT("Arial"), TEXT("Courier New")
	};
#elif PLATFORM_WINDOWS
	const TCHAR* Defaults[] = {
		TEXT("Segoe UI"), TEXT("Segoe UI Variable"), TEXT("Cascadia Mono"), TEXT("Consolas"),
		TEXT("Arial"), TEXT("Courier New"), TEXT("Calibri"), TEXT("Tahoma")
	};
#else
	const TCHAR* Defaults[] = {
		TEXT("Noto Sans"), TEXT("DejaVu Sans"), TEXT("Ubuntu"), TEXT("Cantarell"),
		TEXT("Noto Sans Mono"), TEXT("DejaVu Sans Mono"), TEXT("Liberation Mono"), TEXT("Monospace")
	};
#endif

	for (const TCHAR* Family : Defaults)
	{
		AddSystemFontFamily(Family, Seen, OutFamilies);
	}
}

static TArray<FString> EnumerateSystemFontFamilies()
{
	TSet<FString> Seen;
	TArray<FString> Families;

	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;

#if PLATFORM_MAC
	if (CFArrayRef FontFamilies = CTFontManagerCopyAvailableFontFamilyNames())
	{
		const CFIndex Count = CFArrayGetCount(FontFamilies);
		for (CFIndex Index = 0; Index < Count; ++Index)
		{
			CFStringRef FamilyRef = static_cast<CFStringRef>(CFArrayGetValueAtIndex(FontFamilies, Index));
			if (!FamilyRef)
			{
				continue;
			}

			char Buffer[512];
			if (CFStringGetCString(FamilyRef, Buffer, sizeof(Buffer), kCFStringEncodingUTF8))
			{
				AddSystemFontFamily(FString(UTF8_TO_TCHAR(Buffer)), Seen, Families);
			}
		}
		CFRelease(FontFamilies);
	}
#elif PLATFORM_WINDOWS
	const FString Args = TEXT("-NoProfile -ExecutionPolicy Bypass -Command \"Add-Type -AssemblyName System.Drawing; [System.Drawing.FontFamily]::Families | ForEach-Object { $_.Name }\"");
	FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *Args, &ReturnCode, &StdOut, &StdErr);
#else
	FPlatformProcess::ExecProcess(TEXT("fc-list"), TEXT(": family"), &ReturnCode, &StdOut, &StdErr);
#endif

#if !PLATFORM_MAC
	if (ReturnCode == 0 && !StdOut.IsEmpty())
	{
		TArray<FString> Lines;
		StdOut.ParseIntoArrayLines(Lines, true);
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty())
			{
				continue;
			}

#if PLATFORM_MAC
			if (Line.StartsWith(TEXT("Family:")))
			{
				Line = Line.RightChop(7);
				AddSystemFontFamily(Line, Seen, Families);
			}
#elif PLATFORM_LINUX
			AddCommaSeparatedFontFamilies(Line, Seen, Families);
#else
			AddSystemFontFamily(Line, Seen, Families);
#endif
		}
	}
#endif

	AddFallbackSystemFonts(Seen, Families);
	Families.Sort([](const FString& A, const FString& B) {
		return A.Compare(B, ESearchCase::IgnoreCase) < 0;
	});
	return Families;
}

static bool LoadLocalHistorySession(
	const FString& AgentName,
	const FString& SessionId,
	TArray<FACPChatMessage>& OutMessages,
	FACPRemoteSessionEntry* OutMetadata = nullptr)
{
	if (AgentName == TEXT("Gemini CLI"))
	{
		return FACPGeminiHistoryReader::ParseSession(SessionId, OutMessages, OutMetadata);
	}

	if (AgentName == TEXT("Copilot CLI"))
	{
		return FACPCopilotHistoryReader::ParseSession(SessionId, OutMessages, OutMetadata);
	}

	if (AgentName == TEXT("Codex CLI") || AgentName == TEXT("Codex"))
	{
		FString WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FString RolloutPath = FACPCodexHistoryReader::GetSessionJsonlPath(SessionId, WorkDir);
		if (!RolloutPath.IsEmpty())
		{
			return FACPCodexHistoryReader::ParseSessionJsonl(RolloutPath, OutMessages);
		}
	}

	return false;
}

static bool AgentUsesLaunchResumeHistory(const FString& AgentName)
{
	return AgentName == TEXT("Gemini CLI") || AgentName == TEXT("Copilot CLI");
}

static void SetSessionAgentRegistryFields(
	FACPAgentManager& AgentMgr,
	FJsonObject& SessionObj,
	const FString& AgentName)
{
	FString RegistryId;
	if (FACPAgentConfig* Cfg = AgentMgr.GetAgentConfig(AgentName))
	{
		RegistryId = Cfg->RegistryId;
	}
	SessionObj.SetStringField(TEXT("registryId"), RegistryId);
	SessionObj.SetBoolField(
		TEXT("terminalResumeSupported"),
		FACPTerminalResumeCommand::IsSupported(RegistryId, AgentName));
}

// ── Agent Discovery ──────────────────────────────────────────────────

FString UWebUIBridge::GetAgents()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();

	TArray<TSharedPtr<FJsonValue>> AgentsArray;

	for (const FACPAgentConfig& Config : Configs)
	{
		TSharedPtr<FJsonObject> AgentObj = MakeShared<FJsonObject>();
		AgentObj->SetStringField(TEXT("id"), Config.AgentName);
		AgentObj->SetStringField(TEXT("name"), Config.AgentName);

		// Map status enum to string
		FString StatusStr;
		switch (Config.Status)
		{
		case EACPAgentStatus::Available:     StatusStr = TEXT("available"); break;
		case EACPAgentStatus::NotInstalled:  StatusStr = TEXT("not_installed"); break;
		case EACPAgentStatus::MissingApiKey: StatusStr = TEXT("missing_key"); break;
		default:                             StatusStr = TEXT("unknown"); break;
		}
		AgentObj->SetStringField(TEXT("status"), StatusStr);
		AgentObj->SetStringField(TEXT("statusMessage"), Config.StatusMessage);
		AgentObj->SetBoolField(TEXT("isBuiltIn"), Config.bIsBuiltIn);
		AgentObj->SetBoolField(TEXT("isConnected"), AgentMgr.IsConnectedToAgent(Config.AgentName));
		AgentObj->SetStringField(TEXT("registryId"), Config.RegistryId);
		AgentObj->SetStringField(TEXT("description"), Config.InstallInstructions);

		// Icon URL: local override wins (synthetic/built-in agents), else registry.
		if (!Config.IconUrl.IsEmpty())
		{
			AgentObj->SetStringField(TEXT("iconUrl"), Config.IconUrl);
		}
		else if (!Config.RegistryId.IsEmpty())
		{
			const FACPRegistryAgent* RegAgent = FACPRegistryClient::Get().FindAgent(Config.RegistryId);
			if (RegAgent)
			{
				AgentObj->SetStringField(TEXT("iconUrl"), RegAgent->IconUrl);
			}
		}

		AgentsArray.Add(MakeShared<FJsonValueObject>(AgentObj));
	}

	return JsonArrayToString(AgentsArray);
}

FString UWebUIBridge::GetLastUsedAgent()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->LastUsedAgentName;
	}
	return FString();
}

// ── Onboarding ──────────────────────────────────────────────────────
// State persisted via unified preferences.json (not UE SaveConfig)

bool UWebUIBridge::GetOnboardingCompleted()
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return true;
	}

	// Already loaded from preferences.json by LoadPreferences()
	if (Settings->bOnboardingCompleted)
	{
		return true;
	}

	// Auto-upgrade: if they've used the plugin before, skip onboarding
	if (!Settings->LastUsedAgentName.IsEmpty())
	{
		SetOnboardingCompleted();
		return true;
	}

	return false;
}

void UWebUIBridge::SetOnboardingCompleted()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->bOnboardingCompleted = true;
		Settings->SavePreferences();
	}
}

FString UWebUIBridge::GetDefaultLanguage()
{
	// FPlatformMisc returns a BCP-47-ish tag (e.g. "en-US", "ja-JP", "pt-BR").
	// JS side normalizes to one of the 18 supported locales.
	return FPlatformMisc::GetDefaultLanguage();
}

FString UWebUIBridge::ListSystemFonts()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> FontsArray;

	for (const FString& Family : EnumerateSystemFontFamilies())
	{
		TSharedRef<FJsonObject> FontObj = MakeShared<FJsonObject>();
		FontObj->SetStringField(TEXT("family"), Family);
		FontObj->SetBoolField(TEXT("isMonospace"), IsLikelyMonospaceFontName(Family));
		FontsArray.Add(MakeShared<FJsonValueObject>(FontObj));
	}

	Root->SetArrayField(TEXT("fonts"), FontsArray);
#if PLATFORM_MAC
	Root->SetStringField(TEXT("platform"), TEXT("mac"));
#elif PLATFORM_WINDOWS
	Root->SetStringField(TEXT("platform"), TEXT("windows"));
#elif PLATFORM_LINUX
	Root->SetStringField(TEXT("platform"), TEXT("linux"));
#else
	Root->SetStringField(TEXT("platform"), TEXT("unknown"));
#endif
	return JsonToString(Root);
}

bool UWebUIBridge::GetLanguageOnboardingCompleted()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->bLanguageOnboardingCompleted;
	}
	return true;
}

void UWebUIBridge::SetLanguageOnboardingCompleted()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->bLanguageOnboardingCompleted = true;
		Settings->SavePreferences();
	}
}

// ── Session Lifecycle ────────────────────────────────────────────────

FString UWebUIBridge::CreateSession(const FString& AgentName)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	const FString ModelId = AgentMgr.IsChatGatewayAgent(AgentName)
		? FChatModelRegistry::Get().GetSelectedModel()
		: FString();
	FString SessionId = SessionMgr.CreateSession(AgentName, ModelId);

	FNeoStackAgentRuntimeCreateOptions RuntimeOptions;
	RuntimeOptions.SessionId = SessionId;
	RuntimeOptions.AgentName = AgentName;
	RuntimeOptions.WorkingDirectory = FPaths::ProjectDir();
	RuntimeOptions.bForceFreshProcess = (AgentName == TEXT("Gemini CLI"));
	FNeoStackAgentRuntimeService::Get().CreateSession(RuntimeOptions);

	// Persist agent as last-used
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->LastUsedAgentName = AgentName;
		Settings->SavePreferences();
	}

	// Analytics: track which agent was selected
	FNSAIAnalytics::Get().RecordAgentSelected(AgentName);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sessionId"), SessionId);
	Result->SetStringField(TEXT("agentName"), AgentName);

	return JsonToString(Result);
}

FString UWebUIBridge::GetSessions()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();

	TSet<FString> SeenSessionIds;
	TArray<TSharedPtr<FJsonValue>> SessionsArray;

	// Build a set of agent session IDs that correspond to active Unreal sessions.
	// This lets us deduplicate: remote lists use agent IDs, active sessions use Unreal GUIDs.
	TSet<FString> KnownAgentSessionIds;
	TMap<FString, const FACPActiveSession*> AgentIdToActiveSession;
	TArray<FString> ActiveIds = SessionMgr.GetActiveSessionIds();
	for (const FString& Id : ActiveIds)
	{
		const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
		if (Active && !Active->Metadata.AgentSessionId.IsEmpty())
		{
			KnownAgentSessionIds.Add(Active->Metadata.AgentSessionId);
			AgentIdToActiveSession.Add(Active->Metadata.AgentSessionId, Active);
		}
	}

	// 1. Active in-memory chat sessions (newly created, currently resumed)
	for (const FString& Id : ActiveIds)
	{
		if (SeenSessionIds.Contains(Id)) continue;
		SeenSessionIds.Add(Id);

		const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
		if (!Active) continue;

		TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
		SessionObj->SetStringField(TEXT("sessionId"), Active->Metadata.SessionId);
		SessionObj->SetStringField(TEXT("agentName"), Active->Metadata.AgentName);
		SessionObj->SetStringField(TEXT("title"), Active->Metadata.Title);
		SessionObj->SetNumberField(TEXT("messageCount"), Active->Metadata.MessageCount);
		if (Active->Metadata.CreatedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("createdAt"), Active->Metadata.CreatedAt.ToIso8601());
		}
		if (Active->Metadata.LastModifiedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("lastModifiedAt"), Active->Metadata.LastModifiedAt.ToIso8601());
		}
		SessionObj->SetBoolField(TEXT("isConnected"), Active->bIsConnected);
		SessionObj->SetBoolField(TEXT("isActive"), true);
		SessionObj->SetBoolField(TEXT("hasCustomTitle"), Active->Metadata.bHasCustomTitle);

		SetSessionAgentRegistryFields(AgentMgr, *SessionObj, Active->Metadata.AgentName);

		SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
	}

	// 2. Local NeoStack projection cache. This is the instant path used when
	// opening the panel or switching chats; provider/native history discovery
	// pushes updates separately and should not block this call.
	for (const FNeoStackStoredSession& Stored : FNeoStackChatStore::Get().ListSessions())
	{
		const FString& Id = Stored.Metadata.SessionId;
		if (SeenSessionIds.Contains(Id)) continue;
		SeenSessionIds.Add(Id);

		TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
		SessionObj->SetStringField(TEXT("sessionId"), Stored.Metadata.SessionId);
		SessionObj->SetStringField(TEXT("agentName"), Stored.Metadata.AgentName);
		SessionObj->SetStringField(TEXT("title"), Stored.Metadata.Title);
		SessionObj->SetNumberField(TEXT("messageCount"), Stored.Metadata.MessageCount);
		if (Stored.Metadata.CreatedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("createdAt"), Stored.Metadata.CreatedAt.ToIso8601());
		}
		if (Stored.Metadata.LastModifiedAt.GetTicks() > 0)
		{
			SessionObj->SetStringField(TEXT("lastModifiedAt"), Stored.Metadata.LastModifiedAt.ToIso8601());
		}
		SessionObj->SetBoolField(TEXT("isConnected"), false);
		SessionObj->SetBoolField(TEXT("isActive"), false);
		SessionObj->SetBoolField(TEXT("hasCustomTitle"), Stored.Metadata.bHasCustomTitle);
		SetSessionAgentRegistryFields(AgentMgr, *SessionObj, Stored.Metadata.AgentName);
		SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
	}

	// 3. Cached ACP remote sessions only. Do not scan provider history folders
	// here; those scans can be slow and are pushed through refresh callbacks.
	TArray<FString> AgentNames = AgentMgr.GetAvailableAgentNames();
	for (const FString& AgentName : AgentNames)
	{
		TArray<FACPRemoteSessionEntry> RemoteSessions = AgentMgr.GetCachedSessionList(AgentName);
		for (const FACPRemoteSessionEntry& Entry : RemoteSessions)
		{
			if (SeenSessionIds.Contains(Entry.SessionId)) continue;

			// If this remote session corresponds to an active Unreal session,
			// update the active session's title from the remote data and skip
			// the remote entry (the active session will be listed in section 2)
			if (KnownAgentSessionIds.Contains(Entry.SessionId))
			{
				if (const FACPActiveSession** ActivePtr = AgentIdToActiveSession.Find(Entry.SessionId))
				{
					if (!Entry.Title.IsEmpty())
					{
						SessionMgr.UpdateSessionTitle((*ActivePtr)->Metadata.SessionId, Entry.Title);
					}
				}
				continue;
			}

			SeenSessionIds.Add(Entry.SessionId);

			TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
			SessionObj->SetStringField(TEXT("sessionId"), Entry.SessionId);
			SessionObj->SetStringField(TEXT("agentName"), AgentName);
			SessionObj->SetStringField(TEXT("title"), Entry.Title);
			if (Entry.UpdatedAt.GetTicks() > 0)
			{
				SessionObj->SetStringField(TEXT("lastModifiedAt"), Entry.UpdatedAt.ToIso8601());
			}

			// Apply persisted custom title if available
			if (const FString* Persisted = SessionMgr.GetPersistedCustomTitle(Entry.SessionId))
			{
				SessionObj->SetStringField(TEXT("title"), *Persisted);
				SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
			}

			const FACPActiveSession* Active = SessionMgr.GetActiveSession(Entry.SessionId);
			SessionObj->SetBoolField(TEXT("isConnected"), Active ? Active->bIsConnected : false);
			SessionObj->SetBoolField(TEXT("isActive"), Active != nullptr);

			SetSessionAgentRegistryFields(AgentMgr, *SessionObj, AgentName);

			FACPSessionMetadata Metadata;
			Metadata.SessionId = Entry.SessionId;
			Metadata.AgentName = AgentName;
			Metadata.AgentSessionId = Entry.SessionId;
			Metadata.Title = SessionObj->GetStringField(TEXT("title"));
			Metadata.CreatedAt = Entry.UpdatedAt.GetTicks() > 0 ? Entry.UpdatedAt : FDateTime::Now();
			Metadata.LastModifiedAt = Metadata.CreatedAt;
			bool bHasCustomTitle = false;
			SessionObj->TryGetBoolField(TEXT("hasCustomTitle"), bHasCustomTitle);
			Metadata.bHasCustomTitle = bHasCustomTitle;
			FNeoStackChatStore::Get().UpsertSession(Metadata);

			SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
		}
	}

	return JsonArrayToString(SessionsArray);
}

FString UWebUIBridge::ResumeSession(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// If already active, just switch to it
	const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId);
	if (ActiveSession)
	{
		SessionMgr.SwitchToSession(SessionId);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("agentName"), ActiveSession->Metadata.AgentName);
		return JsonToString(Result);
	}

	// Find which agent owns this session by checking cached ACP lists
	FString AgentName;
	FACPSessionMetadata StoredMetadata;
	bool bHasStoredMetadata = false;
	if (FNeoStackChatStore::Get().LoadSession(SessionId, StoredMetadata))
	{
		bHasStoredMetadata = true;
		AgentName = StoredMetadata.AgentName;
	}
	if (AgentName.IsEmpty())
	{
		for (const FString& Name : AgentMgr.GetAvailableAgentNames())
		{
			TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
			Sessions.Append(GetLocalHistorySessionsForAgent(Name, FPaths::ProjectDir()));
			for (const FACPRemoteSessionEntry& Entry : Sessions)
			{
				if (Entry.SessionId == SessionId)
				{
					AgentName = Name;
					break;
				}
			}
			if (!AgentName.IsEmpty()) break;
		}
	}

	if (AgentName.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Session not found in any agent's session list"));
		return JsonToString(Result);
	}

	// Create empty active session — messages will arrive via ACP replay
	if (!SessionMgr.ResumeSession(SessionId))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to create active session"));
		return JsonToString(Result);
	}

	// Set agent name on the session metadata
	if (FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId))
	{
		Session->Metadata.AgentName = AgentName;
		Session->Metadata.AgentSessionId = bHasStoredMetadata && !StoredMetadata.AgentSessionId.IsEmpty()
			? StoredMetadata.AgentSessionId
			: SessionId;
		TArray<FACPChatMessage> StoredMessages;
		if (FNeoStackChatStore::Get().LoadMessages(SessionId, StoredMessages))
		{
			Session->Messages = MoveTemp(StoredMessages);
			Session->Metadata.MessageCount = Session->Messages.Num();
			Session->bIsLoadingHistory = false;
		}
		else if (AgentUsesLaunchResumeHistory(AgentName))
		{
			FACPRemoteSessionEntry Metadata;
			TArray<FACPChatMessage> Messages;
			if (LoadLocalHistorySession(AgentName, SessionId, Messages, &Metadata))
			{
				Session->Messages = MoveTemp(Messages);
				Session->Metadata.MessageCount = Session->Messages.Num();
				Session->Metadata.Title = Metadata.Title;
				if (Metadata.UpdatedAt.GetTicks() > 0)
				{
					Session->Metadata.LastModifiedAt = Metadata.UpdatedAt;
				}
			}
			Session->bIsLoadingHistory = false;
		}
	}

	FNeoStackAgentRuntimeResumeOptions RuntimeOptions;
	RuntimeOptions.SessionId = SessionId;
	RuntimeOptions.AgentName = AgentName;
	RuntimeOptions.WorkingDirectory = FPaths::ProjectDir();
	RuntimeOptions.bLaunchResume = AgentUsesLaunchResumeHistory(AgentName);
	const FNeoStackAgentRuntimeResumeResult RuntimeResult =
		FNeoStackAgentRuntimeService::Get().ResumeSession(RuntimeOptions);

	Result->SetBoolField(TEXT("success"), RuntimeResult.bStarted);
	Result->SetBoolField(TEXT("loading"), RuntimeResult.bLoading);
	Result->SetStringField(TEXT("agentName"), AgentName);
	if (!RuntimeResult.Error.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), RuntimeResult.Error);
	}
	return JsonToString(Result);
}

FString UWebUIBridge::GetSessionMessages(const FString& SessionId)
{
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);

	if (Session && Session->Messages.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> MessagesArray;
		for (const FACPChatMessage& Msg : Session->Messages)
		{
			TSharedPtr<FJsonObject> MsgJson = MessageToJson(Msg);
			if (MsgJson.IsValid())
			{
				MessagesArray.Add(MakeShared<FJsonValueObject>(MsgJson));
			}
		}

		return JsonArrayToString(MessagesArray);
	}

	{
		TArray<FACPChatMessage> StoredMessages;
		if (FNeoStackChatStore::Get().LoadMessages(SessionId, StoredMessages))
		{
			TArray<TSharedPtr<FJsonValue>> StoredMessagesArray;
			for (const FACPChatMessage& Msg : StoredMessages)
			{
				TSharedPtr<FJsonObject> MsgJson = MessageToJson(Msg);
				if (MsgJson.IsValid())
				{
					StoredMessagesArray.Add(MakeShared<FJsonValueObject>(MsgJson));
				}
			}
			return JsonArrayToString(StoredMessagesArray);
		}
	}

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	if (Session)
	{
		for (const FACPChatMessage& Msg : Session->Messages)
		{
			TSharedPtr<FJsonObject> MsgJson = MessageToJson(Msg);
			if (MsgJson.IsValid())
			{
				MessagesArray.Add(MakeShared<FJsonValueObject>(MsgJson));
			}
		}
	}

	return JsonArrayToString(MessagesArray);
}

FString UWebUIBridge::GetSessionTerminalResumeCommand(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	FString AgentName;
	if (const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId))
	{
		AgentName = ActiveSession->Metadata.AgentName;
	}
	else
	{
		for (const FString& Name : AgentMgr.GetAvailableAgentNames())
		{
			TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
			Sessions.Append(GetLocalHistorySessionsForAgent(Name, FPaths::ProjectDir()));
			for (const FACPRemoteSessionEntry& Entry : Sessions)
			{
				if (Entry.SessionId == SessionId)
				{
					AgentName = Name;
					break;
				}
			}
			if (!AgentName.IsEmpty())
			{
				break;
			}
		}
	}

	if (AgentName.IsEmpty())
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("error"), TEXT("Session not found"));
		return JsonToString(Result);
	}

	FString RegistryId;
	if (FACPAgentConfig* Cfg = AgentMgr.GetAgentConfig(AgentName))
	{
		RegistryId = Cfg->RegistryId;
	}

	FString CommandLine;
	if (!FACPTerminalResumeCommand::TryBuildCommandLine(RegistryId, AgentName, SessionId, CommandLine))
	{
		Result->SetBoolField(TEXT("supported"), false);
		Result->SetStringField(TEXT("agentName"), AgentName);
		Result->SetStringField(TEXT("registryId"), RegistryId);
		Result->SetStringField(TEXT("error"), TEXT("No terminal resume command for this agent"));
		return JsonToString(Result);
	}

	Result->SetBoolField(TEXT("supported"), true);
	Result->SetStringField(TEXT("command"), CommandLine);
	Result->SetStringField(TEXT("agentName"), AgentName);
	Result->SetStringField(TEXT("registryId"), RegistryId);
	return JsonToString(Result);
}

static FString MessageRoleToSummaryLabel(const EACPMessageRole Role)
{
	switch (Role)
	{
	case EACPMessageRole::User: return TEXT("User");
	case EACPMessageRole::Assistant: return TEXT("Assistant");
	case EACPMessageRole::System: return TEXT("System");
	default: return TEXT("Unknown");
	}
}


// ── Provider Settings ────────────────────────────────────────────────

FString UWebUIBridge::GetProviderSettings()
{
	// New backing: reads from FChatModelRegistry + FChatProviderSettings.
	// The returned JSON shape is kept compatible with the old Svelte settings
	// panel: {priority, providers: [{id, name, description, requiresApiKey,
	// defaultBaseUrl, defaultModel, hasApiKey, apiKeyMasked, baseUrl,
	// isUserDefined, supportsModelDiscovery, enableModelDiscovery, configured,
	// models: [...]}]}. The `priority` field is retained as an ordering hint
	// (insertion order of registered providers) even though the new layer
	// does not do priority-based routing.

	const UACPSettings* Settings = UACPSettings::Get();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	const TArray<TSharedRef<IChatProvider>> AllProviders = FChatModelRegistry::Get().GetAllProviders();

	TArray<TSharedPtr<FJsonValue>> PriorityArray;
	for (const TSharedRef<IChatProvider>& Prov : AllProviders)
	{
		const FString ProviderId = Prov->GetId();
		const FChatProviderSettings* Row = Settings ? Settings->FindChatProviderSettings(ProviderId) : nullptr;
		if (!Row || Row->bEnabled)
		{
			PriorityArray.Add(MakeShared<FJsonValueString>(ProviderId));
		}
	}
	Result->SetArrayField(TEXT("priority"), PriorityArray);

	TArray<TSharedPtr<FJsonValue>> ProvidersArray;
	for (const TSharedRef<IChatProvider>& Prov : AllProviders)
	{
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		const FString ProviderId = Prov->GetId();

		P->SetStringField(TEXT("id"), ProviderId);
		P->SetStringField(TEXT("name"), Prov->GetDisplayName());
		P->SetStringField(TEXT("description"), Prov->GetDescription());
		P->SetBoolField(TEXT("requiresApiKey"), Prov->RequiresApiKey());
		P->SetStringField(TEXT("defaultBaseUrl"), TEXT("")); // interface no longer exposes this; UI shows resolved baseUrl instead
		P->SetStringField(TEXT("defaultModel"), TEXT(""));
		P->SetBoolField(TEXT("supportsModelDiscovery"), Prov->SupportsModelDiscovery());

		const bool bIsUserDefined = ProviderId.StartsWith(TEXT("userprovider_"));
		P->SetBoolField(TEXT("isUserDefined"), bIsUserDefined);

		FString Err;
		const bool bConfigured = Prov->ValidateConfig(Err);
		P->SetBoolField(TEXT("configured"), bConfigured);
		if (!bConfigured && !Err.IsEmpty())
		{
			P->SetStringField(TEXT("configError"), Err);
		}

		if (Settings)
		{
			FString ApiKey = Settings->GetChatProviderApiKey(ProviderId);
			P->SetBoolField(TEXT("hasApiKey"), !ApiKey.IsEmpty());
			if (ApiKey.Len() > 4)
			{
				ApiKey = FString::ChrN(ApiKey.Len() - 4, TEXT('*')) + ApiKey.Right(4);
			}
			P->SetStringField(TEXT("apiKeyMasked"), ApiKey);

			const FChatProviderSettings* Row = Settings->FindChatProviderSettings(ProviderId);
			P->SetBoolField(TEXT("inPriorityList"), Row ? Row->bEnabled : true);
			P->SetStringField(TEXT("baseUrl"), Settings->GetChatProviderBaseUrlOverride(ProviderId));
			P->SetBoolField(TEXT("enableModelDiscovery"), Prov->SupportsModelDiscovery());

			if (bIsUserDefined)
			{
				if (const FUserChatProvider* Def = Settings->FindUserChatProvider(ProviderId))
				{
					TArray<TSharedPtr<FJsonValue>> ModelsArr;
					for (const FChatModelEntry& M : Def->StaticModels)
					{
						TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
						MObj->SetStringField(TEXT("id"), M.ModelId);
						MObj->SetStringField(TEXT("name"), M.DisplayName);
						MObj->SetStringField(TEXT("description"), M.Description);
						ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
					}
					P->SetArrayField(TEXT("models"), ModelsArr);
					P->SetBoolField(TEXT("requiresApiKey"), Def->bRequiresApiKey);
					P->SetStringField(TEXT("baseUrl"), Def->BaseUrl);
					P->SetBoolField(TEXT("enableModelDiscovery"), Def->bEnableDiscovery);
				}
			}
		}
		else
		{
			P->SetBoolField(TEXT("hasApiKey"), false);
			P->SetStringField(TEXT("apiKeyMasked"), TEXT(""));
			P->SetStringField(TEXT("baseUrl"), TEXT(""));
			P->SetBoolField(TEXT("inPriorityList"), true);
			P->SetBoolField(TEXT("enableModelDiscovery"), Prov->SupportsModelDiscovery());
		}

		ProvidersArray.Add(MakeShared<FJsonValueObject>(P));
	}

	Result->SetArrayField(TEXT("providers"), ProvidersArray);
	return JsonToString(Result);
}

void UWebUIBridge::SetProviderPriority(const FString& /*PriorityJson*/)
{
	// The new chat provider layer does not do priority-based routing: every
	// model is owned by exactly one provider. Kept as a no-op for UI compat.
}

void UWebUIBridge::AddProvider(const FString& ProviderId)
{
	// Enable a provider row in settings. New layer treats "enabled" as the
	// equivalent of "in the priority list".
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString CleanProviderId = ProviderId.TrimStartAndEnd().ToLower();
		Settings->SetChatProviderEnabled(CleanProviderId, true);
	}
}

void UWebUIBridge::RemoveProvider(const FString& ProviderId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetChatProviderEnabled(ProviderId.TrimStartAndEnd().ToLower(), false);
	}
}

void UWebUIBridge::SetProviderApiKey(const FString& ProviderId, const FString& ApiKey)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetChatProviderApiKey(ProviderId, ApiKey);
		// Kick a fresh discovery for this provider now that auth changed.
		FChatModelRegistry::Get().RefreshProvider(ProviderId);
		if (ProviderId.Equals(TEXT("neostack"), ESearchCase::IgnoreCase))
		{
			FEntitlementClient::Get().Refresh();
		}
	}
}

void UWebUIBridge::SetProviderBaseUrl(const FString& ProviderId, const FString& BaseUrl)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetChatProviderBaseUrlOverride(ProviderId, BaseUrl);
		FChatModelRegistry::Get().RefreshProvider(ProviderId);
	}
}

// ── Custom Provider CRUD (new chat layer: FUserChatProvider) ────────

FString UWebUIBridge::CreateCustomProvider(const FString& DisplayName, const FString& BaseUrl)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		const FString ProviderId = Settings->CreateUserChatProvider(DisplayName, BaseUrl);
		FChatModelRegistry::Get().SyncUserProviders();
		Result->SetStringField(TEXT("providerId"), ProviderId);
	}
	return JsonToString(Result);
}

void UWebUIBridge::DeleteCustomProvider(const FString& ProviderId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->DeleteUserChatProvider(ProviderId);
		FChatModelRegistry::Get().SyncUserProviders();
	}
}

void UWebUIBridge::UpdateCustomProvider(const FString& ProviderId, const FString& DisplayName, const FString& BaseUrl)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->UpdateUserChatProvider(ProviderId, [&](FUserChatProvider& P)
		{
			if (!DisplayName.IsEmpty()) P.DisplayName = DisplayName.TrimStartAndEnd();
			P.BaseUrl = BaseUrl.TrimStartAndEnd();
		});
		FChatModelRegistry::Get().SyncUserProviders();
	}
}

FString UWebUIBridge::AddCustomProviderModel(const FString& ProviderId, const FString& ModelId, const FString& DisplayName, const FString& Description)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		// New layer only supports models on user-defined providers. Extra models
		// on built-in providers were cut from the design — users who want custom
		// models on top of a built-in endpoint create a user provider pointing
		// at the same URL.
		if (Settings->FindUserChatProvider(ProviderId))
		{
			FChatModelEntry Entry;
			Entry.ModelId = ModelId;
			Entry.DisplayName = DisplayName.IsEmpty() ? ModelId : DisplayName;
			Entry.Description = Description;
			Settings->AddUserChatProviderModel(ProviderId, Entry);
			FChatModelRegistry::Get().SyncUserProviders();
			Result->SetBoolField(TEXT("success"), true);
		}
		else
		{
			Result->SetBoolField(TEXT("success"), false);
			Result->SetStringField(TEXT("error"), TEXT("Cannot add models to built-in providers; create a custom provider instead."));
		}
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
	}
	return JsonToString(Result);
}

void UWebUIBridge::RemoveCustomProviderModel(const FString& ProviderId, const FString& ModelId)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		if (Settings->FindUserChatProvider(ProviderId))
		{
			Settings->RemoveUserChatProviderModel(ProviderId, ModelId);
			FChatModelRegistry::Get().SyncUserProviders();
		}
	}
}

FString UWebUIBridge::ImportCustomProviderModels(const FString& ProviderId, const FString& ModelsJson)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		Result->SetNumberField(TEXT("imported"), 0);
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("Settings not available")));
		Result->SetArrayField(TEXT("errors"), ErrorsArr);
		return JsonToString(Result);
	}

	if (!Settings->FindUserChatProvider(ProviderId))
	{
		Result->SetNumberField(TEXT("imported"), 0);
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("Import only supported on user-defined chat providers")));
		Result->SetArrayField(TEXT("errors"), ErrorsArr);
		return JsonToString(Result);
	}

	// Parse flexible JSON: either [{id,...}] or {"data":[{id,...}]}
	TArray<TSharedPtr<FJsonValue>> ModelsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ModelsJson);
	if (!FJsonSerializer::Deserialize(Reader, ModelsArray))
	{
		TSharedPtr<FJsonObject> Wrapper;
		TSharedRef<TJsonReader<>> Reader2 = TJsonReaderFactory<>::Create(ModelsJson);
		if (FJsonSerializer::Deserialize(Reader2, Wrapper) && Wrapper.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* DataField;
			if (Wrapper->TryGetArrayField(TEXT("data"), DataField))
			{
				ModelsArray = *DataField;
			}
		}
	}

	if (ModelsArray.Num() == 0)
	{
		Result->SetNumberField(TEXT("imported"), 0);
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("No valid models found in JSON. Expected [{\"id\":\"...\"}] or {\"data\":[{\"id\":\"...\"}]}")));
		Result->SetArrayField(TEXT("errors"), ErrorsArr);
		return JsonToString(Result);
	}

	int32 Imported = 0;
	for (const TSharedPtr<FJsonValue>& Val : ModelsArray)
	{
		TSharedPtr<FJsonObject> Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;

		FChatModelEntry Entry;
		if (!Obj->TryGetStringField(TEXT("id"), Entry.ModelId) || Entry.ModelId.IsEmpty())
		{
			continue;
		}
		Obj->TryGetStringField(TEXT("name"), Entry.DisplayName);
		if (Entry.DisplayName.IsEmpty()) Entry.DisplayName = Entry.ModelId;
		Obj->TryGetStringField(TEXT("description"), Entry.Description);

		Settings->AddUserChatProviderModel(ProviderId, Entry);
		++Imported;
	}
	FChatModelRegistry::Get().SyncUserProviders();

	Result->SetNumberField(TEXT("imported"), Imported);
	Result->SetArrayField(TEXT("errors"), ErrorsArr);
	return JsonToString(Result);
}

void UWebUIBridge::SetCustomProviderModelDiscovery(const FString& ProviderId, bool bEnabled)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->UpdateUserChatProvider(ProviderId, [bEnabled](FUserChatProvider& P)
		{
			P.bEnableDiscovery = bEnabled;
		});
		FChatModelRegistry::Get().SyncUserProviders();
		FChatModelRegistry::Get().RefreshProvider(ProviderId);
	}
}

void UWebUIBridge::SetCustomProviderRequiresApiKey(const FString& ProviderId, bool bRequiresApiKey)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->UpdateUserChatProvider(ProviderId, [bRequiresApiKey](FUserChatProvider& P)
		{
			P.bRequiresApiKey = bRequiresApiKey;
		});
		FChatModelRegistry::Get().SyncUserProviders();
	}
}

void UWebUIBridge::RefreshProviderModels()
{
	FChatModelRegistry::Get().RefreshAll();
}

FString UWebUIBridge::GetEnabledModels()
{
	// The new chat layer does not have a concept of "disabled models" — every
	// discovered model is available. Return an empty enabled set with
	// hasCustomSelection=false so the UI treats it as "show all".
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("enabledModels"), {});
	Root->SetBoolField(TEXT("hasCustomSelection"), false);
	return JsonToString(Root);
}

void UWebUIBridge::SetModelEnabled(const FString& /*ModelId*/, bool /*bEnabled*/)
{
	// No-op in the new chat layer. See GetEnabledModels() comment.
}

void UWebUIBridge::SetEnabledModels(const FString& /*ModelIdsJson*/)
{
	// No-op in the new chat layer. See GetEnabledModels() comment.
}

// ── Messaging ────────────────────────────────────────────────────────

namespace
{
	// Keep automatic @ mention context bounded so adapter/model limits are not exceeded.
	constexpr int32 MaxMentionPathsPerPrompt = 4;
	constexpr int32 MaxMentionItemChars = 6000;
	constexpr int32 MaxMentionContextChars = 24000;

	static FString TruncateForPromptBudget(const FString& Input, int32 MaxChars, bool& bOutTruncated)
	{
		bOutTruncated = false;
		if (MaxChars <= 0 || Input.Len() <= MaxChars)
		{
			return Input;
		}

		const FString Suffix = TEXT("\n...[truncated for prompt size]\n");
		const int32 KeepChars = FMath::Max(0, MaxChars - Suffix.Len());
		bOutTruncated = true;
		return Input.Left(KeepChars) + Suffix;
	}
}

// Helper: parse @/Game/... and @Source/... paths from message text (mirrors SAgentChatWindow::ParseAtMentionPaths)
static TArray<FString> ParseAtMentionPaths(const FString& MessageText)
{
	TArray<FString> Paths;
	int32 SearchStart = 0;
	while (SearchStart < MessageText.Len())
	{
		int32 AtIndex = MessageText.Find(TEXT("@"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (AtIndex == INDEX_NONE) break;

		int32 PathStart = AtIndex + 1;
		if (PathStart >= MessageText.Len()) break;

		int32 PathEnd = PathStart;
		while (PathEnd < MessageText.Len() && !FChar::IsWhitespace(MessageText[PathEnd]))
		{
			++PathEnd;
		}

		FString Path = MessageText.Mid(PathStart, PathEnd - PathStart);
		if (Path.StartsWith(TEXT("/")) || Path.StartsWith(TEXT("Source/")))
		{
			Paths.AddUnique(Path);
		}
		SearchStart = PathEnd;
	}
	return Paths;
}

static EACPContextType GuessContextTypeForPath(const FString& Path)
{
	if (Path.StartsWith(TEXT("/Game/")) || Path.StartsWith(TEXT("/Engine/")))
	{
		return EACPContextType::Asset;
	}
	if (Path.EndsWith(TEXT(".cpp")) || Path.EndsWith(TEXT(".h")) || Path.EndsWith(TEXT(".hpp")) || Path.EndsWith(TEXT(".cs")))
	{
		return EACPContextType::CppFile;
	}
	if (FPaths::DirectoryExists(Path))
	{
		return EACPContextType::Folder;
	}
	return EACPContextType::Unknown;
}

static TArray<FACPMessageContext> BuildMessageContextsForPaths(const TArray<FString>& Paths)
{
	TArray<FACPMessageContext> Contexts;
	const int32 PathsToProcess = FMath::Min(Paths.Num(), MaxMentionPathsPerPrompt);
	for (int32 PathIndex = 0; PathIndex < PathsToProcess; ++PathIndex)
	{
		const FString& Path = Paths[PathIndex];
		FACPMessageContext Context;
		Context.Path = Path;
		Context.DisplayName = FPaths::GetCleanFilename(Path);
		if (Context.DisplayName.IsEmpty())
		{
			Context.DisplayName = Path;
		}
		Context.Type = GuessContextTypeForPath(Path);
		Context.Status = EACPContextStatus::Resolved;
		Context.bTruncated = false;
		Contexts.Add(MoveTemp(Context));
	}

	for (int32 PathIndex = PathsToProcess; PathIndex < Paths.Num(); ++PathIndex)
	{
		FACPMessageContext Context;
		Context.Path = Paths[PathIndex];
		Context.DisplayName = FPaths::GetCleanFilename(Context.Path);
		if (Context.DisplayName.IsEmpty())
		{
			Context.DisplayName = Context.Path;
		}
		Context.Type = GuessContextTypeForPath(Context.Path);
		Context.Status = EACPContextStatus::Truncated;
		Context.bTruncated = true;
		Context.ErrorMessage = TEXT("Omitted from prompt context budget");
		Contexts.Add(MoveTemp(Context));
	}

	return Contexts;
}

// Helper: resolve paths to context text using Lua open_asset()+info() / read_file()
static FString BuildContextForPaths(const TArray<FString>& Paths)
{
	if (Paths.Num() == 0) return FString();

	FString ContextText = TEXT("## Referenced Context\n\n");
	const int32 PathsToProcess = FMath::Min(Paths.Num(), MaxMentionPathsPerPrompt);
	int32 TruncatedItemCount = 0;
	int32 OmittedItemCount = 0;

	// Build a single Lua script that processes all paths (one state creation for all)
	FString Script = TEXT(
		"local function dump(t, indent)\n"
		"  indent = indent or \"\"\n"
		"  if type(t) ~= \"table\" then print(indent .. tostring(t)) return end\n"
		"  local keys = {}\n"
		"  for k in pairs(t) do keys[#keys+1] = tostring(k) end\n"
		"  table.sort(keys)\n"
		"  for _, k in ipairs(keys) do\n"
		"    local v = t[k]\n"
		"    if type(v) == \"table\" then\n"
		"      print(indent .. k .. \":\")\n"
		"      dump(v, indent .. \"  \")\n"
		"    else\n"
		"      print(indent .. k .. \": \" .. tostring(v))\n"
		"    end\n"
		"  end\n"
		"end\n\n"
	);

	for (int32 i = 0; i < PathsToProcess; ++i)
	{
		// Escape quotes in path for Lua string literal
		FString EscapedPath = Paths[i].Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		Script += FString::Printf(TEXT(
			"print(\"###ASSET_START:%d\")\n"
			"do\n"
			"  local h = open_asset(\"%s\")\n"
			"  if h then\n"
			"    local i = h:info()\n"
			"    if i then dump(i) end\n"
			"  else\n"
			"    local f = read_file(\"%s\")\n"
			"    if f and f.content then\n"
			"      print(f.content)\n"
			"    else\n"
			"      print(\"Could not load asset or file\")\n"
			"    end\n"
			"  end\n"
			"end\n"
			"print(\"###ASSET_END:%d\")\n\n"
		), i, *EscapedPath, *EscapedPath, i);
	}

	// Multi-asset content loader — composed from open_asset/read_file calls; sync only.
	FScriptResult Result = FNeoLuaState::ExecuteSyncBlocking(Script);

	// Parse trace output into per-path segments
	TArray<FString> PerPathOutput;
	PerPathOutput.SetNum(PathsToProcess);

	int32 CurrentIndex = -1;
	for (const FString& Line : Result.Trace)
	{
		if (Line.StartsWith(TEXT("###ASSET_START:")))
		{
			CurrentIndex = FCString::Atoi(*Line.Mid(15));
			continue;
		}
		if (Line.StartsWith(TEXT("###ASSET_END:")))
		{
			CurrentIndex = -1;
			continue;
		}
		// Skip internal [OK]/[FAIL] log lines from open_asset/info
		if (Line.StartsWith(TEXT("[OK]")) || Line.StartsWith(TEXT("[FAIL]")))
		{
			continue;
		}
		if (CurrentIndex >= 0 && CurrentIndex < PathsToProcess)
		{
			if (!PerPathOutput[CurrentIndex].IsEmpty())
			{
				PerPathOutput[CurrentIndex] += TEXT("\n");
			}
			PerPathOutput[CurrentIndex] += Line;
		}
	}

	// Build final context text from parsed segments
	for (int32 PathIndex = 0; PathIndex < PathsToProcess; ++PathIndex)
	{
		FString DisplayName = FPaths::GetCleanFilename(Paths[PathIndex]);
		FString EntryBody = PerPathOutput[PathIndex];

		if (EntryBody.IsEmpty())
		{
			EntryBody = TEXT("No info available");
		}

		bool bEntryTruncated = false;
		EntryBody = TruncateForPromptBudget(EntryBody, MaxMentionItemChars, bEntryTruncated);
		if (bEntryTruncated)
		{
			++TruncatedItemCount;
		}

		const FString EntryText = FString::Printf(TEXT("### %s\n```\n%s\n```\n\n"), *DisplayName, *EntryBody);
		const int32 RemainingBudget = MaxMentionContextChars - ContextText.Len();
		if (EntryText.Len() > RemainingBudget)
		{
			OmittedItemCount += (PathsToProcess - PathIndex);
			break;
		}

		ContextText += EntryText;
	}

	if (Paths.Num() > PathsToProcess)
	{
		OmittedItemCount += (Paths.Num() - PathsToProcess);
	}

	if (TruncatedItemCount > 0 || OmittedItemCount > 0)
	{
		const FString SizeNote = FString::Printf(
			TEXT("> Note: mention context was size-limited (truncated=%d, omitted=%d).\n\n"),
			TruncatedItemCount,
			OmittedItemCount);
		const int32 RemainingBudget = MaxMentionContextChars - ContextText.Len();
		if (RemainingBudget > 0)
		{
			bool bIgnored = false;
			ContextText += TruncateForPromptBudget(SizeNote, RemainingBudget, bIgnored);
		}
	}

	return ContextText;
}

void UWebUIBridge::SendPrompt(const FString& SessionId, const FString& Text)
{
	const FString TrimmedSessionId = SessionId.TrimStartAndEnd();
	FString TrimmedText = Text;
	TrimmedText.TrimStartAndEndInline();

	if (TrimmedSessionId.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: SendPrompt ignored - empty session ID"));
		return;
	}

	if (TrimmedText.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("WebUIBridge: SendPrompt ignored - empty text for session %s"), *TrimmedSessionId);
		return;
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(TrimmedSessionId);
	if (!Session)
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: SendPrompt ignored - session not found: %s"), *TrimmedSessionId);
		return;
	}

	// Parse @ mentions and resolve context (same as Slate UI)
	TArray<FString> MentionedPaths = ParseAtMentionPaths(TrimmedText);
	TArray<FACPMessageContext> MentionContexts = BuildMessageContextsForPaths(MentionedPaths);
	FString ContextPrefix = BuildContextForPaths(MentionedPaths);

	// Build the full prompt with context prepended
	FString FullPrompt = ContextPrefix.IsEmpty() ? TrimmedText : (ContextPrefix + TrimmedText);

	// Add user message to session (show original text, not with context)
	SessionMgr.AddUserMessage(TrimmedSessionId, TrimmedText, MentionContexts, FullPrompt);

	// Provisional title from first message (like Zed) — shows immediately in sidebar,
	// replaced when the agent sends session_info_update with a proper title.
	if ((Session->Metadata.MessageCount <= 1 && Session->Metadata.Title.IsEmpty()) || Session->Metadata.Title == TEXT("New conversation"))
	{
		// Take first line, truncate to 80 chars
		FString FirstLine = TrimmedText;
		int32 NewlineIdx;
		if (FirstLine.FindChar(TEXT('\n'), NewlineIdx))
		{
			FirstLine = FirstLine.Left(NewlineIdx);
		}
		FirstLine.TrimStartAndEndInline();
		if (FirstLine.Len() > 80)
		{
			FirstLine = FirstLine.Left(77) + TEXT("...");
		}
		if (!FirstLine.IsEmpty())
		{
			SessionMgr.UpdateSessionTitle(TrimmedSessionId, FirstLine);
		}
	}

	// Runtime service owns the provider split underneath NeoStack sessions.
	if (FChatSessionManager::Get().HasSession(TrimmedSessionId))
	{
		UE_LOG(LogNeoStackAI, Log,
			TEXT("WebUIBridge: Sending prompt for built-in chat session %s (full_len=%d)"),
			*TrimmedSessionId, FullPrompt.Len());
		FNeoStackAgentRuntimeService::Get().SendPrompt(TrimmedSessionId, Session->Metadata.AgentName, FullPrompt);
		return;
	}

	// Get the agent for this session (ACP path)
	FString AgentName = AgentMgr.GetSessionAgent(TrimmedSessionId);
	if (AgentName.IsEmpty())
	{
		AgentName = Session->Metadata.AgentName;
	}

	if (!AgentName.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Sending prompt for session %s via agent %s (user_len=%d, mention_context_len=%d, full_len=%d, mentions=%d)"),
			*TrimmedSessionId, *AgentName, TrimmedText.Len(), ContextPrefix.Len(), FullPrompt.Len(), MentionedPaths.Num());
		FNeoStackAgentRuntimeService::Get().SendPrompt(TrimmedSessionId, AgentName, FullPrompt);
	}
	else
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: SendPrompt ignored - no agent mapped for session %s"), *TrimmedSessionId);
	}
}

void UWebUIBridge::CancelPrompt(const FString& SessionId)
{
	if (SessionId.IsEmpty()) return;

	FNeoStackAgentRuntimeService::Get().CancelPrompt(SessionId);
}

// ── Model & Reasoning ───────────────────────────────────────────────

FString UWebUIBridge::GetModels(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("[]");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// Built-in chat: pull flat grouped list from the new FChatModelRegistry.
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		// Optionally scope the picker to a single chat provider — used by the
		// NeoStack Cloud agent so it only shows our own models, not the union
		// of every configured provider (which is what OpenRouter does).
		FString ScopedProviderId;
		if (const FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName))
		{
			ScopedProviderId = Config->GatewayProviderId;
		}

		TArray<TSharedPtr<FJsonValue>> ModelsArray;
		for (const FChatModelInfo& M : FChatModelRegistry::Get().GetAllModelsFlat())
		{
			if (!ScopedProviderId.IsEmpty() && M.ProviderId != ScopedProviderId)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"), M.GetPrefixedId());
			Obj->SetStringField(TEXT("name"), M.DisplayName);
			Obj->SetStringField(TEXT("description"), M.Description);
			Obj->SetBoolField(TEXT("supportsReasoning"), M.bSupportsReasoning);
			Obj->SetStringField(TEXT("provider"), M.ProviderId);
			Obj->SetStringField(TEXT("providerDisplayName"), M.ProviderDisplayName);
			ModelsArray.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("models"), ModelsArray);
		Result->SetStringField(TEXT("currentModelId"), FChatModelRegistry::Get().GetSelectedModel());
		return JsonToString(Result);
	}

	// ACP agents: unchanged.
	FACPSessionModelState ModelState = AgentMgr.GetAgentModelState(AgentName);

	bool bACPHasReasoning = false;
	TSharedPtr<FACPClient> ACPClient = AgentMgr.GetClient(AgentName);
	if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
	{
		bACPHasReasoning = true;
	}

	TArray<TSharedPtr<FJsonValue>> ModelsArray;
	for (const FACPModelInfo& Model : ModelState.AvailableModels)
	{
		TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
		ModelObj->SetStringField(TEXT("id"), Model.ModelId);
		ModelObj->SetStringField(TEXT("name"), Model.Name);
		ModelObj->SetStringField(TEXT("description"), Model.Description);
		ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
		if (!Model.ProviderId.IsEmpty()) ModelObj->SetStringField(TEXT("provider"), Model.ProviderId);
		if (!Model.ProviderDisplayName.IsEmpty()) ModelObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
		ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), ModelsArray);
	Result->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);
	return JsonToString(Result);
}

FString UWebUIBridge::GetAllModels(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("[]");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// Built-in chat: the new registry always returns the full flat list from
	// GetAllModelsFlat, so GetModels and GetAllModels are equivalent.
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		return GetModels(AgentName);
	}

	// ACP path unchanged.
	FACPSessionModelState ModelState = AgentMgr.GetAgentModelState(AgentName);
	const TArray<FACPModelInfo> FullModels = AgentMgr.GetAgentFullModelList(AgentName);

	bool bACPHasReasoning = false;
	TSharedPtr<FACPClient> ACPClient = AgentMgr.GetClient(AgentName);
	if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
	{
		bACPHasReasoning = true;
	}

	TArray<TSharedPtr<FJsonValue>> ModelsArray;
	for (const FACPModelInfo& Model : FullModels)
	{
		TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
		ModelObj->SetStringField(TEXT("id"), Model.ModelId);
		ModelObj->SetStringField(TEXT("name"), Model.Name);
		ModelObj->SetStringField(TEXT("description"), Model.Description);
		ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
		if (!Model.ProviderId.IsEmpty()) ModelObj->SetStringField(TEXT("provider"), Model.ProviderId);
		if (!Model.ProviderDisplayName.IsEmpty()) ModelObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
		ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
	}

	if (ModelsArray.Num() == 0)
	{
		for (const FACPModelInfo& Model : ModelState.AvailableModels)
		{
			TSharedPtr<FJsonObject> ModelObj = MakeShared<FJsonObject>();
			ModelObj->SetStringField(TEXT("id"), Model.ModelId);
			ModelObj->SetStringField(TEXT("name"), Model.Name);
			ModelObj->SetStringField(TEXT("description"), Model.Description);
			ModelObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
			ModelsArray.Add(MakeShared<FJsonValueObject>(ModelObj));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), ModelsArray);
	Result->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);
	return JsonToString(Result);
}

void UWebUIBridge::SetModel(const FString& AgentName, const FString& ModelId)
{
	if (AgentName.IsEmpty() || ModelId.IsEmpty()) return;

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// Built-in chat: persist via registry, NOT via agent manager.
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		FChatSessionManager::Get().SetSelectedModel(ModelId);
		if (UACPSettings* Settings = UACPSettings::Get())
		{
			Settings->SaveModelForAgent(AgentName, ModelId);
		}
		return;
	}

	AgentMgr.SetAgentModel(AgentName, ModelId);
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModelForAgent(AgentName, ModelId);
	}
}

FString UWebUIBridge::GetReasoningLevel(const FString& AgentName)
{
	if (AgentName.IsEmpty())
	{
		return TEXT("high");
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	UACPSettings* Settings = UACPSettings::Get();

	// Prefer persisted value as fallback
	const FString SavedLevel = Settings ? Settings->GetSavedReasoningForAgent(AgentName) : FString();

	// Built-in chat: read from settings (the session applies it per-turn).
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		return SavedLevel.IsEmpty() ? TEXT("none") : SavedLevel;
	}

	// Check the specific ACP client for this agent.
	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid() && Client->SupportsReasoningEffortControl())
	{
		const FString& Effort = Client->GetCurrentReasoningEffort();
		if (!Effort.IsEmpty())
		{
			// Map ACP thinking values to UI reasoning levels
			if (Effort == TEXT("off")) return TEXT("none");
			return Effort;
		}
	}

	return SavedLevel.IsEmpty() ? TEXT("high") : SavedLevel;
}

void UWebUIBridge::SetReasoningLevel(const FString& AgentName, const FString& Level)
{
	if (AgentName.IsEmpty() || Level.IsEmpty()) return;

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveReasoningForAgent(AgentName, Level);
	}

	// Built-in chat: the session picks this up on its next prompt via
	// FChatSessionManager; nothing live to update.
	if (AgentMgr.IsChatGatewayAgent(AgentName))
	{
		FChatSessionManager::Get().SetReasoningEffort(Level);
		return;
	}

	// ACP client — map UI level to thinking config option value
	FString ThinkingValue = Level == TEXT("none") ? TEXT("off") : Level;
	TSharedPtr<FACPClient> Client = AgentMgr.GetClient(AgentName);
	if (Client.IsValid() && Client->SupportsReasoningEffortControl())
	{
		Client->SetReasoningEffort(ThinkingValue);
	}
}

// ── Mode Selection ──────────────────────────────────────────────────

FString UWebUIBridge::GetModes(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return TEXT("{\"modes\":[],\"currentModeId\":\"\"}");

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionModeState ModeState = AgentMgr.GetAgentModeState(AgentName);

	TArray<TSharedPtr<FJsonValue>> ModesArray;
	for (const FACPSessionMode& Mode : ModeState.AvailableModes)
	{
		TSharedPtr<FJsonObject> ModeObj = MakeShared<FJsonObject>();
		ModeObj->SetStringField(TEXT("id"), Mode.ModeId);
		ModeObj->SetStringField(TEXT("name"), Mode.Name);
		ModeObj->SetStringField(TEXT("description"), Mode.Description);
		ModesArray.Add(MakeShared<FJsonValueObject>(ModeObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("modes"), ModesArray);
	Result->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);

	return JsonToString(Result);
}

void UWebUIBridge::SetMode(const FString& AgentName, const FString& ModeId)
{
	if (AgentName.IsEmpty() || ModeId.IsEmpty()) return;
	FACPAgentManager::Get().SetAgentMode(AgentName, ModeId);

	// Persist the selection
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SaveModeForAgent(AgentName, ModeId);
	}
}

// ── Context Mentions ────────────────────────────────────────────────

FString UWebUIBridge::SearchContextItems(const FString& Query)
{
	TArray<TSharedPtr<FJsonValue>> Results;
	const int32 MaxResults = 50;

	// ── Icon cache ──────────────────────────────────────────────────
	static TMap<FName, FString> IconCache;
	auto GetClassIconSVG = [](FName ClassName) -> FString
	{
		if (const FString* Cached = IconCache.Find(ClassName))
		{
			return *Cached;
		}

		const FString SlateDir = FPaths::EngineContentDir() / TEXT("Editor/Slate");
		FString SvgContent;

		FString DirectPath = SlateDir / FString::Printf(TEXT("Starship/AssetIcons/%s_16.svg"), *ClassName.ToString());
		if (FFileHelper::LoadFileToString(SvgContent, *DirectPath))
		{
			IconCache.Add(ClassName, SvgContent);
			return SvgContent;
		}

		UClass* Class = NeoTypeResolver::FindClassRobust(ClassName.ToString());
		if (Class)
		{
			for (const UStruct* Super = Class->GetSuperStruct(); Super; Super = Super->GetSuperStruct())
			{
				FString SuperSvgPath = SlateDir / FString::Printf(TEXT("Starship/AssetIcons/%s_16.svg"), *Super->GetName());
				if (FFileHelper::LoadFileToString(SvgContent, *SuperSvgPath))
				{
					IconCache.Add(ClassName, SvgContent);
					return SvgContent;
				}
			}
		}

		FString DefaultPath = SlateDir / TEXT("Starship/AssetIcons/Default_16.svg");
		if (FFileHelper::LoadFileToString(SvgContent, *DefaultPath))
		{
			IconCache.Add(ClassName, SvgContent);
			return SvgContent;
		}

		IconCache.Add(ClassName, FString());
		return FString();
	};

	// ── Helper: add an asset item to results ────────────────────────
	auto AddAssetItem = [&](const FString& Name, const FString& Path, const FName& ClassName)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Name);
		Item->SetStringField(TEXT("path"), Path);
		Item->SetStringField(TEXT("type"), TEXT("asset"));

		FString IconSVG = GetClassIconSVG(ClassName);
		if (!IconSVG.IsEmpty())
		{
			Item->SetStringField(TEXT("icon"), IconSVG);
		}

		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	auto AddFolderItem = [&](const FString& FolderName, const FString& FolderPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), FolderName + TEXT("/"));
		Item->SetStringField(TEXT("path"), FolderPath);
		Item->SetStringField(TEXT("type"), TEXT("folder"));
		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	auto AddFileItem = [&](const FString& FileName, const FString& RelPath)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), FileName);
		Item->SetStringField(TEXT("path"), RelPath);
		Item->SetStringField(TEXT("type"), TEXT("file"));
		Results.Add(MakeShared<FJsonValueObject>(Item));
	};

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	constexpr bool bIncludeEngine = false;
	constexpr bool bIncludePlugins = false;

	const bool bIsPathQuery = Query.StartsWith(TEXT("/")) || Query.StartsWith(TEXT("Source/"));
	const bool bIsSearch = !bIsPathQuery && !Query.IsEmpty();

	if (bIsPathQuery)
	{
		// ── BROWSE MODE: list subfolders + assets at a specific path ────
		// Query is like "/Game/Animations" or "Source/UST"
		FString BrowsePath = Query;

		// Trim trailing filter text: "/Game/Animations/Loc" → browse "/Game/Animations", filter "Loc"
		// We detect this by checking if the last segment matches an existing sub-path
		FString ParentPath = BrowsePath;
		FString LeafFilter;

		if (BrowsePath.StartsWith(TEXT("Source")))
		{
			// ── Filesystem browse (C++ source) ──
			FString AbsDir = FPaths::ProjectDir() / BrowsePath;
			if (!FPaths::DirectoryExists(AbsDir))
			{
				// Last segment might be a partial filter
				int32 LastSlash;
				if (BrowsePath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					ParentPath = BrowsePath.Left(LastSlash);
					LeafFilter = BrowsePath.Mid(LastSlash + 1).ToLower();
					AbsDir = FPaths::ProjectDir() / ParentPath;
				}
			}

			if (FPaths::DirectoryExists(AbsDir))
			{
				// List subdirectories
				TArray<FString> SubDirs;
				IFileManager::Get().FindFiles(SubDirs, *(AbsDir / TEXT("*")), false, true);
				SubDirs.Sort();
				for (const FString& Dir : SubDirs)
				{
					if (Results.Num() >= MaxResults) break;
					if (!LeafFilter.IsEmpty() && !Dir.ToLower().Contains(LeafFilter)) continue;
					AddFolderItem(Dir, ParentPath / Dir);
				}

				// List files
				TArray<FString> Files;
				IFileManager::Get().FindFiles(Files, *(AbsDir / TEXT("*")), true, false);
				Files.Sort();
				for (const FString& File : Files)
				{
					if (Results.Num() >= MaxResults) break;
					if (!LeafFilter.IsEmpty() && !File.ToLower().Contains(LeafFilter)) continue;
					FString Ext = FPaths::GetExtension(File).ToLower();
					if (Ext == TEXT("h") || Ext == TEXT("cpp") || Ext == TEXT("cs") || Ext == TEXT("build") || Ext == TEXT("target"))
					{
						AddFileItem(File, ParentPath / File);
					}
				}
			}
		}
		else
		{
			// ── Asset Registry browse ──
			// Check if path with trailing part is a valid package path
			TArray<FString> SubPaths;
			AssetRegistry.GetSubPaths(BrowsePath, SubPaths, false);

			if (SubPaths.Num() == 0)
			{
				// Might be a partial filter: "/Game/Animations/Loc" → browse "/Game/Animations", filter "Loc"
				int32 LastSlash;
				if (BrowsePath.FindLastChar(TEXT('/'), LastSlash) && LastSlash > 0)
				{
					ParentPath = BrowsePath.Left(LastSlash);
					LeafFilter = BrowsePath.Mid(LastSlash + 1).ToLower();
					AssetRegistry.GetSubPaths(ParentPath, SubPaths, false);
				}
			}
			else
			{
				ParentPath = BrowsePath;
			}

			// Subfolders
			SubPaths.Sort();
			for (const FString& Sub : SubPaths)
			{
				if (Results.Num() >= MaxResults) break;
				FString FolderName = FPaths::GetCleanFilename(Sub);
				if (!LeafFilter.IsEmpty() && !FolderName.ToLower().Contains(LeafFilter)) continue;
				AddFolderItem(FolderName, Sub);
			}

			// Assets in this folder (non-recursive)
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*ParentPath));
			Filter.bRecursivePaths = false;

			TArray<FAssetData> FolderAssets;
			AssetRegistry.GetAssets(Filter, FolderAssets);

			FolderAssets.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.AssetName.LexicalLess(B.AssetName);
			});

			for (const FAssetData& Asset : FolderAssets)
			{
				if (Results.Num() >= MaxResults) break;
				FString Name = Asset.AssetName.ToString();
				if (!LeafFilter.IsEmpty() && !Name.ToLower().Contains(LeafFilter)) continue;
				FName ClassName = Asset.AssetClassPath.GetAssetName();
				AddAssetItem(Name, Asset.PackageName.ToString(), ClassName);
			}
		}
	}
	else if (bIsSearch)
	{
		// ── SEARCH MODE: filter across all assets + source files by name ──
		const FString LowerQuery = Query.ToLower();

		FARFilter Filter;
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
		if (bIncludeEngine) Filter.PackagePaths.Add(FName(TEXT("/Engine")));
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AllAssets;
		AssetRegistry.GetAssets(Filter, AllAssets);

		if (bIncludePlugins)
		{
			TArray<FAssetData> AllRegistered;
			AssetRegistry.GetAllAssets(AllRegistered, true);
			for (FAssetData& Asset : AllRegistered)
			{
				FString PkgPath = Asset.PackagePath.ToString();
				if (!PkgPath.StartsWith(TEXT("/Game")) && !PkgPath.StartsWith(TEXT("/Engine")))
				{
					AllAssets.Add(MoveTemp(Asset));
				}
			}
		}

		for (const FAssetData& Asset : AllAssets)
		{
			if (Results.Num() >= MaxResults) break;
			FString Name = Asset.AssetName.ToString();
			FString Path = Asset.PackageName.ToString();
			if (!Name.ToLower().Contains(LowerQuery) && !Path.ToLower().Contains(LowerQuery)) continue;
			FName ClassName = Asset.AssetClassPath.GetAssetName();
			AddAssetItem(Name, Path, ClassName);
		}

		// Also search C++ source files
		if (Results.Num() < MaxResults)
		{
			FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
			if (FPaths::DirectoryExists(SourceDir))
			{
				TArray<FString> FoundFiles;
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDir, TEXT("*.h"), true, false);
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SourceDir, TEXT("*.cpp"), true, false);
				for (const FString& FilePath : FoundFiles)
				{
					if (Results.Num() >= MaxResults) break;
					FString FileName = FPaths::GetCleanFilename(FilePath);
					if (!FileName.ToLower().Contains(LowerQuery) && !FilePath.ToLower().Contains(LowerQuery)) continue;
					FString RelativePath = FilePath;
					FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectDir());
					AddFileItem(FileName, RelativePath);
				}
			}
		}
	}
	else
	{
		// ── ROOT VIEW: show top-level browsable paths ──
		// Content roots
		AddFolderItem(TEXT("Game"), TEXT("/Game"));
		if (bIncludeEngine) AddFolderItem(TEXT("Engine"), TEXT("/Engine"));

		// Plugin content roots
		if (bIncludePlugins)
		{
			TArray<FString> RootPaths;
			AssetRegistry.GetSubPaths(TEXT("/"), RootPaths, false);
			RootPaths.Sort();
			for (const FString& RootPath : RootPaths)
			{
				if (RootPath == TEXT("/Game") || RootPath == TEXT("/Engine") || RootPath == TEXT("/Script") || RootPath == TEXT("/Temp")) continue;
				FString Name = FPaths::GetCleanFilename(RootPath);
				AddFolderItem(Name, RootPath);
			}
		}

		// Source directory
		FString SourceDir = FPaths::ProjectDir() / TEXT("Source");
		if (FPaths::DirectoryExists(SourceDir))
		{
			AddFolderItem(TEXT("Source"), TEXT("Source"));
		}
	}

	return JsonArrayToString(Results);
}

static FString SanitizeExportFilename(FString Name)
{
	Name.TrimStartAndEndInline();
	if (Name.IsEmpty())
	{
		Name = TEXT("chat-session");
	}

	Name.ReplaceInline(TEXT("/"), TEXT("-"));
	Name.ReplaceInline(TEXT("\\"), TEXT("-"));
	Name.ReplaceInline(TEXT(":"), TEXT("-"));
	Name.ReplaceInline(TEXT("\""), TEXT(""));
	Name.ReplaceInline(TEXT("<"), TEXT(""));
	Name.ReplaceInline(TEXT(">"), TEXT(""));
	Name.ReplaceInline(TEXT("|"), TEXT("-"));
	Name.ReplaceInline(TEXT("*"), TEXT(""));
	Name.ReplaceInline(TEXT("?"), TEXT(""));
	Name.ReplaceInline(TEXT("\n"), TEXT(" "));
	Name.ReplaceInline(TEXT("\r"), TEXT(" "));
	Name.ReplaceInline(TEXT("\t"), TEXT(" "));
	while (Name.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
	Name.TrimStartAndEndInline();

	if (Name.Len() > 64)
	{
		Name = Name.Left(64);
		Name.TrimEndInline();
	}
	if (Name.IsEmpty())
	{
		Name = TEXT("chat-session");
	}
	return Name;
}

static FString BuildSessionMarkdown(const FACPActiveSession& Session)
{
	const FString SessionTitle = Session.Metadata.Title.IsEmpty() ? TEXT("New chat") : Session.Metadata.Title;
	const FString CreatedAt = Session.Metadata.CreatedAt.GetTicks() > 0
		? Session.Metadata.CreatedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S"))
		: TEXT("Unknown");
	const FString LastModifiedAt = Session.Metadata.LastModifiedAt.GetTicks() > 0
		? Session.Metadata.LastModifiedAt.ToString(TEXT("%Y-%m-%d %H:%M:%S"))
		: TEXT("Unknown");

	FString Markdown;
	Markdown.Reserve(32768);
	Markdown += FString::Printf(TEXT("# %s\n\n"), *SessionTitle);
	Markdown += FString::Printf(TEXT("- Agent: `%s`\n"), *Session.Metadata.AgentName);
	Markdown += FString::Printf(TEXT("- Session ID: `%s`\n"), *Session.Metadata.SessionId);
	Markdown += FString::Printf(TEXT("- Created: `%s`\n"), *CreatedAt);
	Markdown += FString::Printf(TEXT("- Last Modified: `%s`\n"), *LastModifiedAt);
	Markdown += FString::Printf(TEXT("- Message Count: `%d`\n\n"), Session.Messages.Num());
	Markdown += TEXT("---\n\n");

	if (Session.Messages.Num() == 0)
	{
		Markdown += TEXT("_No messages in this session._\n");
		return Markdown;
	}

	for (const FACPChatMessage& Message : Session.Messages)
	{
		FString Heading = MessageRoleToSummaryLabel(Message.Role);
		if (Message.Timestamp.GetTicks() > 0)
		{
			Heading += FString::Printf(TEXT(" (%s)"), *Message.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		}
		Markdown += FString::Printf(TEXT("## %s\n\n"), *Heading);

		for (const FACPContentBlock& Block : Message.ContentBlocks)
		{
			switch (Block.Type)
			{
			case EACPContentBlockType::Text:
				Markdown += Block.Text + TEXT("\n\n");
				break;

			case EACPContentBlockType::Thought:
				Markdown += TEXT("<details>\n<summary>Thinking</summary>\n\n");
				Markdown += Block.Text + TEXT("\n\n");
				Markdown += TEXT("</details>\n\n");
				break;

			case EACPContentBlockType::ToolCall:
				Markdown += FString::Printf(TEXT("### Tool Call: %s\n\n"), Block.ToolName.IsEmpty() ? TEXT("tool") : *Block.ToolName);
				if (!Block.ToolArguments.IsEmpty())
				{
					Markdown += TEXT("```json\n");
					Markdown += Block.ToolArguments;
					Markdown += TEXT("\n```\n\n");
				}
				break;

			case EACPContentBlockType::ToolResult:
			{
				const TCHAR* ResultStatus = Block.bToolSuccess ? TEXT("Success") : TEXT("Error");
				Markdown += FString::Printf(TEXT("### Tool Result (%s)\n\n"), ResultStatus);
				FString ToolResult = Block.ToolResultContent;
				const int32 MaxResultChars = 120000;
				bool bTruncated = false;
				if (ToolResult.Len() > MaxResultChars)
				{
					ToolResult = ToolResult.Left(MaxResultChars);
					bTruncated = true;
				}
				if (!ToolResult.IsEmpty())
				{
					Markdown += TEXT("```\n");
					Markdown += ToolResult;
					Markdown += TEXT("\n```\n\n");
				}
				if (Block.ToolResultImages.Num() > 0)
				{
					Markdown += FString::Printf(TEXT("- Images: %d\n\n"), Block.ToolResultImages.Num());
				}
				if (bTruncated)
				{
					Markdown += TEXT("_Tool result truncated for export size limits._\n\n");
				}
				break;
			}

			case EACPContentBlockType::Error:
				Markdown += FString::Printf(TEXT("> **Error:** %s\n\n"), *Block.Text);
				break;

			case EACPContentBlockType::System:
				Markdown += FString::Printf(TEXT("> **System:** %s\n\n"), *Block.Text);
				break;

			case EACPContentBlockType::Image:
				Markdown += TEXT("_Image block omitted from markdown export._\n\n");
				break;

			default:
				break;
			}
		}
	}

	return Markdown;
}

// ── Session Management ──────────────────────────────────────────────

FString UWebUIBridge::RenameSession(const FString& SessionId, const FString& NewTitle)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	FString TrimmedTitle = NewTitle.TrimStartAndEnd();

	if (TrimmedTitle.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	SessionMgr.SetCustomTitle(SessionId, TrimmedTitle);
	FACPSessionMetadata StoredMetadata;
	if (FNeoStackChatStore::Get().LoadSession(SessionId, StoredMetadata))
	{
		StoredMetadata.Title = TrimmedTitle;
		StoredMetadata.bHasCustomTitle = true;
		StoredMetadata.LastModifiedAt = FDateTime::Now();
		FNeoStackChatStore::Get().UpsertSession(StoredMetadata);
	}
	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::DeleteSession(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (SessionId.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		return JsonToString(Result);
	}

	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	FACPSessionManager& SessionMgr = FACPSessionManager::Get();

	// Find the agent that owns this session and resolve the agent's native session ID.
	// The SessionId from JS may be a Unreal GUID (for sessions created through the UI)
	// or the agent's native session ID (for sessions only listed from the remote list).
	// The JSONL file on disk is named after the agent's native ID, not the Unreal GUID.
	FString AgentName = AgentMgr.GetSessionAgent(SessionId);
	FString AgentSessionId;

	// Get the agent's native session ID from the active session metadata (before closing it)
	const FACPActiveSession* ActiveSession = SessionMgr.GetActiveSession(SessionId);
	if (ActiveSession)
	{
		AgentSessionId = ActiveSession->Metadata.AgentSessionId;
	}

	// Also try to grab the session's cwd from the cached remote list
	FString SessionCwd;

	{
		// Search cached session lists — always, to grab Cwd even when AgentName is already known
		FString LookupId = AgentSessionId.IsEmpty() ? SessionId : AgentSessionId;
		for (const FString& Name : AgentMgr.GetAvailableAgentNames())
		{
			TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(Name);
			for (const FACPRemoteSessionEntry& Entry : Sessions)
			{
				if (Entry.SessionId == LookupId || Entry.SessionId == SessionId)
				{
					if (AgentName.IsEmpty()) AgentName = Name;
					if (SessionCwd.IsEmpty()) SessionCwd = Entry.Cwd;
					break;
				}
			}
			if (!AgentName.IsEmpty() && !SessionCwd.IsEmpty()) break;
		}
	}

	// The ID to use for file deletion: prefer the agent's native ID, fall back to SessionId
	// (which is already the native ID for sessions that were never opened through the UI)
	FString FileSessionId = AgentSessionId.IsEmpty() ? SessionId : AgentSessionId;

	// Resolve the working directory for file operations:
	// 1. Cwd from the remote session entry (most accurate — recorded in the session file itself)
	// 2. Agent's configured WorkingDirectory
	// 3. FPaths::ProjectDir() as last resort
	FString WorkingDir = SessionCwd;
	if (WorkingDir.IsEmpty())
	{
		if (FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName))
		{
			WorkingDir = Config->WorkingDirectory;
		}
	}
	if (WorkingDir.IsEmpty())
	{
		WorkingDir = FPaths::ProjectDir();
	}

	// Close the active session
	SessionMgr.CloseSession(SessionId);
	FNeoStackAgentRuntimeService::Get().CloseSession(SessionId);
	FNeoStackChatStore::Get().MarkSessionDeleted(SessionId);

	// Try ACP session/delete if the agent supports it (Zed parity)
	if (!AgentName.IsEmpty() && !FileSessionId.IsEmpty())
	{
		AgentMgr.DeleteRemoteSession(AgentName, FileSessionId);
	}

	// Delete the agent's native session file on disk.
	if (!AgentName.IsEmpty())
	{
		if (ACPAgentIdentity::IsClaudeName(AgentName))
		{
			FString SessionFilePath = FACPClaudeCodeHistoryReader::GetSessionJsonlPath(FileSessionId, WorkingDir);
			if (FPaths::FileExists(SessionFilePath))
			{
				IFileManager::Get().Delete(*SessionFilePath);
				UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Deleted Claude Code session file: %s"), *SessionFilePath);
			}
			else
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: Claude Code session file not found for deletion: %s"), *SessionFilePath);
			}

			// Also delete the session directory (contains subagent data)
			FString SessionDirPath = FPaths::GetPath(SessionFilePath) / FileSessionId;
			if (FPaths::DirectoryExists(SessionDirPath))
			{
				IFileManager::Get().DeleteDirectory(*SessionDirPath, false, true);
				UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Deleted Claude Code session directory: %s"), *SessionDirPath);
			}
		}
		else if (ACPAgentIdentity::IsCodexName(AgentName))
		{
			// Codex stores sessions as: ~/.codex/sessions/[YYYY/MM/DD/]rollout-YYYY-MM-DDThh-mm-ss-<session-uuid>.jsonl
			// The session UUID is the last component of the filename before .jsonl
			FString CodexHome = FPaths::Combine(FPlatformProcess::UserHomeDir(), TEXT(".codex"));
			FString SessionsDir = FPaths::Combine(CodexHome, TEXT("sessions"));

			if (FPaths::DirectoryExists(SessionsDir))
			{
				// Search recursively for the file containing the session UUID
				TArray<FString> FoundFiles;
				FString SearchPattern = FString::Printf(TEXT("*-%s.jsonl"), *FileSessionId);
				IFileManager::Get().FindFilesRecursive(FoundFiles, *SessionsDir, *SearchPattern, true, false);

				if (FoundFiles.Num() > 0)
				{
					IFileManager::Get().Delete(*FoundFiles[0]);
					UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Deleted Codex session file: %s"), *FoundFiles[0]);
				}
				else
				{
					UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: Codex session file not found for deletion (searched %s for *-%s.jsonl)"), *SessionsDir, *FileSessionId);
				}
			}

			// Also check archived sessions
			FString ArchivedDir = FPaths::Combine(CodexHome, TEXT("archived_sessions"));
			if (FPaths::DirectoryExists(ArchivedDir))
			{
				TArray<FString> ArchivedFiles;
				FString SearchPattern = FString::Printf(TEXT("*-%s.jsonl"), *FileSessionId);
				IFileManager::Get().FindFilesRecursive(ArchivedFiles, *ArchivedDir, *SearchPattern, true, false);

				if (ArchivedFiles.Num() > 0)
				{
					IFileManager::Get().Delete(*ArchivedFiles[0]);
					UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Deleted archived Codex session file: %s"), *ArchivedFiles[0]);
				}
			}
		}

		// Refresh the cached session list so the deleted session is removed
		AgentMgr.RequestSessionList(AgentName);
	}

	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::ExportSessionToMarkdown(const FString& SessionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), false);
	Result->SetBoolField(TEXT("canceled"), false);

	if (SessionId.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("Empty session ID"));
		return JsonToString(Result);
	}

	FACPSessionManager& SessionMgr = FACPSessionManager::Get();
	const FACPActiveSession* Session = SessionMgr.GetActiveSession(SessionId);
	if (!Session)
	{
		Result->SetStringField(
			TEXT("error"),
			TEXT("Session is not loaded in memory. Open the chat once to load ACP history, then export.")
		);
		return JsonToString(Result);
	}

	if (Session->Messages.Num() == 0 && Session->bIsLoadingHistory)
	{
		Result->SetStringField(
			TEXT("error"),
			TEXT("Session history is still loading from ACP. Wait a moment and try export again.")
		);
		return JsonToString(Result);
	}

	const FString Markdown = BuildSessionMarkdown(*Session);

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		Result->SetStringField(TEXT("error"), TEXT("Desktop platform unavailable"));
		return JsonToString(Result);
	}

	const void* ParentWindowHandle = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	const FString DefaultFileName = SanitizeExportFilename(Session->Metadata.Title) + TEXT(".md");
	TArray<FString> SaveFilenames;
	const bool bDialogAccepted = DesktopPlatform->SaveFileDialog(
		ParentWindowHandle,
		TEXT("Export Conversation"),
		FPaths::ProjectSavedDir(),
		DefaultFileName,
		TEXT("Markdown Files (*.md)|*.md"),
		0,
		SaveFilenames
	);

	if (!bDialogAccepted || SaveFilenames.Num() == 0)
	{
		Result->SetBoolField(TEXT("canceled"), true);
		return JsonToString(Result);
	}

	const FString& SavePath = SaveFilenames[0];
	const bool bSaved = FFileHelper::SaveStringToFile(
		Markdown,
		*SavePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM
	);
	if (!bSaved)
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to write markdown file"));
		return JsonToString(Result);
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("savedPath"), SavePath);
	return JsonToString(Result);
}

// ── Project Indexing ────────────────────────────────────────────────

FString UWebUIBridge::GetIndexingSettings()
{
	return UProjectIndexManager::Get().GetSettingsJson();
}

FString UWebUIBridge::GetIndexingStatus()
{
	return UProjectIndexManager::Get().GetStatusJson();
}

void UWebUIBridge::SetIndexingProvider(const FString& Provider)
{
	UProjectIndexManager::Get().SetProvider(Provider);
}

void UWebUIBridge::SetIndexingEndpointUrl(const FString& Url)
{
	UProjectIndexManager::Get().SetEndpointUrl(Url);
}

void UWebUIBridge::SetIndexingApiKey(const FString& Key)
{
	UProjectIndexManager::Get().SetApiKey(Key);
}

void UWebUIBridge::SetIndexingModel(const FString& Model)
{
	UProjectIndexManager::Get().SetModel(Model);
}

void UWebUIBridge::SetIndexingDimensions(int32 Dims)
{
	UProjectIndexManager::Get().SetDimensions(Dims);
}

void UWebUIBridge::SetAutoIndex(bool bEnabled)
{
	UProjectIndexManager::Get().SetAutoIndex(bEnabled);
}

void UWebUIBridge::SetIndexingScopeEnabled(const FString& ScopeKey, bool bEnabled)
{
	UProjectIndexManager::Get().SetScopeEnabled(ScopeKey, bEnabled);
}

void UWebUIBridge::StartIndexing()
{
	UProjectIndexManager::Get().StartIndexing();
}

void UWebUIBridge::ClearIndex()
{
	UProjectIndexManager::Get().ClearIndex();
}

// ── Studio / Generative Providers ──────────────────────────────────

static FString StatusToString(EGenerativeJobStatus Status)
{
	switch (Status)
	{
	case EGenerativeJobStatus::Pending:   return TEXT("pending");
	case EGenerativeJobStatus::Running:   return TEXT("running");
	case EGenerativeJobStatus::Succeeded: return TEXT("succeeded");
	case EGenerativeJobStatus::Failed:    return TEXT("failed");
	case EGenerativeJobStatus::Cancelled: return TEXT("cancelled");
	default:                              return TEXT("unknown");
	}
}

static TSharedRef<FJsonObject> JobToJson(const FGenerativeJob& Job)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("providerId"), Job.ProviderId);
	Obj->SetStringField(TEXT("actionId"), Job.ActionId);
	Obj->SetStringField(TEXT("jobId"), Job.JobId);
	Obj->SetStringField(TEXT("status"), StatusToString(Job.Status));
	Obj->SetNumberField(TEXT("progress"), Job.Progress);
	Obj->SetStringField(TEXT("resultUrl"), Job.ResultUrl);
	Obj->SetStringField(TEXT("thumbnailUrl"), Job.ThumbnailUrl);
	Obj->SetStringField(TEXT("error"), Job.ErrorMessage);

	// Extra URLs (format → url)
	TSharedRef<FJsonObject> ExtrasObj = MakeShared<FJsonObject>();
	for (const auto& Pair : Job.ExtraUrls)
	{
		ExtrasObj->SetStringField(Pair.Key, Pair.Value);
	}
	Obj->SetObjectField(TEXT("extraUrls"), ExtrasObj);

	// Image URLs
	TArray<TSharedPtr<FJsonValue>> ImagesArr;
	for (const FString& Url : Job.ImageUrls)
	{
		ImagesArr.Add(MakeShared<FJsonValueString>(Url));
	}
	Obj->SetArrayField(TEXT("imageUrls"), ImagesArr);

	return Obj;
}

FString UWebUIBridge::GetGenerativeProviders()
{
	auto& Registry = FGenerativeProviderRegistry::Get();
	TArray<TSharedPtr<FJsonValue>> ProvidersArr;

	for (const auto& Provider : Registry.GetAll())
	{
		TSharedRef<FJsonObject> ProvObj = MakeShared<FJsonObject>();
		ProvObj->SetStringField(TEXT("id"), Provider->GetId());
		ProvObj->SetStringField(TEXT("displayName"), Provider->GetDisplayName());
		ProvObj->SetStringField(TEXT("website"), Provider->GetWebsite());

		// Actions
		TArray<TSharedPtr<FJsonValue>> ActionsArr;
		for (const auto& Action : Provider->GetActions())
		{
			TSharedRef<FJsonObject> ActObj = MakeShared<FJsonObject>();
			ActObj->SetStringField(TEXT("actionId"), Action.ActionId);
			ActObj->SetStringField(TEXT("description"), Action.Description);
			ActObj->SetStringField(TEXT("creditCost"), Action.CreditCost);
			ActObj->SetBoolField(TEXT("isSynchronous"), Action.bIsSynchronous);

			// Input hints
			TArray<TSharedPtr<FJsonValue>> InHints;
			for (const FString& H : Action.InputHints) InHints.Add(MakeShared<FJsonValueString>(H));
			ActObj->SetArrayField(TEXT("inputHints"), InHints);

			// Output hints
			TArray<TSharedPtr<FJsonValue>> OutHints;
			for (const FString& H : Action.OutputHints) OutHints.Add(MakeShared<FJsonValueString>(H));
			ActObj->SetArrayField(TEXT("outputHints"), OutHints);

			// Schema (already a FJsonObject)
			if (Action.ParamsSchema.IsValid())
			{
				ActObj->SetObjectField(TEXT("paramsSchema"), Action.ParamsSchema);
			}

			ActionsArr.Add(MakeShared<FJsonValueObject>(ActObj));
		}
		ProvObj->SetArrayField(TEXT("actions"), ActionsArr);

		ProvidersArr.Add(MakeShared<FJsonValueObject>(ProvObj));
	}

	return JsonArrayToString(ProvidersArr);
}

// Spin-pump until the predicate returns true. Pumps the HTTP module + game-thread
// task graph so async HTTP completions (which fire callbacks via these systems)
// can deliver into our `bDone` flag while we wait. Same wall-clock as the old
// sync provider API; freezes Slate during the wait.
//
// Used only by WebUIBridge UFUNCTIONS, which are infrequent admin-style calls
// from the embedded UI panel. The agent's hot path goes through the Lua
// `generate()` binding, which yields via coroutine and never freezes the editor.
namespace
{
	template<typename PredicateT>
	void WebUI_PumpUntil(PredicateT Predicate)
	{
		while (!Predicate())
		{
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread_Local);
			FPlatformProcess::Sleep(0.005f);
		}
	}
}

FString UWebUIBridge::SubmitGenerativeJob(const FString& ProviderId, const FString& ActionId, const FString& ParamsJson)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	// Parse params JSON
	TSharedPtr<FJsonObject> Params;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ParamsJson);
	if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	FGenerativeJob CapturedJob;
	bool bDone = false;

	Provider->Submit(ActionId, Params,
		[&CapturedJob, &bDone, ProviderId, ActionId](const FGenerativeJob& Job)
		{
			CapturedJob = Job;
			CapturedJob.ProviderId = ProviderId;
			CapturedJob.ActionId = ActionId;
			bDone = true;
		});

	WebUI_PumpUntil([&bDone]() { return bDone; });

	Result->SetBoolField(TEXT("success"), CapturedJob.Status != EGenerativeJobStatus::Failed);
	Result->SetObjectField(TEXT("job"), JobToJson(CapturedJob));

	return JsonToString(Result);
}

FString UWebUIBridge::CheckGenerativeJobStatus(const FString& ProviderId, const FString& JobId, const FString& ActionId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	FGenerativeJob CapturedJob;
	bool bDone = false;

	Provider->CheckStatus(JobId, ActionId,
		[&CapturedJob, &bDone, ProviderId, ActionId](const FGenerativeJob& Job)
		{
			CapturedJob = Job;
			CapturedJob.ProviderId = ProviderId;
			CapturedJob.ActionId = ActionId;
			bDone = true;
		});

	WebUI_PumpUntil([&bDone]() { return bDone; });

	Result->SetBoolField(TEXT("success"), true);
	Result->SetObjectField(TEXT("job"), JobToJson(CapturedJob));

	return JsonToString(Result);
}

FString UWebUIBridge::GetGenerativeBalance(const FString& ProviderId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto Provider = FGenerativeProviderRegistry::Get().Find(ProviderId);
	if (!Provider.IsValid())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Provider '%s' not found"), *ProviderId));
		return JsonToString(Result);
	}

	int32 CapturedBalance = -1;
	FString CapturedError;
	bool bDone = false;

	Provider->GetBalance(
		[&CapturedBalance, &CapturedError, &bDone](int32 Balance, const FString& Error)
		{
			CapturedBalance = Balance;
			CapturedError = Error;
			bDone = true;
		});

	WebUI_PumpUntil([&bDone]() { return bDone; });

	const bool bOk = (CapturedBalance >= 0);
	Result->SetBoolField(TEXT("success"), bOk);
	Result->SetNumberField(TEXT("balance"), CapturedBalance);
	if (!bOk && !CapturedError.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), CapturedError);
	}

	return JsonToString(Result);
}

// ── Terminal ────────────────────────────────────────────────────────

FString UWebUIBridge::StartTerminal(const FString& WorkingDir, const FString& Shell)
{
	FString EffectiveDir = WorkingDir;
	if (EffectiveDir.IsEmpty())
	{
		EffectiveDir = FPaths::ProjectDir();
	}

	FString TerminalId = FTerminalManager::Get().StartTerminal(EffectiveDir, Shell);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (TerminalId.IsEmpty())
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to create terminal session"));
	}
	else
	{
		Result->SetStringField(TEXT("terminalId"), TerminalId);
	}
	return JsonToString(Result);
}

void UWebUIBridge::WriteTerminal(const FString& TerminalId, const FString& Data)
{
	if (TerminalId.IsEmpty() || Data.IsEmpty()) return;
	FTerminalManager::Get().WriteTerminal(TerminalId, Data);
}

void UWebUIBridge::ResizeTerminal(const FString& TerminalId, int32 Cols, int32 Rows)
{
	if (TerminalId.IsEmpty() || Cols <= 0 || Rows <= 0) return;
	FTerminalManager::Get().ResizeTerminal(TerminalId, Cols, Rows);
}

void UWebUIBridge::CloseTerminal(const FString& TerminalId)
{
	if (TerminalId.IsEmpty()) return;
	FTerminalManager::Get().CloseTerminal(TerminalId);
}

void UWebUIBridge::BindOnTerminalOutput(FWebJSFunction Callback)
{
	OnTerminalOutputCallback = Callback;

	if (!TerminalOutputHandle.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		TerminalOutputHandle = FTerminalManager::Get().OnTerminalOutput.AddLambda(
			[WeakThis](const FString& TerminalId, const FString& Base64Data)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TerminalId, Base64Data]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnTerminalOutputCallback.IsValid())
						{
							Self->OnTerminalOutputCallback(TerminalId, Base64Data);
						}
					}
				});
			});
	}
}

void UWebUIBridge::BindOnTerminalExit(FWebJSFunction Callback)
{
	OnTerminalExitCallback = Callback;

	if (!TerminalExitHandle.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		TerminalExitHandle = FTerminalManager::Get().OnTerminalExit.AddLambda(
			[WeakThis](const FString& TerminalId, int32 ExitCode)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakThis, TerminalId, ExitCode]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnTerminalExitCallback.IsValid())
						{
							Self->OnTerminalExitCallback(TerminalId, ExitCode);
						}
					}
				});
			});
	}
}

// ── Source Control ──────────────────────────────────────────────────

FString UWebUIBridge::GetSourceControlStatus()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

#if ENGINE_MINOR_VERSION >= 6
	ISourceControlModule* SCModule = ISourceControlModule::GetPtr();
#else
	ISourceControlModule* SCModule = FModuleManager::GetModulePtr<ISourceControlModule>("SourceControl");
#endif
	if (!SCModule || !SCModule->IsEnabled())
	{
		Result->SetBoolField(TEXT("enabled"), false);
		Result->SetStringField(TEXT("provider"), TEXT(""));
		Result->SetStringField(TEXT("branch"), TEXT(""));
		Result->SetNumberField(TEXT("changesCount"), -1);
		Result->SetBoolField(TEXT("connected"), false);
		return JsonToString(Result);
	}

	ISourceControlProvider& Provider = SCModule->GetProvider();
	Result->SetBoolField(TEXT("enabled"), true);
	Result->SetStringField(TEXT("provider"), Provider.GetName().ToString());
	Result->SetBoolField(TEXT("connected"), Provider.IsAvailable());

	TMap<ISourceControlProvider::EStatus, FString> Status = Provider.GetStatus();
	FString* Branch = Status.Find(ISourceControlProvider::EStatus::Branch);
	Result->SetStringField(TEXT("branch"), Branch ? *Branch : TEXT(""));

	TOptional<int> NumChanges = Provider.GetNumLocalChanges();
	Result->SetNumberField(TEXT("changesCount"), NumChanges.IsSet() ? NumChanges.GetValue() : -1);

	return JsonToString(Result);
}

void UWebUIBridge::OpenSourceControlChangelist()
{
	ISourceControlWindowsModule& SCWindows = ISourceControlWindowsModule::Get();
	if (SCWindows.CanShowChangelistsTab())
	{
		SCWindows.ShowChangelistsTab();
	}
}

void UWebUIBridge::OpenSourceControlSubmit()
{
	FSourceControlWindows::ChoosePackagesToCheckIn();
}

// ── NeoStack Sign-in (Device Authorization Grant) ──────────────────

namespace
{
	const TCHAR* DeviceAuthStatusToString(ENeoStackDeviceAuthStatus S)
	{
		switch (S)
		{
		case ENeoStackDeviceAuthStatus::Idle:           return TEXT("idle");
		case ENeoStackDeviceAuthStatus::RequestingCode: return TEXT("requesting");
		case ENeoStackDeviceAuthStatus::WaitingForUser: return TEXT("waiting");
		case ENeoStackDeviceAuthStatus::Polling:        return TEXT("polling");
		case ENeoStackDeviceAuthStatus::Redeeming:      return TEXT("redeeming");
		case ENeoStackDeviceAuthStatus::Success:        return TEXT("success");
		case ENeoStackDeviceAuthStatus::Error:          return TEXT("error");
		default:                                        return TEXT("idle");
		}
	}
}

void UWebUIBridge::StartNeoStackDeviceAuth()
{
	FNeoStackDeviceAuth::Get().Begin();
}

void UWebUIBridge::CancelNeoStackDeviceAuth()
{
	FNeoStackDeviceAuth::Get().Cancel();
}

void UWebUIBridge::BindOnDeviceAuthStatusChanged(FWebJSFunction Callback)
{
	OnDeviceAuthStatusCallback = Callback;

	if (DeviceAuthStatusHandle.IsValid())
	{
		FNeoStackDeviceAuth::Get().OnStatusChanged().Remove(DeviceAuthStatusHandle);
	}
	TWeakObjectPtr<UWebUIBridge> WeakThis(this);
	DeviceAuthStatusHandle = FNeoStackDeviceAuth::Get().OnStatusChanged().AddLambda(
		[WeakThis](ENeoStackDeviceAuthStatus S, const FString& Message, const FString& VerifyUri)
		{
			UWebUIBridge* Self = WeakThis.Get();
			if (!Self || !Self->OnDeviceAuthStatusCallback.IsValid()) return;
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("status"), DeviceAuthStatusToString(S));
			Obj->SetStringField(TEXT("message"), Message);
			Obj->SetStringField(TEXT("verificationUri"), VerifyUri);
			Self->OnDeviceAuthStatusCallback(JsonToString(Obj));
		});
}

// ── Agent Setup ─────────────────────────────────────────────────────

FString UWebUIBridge::GetAgentInstallInfo(const FString& AgentName)
{
	// Legacy — agent info is now provided by the ACP registry
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("agentName"), AgentName);
	return JsonToString(Result);
}

void UWebUIBridge::InstallAgent(const FString& AgentName)
{
	// Legacy — agent installation is now handled via the ACP registry
	UE_LOG(LogTemp, Warning, TEXT("WebUIBridge::InstallAgent called for '%s' — use registry-based install instead"), *AgentName);
}

FString UWebUIBridge::RefreshAgentStatus(const FString& AgentName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (AgentName.IsEmpty())
	{
		Result->SetStringField(TEXT("status"), TEXT("unknown"));
		Result->SetStringField(TEXT("statusMessage"), TEXT("Empty agent name"));
		return JsonToString(Result);
	}

	// Invalidate cache and re-evaluate
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->InvalidateAgentStatusCache();
	}

	// Re-fetch configs (this triggers re-evaluation of all agents)
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FACPAgentConfig> Configs = AgentMgr.GetAllAgentConfigs();

	// Find the requested agent
	for (const FACPAgentConfig& Config : Configs)
	{
		if (Config.AgentName == AgentName)
		{
			FString StatusStr;
			switch (Config.Status)
			{
			case EACPAgentStatus::Available:     StatusStr = TEXT("available"); break;
			case EACPAgentStatus::NotInstalled:  StatusStr = TEXT("not_installed"); break;
			case EACPAgentStatus::MissingApiKey: StatusStr = TEXT("missing_key"); break;
			default:                             StatusStr = TEXT("unknown"); break;
			}
			Result->SetStringField(TEXT("status"), StatusStr);
			Result->SetStringField(TEXT("statusMessage"), Config.StatusMessage);
			return JsonToString(Result);
		}
	}

	Result->SetStringField(TEXT("status"), TEXT("unknown"));
	Result->SetStringField(TEXT("statusMessage"), TEXT("Agent not found"));
	return JsonToString(Result);
}

FString UWebUIBridge::GetRegistryAgents()
{
	const FACPRegistryClient& Registry = FACPRegistryClient::Get();
	const TArray<FACPRegistryAgent>& Agents = Registry.GetAgents();
	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();

	TArray<TSharedPtr<FJsonValue>> AgentsArr;
	AgentsArr.Reserve(Agents.Num());

	for (const FACPRegistryAgent& Agent : Agents)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Agent.Id);
		Obj->SetStringField(TEXT("name"), Agent.Name);
		Obj->SetStringField(TEXT("version"), Agent.Version);
		Obj->SetStringField(TEXT("description"), Agent.Description);
		Obj->SetStringField(TEXT("license"), Agent.License);
		Obj->SetStringField(TEXT("icon"), Agent.IconUrl);
		Obj->SetStringField(TEXT("repository"), Agent.Repository);

		// Authors
		TArray<TSharedPtr<FJsonValue>> AuthorsArr;
		for (const FString& Author : Agent.Authors)
		{
			AuthorsArr.Add(MakeShared<FJsonValueString>(Author));
		}
		Obj->SetArrayField(TEXT("authors"), AuthorsArr);

		// Distribution availability
		Obj->SetBoolField(TEXT("hasBinary"), Agent.Distribution.HasBinaryForPlatform(PlatformKey));
		Obj->SetBoolField(TEXT("hasNpx"), Agent.Distribution.HasNpx());
		Obj->SetBoolField(TEXT("hasUvx"), Agent.Distribution.HasUvx());

		if (Agent.Distribution.HasNpx())
		{
			Obj->SetStringField(TEXT("npxPackage"), Agent.Distribution.NpxPackage);
		}
		if (Agent.Distribution.HasUvx())
		{
			Obj->SetStringField(TEXT("uvxPackage"), Agent.Distribution.UvxPackage);
		}

		// Install status — check if the agent is in the user's InstalledAgentIds list
		const UACPSettings* Settings = UACPSettings::Get();
		bool bIsInstalled = Settings && Settings->InstalledAgentIds.Contains(Agent.Id);
		Obj->SetBoolField(TEXT("isInstalled"), bIsInstalled);

		// Update availability
		const TArray<FAgentUpdateInfo>& Updates = Registry.GetAgentUpdates();
		const FAgentUpdateInfo* UpdateInfo = Updates.FindByPredicate(
			[&Agent](const FAgentUpdateInfo& U) { return U.AgentId == Agent.Id; });
		if (UpdateInfo)
		{
			Obj->SetBoolField(TEXT("updateAvailable"), true);
			Obj->SetStringField(TEXT("installedVersion"), UpdateInfo->InstalledVersion);
			Obj->SetStringField(TEXT("latestVersion"), UpdateInfo->LatestVersion);
		}

		AgentsArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToString(AgentsArr);
}

void UWebUIBridge::RefreshRegistry()
{
	FACPRegistryClient::Get().RefreshAsync();
}

FString UWebUIBridge::GetAgentUpdates()
{
	const FACPRegistryClient& Registry = FACPRegistryClient::Get();
	const TArray<FAgentUpdateInfo>& Updates = Registry.GetAgentUpdates();

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FAgentUpdateInfo& Update : Updates)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("agentId"), Update.AgentId);
		Obj->SetStringField(TEXT("agentName"), Update.AgentName);
		Obj->SetStringField(TEXT("installedVersion"), Update.InstalledVersion);
		Obj->SetStringField(TEXT("latestVersion"), Update.LatestVersion);
		Obj->SetBoolField(TEXT("isNpx"), Update.bIsNpx);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	return JsonArrayToString(Arr);
}

void UWebUIBridge::UpdateRegistryAgent(const FString& AgentId)
{
	// For managed binary agents: delete the old version directory. The next
	// agent config refresh will either use an alternate distribution (npx/uvx)
	// or lazily download the current binary when appropriate.
	FACPRegistryClient& Registry = FACPRegistryClient::Get();
	const FACPRegistryAgent* Agent = Registry.FindAgent(AgentId);
	if (!Agent)
	{
		return;
	}

	const FString PlatformKey = FACPRegistryClient::GetCurrentPlatformKey();
	if (Agent->Distribution.HasBinaryForPlatform(PlatformKey))
	{
		// Uninstall old binary (clears managed directory)
		const bool bRemovedBinary = FAgentInstaller::Get().UninstallRegistryAgent(AgentId);

		// Re-initialize configs so they resolve against current registry/install state.
		FACPAgentManager::Get().InitializeDefaultAgents();

		if (bRemovedBinary)
		{
			UE_LOG(LogNeoStackAI, Log, TEXT("WebUI: Agent '%s' update triggered — old managed binary removed; current registry version is v%s"),
				*AgentId, *Agent->Version);
		}
		else
		{
			UE_LOG(LogNeoStackAI, Verbose, TEXT("WebUI: Agent '%s' update requested but no managed binary directory was present"), *AgentId);
		}
	}

	Registry.RefreshCachedUpdates();
}

void UWebUIBridge::InstallRegistryAgent(const FString& AgentId, const FString& Method)
{
	// Zed model: "Install" = add agent ID to settings. No download, no process spawn.
	// Process spawns lazily when the user first opens a chat with this agent.
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return;
	}

	if (!Settings->InstalledAgentIds.Contains(AgentId))
	{
		Settings->InstalledAgentIds.Add(AgentId);
		Settings->SaveInstalledAgentIds();
		UE_LOG(LogNeoStackAI, Log, TEXT("WebUI: Installed agent '%s' (added to settings, total: %d)"), *AgentId, Settings->InstalledAgentIds.Num());

		// Reinitialize agent configs so the sidebar picks up the new agent
		FACPAgentManager::Get().InitializeDefaultAgents();
		FACPRegistryClient::Get().RefreshCachedUpdates();
	}
	else
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("WebUI: Agent '%s' already installed"), *AgentId);
	}
}

void UWebUIBridge::UninstallRegistryAgent(const FString& AgentId)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings)
	{
		return;
	}

	if (Settings->InstalledAgentIds.Remove(AgentId) > 0)
	{
		Settings->SaveInstalledAgentIds();
		UE_LOG(LogNeoStackAI, Log, TEXT("WebUI: Uninstalled agent '%s' (removed from settings)"), *AgentId);

		// Disconnect if connected
		FACPAgentManager::Get().DisconnectFromAgent(AgentId);

		// Reinitialize
		FACPAgentManager::Get().InitializeDefaultAgents();
		FACPRegistryClient::Get().RefreshCachedUpdates();
	}
}

FString UWebUIBridge::GetPrerequisiteStatus()
{
	FAgentInstaller& Installer = FAgentInstaller::Get();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	auto CheckTool = [&](const FString& Name, const FString& ExecutableName, const FString& VersionFlag)
	{
		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		FString ResolvedPath;
		bool bFound = Installer.ResolveExecutable(ExecutableName, ResolvedPath)
			|| Installer.ResolveExecutableViaLoginShell(ExecutableName, ResolvedPath);

		ToolObj->SetBoolField(TEXT("found"), bFound);
		ToolObj->SetStringField(TEXT("path"), bFound ? ResolvedPath : TEXT(""));

		// Try to get version
		if (bFound)
		{
			FString StdOut, StdErr;
			int32 ReturnCode = -1;
			FPlatformProcess::ExecProcess(*ResolvedPath, *VersionFlag, &ReturnCode, &StdOut, &StdErr);
			if (ReturnCode == 0)
			{
				FString Version = StdOut.TrimStartAndEnd();
				// Take first line only
				int32 NewlineIdx;
				if (Version.FindChar(TEXT('\n'), NewlineIdx))
				{
					Version = Version.Left(NewlineIdx).TrimStartAndEnd();
				}
				ToolObj->SetStringField(TEXT("version"), Version);
			}
		}

		Result->SetObjectField(Name, ToolObj);
	};

	CheckTool(TEXT("node"), TEXT("node"), TEXT("--version"));
	CheckTool(TEXT("npm"), TEXT("npm"), TEXT("--version"));
	CheckTool(TEXT("npx"), TEXT("npx"), TEXT("--version"));
	CheckTool(TEXT("git"), TEXT("git"), TEXT("--version"));
	CheckTool(TEXT("uv"), TEXT("uv"), TEXT("--version"));
	CheckTool(TEXT("uvx"), TEXT("uvx"), TEXT("--version"));
	CheckTool(TEXT("bun"), TEXT("bun"), TEXT("--version"));

	return JsonToString(Result);
}

FString UWebUIBridge::GetMcpConnectionInfo()
{
	const UACPSettings* Settings = UACPSettings::Get();
	int32 Port = FMCPServer::Get().GetPort();
	if (Port <= 0)
	{
		Port = Settings ? Settings->MCPServerPort : 9315;
	}

	const FString ServerName = TEXT("unreal-editor");
	const FString RecommendedUrl = FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port);
	const FString LocalhostUrl = FString::Printf(TEXT("http://localhost:%d/mcp"), Port);
	const FString LegacySseUrl = FString::Printf(TEXT("http://127.0.0.1:%d/sse"), Port);
	const FString LegacyMessageUrl = FString::Printf(TEXT("http://127.0.0.1:%d/message"), Port);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("serverName"), ServerName);
	Result->SetNumberField(TEXT("port"), Port);
	Result->SetBoolField(TEXT("isRunning"), FMCPServer::Get().IsRunning());
	Result->SetStringField(TEXT("recommendedUrl"), RecommendedUrl);
	Result->SetStringField(TEXT("localhostUrl"), LocalhostUrl);
	Result->SetStringField(TEXT("legacySseUrl"), LegacySseUrl);
	Result->SetStringField(TEXT("legacyMessageUrl"), LegacyMessageUrl);
	Result->SetStringField(TEXT("transport"), TEXT("streamable_http"));
	return JsonToString(Result);
}

void UWebUIBridge::CopyToClipboard(const FString& Text)
{
	FPlatformApplicationMisc::ClipboardCopy(*Text);
}

FString UWebUIBridge::GetClipboardText()
{
	FString ClipboardContent;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardContent);
	return ClipboardContent;
}

void UWebUIBridge::OpenUrl(const FString& Url)
{
	if (!Url.IsEmpty())
	{
		FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	}
}

void UWebUIBridge::OpenPath(const FString& Path, int32 Line)
{
	if (Path.IsEmpty()) return;
	UE_LOG(LogNeoStackAI, Log, TEXT("OpenPath: '%s' (line %d)"), *Path, Line);

	// Check if it's a UE asset path (/Game/, /Engine/, /Script/, or any /MountPoint/ path)
	if (Path.StartsWith(TEXT("/")))
	{
		// Try to find as an asset first
		FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FAssetData AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(Path));

		if (!AssetData.IsValid())
		{
			// Try with .0 suffix stripped (some paths include sub-object)
			FString CleanPath = Path;
			int32 DotIndex;
			if (CleanPath.FindLastChar('.', DotIndex))
			{
				CleanPath = CleanPath.Left(DotIndex);
				AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(CleanPath));
			}
		}

		if (!AssetData.IsValid())
		{
			// Package path without .AssetName suffix (e.g. /Game/Path/BP_C7 → /Game/Path/BP_C7.BP_C7)
			FString AssetName = FPaths::GetBaseFilename(Path);
			if (!AssetName.IsEmpty())
			{
				FString FullObjectPath = Path + TEXT(".") + AssetName;
				AssetData = AssetRegistry.Get().GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
			}
		}

		if (!AssetData.IsValid())
		{
			// Fall back to package name lookup (handles any asset in the package)
			TArray<FAssetData> PackageAssets;
			AssetRegistry.Get().GetAssetsByPackageName(*Path, PackageAssets);
			if (PackageAssets.Num() > 0)
			{
				AssetData = PackageAssets[0];
			}
		}

		if (AssetData.IsValid())
		{
			// Load and open the asset in its editor
			UObject* LoadedAsset = AssetData.GetAsset();
			if (!LoadedAsset)
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("OpenPath: GetAsset() returned null for '%s', trying LoadObject"), *Path);
				LoadedAsset = LoadObject<UObject>(nullptr, *AssetData.GetSoftObjectPath().ToString());
			}

			if (LoadedAsset)
			{
				UE_LOG(LogNeoStackAI, Log, TEXT("OpenPath: Opening asset '%s' (%s)"), *LoadedAsset->GetName(), *LoadedAsset->GetClass()->GetName());
				NeoLuaEditor::OpenAssetEditor(LoadedAsset);
			}
			else
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("OpenPath: Failed to load asset for '%s', falling back to Content Browser sync"), *Path);
				// At least navigate Content Browser to the asset
				TArray<FAssetData> Assets = { AssetData };
				IContentBrowserSingleton::Get().SyncBrowserToAssets(Assets, /*bAllowLockedBrowsers=*/true, /*bFocusContentBrowser=*/true);
			}
			return;
		}
	}

	// Check if it's a filesystem source file
	FString FullPath = Path;
	if (!FPaths::FileExists(FullPath))
	{
		// Try relative to project
		FullPath = FPaths::Combine(FPaths::ProjectDir(), Path);
	}

	if (FPaths::FileExists(FullPath))
	{
		FString Extension = FPaths::GetExtension(FullPath).ToLower();

		// Source/text files — open in IDE with line number
		if (Extension == TEXT("h") || Extension == TEXT("cpp") || Extension == TEXT("c") ||
			Extension == TEXT("cs") || Extension == TEXT("py") || Extension == TEXT("js") ||
			Extension == TEXT("ts") || Extension == TEXT("ini") || Extension == TEXT("txt") ||
			Extension == TEXT("md") || Extension == TEXT("json") || Extension == TEXT("yaml") ||
			Extension == TEXT("yml") || Extension == TEXT("xml") || Extension == TEXT("cfg") ||
			Extension == TEXT("lua") || Extension == TEXT("toml") || Extension == TEXT("csv") ||
			Extension == TEXT("log") || Extension == TEXT("html") || Extension == TEXT("css") ||
			Extension == TEXT("svelte") || Extension == TEXT("sh") || Extension == TEXT("bat"))
		{
			FSourceCodeNavigation::OpenSourceFile(FullPath, Line, 0);
			return;
		}

		// Other files — open with OS default application
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*FullPath);
		return;
	}

	// Last resort: try loading as UObject path
	if (Path.StartsWith(TEXT("/")))
	{
		if (UObject* Obj = LoadObject<UObject>(nullptr, *Path))
		{
			NeoLuaEditor::OpenAssetEditor(Obj);
		}
	}
}

// ── Notification Settings ────────────────────────────────────────────

void UWebUIBridge::FireCompletionNotifications(bool bSuccess)
{
	// OnAgentStateChanged can fire mid-serialize (e.g. the agent is prompting,
	// the user saves a widget blueprint, the save triggers a state transition).
	// Both UUserPreferencesSubsystem::Get() and the sound-loading path below
	// go through StaticFindObjectFast, which asserts illegal during a package
	// save. Defer the *whole* function to the next game-thread tick so we
	// never run synchronously inside whatever work posted the state change.
	AsyncTask(ENamedThreads::GameThread, [bSuccess]()
	{
	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (!Prefs) return;

	// Check "only when unfocused" gate
	if (Prefs->bOnlyNotifyWhenUnfocused)
	{
		if (FPlatformApplicationMisc::IsThisApplicationForeground())
		{
			return;
		}
	}

	// Toast notification
	if (Prefs->bNotifyOnTaskComplete)
	{
		FNotificationInfo Info(
			bSuccess
				? FText::FromString(TEXT("Agent finished the task."))
				: FText::FromString(TEXT("Agent encountered an error."))
		);
		Info.bFireAndForget = true;
		Info.FadeOutDuration = 1.0f;
		Info.ExpireDuration = 4.0f;
		Info.bUseSuccessFailIcons = true;
		Info.Image = bSuccess
			? FCoreStyle::Get().GetBrush(TEXT("NotificationList.SuccessImage"))
			: FCoreStyle::Get().GetBrush(TEXT("NotificationList.FailImage"));
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	// Taskbar flash
	if (Prefs->bFlashTaskbarOnComplete)
	{
		if (FSlateApplication::IsInitialized())
		{
			TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
			if (ActiveWindow.IsValid())
			{
				TSharedPtr<FGenericWindow> NativeWindow = ActiveWindow->GetNativeWindow();
				if (NativeWindow.IsValid())
				{
					NativeWindow->DrawAttention(FWindowDrawAttentionParameters(EWindowDrawAttentionRequestType::UntilActivated));
				}
			}
		}
	}

	// Sound
	if (Prefs->bPlayCompletionSound)
	{
		const float Volume = Prefs->CompletionSoundVolume;
		const FSoftObjectPath SoundPath = bSuccess ? Prefs->CompletionSound : Prefs->ErrorSound;

		{
			if (!GEditor) return;

			USoundBase* Sound = nullptr;

			// Try custom sound first
			if (SoundPath.IsValid())
			{
				Sound = Cast<USoundBase>(SoundPath.TryLoad());
			}

			// Fall back to default editor sounds
			if (!Sound)
			{
				const FString DefaultPath = bSuccess
					? TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue")
					: TEXT("/Engine/EditorSounds/Notifications/CompileFailed_Cue.CompileFailed_Cue");
				Sound = Cast<USoundBase>(StaticLoadObject(USoundBase::StaticClass(), nullptr, *DefaultPath));
			}

			if (Sound)
			{
				UAudioComponent* AudioComp = GEditor->PlayPreviewSound(Sound);
				if (AudioComp)
				{
					AudioComp->VolumeMultiplier = Volume;
				}
			}
		}
	}
	}); // end outer AsyncTask from FireCompletionNotifications entry
}

void UWebUIBridge::FirePermissionRequestNotification()
{
	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (!Prefs || !Prefs->bPlayPermissionRequestSound)
	{
		return;
	}

	if (Prefs->bOnlyNotifyWhenUnfocused && FPlatformApplicationMisc::IsThisApplicationForeground())
	{
		return;
	}

	if (!Prefs->PermissionRequestSound.IsValid())
	{
		return;
	}

	if (!GEditor)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	static constexpr double PermissionSoundCooldownSeconds = 2.0;
	if (Now - LastPermissionRequestSoundTime < PermissionSoundCooldownSeconds)
	{
		return;
	}

	USoundBase* Sound = Cast<USoundBase>(Prefs->PermissionRequestSound.TryLoad());
	if (!Sound)
	{
		return;
	}

	UAudioComponent* AudioComp = GEditor->PlayPreviewSound(Sound);
	if (AudioComp)
	{
		AudioComp->VolumeMultiplier = Prefs->PermissionRequestSoundVolume;
	}
	LastPermissionRequestSoundTime = Now;
}

FString UWebUIBridge::GetNotificationSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (Prefs)
	{
		Root->SetBoolField(TEXT("onlyWhenUnfocused"), Prefs->bOnlyNotifyWhenUnfocused);
		Root->SetBoolField(TEXT("notifyOnComplete"), Prefs->bNotifyOnTaskComplete);
		Root->SetBoolField(TEXT("flashTaskbar"), Prefs->bFlashTaskbarOnComplete);
		Root->SetBoolField(TEXT("playSound"), Prefs->bPlayCompletionSound);
		Root->SetNumberField(TEXT("soundVolume"), Prefs->CompletionSoundVolume);
		Root->SetStringField(TEXT("completionSound"), Prefs->CompletionSound.ToString());
		Root->SetStringField(TEXT("errorSound"), Prefs->ErrorSound.ToString());
		Root->SetBoolField(TEXT("playPermissionSound"), Prefs->bPlayPermissionRequestSound);
		Root->SetNumberField(TEXT("permissionSoundVolume"), Prefs->PermissionRequestSoundVolume);
		Root->SetStringField(TEXT("permissionRequestSound"), Prefs->PermissionRequestSound.ToString());
	}
	return JsonToString(Root);
}

void UWebUIBridge::SetNotificationSetting(const FString& Key, const FString& Value)
{
	UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (!Prefs) return;

	if (Key == TEXT("onlyWhenUnfocused"))
	{
		Prefs->bOnlyNotifyWhenUnfocused = Value.ToBool();
	}
	else if (Key == TEXT("notifyOnComplete"))
	{
		Prefs->bNotifyOnTaskComplete = Value.ToBool();
	}
	else if (Key == TEXT("flashTaskbar"))
	{
		Prefs->bFlashTaskbarOnComplete = Value.ToBool();
	}
	else if (Key == TEXT("playSound"))
	{
		Prefs->bPlayCompletionSound = Value.ToBool();
	}
	else if (Key == TEXT("soundVolume"))
	{
		Prefs->CompletionSoundVolume = FMath::Clamp(FCString::Atof(*Value), 0.0f, 1.0f);
	}
	else if (Key == TEXT("playPermissionSound"))
	{
		Prefs->bPlayPermissionRequestSound = Value.ToBool();
	}
	else if (Key == TEXT("permissionSoundVolume"))
	{
		Prefs->PermissionRequestSoundVolume = FMath::Clamp(FCString::Atof(*Value), 0.0f, 1.0f);
	}
	else if (Key == TEXT("completionSound"))
	{
		Prefs->CompletionSound = FSoftObjectPath(Value);
	}
	else if (Key == TEXT("errorSound"))
	{
		Prefs->ErrorSound = FSoftObjectPath(Value);
	}
	else if (Key == TEXT("permissionRequestSound"))
	{
		Prefs->PermissionRequestSound = FSoftObjectPath(Value);
	}
	else
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: Unknown notification setting key: %s"), *Key);
		return;
	}

	Prefs->SavePreferences();
}

FString UWebUIBridge::ListSoundAssets(const FString& Query)
{
	TArray<TSharedPtr<FJsonValue>> Results;

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// Block on cold-start scan; otherwise the picker would silently return
	// "no SoundBase assets in /Game" while the registry is still indexing.
	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.WaitForCompletion();
	}

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.ClassPaths.Add(USoundBase::StaticClass()->GetClassPathName());

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	const FString QueryLower = Query.ToLower();
	for (const FAssetData& Asset : Assets)
	{
		const FString AssetName = Asset.AssetName.ToString();
		if (!QueryLower.IsEmpty() && !AssetName.ToLower().Contains(QueryLower))
		{
			continue;
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Asset.GetSoftObjectPath().ToString());
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("folder"), Asset.PackagePath.ToString());
		Entry->SetStringField(TEXT("className"), Asset.AssetClassPath.GetAssetName().ToString());
		Results.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Results.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetStringField(TEXT("name")) < B->AsObject()->GetStringField(TEXT("name"));
	});

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("sounds"), Results);
	return JsonToString(Root);
}

void UWebUIBridge::PreviewNotificationSound(const FString& SoundPath, float Volume)
{
	if (!GEditor || SoundPath.IsEmpty())
	{
		return;
	}

	const FSoftObjectPath Path(SoundPath);
	if (!Path.IsValid())
	{
		return;
	}

	USoundBase* Sound = Cast<USoundBase>(Path.TryLoad());
	if (!Sound)
	{
		return;
	}

	UAudioComponent* AudioComp = GEditor->PlayPreviewSound(Sound);
	if (AudioComp)
	{
		AudioComp->VolumeMultiplier = FMath::Clamp(Volume, 0.0f, 1.0f);
	}
}

bool UWebUIBridge::SoundAssetExists(const FString& SoundPath)
{
	if (SoundPath.IsEmpty())
	{
		return false;
	}

	const FSoftObjectPath Path(SoundPath);
	if (!Path.IsValid())
	{
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.WaitForCompletion();
	}

	return AssetRegistry.GetAssetByObjectPath(Path).IsValid();
}

FString UWebUIBridge::GetAgentExecutionSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (Prefs)
	{
		Root->SetStringField(TEXT("systemPromptAppend"), Prefs->ACPSystemPromptAppend);
		Root->SetNumberField(TEXT("toolTimeout"), Prefs->ToolExecutionTimeoutSeconds);
		Root->SetNumberField(TEXT("agentResponseTimeout"), Prefs->AgentResponseTimeoutSeconds);
	}
	return JsonToString(Root);
}

void UWebUIBridge::SetAgentExecutionSetting(const FString& Key, const FString& Value)
{
	UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (!Prefs) return;

	if (Key == TEXT("systemPromptAppend"))
	{
		Prefs->ACPSystemPromptAppend = Value;
	}
	else if (Key == TEXT("toolTimeout"))
	{
		Prefs->ToolExecutionTimeoutSeconds = FMath::Clamp(FCString::Atoi(*Value), 0, 600);
	}
	else if (Key == TEXT("agentResponseTimeout"))
	{
		Prefs->AgentResponseTimeoutSeconds = FMath::Clamp(FCString::Atoi(*Value), 0, 86400);
	}
	else
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: Unknown agent execution setting key: %s"), *Key);
		return;
	}

	Prefs->SavePreferences();
}

FString UWebUIBridge::GetIssueReportSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		Root->SetBoolField(TEXT("disabled"), Settings->bDisableAgentIssueReports);
	}
	else
	{
		Root->SetBoolField(TEXT("disabled"), false);
	}
	return JsonToString(Root);
}

FString UWebUIBridge::GetEntitlementStatus()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	const FEntitlementClient& Client = FEntitlementClient::Get();
	const EEntitlementResult R = Client.GetResult();

	const TCHAR* StatusStr = TEXT("unknown");
	switch (R)
	{
	case EEntitlementResult::Lifetime:           StatusStr = TEXT("lifetime"); break;
	case EEntitlementResult::ActiveSubscription: StatusStr = TEXT("subscription"); break;
	case EEntitlementResult::NotEntitled:        StatusStr = TEXT("none"); break;
	case EEntitlementResult::NetworkError:       StatusStr = TEXT("network"); break;
	case EEntitlementResult::Unknown:            StatusStr = TEXT("unknown"); break;
	}

	// Compile-time variant; baked into the DLL by the release workflow.
#if NEOSTACK_BUILD_VARIANT_BINARY
	const bool bBinaryBuild = true;
#else
	const bool bBinaryBuild = false;
#endif

	Root->SetBoolField(TEXT("entitled"), Client.IsEntitled());
	Root->SetStringField(TEXT("status"), StatusStr);
	Root->SetBoolField(TEXT("isBinaryBuild"), bBinaryBuild);
	return JsonToString(Root);
}

namespace
{
	bool ResolveBinaryBuildFlag()
	{
#if NEOSTACK_BUILD_VARIANT_BINARY
		return true;
#else
		return false;
#endif
	}

	FString ResolveNeoStackApiKey()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			return Settings->GetChatProviderApiKey(TEXT("neostack"));
		}
		return FString();
	}

	FString EntitlementResultToClientStatus(EEntitlementResult R)
	{
		switch (R)
		{
		case EEntitlementResult::Lifetime:            return TEXT("lifetime");
		case EEntitlementResult::ActiveSubscription: return TEXT("subscription");
		case EEntitlementResult::NotEntitled:        return TEXT("none");
		case EEntitlementResult::NetworkError:       return TEXT("network");
		case EEntitlementResult::Unknown:
		default:                                     return TEXT("unknown");
		}
	}

	FString ResolveConnectionState(bool bHasApiKey, bool bCheckPending, EEntitlementResult R, bool bHasServerPayload)
	{
		if (!bHasApiKey)
		{
			return TEXT("disconnected");
		}
		if (bCheckPending)
		{
			return TEXT("loading");
		}
		if (R == EEntitlementResult::NetworkError)
		{
			return bHasServerPayload ? TEXT("offline") : TEXT("offline");
		}
		if (R == EEntitlementResult::NotEntitled && !bHasServerPayload)
		{
			return TEXT("key_rejected");
		}
		if (bHasServerPayload)
		{
			return TEXT("connected");
		}
		return TEXT("loading");
	}
}

FString UWebUIBridge::GetNeoStackAccountStatus()
{
	const FEntitlementClient& Client = FEntitlementClient::Get();
	const bool bBinaryBuild = ResolveBinaryBuildFlag();
	const FString ApiKey = ResolveNeoStackApiKey();
	const bool bHasApiKey = !ApiKey.IsEmpty();
	const bool bCheckPending = Client.IsCheckPending();
	const EEntitlementResult R = Client.GetResult();
	const FString& Cached = Client.GetCachedAccountJson();
	const bool bHasServerPayload = !Cached.IsEmpty();

	TSharedPtr<FJsonObject> Root;
	if (bHasServerPayload)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Cached);
		FJsonSerializer::Deserialize(Reader, Root);
	}
	if (!Root.IsValid())
	{
		Root = MakeShared<FJsonObject>();
	}

	Root->SetBoolField(TEXT("hasApiKey"), bHasApiKey);
	Root->SetBoolField(TEXT("entitled"), Client.IsEntitled());
	Root->SetBoolField(TEXT("isBinaryBuild"), bBinaryBuild);
	Root->SetBoolField(TEXT("checkPending"), bCheckPending);
	Root->SetStringField(TEXT("clientStatus"), EntitlementResultToClientStatus(R));
	Root->SetStringField(
		TEXT("connectionState"),
		ResolveConnectionState(bHasApiKey, bCheckPending, R, bHasServerPayload));
	Root->SetBoolField(TEXT("connected"), ResolveConnectionState(bHasApiKey, bCheckPending, R, bHasServerPayload) == TEXT("connected"));

	return JsonToString(Root.ToSharedRef());
}

void UWebUIBridge::BindOnNeoStackAccountChanged(FWebJSFunction Callback)
{
	OnNeoStackAccountChangedCallback = Callback;

	if (EntitlementAccountChangedHandle.IsValid())
	{
		FEntitlementClient::Get().OnAccountChanged().Remove(EntitlementAccountChangedHandle);
	}
	TWeakObjectPtr<UWebUIBridge> WeakThis(this);
	EntitlementAccountChangedHandle = FEntitlementClient::Get().OnAccountChanged().AddLambda(
		[WeakThis]()
		{
			UWebUIBridge* Self = WeakThis.Get();
			if (!Self || !Self->OnNeoStackAccountChangedCallback.IsValid()) return;
			Self->OnNeoStackAccountChangedCallback(Self->GetNeoStackAccountStatus());
		});
}

void UWebUIBridge::ClearNeoStackCloudKey()
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		Settings->SetChatProviderApiKey(TEXT("neostack"), FString());
		FChatModelRegistry::Get().RefreshProvider(TEXT("neostack"));
	}
	FEntitlementClient::Get().Refresh();
}

void UWebUIBridge::SetIssueReportDisabled(bool bDisabled)
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;
	if (Settings->bDisableAgentIssueReports == bDisabled) return;

	Settings->bDisableAgentIssueReports = bDisabled;
	Settings->SaveConfig();
}

FString UWebUIBridge::GetGenerationSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();

	const UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
	if (Prefs)
	{
		Root->SetStringField(TEXT("imageModel"), Prefs->ImageGenerationDefaultModel);
		Root->SetStringField(TEXT("meshyArtStyle"), Prefs->MeshyDefaultArtStyle);
	}

	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		Root->SetStringField(TEXT("meshyApiKey"), Settings->MeshyApiKey);
		Root->SetStringField(TEXT("tripoApiKey"), Settings->TripoApiKey);
		Root->SetStringField(TEXT("elevenLabsApiKey"), Settings->ElevenLabsApiKey);
		Root->SetStringField(TEXT("falApiKey"), Settings->FalApiKey);
		Root->SetStringField(TEXT("openAIApiKey"), Settings->OpenAIApiKey);
	}

	return JsonToString(Root);
}

void UWebUIBridge::SetGenerationSetting(const FString& Key, const FString& Value)
{
	// Route defaults → user prefs subsystem; route secrets → UACPSettings (INI).
	if (Key == TEXT("imageModel") || Key == TEXT("meshyArtStyle"))
	{
		UUserPreferencesSubsystem* Prefs = UUserPreferencesSubsystem::Get();
		if (!Prefs) return;
		if (Key == TEXT("imageModel"))
		{
			Prefs->ImageGenerationDefaultModel = Value;
		}
		else
		{
			Prefs->MeshyDefaultArtStyle = Value;
		}
		Prefs->SavePreferences();
		return;
	}

	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	if (Key == TEXT("meshyApiKey"))
	{
		Settings->MeshyApiKey = Value;
	}
	else if (Key == TEXT("tripoApiKey"))
	{
		Settings->TripoApiKey = Value;
	}
	else if (Key == TEXT("elevenLabsApiKey"))
	{
		Settings->ElevenLabsApiKey = Value;
	}
	else if (Key == TEXT("falApiKey"))
	{
		Settings->FalApiKey = Value;
	}
	else if (Key == TEXT("openAIApiKey"))
	{
		Settings->OpenAIApiKey = Value;
	}
	else
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("WebUIBridge: Unknown generation setting key: %s"), *Key);
		return;
	}

	Settings->SavePreferences();
}

void UWebUIBridge::OpenPluginSettings()
{
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("NeoStack AI"));
	}
}

FString UWebUIBridge::GetExtensionSettings()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("coreApiVersion"), FNeoStackExtensionRegistry::GetCoreApiVersion());
	Root->SetStringField(TEXT("projectFile"), FPaths::GetProjectFilePath());

	// Build a quick PluginName → catalog entry lookup so each managed
	// extension row can include its server-side `latestVersion` without an
	// extra round-trip from the panel. Empty when the catalog is cold —
	// the panel just hides the "out of date" badge in that case.
	const FNeoStackCatalogResult Catalog = FNeoStackExtensionCatalog::GetCachedResult();
	TMap<FString, const FNeoStackCatalogEntry*> ByPluginName;
	if (Catalog.Status == ENeoStackCatalogStatus::Ready)
	{
		ByPluginName.Reserve(Catalog.Entries.Num());
		for (const FNeoStackCatalogEntry& E : Catalog.Entries)
		{
			if (!E.PluginName.IsEmpty())
			{
				ByPluginName.Add(E.PluginName, &E);
			}
		}
	}

	const TArray<FNeoStackManagedExtension> Extensions = FNeoStackExtensionProjectService::GetManagedExtensions();
	TArray<TSharedPtr<FJsonValue>> ExtensionsArray;
	bool bRestartRequired = false;
	int32 OutdatedCount = 0;

	for (const FNeoStackManagedExtension& Extension : Extensions)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("pluginName"), Extension.PluginName);
		Obj->SetStringField(TEXT("extensionId"), Extension.ExtensionId);
		Obj->SetStringField(TEXT("displayName"), Extension.DisplayName);
		Obj->SetStringField(TEXT("description"), Extension.Description);
		Obj->SetStringField(TEXT("version"), Extension.Version);

		// Surface server-derived fields when we have catalog data.
		const FNeoStackCatalogEntry* const* CatalogFound = ByPluginName.Find(Extension.PluginName);
		if (CatalogFound && *CatalogFound && Extension.bEnabledInProject)
		{
			const FNeoStackCatalogEntry& Entry = **CatalogFound;
			Obj->SetStringField(TEXT("slug"), Entry.Slug);
			Obj->SetStringField(TEXT("latestVersion"), Entry.LatestVersion);
			Obj->SetStringField(TEXT("latestChannel"), Entry.LatestChannel);
			const bool bUpdateAvailable = !Entry.LatestVersion.IsEmpty()
				&& Entry.LatestVersion != Extension.Version;
			Obj->SetBoolField(TEXT("updateAvailable"), bUpdateAvailable);
			if (bUpdateAvailable) ++OutdatedCount;
		}
		else
		{
			Obj->SetBoolField(TEXT("updateAvailable"), false);
		}
		Obj->SetStringField(TEXT("vendor"), Extension.Vendor);
		Obj->SetStringField(TEXT("category"), Extension.Category);
		Obj->SetStringField(TEXT("statusMessage"), Extension.StatusMessage);
		Obj->SetStringField(TEXT("baseDir"), Extension.BaseDir);
		Obj->SetStringField(TEXT("runtimeState"), ExtensionRuntimeStateToString(Extension));
		Obj->SetBoolField(TEXT("enabledInProject"), Extension.bEnabledInProject);
		Obj->SetBoolField(TEXT("hasExplicitProjectEntry"), Extension.bHasExplicitProjectEntry);
		Obj->SetBoolField(TEXT("loadedInSession"), Extension.bLoadedInSession);
		Obj->SetBoolField(TEXT("mountedInSession"), Extension.bMountedInSession);
		Obj->SetBoolField(TEXT("activeInSession"), Extension.bActiveInSession);
		Obj->SetBoolField(TEXT("restartRequired"), Extension.bRestartRequired);
		Obj->SetBoolField(TEXT("canToggle"), Extension.bCanToggle);
		Obj->SetBoolField(TEXT("isProjectPlugin"), Extension.bIsProjectPlugin);
		Obj->SetBoolField(TEXT("isInstalledOnEngine"), Extension.bIsInstalledOnEngine);
		Obj->SetBoolField(TEXT("explicitlyLoaded"), Extension.bExplicitlyLoaded);
		Obj->SetBoolField(TEXT("enabledByDefault"), Extension.bEnabledByDefault);
		Obj->SetBoolField(TEXT("isBetaVersion"), Extension.bIsBetaVersion);
		Obj->SetBoolField(TEXT("isExperimentalVersion"), Extension.bIsExperimentalVersion);
		Obj->SetBoolField(TEXT("isBuiltIn"), Extension.bIsBuiltIn);
		Obj->SetBoolField(TEXT("isThirdParty"), Extension.bIsThirdParty);
		Obj->SetBoolField(TEXT("hasRuntimeDescriptor"), Extension.bHasRuntimeDescriptor);
		Obj->SetBoolField(TEXT("hasUIMetadata"), Extension.UIMetadata.bHasMetadata);
		Obj->SetStringField(TEXT("domain"), Extension.UIMetadata.Domain);
		Obj->SetStringField(TEXT("domainLabel"), Extension.UIMetadata.DomainLabel);
		Obj->SetNumberField(TEXT("sortOrder"), Extension.UIMetadata.SortOrder);
		Obj->SetStringField(TEXT("agentSummary"), Extension.UIMetadata.AgentSummary);
		Obj->SetStringField(TEXT("whenToEnable"), Extension.UIMetadata.WhenToEnable);
		Obj->SetBoolField(TEXT("isRecommended"), Extension.UIMetadata.bIsRecommended);

		TArray<TSharedPtr<FJsonValue>> EnablesAgentToArray;
		for (const FString& Capability : Extension.UIMetadata.EnablesAgentTo)
		{
			EnablesAgentToArray.Add(MakeShared<FJsonValueString>(Capability));
		}
		Obj->SetArrayField(TEXT("enablesAgentTo"), EnablesAgentToArray);

		TArray<TSharedPtr<FJsonValue>> DepsArray;
		for (const FNeoStackExtensionDependency& Dep : Extension.Dependencies)
		{
			TSharedRef<FJsonObject> DepObj = MakeShared<FJsonObject>();
			DepObj->SetStringField(TEXT("name"), Dep.Name);
			DepObj->SetBoolField(TEXT("optional"), Dep.bOptional);
			DepObj->SetBoolField(TEXT("enabled"), Dep.bEnabled);
			DepObj->SetBoolField(TEXT("installed"), Dep.bInstalled);
			DepsArray.Add(MakeShared<FJsonValueObject>(DepObj));
		}
		Obj->SetArrayField(TEXT("dependencies"), DepsArray);

		bRestartRequired |= Extension.bRestartRequired;
		ExtensionsArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	Root->SetBoolField(TEXT("restartRequired"), bRestartRequired);
	Root->SetNumberField(TEXT("outdatedCount"), OutdatedCount);
	Root->SetArrayField(TEXT("extensions"), ExtensionsArray);
	return JsonToString(Root);
}

FString UWebUIBridge::SetExtensionEnabled(const FString& PluginName, bool bEnabled)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pluginName"), PluginName);
	Result->SetBoolField(TEXT("enabledInProject"), bEnabled);
	Result->SetBoolField(TEXT("restartRequired"), true);

	FString Error;
	const bool bSuccess = FNeoStackExtensionProjectService::SetProjectExtensionEnabled(PluginName, bEnabled, Error);
	Result->SetBoolField(TEXT("success"), bSuccess);
	if (!bSuccess)
	{
		Result->SetStringField(TEXT("error"), Error);
	}

	return JsonToString(Result);
}

FString UWebUIBridge::SetExtensionPluginsEnabled(const FString& PluginNamesJson, bool bEnabled)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("restartRequired"), true);

	TArray<TSharedPtr<FJsonValue>> NamesValues;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PluginNamesJson);
	if (!FJsonSerializer::Deserialize(Reader, NamesValues))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Expected a JSON array of plugin names."));
		return JsonToString(Result);
	}

	TArray<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : NamesValues)
	{
		FString Name;
		if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
		{
			Names.Add(MoveTemp(Name));
		}
	}

	FString Error;
	const bool bSuccess = FNeoStackExtensionProjectService::SetProjectExtensionsEnabled(Names, bEnabled, Error);
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetNumberField(TEXT("count"), Names.Num());
	if (!bSuccess)
	{
		Result->SetStringField(TEXT("error"), Error);
	}

	return JsonToString(Result);
}

void UWebUIBridge::RefreshExtensionCatalog(const FString& Channel, bool bIncludeAllEngines)
{
	// Normalize channel; unknown values fall back to stable for safety.
	FString ResolvedChannel = Channel.IsEmpty() ? TEXT("stable") : Channel;
	ResolvedChannel = ResolvedChannel.ToLower();
	if (ResolvedChannel != TEXT("stable")
		&& ResolvedChannel != TEXT("beta")
		&& ResolvedChannel != TEXT("dev")
		&& ResolvedChannel != TEXT("alpha"))
	{
		ResolvedChannel = TEXT("stable");
	}

	// Engine version is auto-detected from the editor build unless the
	// caller opts into the all-engines admin view.
	FString EngineVersion;
	if (!bIncludeAllEngines)
	{
		EngineVersion = FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	FNeoStackExtensionCatalog::RefreshAsync(ResolvedChannel, EngineVersion);
}

FString UWebUIBridge::GetExtensionCatalog()
{
	return FNeoStackExtensionCatalog::ResultToJson(FNeoStackExtensionCatalog::GetCachedResult());
}

// ── Extension installer (batch install / update / uninstall) ────
//
// The orchestrator is an in-memory queue driven one op at a time. These
// bridge methods are thin glue; the real work lives in
// Private/Extensions/NeoStackExtensionInstaller*.cpp.

static ENeoStackOpKind ParseKind(const FString& Kind)
{
	const FString Lower = Kind.ToLower();
	if (Lower == TEXT("update")) return ENeoStackOpKind::Update;
	if (Lower == TEXT("uninstall")) return ENeoStackOpKind::Uninstall;
	return ENeoStackOpKind::Install;
}

void UWebUIBridge::QueueExtensionOp(const FString& Slug, const FString& PluginName, const FString& Kind, const FString& Channel)
{
	if (Slug.IsEmpty() || PluginName.IsEmpty()) return;
	FNeoStackExtensionInstaller::Queue(Slug, PluginName, ParseKind(Kind), Channel);
}

void UWebUIBridge::DequeueExtensionOp(const FString& Slug, const FString& Kind)
{
	if (Slug.IsEmpty()) return;
	FNeoStackExtensionInstaller::Dequeue(Slug, ParseKind(Kind));
}

void UWebUIBridge::ClearExtensionOpQueue()
{
	FNeoStackExtensionInstaller::ClearPending();
}

FString UWebUIBridge::GetExtensionOpQueue()
{
	return FNeoStackExtensionInstaller::QueueToJson();
}

void UWebUIBridge::ApplyExtensionOps()
{
	FNeoStackExtensionInstaller::Apply();
}

FString UWebUIBridge::QueueAllOutdatedExtensions(const FString& Channel)
{
	const FString ResolvedChannel = Channel.IsEmpty() ? TEXT("stable") : Channel;
	const FString EngineVersion = FString::Printf(TEXT("%d.%d"),
		ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);

	const TArray<FNeoStackExtensionUpdateCandidate> Candidates =
		FNeoStackExtensionInstaller::ComputePendingUpdates(ResolvedChannel, EngineVersion);

	TArray<TSharedPtr<FJsonValue>> OpsArr;
	for (const FNeoStackExtensionUpdateCandidate& C : Candidates)
	{
		FNeoStackExtensionInstaller::Queue(C.Slug, C.PluginName, ENeoStackOpKind::Update, ResolvedChannel);

		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("slug"), C.Slug);
		O->SetStringField(TEXT("pluginName"), C.PluginName);
		O->SetStringField(TEXT("displayName"), C.DisplayName);
		O->SetStringField(TEXT("installedVersion"), C.InstalledVersion);
		O->SetStringField(TEXT("latestVersion"), C.LatestVersion);
		OpsArr.Add(MakeShared<FJsonValueObject>(O));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("queued"), Candidates.Num());
	Root->SetArrayField(TEXT("ops"), OpsArr);
	return JsonToString(Root);
}

FString UWebUIBridge::GetExtensionOpState()
{
	return FNeoStackExtensionInstaller::StateToJson();
}

void UWebUIBridge::RestartEditor()
{
	if (FNeoStackExtensionInstaller::HasPendingRestartOps())
	{
		if (FNeoStackExtensionInstaller::LaunchPendingRestartUpdater() && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
		return;
	}

	FUnrealEdMisc::Get().RestartEditor(false);
}

void UWebUIBridge::CheckForPluginUpdate()
{
	FNeoStackAIModule::CheckForPluginUpdate();
}

// ── Agent Skills ─────────────────────────────────────────────────────

FString UWebUIBridge::GetSkills()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Root->SetStringField(TEXT("manifestPath"), FNeoStackSkillInstaller::GetManifestPath());

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FNeoStackSkillStatus& S : FNeoStackSkillInstaller::GetStatus())
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),              S.Name);
		Obj->SetStringField(TEXT("description"),       S.Description);
		Obj->SetStringField(TEXT("sourceId"),          S.SourceId);
		Obj->SetStringField(TEXT("sourceDisplayName"), S.SourceDisplayName);
		Obj->SetStringField(TEXT("sourceVersion"),     S.SourceVersion);
		Obj->SetStringField(TEXT("conflictNewPath"),   S.ConflictNewPath);
		Obj->SetBoolField  (TEXT("userEdited"),        S.bUserEdited);
		Obj->SetBoolField  (TEXT("conflictPending"),   S.bConflictPending);

		TArray<TSharedPtr<FJsonValue>> TagsArr;
		for (const FString& T : S.Tags) { TagsArr.Add(MakeShared<FJsonValueString>(T)); }
		Obj->SetArrayField(TEXT("tags"), TagsArr);

		TArray<TSharedPtr<FJsonValue>> PathsArr;
		for (const FString& P : S.InstalledPaths) { PathsArr.Add(MakeShared<FJsonValueString>(P)); }
		Obj->SetArrayField(TEXT("installedPaths"), PathsArr);

		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("skills"), Arr);

	TArray<TSharedPtr<FJsonValue>> ProjectArr;
	for (const FNeoStackProjectSkillStatus& P : FNeoStackSkillInstaller::GetProjectSkills())
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"),        P.Name);
		Obj->SetStringField(TEXT("folderName"),  P.FolderName);
		Obj->SetStringField(TEXT("description"), P.Description);
		Obj->SetStringField(TEXT("parseError"),  P.ParseError);

		TArray<TSharedPtr<FJsonValue>> TagsArr;
		for (const FString& T : P.Tags) { TagsArr.Add(MakeShared<FJsonValueString>(T)); }
		Obj->SetArrayField(TEXT("tags"), TagsArr);

		TArray<TSharedPtr<FJsonValue>> PathsArr;
		for (const FString& Path : P.Paths) { PathsArr.Add(MakeShared<FJsonValueString>(Path)); }
		Obj->SetArrayField(TEXT("paths"), PathsArr);

		ProjectArr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	Root->SetArrayField(TEXT("projectSkills"), ProjectArr);

	return JsonToString(Root);
}

FString UWebUIBridge::GetSkillBody(const FString& SkillName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), SkillName);

	const FString Body = FNeoStackSkillInstaller::ReadSkillBody(SkillName);
	if (Body.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("skill not found"));
		return JsonToString(Result);
	}
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("body"), Body);
	return JsonToString(Result);
}

FString UWebUIBridge::GetSkillConflictBody(const FString& SkillName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), SkillName);

	const FString Body = FNeoStackSkillInstaller::ReadConflictBody(SkillName);
	if (Body.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("no conflict pending"));
		return JsonToString(Result);
	}
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("body"), Body);
	return JsonToString(Result);
}

FString UWebUIBridge::ResolveSkillConflict(const FString& SkillName, const FString& Mode)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bOk = FNeoStackSkillInstaller::ResolveConflict(SkillName, Mode);
	Result->SetBoolField(TEXT("success"), bOk);
	if (!bOk)
	{
		Result->SetStringField(TEXT("error"), TEXT("no conflict pending for this skill"));
	}
	return JsonToString(Result);
}

FString UWebUIBridge::RescanSkills()
{
	FNeoStackSkillRegistry::Get().Refresh();
	const FNeoStackSkillSyncReport R = FNeoStackSkillInstaller::SyncProject();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("installed"),      R.Installed);
	Result->SetNumberField(TEXT("updated"),        R.Updated);
	Result->SetNumberField(TEXT("noOp"),           R.NoOp);
	Result->SetNumberField(TEXT("userEditsKept"),  R.UserEditsKept);
	Result->SetNumberField(TEXT("conflicts"),      R.Conflicts);
	Result->SetNumberField(TEXT("orphansRemoved"), R.OrphansRemoved);
	Result->SetNumberField(TEXT("orphansKept"),    R.OrphansKept);

	TArray<TSharedPtr<FJsonValue>> Errs;
	for (const FString& E : R.Errors) { Errs.Add(MakeShared<FJsonValueString>(E)); }
	Result->SetArrayField(TEXT("errors"), Errs);
	return JsonToString(Result);
}

void UWebUIBridge::OpenSkillFile(const FString& SkillName, bool bUpstream)
{
	// Find the first target that has the requested file and hand it to the OS.
	TArray<FString> Candidates;
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	for (const TCHAR* RootRel : { TEXT(".claude/skills"), TEXT(".agents/skills") })
	{
		const FString Base = FPaths::Combine(Project, RootRel, SkillName);
		Candidates.Add(FPaths::Combine(Base, bUpstream ? TEXT("SKILL.new.md") : TEXT("SKILL.md")));
	}
	for (const FString& Path : Candidates)
	{
		if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*Path))
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
			return;
		}
	}
	UE_LOG(LogNeoStackAI, Warning, TEXT("OpenSkillFile: no matching file on disk for '%s' (upstream=%d)"),
		*SkillName, bUpstream ? 1 : 0);
}

// ── Agent Authentication ────────────────────────────────────────────

FString UWebUIBridge::GetAuthMethods(const FString& AgentName)
{
	TArray<FACPAuthMethod> Methods = FACPAgentManager::Get().GetAuthMethods(AgentName);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FACPAuthMethod& M : Methods)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), M.Id);
		Obj->SetStringField(TEXT("name"), M.Name);
		Obj->SetStringField(TEXT("description"), M.Description);
		Obj->SetBoolField(TEXT("isTerminalAuth"), M.bIsTerminalAuth);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Arr, Writer);
	return Out;
}

void UWebUIBridge::StartAgentLogin(const FString& AgentName, const FString& MethodId)
{
	UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Starting login for %s (method: %s)"), *AgentName, *MethodId);
	FACPAgentManager::Get().AuthenticateAgent(AgentName, MethodId);
}

void UWebUIBridge::BindOnLoginComplete(FWebJSFunction Callback)
{
	OnLoginCompleteCallback = Callback;
	BindDelegates();
}

// ── Agent Usage / Rate Limits ────────────────────────────────────────

// Helper: serialize rate limit window to JSON
static TSharedPtr<FJsonObject> RateLimitWindowToJson(const FAgentRateLimitWindow& Window)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("usedPercent"), Window.UsedPercent);
	Obj->SetStringField(TEXT("resetsAt"), Window.ResetsAt.ToIso8601());
	Obj->SetNumberField(TEXT("windowDurationMinutes"), Window.WindowDurationMinutes);
	Obj->SetBoolField(TEXT("hasData"), Window.HasData());
	return Obj;
}

// Helper: serialize full rate limit data to JSON string (includes Meshy balance)
static FString RateLimitDataToJsonString(const FAgentRateLimitData& Data)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("hasData"), Data.bHasData);
	Obj->SetBoolField(TEXT("isLoading"), Data.bIsLoading);
	Obj->SetStringField(TEXT("errorMessage"), Data.ErrorMessage);
	Obj->SetStringField(TEXT("agentName"), Data.AgentName);
	Obj->SetStringField(TEXT("planType"), Data.PlanType);
	Obj->SetStringField(TEXT("lastUpdated"), Data.LastUpdated.ToIso8601());

	// Rate limit windows
	Obj->SetObjectField(TEXT("primary"), RateLimitWindowToJson(Data.Primary));
	Obj->SetObjectField(TEXT("secondary"), RateLimitWindowToJson(Data.Secondary));
	Obj->SetObjectField(TEXT("modelSpecific"), RateLimitWindowToJson(Data.ModelSpecific));
	Obj->SetStringField(TEXT("modelSpecificLabel"), Data.ModelSpecificLabel);

	// Extra usage (Claude Extra)
	TSharedRef<FJsonObject> ExtraObj = MakeShared<FJsonObject>();
	ExtraObj->SetBoolField(TEXT("isEnabled"), Data.ExtraUsage.bIsEnabled);
	ExtraObj->SetNumberField(TEXT("usedAmount"), Data.ExtraUsage.UsedAmount);
	ExtraObj->SetNumberField(TEXT("limitAmount"), Data.ExtraUsage.LimitAmount);
	ExtraObj->SetStringField(TEXT("currencyCode"), Data.ExtraUsage.CurrencyCode);
	ExtraObj->SetBoolField(TEXT("hasData"), Data.ExtraUsage.HasData());
	Obj->SetObjectField(TEXT("extraUsage"), ExtraObj);

	// Meshy credits (global, not per-agent)
	TSharedRef<FJsonObject> MeshyObj = MakeShared<FJsonObject>();
	UACPSettings* Settings = UACPSettings::Get();
	bool bMeshyConfigured = Settings && Settings->HasMeshyAuth();
	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();
	MeshyObj->SetBoolField(TEXT("configured"), bMeshyConfigured);
	MeshyObj->SetNumberField(TEXT("balance"), Monitor.GetCachedMeshyBalance());
	MeshyObj->SetBoolField(TEXT("isLoading"), Monitor.IsMeshyBalanceLoading());
	MeshyObj->SetStringField(TEXT("error"), Monitor.GetMeshyBalanceError());
	Obj->SetObjectField(TEXT("meshy"), MeshyObj);

	return JsonToString(Obj);
}

FString UWebUIBridge::GetAgentUsage(const FString& AgentName)
{
	if (AgentName.IsEmpty())
	{
		return TEXT("{\"hasData\":false}");
	}

	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();

	// If this agent is supported, request an update (non-blocking, will fire callback when done)
	if (FAgentUsageMonitor::IsAgentSupported(AgentName))
	{
		Monitor.RequestUsageUpdate(AgentName);
	}

	// Also trigger Meshy balance fetch if configured
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->HasMeshyAuth())
	{
		Monitor.RequestMeshyBalanceUpdate();
	}

	// Return whatever is cached (may be empty if first fetch)
	const FAgentRateLimitData& Data = Monitor.GetCachedUsage(AgentName);
	return RateLimitDataToJsonString(Data);
}

void UWebUIBridge::RefreshAgentUsage(const FString& AgentName)
{
	if (AgentName.IsEmpty()) return;

	FAgentUsageMonitor& Monitor = FAgentUsageMonitor::Get();

	if (FAgentUsageMonitor::IsAgentSupported(AgentName))
	{
		Monitor.RequestUsageUpdate(AgentName);
	}

	// Also refresh Meshy balance
	UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->HasMeshyAuth())
	{
		Monitor.RequestMeshyBalanceUpdate();
	}
}

void UWebUIBridge::BindOnUsageUpdated(FWebJSFunction Callback)
{
	OnUsageUpdatedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnMcpStatus(FWebJSFunction Callback)
{
	OnMcpStatusCallback = Callback;
	BindDelegates();
}
void UWebUIBridge::BindOnSessionListUpdated(FWebJSFunction Callback)
{
	OnSessionListUpdatedCallback = Callback;
	BindDelegates();

	UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: BindOnSessionListUpdated called"));

	// Immediately push any already-cached session lists so the UI doesn't miss
	// data that arrived before this callback was bound.
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FString> AllAgents = AgentMgr.GetAvailableAgentNames();

	for (const FString& AgentName : AllAgents)
	{
		TArray<FACPRemoteSessionEntry> Sessions = AgentMgr.GetCachedSessionList(AgentName);
		Sessions.Append(GetLocalHistorySessionsForAgent(AgentName, FPaths::ProjectDir()));
		if (Sessions.Num() > 0)
		{
			UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Pushing %d cached sessions for '%s'"), Sessions.Num(), *AgentName);
			AgentMgr.OnAgentSessionListReceived.Broadcast(AgentName, Sessions);
		}
	}

	// Proactively connect ACP agents that aren't connected yet so their
	// session lists get fetched. This must happen AFTER the callback is set
	// (above) so push updates reach the JS side when they arrive.
	int32 ConnectingCount = 0;
	for (const FString& AgentName : AllAgents)
	{
		if (AgentMgr.IsChatGatewayAgent(AgentName)) continue;
		if (AgentMgr.IsConnectedToAgent(AgentName))
		{
			// Already connected — just request the session list again
			AgentMgr.RequestSessionList(AgentName);
			continue;
		}

		FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName);
		if (Config && Config->Status == EACPAgentStatus::Available)
		{
			UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Auto-connecting agent '%s' for session listing"), *AgentName);
			AgentMgr.ConnectToAgent(AgentName);
			ConnectingCount++;
		}
	}
	UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: Auto-connecting %d agents for session listing"), ConnectingCount);
}

FString UWebUIBridge::RefreshSessionList()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();
	TArray<FString> AllAgents = AgentMgr.GetAvailableAgentNames();

	int32 ConnectingCount = 0;
	for (const FString& AgentName : AllAgents)
	{
		if (AgentMgr.IsChatGatewayAgent(AgentName)) continue;

		const TArray<FACPRemoteSessionEntry> LocalHistorySessions = GetLocalHistorySessionsForAgent(AgentName, FPaths::ProjectDir());
		if (LocalHistorySessions.Num() > 0)
		{
			AgentMgr.OnAgentSessionListReceived.Broadcast(AgentName, LocalHistorySessions);
		}

		if (AgentMgr.IsConnectedToAgent(AgentName))
		{
			// Already connected — just request the session list
			AgentMgr.RequestSessionList(AgentName);
		}
		else
		{
			FACPAgentConfig* Config = AgentMgr.GetAgentConfig(AgentName);
			if (Config && Config->Status == EACPAgentStatus::Available)
			{
				UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: RefreshSessionList: connecting agent '%s'"), *AgentName);
				AgentMgr.ConnectToAgent(AgentName);
				ConnectingCount++;
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("connectingCount"), ConnectingCount);
	return JsonToString(Result);
}

void UWebUIBridge::NotifyMcpStatus(const FString& SessionId, const FString& Status)
{
	// Copy SessionId before clearing McpWaitingSessionId — callers pass
	// McpWaitingSessionId by const ref, so clearing it would alias-invalidate SessionId.
	const FString CapturedSessionId = SessionId;

	// Clean up MCP listeners
	if (McpToolsDiscoveredHandle.IsValid())
	{
		FMCPServer::Get().OnClientToolsDiscovered.Remove(McpToolsDiscoveredHandle);
		McpToolsDiscoveredHandle.Reset();
	}
	if (McpTimeoutTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(McpTimeoutTickerHandle);
		McpTimeoutTickerHandle.Reset();
	}
	McpWaitingSessionId.Empty();

	// Fire JS callback
	if (OnMcpStatusCallback.IsValid())
	{
		TWeakObjectPtr<UWebUIBridge> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId, Status]()
		{
			if (UWebUIBridge* Self = WeakThis.Get())
			{
				if (Self->OnMcpStatusCallback.IsValid())
				{
					Self->OnMcpStatusCallback(CapturedSessionId, Status);
				}
			}
		});
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("WebUIBridge: MCP status '%s' for session %s"), *Status, *CapturedSessionId);
}

// ── Attachments ──────────────────────────────────────────────────────

// Helper: serialize attachment list to JSON string (metadata only, no base64)
static FString SerializeAttachmentList(const TArray<FACPContextAttachment>& Attachments)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FACPContextAttachment& Att : Attachments)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Att.AttachmentId.ToString());

		FString TypeStr;
		FString DisplayName;
		switch (Att.Type)
		{
		case EACPAttachmentType::BlueprintNode:
			TypeStr = TEXT("blueprint_node");
			DisplayName = Att.NodeAttachment.NodeTitle;
			break;
		case EACPAttachmentType::Blueprint:
			TypeStr = TEXT("blueprint");
			DisplayName = Att.BlueprintAttachment.DisplayName;
			break;
		case EACPAttachmentType::ImageAsset:
			TypeStr = TEXT("image");
			DisplayName = Att.ImageAttachment.DisplayName;
			Obj->SetStringField(TEXT("mimeType"), Att.ImageAttachment.MimeType);
			Obj->SetNumberField(TEXT("width"), Att.ImageAttachment.Width);
			Obj->SetNumberField(TEXT("height"), Att.ImageAttachment.Height);
			Obj->SetStringField(TEXT("thumbnail"), Att.ImageAttachment.ImageBase64);
			break;
		case EACPAttachmentType::FileAsset:
			TypeStr = TEXT("file");
			DisplayName = Att.FileAttachment.DisplayName;
			Obj->SetStringField(TEXT("mimeType"), Att.FileAttachment.MimeType);
			Obj->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Att.FileAttachment.SizeBytes));
			Obj->SetBoolField(TEXT("hasExtractedText"), Att.FileAttachment.bHasExtractedText);
			break;
		case EACPAttachmentType::Actor:
			TypeStr = TEXT("actor");
			DisplayName = Att.ActorAttachment.ActorLabel;
			Obj->SetStringField(TEXT("className"), Att.ActorAttachment.ClassName);
			break;
		case EACPAttachmentType::GenericObject:
			TypeStr = TEXT("object");
			DisplayName = Att.GenericObjectAttachment.DisplayName;
			Obj->SetStringField(TEXT("className"), Att.GenericObjectAttachment.ClassName);
			Obj->SetStringField(TEXT("assetPath"), Att.GenericObjectAttachment.AssetPath);
			break;
		}
		Obj->SetStringField(TEXT("type"), TypeStr);
		Obj->SetStringField(TEXT("displayName"), DisplayName);
		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}
	return JsonArrayToString(Arr);
}

FString UWebUIBridge::PasteClipboardImage()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	FACPClipboardImageData ClipData = FACPClipboardImageReader::ReadImageFromClipboard();
	if (!ClipData.bIsValid)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("No image on clipboard"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();

	if (ClipData.EncodedData.Num() > 0)
	{
		// macOS path — already PNG/JPEG encoded
		AttMgr.AddImageFromEncodedData(ClipData.EncodedData, ClipData.MimeType, ClipData.Width, ClipData.Height, TEXT("Pasted Image"));
	}
	else if (ClipData.RawPixels.Num() > 0)
	{
		// Windows path — raw BGRA pixels
		AttMgr.AddImageFromRawData(ClipData.RawPixels, ClipData.Width, ClipData.Height, TEXT("Pasted Image"));
	}

	Result->SetBoolField(TEXT("success"), true);
	return JsonToString(Result);
}

FString UWebUIBridge::OpenImagePicker()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("count"), 0);
		return JsonToString(Result);
	}

	TArray<FString> OutFiles;
	const void* ParentWindowHandle = FSlateApplication::Get().GetActiveTopLevelWindow().IsValid()
		? FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle()
		: nullptr;

	if (DesktopPlatform->OpenFileDialog(
		ParentWindowHandle,
		TEXT("Select Attachments"),
		FPaths::ProjectDir(),
		TEXT(""),
		TEXT("Supported Files (*.png;*.jpg;*.jpeg;*.bmp;*.pdf;*.txt;*.md;*.json;*.csv;*.xml;*.yaml;*.yml;*.log;*.ini)|*.png;*.jpg;*.jpeg;*.bmp;*.pdf;*.txt;*.md;*.json;*.csv;*.xml;*.yaml;*.yml;*.log;*.ini"),
		EFileDialogFlags::Multiple,
		OutFiles))
	{
		FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
		for (const FString& FilePath : OutFiles)
		{
			const FString Ext = FPaths::GetExtension(FilePath).ToLower();
			if (Ext == TEXT("png") || Ext == TEXT("jpg") || Ext == TEXT("jpeg") || Ext == TEXT("bmp"))
			{
				AttMgr.AddImageFromFile(FilePath);
			}
			else
			{
				AttMgr.AddFileFromPath(FilePath);
			}
		}
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("count"), OutFiles.Num());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("count"), 0);
	}

	return JsonToString(Result);
}

FString UWebUIBridge::AddImageFromBase64(const FString& Base64Data, const FString& MimeType, int32 Width, int32 Height, const FString& DisplayName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Base64Data.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty base64 data"));
		return JsonToString(Result);
	}

	TArray<uint8> DecodedBytes;
	if (!FBase64::Decode(Base64Data, DecodedBytes))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to decode base64"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
	AttMgr.AddImageFromEncodedData(DecodedBytes, MimeType.IsEmpty() ? TEXT("image/png") : MimeType, Width, Height,
		DisplayName.IsEmpty() ? TEXT("Dropped Image") : DisplayName);

	// Return the ID of the last added attachment
	const TArray<FACPContextAttachment>& Atts = AttMgr.GetAttachments();
	if (Atts.Num() > 0)
	{
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("attachmentId"), Atts.Last().AttachmentId.ToString());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Attachment was not added"));
	}

	return JsonToString(Result);
}

FString UWebUIBridge::AddFileFromBase64(const FString& Base64Data, const FString& MimeType, const FString& DisplayName)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Base64Data.IsEmpty())
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Empty base64 data"));
		return JsonToString(Result);
	}

	TArray<uint8> DecodedBytes;
	if (!FBase64::Decode(Base64Data, DecodedBytes))
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Failed to decode base64"));
		return JsonToString(Result);
	}

	FACPAttachmentManager& AttMgr = FACPAttachmentManager::Get();
	AttMgr.AddFileFromEncodedData(
		DecodedBytes,
		MimeType.IsEmpty() ? TEXT("application/octet-stream") : MimeType,
		DisplayName.IsEmpty() ? TEXT("Dropped File") : DisplayName);

	const TArray<FACPContextAttachment>& Atts = AttMgr.GetAttachments();
	if (Atts.Num() > 0)
	{
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("attachmentId"), Atts.Last().AttachmentId.ToString());
	}
	else
	{
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Attachment was not added"));
	}

	return JsonToString(Result);
}

void UWebUIBridge::RemoveAttachment(const FString& AttachmentId)
{
	FGuid Guid;
	if (FGuid::Parse(AttachmentId, Guid))
	{
		FACPAttachmentManager::Get().RemoveAttachment(Guid);
	}
}

FString UWebUIBridge::GetAttachments()
{
	return SerializeAttachmentList(FACPAttachmentManager::Get().GetAttachments());
}

void UWebUIBridge::BindOnAttachmentsChanged(FWebJSFunction Callback)
{
	OnAttachmentsChangedCallback = Callback;

	// Subscribe to attachment manager delegate (not part of BindDelegates since it's on AttachmentManager, not AgentManager)
	if (!AttachmentsChangedHandle.IsValid())
	{
		AttachmentsChangedHandle = FACPAttachmentManager::Get().OnAttachmentsChanged.AddLambda(
			[this]()
			{
				if (!OnAttachmentsChangedCallback.IsValid()) return;

				FString JsonStr = SerializeAttachmentList(FACPAttachmentManager::Get().GetAttachments());

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnAttachmentsChangedCallback.IsValid())
						{
							Self->OnAttachmentsChangedCallback(CapturedJson);
						}
					}
				});
			}
		);
	}

}

FString UWebUIBridge::SerializeAgentUpdate(const FString& AgentName, const FACPSessionUpdate& Update) const
{
	TSharedRef<FJsonObject> UpdateJson = MakeShared<FJsonObject>();
	UpdateJson->SetStringField(TEXT("agentName"), AgentName);

	FString TypeStr;
	switch (Update.UpdateType)
	{
	case EACPUpdateType::AgentMessageChunk:  TypeStr = TEXT("text_chunk"); break;
	case EACPUpdateType::AgentThoughtChunk:  TypeStr = TEXT("thought_chunk"); break;
	case EACPUpdateType::ToolCall:            TypeStr = TEXT("tool_call"); break;
	case EACPUpdateType::ToolCallUpdate:      TypeStr = TEXT("tool_result"); break;
	case EACPUpdateType::Error:               TypeStr = TEXT("error"); break;
	case EACPUpdateType::UserMessageChunk:    TypeStr = TEXT("user_message_chunk"); break;
	case EACPUpdateType::UsageUpdate:         TypeStr = TEXT("usage"); break;
	case EACPUpdateType::Plan:                TypeStr = TEXT("plan"); break;
	case EACPUpdateType::HistoryReplayStarted: TypeStr = TEXT("history_replay_started"); break;
	case EACPUpdateType::HistoryReplayFinished: TypeStr = TEXT("history_replay_finished"); break;
	default:                                  TypeStr = TEXT("unknown"); break;
	}
	UpdateJson->SetStringField(TEXT("type"), TypeStr);
	UpdateJson->SetStringField(TEXT("text"), Update.TextChunk);

	if (Update.bIsSystemStatus)
	{
		UpdateJson->SetStringField(TEXT("systemStatus"), Update.SystemStatus);
	}

	if (!Update.ToolCallId.IsEmpty())
	{
		UpdateJson->SetStringField(TEXT("toolCallId"), Update.ToolCallId);
		UpdateJson->SetStringField(TEXT("toolName"), Update.ToolName);
		UpdateJson->SetStringField(TEXT("toolArguments"), Update.ToolArguments);
		UpdateJson->SetStringField(TEXT("toolResult"), Update.ToolResult);
		UpdateJson->SetBoolField(TEXT("toolSuccess"), Update.bToolSuccess);
		if (!Update.ParentToolCallId.IsEmpty())
		{
			UpdateJson->SetStringField(TEXT("parentToolCallId"), Update.ParentToolCallId);
		}

		if (Update.ToolResultImages.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ImagesArr;
			for (const FACPToolResultImage& Img : Update.ToolResultImages)
			{
				TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
				ImgObj->SetStringField(TEXT("base64"), Img.Base64Data);
				ImgObj->SetStringField(TEXT("mimeType"), Img.MimeType);
				ImgObj->SetNumberField(TEXT("width"), Img.Width);
				ImgObj->SetNumberField(TEXT("height"), Img.Height);
				ImagesArr.Add(MakeShared<FJsonValueObject>(ImgObj));
			}
			UpdateJson->SetArrayField(TEXT("images"), ImagesArr);
		}

		// ACP spec tool_call.locations[] — surface to WebUI for clickable file:line links.
		// Field is `locations: [{path, line?}]` to match the spec wire shape.
		// (Svelte ToolCallBlock.svelte can render these as jump-to-file links — currently a follow-up.)
		if (Update.Locations.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LocArr;
			for (const FACPToolCallLocation& Loc : Update.Locations)
			{
				TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
				LocObj->SetStringField(TEXT("path"), Loc.Path);
				if (Loc.Line != INDEX_NONE)
				{
					LocObj->SetNumberField(TEXT("line"), Loc.Line);
				}
				LocArr.Add(MakeShared<FJsonValueObject>(LocObj));
			}
			UpdateJson->SetArrayField(TEXT("locations"), LocArr);
		}
	}

	if (!Update.ErrorMessage.IsEmpty())
	{
		UpdateJson->SetStringField(TEXT("errorMessage"), Update.ErrorMessage);
		UpdateJson->SetNumberField(TEXT("errorCode"), Update.ErrorCode);
	}

	if (Update.UpdateType == EACPUpdateType::UsageUpdate)
	{
		const FACPUsageData& U = Update.Usage;
		UpdateJson->SetNumberField(TEXT("inputTokens"), U.InputTokens);
		UpdateJson->SetNumberField(TEXT("outputTokens"), U.OutputTokens);
		UpdateJson->SetNumberField(TEXT("totalTokens"), U.TotalTokens);
		UpdateJson->SetNumberField(TEXT("cacheReadTokens"), U.CacheReadTokens);
		UpdateJson->SetNumberField(TEXT("cacheCreationTokens"), U.CacheCreationTokens);
		UpdateJson->SetNumberField(TEXT("reasoningTokens"), U.ReasoningTokens);
		UpdateJson->SetNumberField(TEXT("costAmount"), U.CostAmount);
		UpdateJson->SetStringField(TEXT("costCurrency"), U.CostCurrency);
		UpdateJson->SetNumberField(TEXT("turnCostUSD"), U.TurnCostUSD);
		UpdateJson->SetNumberField(TEXT("contextUsed"), U.ContextUsed);
		UpdateJson->SetNumberField(TEXT("contextSize"), U.ContextSize);
		UpdateJson->SetNumberField(TEXT("numTurns"), U.NumTurns);
		UpdateJson->SetNumberField(TEXT("durationMs"), U.DurationMs);

		if (U.ModelUsage.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ModelArr;
			for (const FModelUsageEntry& M : U.ModelUsage)
			{
				TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
				MObj->SetStringField(TEXT("modelName"), M.ModelName);
				MObj->SetNumberField(TEXT("inputTokens"), M.InputTokens);
				MObj->SetNumberField(TEXT("outputTokens"), M.OutputTokens);
				MObj->SetNumberField(TEXT("cacheReadTokens"), M.CacheReadTokens);
				MObj->SetNumberField(TEXT("cacheCreationTokens"), M.CacheCreationTokens);
				MObj->SetNumberField(TEXT("costUSD"), M.CostUSD);
				MObj->SetNumberField(TEXT("contextWindow"), M.ContextWindow);
				MObj->SetNumberField(TEXT("maxOutputTokens"), M.MaxOutputTokens);
				ModelArr.Add(MakeShared<FJsonValueObject>(MObj));
			}
			UpdateJson->SetArrayField(TEXT("modelUsage"), ModelArr);
		}
	}

	if (Update.UpdateType == EACPUpdateType::HistoryReplayFinished)
	{
		UpdateJson->SetNumberField(TEXT("replayMessageCount"), Update.ReplayMessageCount);
		UpdateJson->SetBoolField(TEXT("replayEmpty"), Update.bReplayEmpty);
	}
	else if (Update.UpdateType == EACPUpdateType::HistoryReplayStarted)
	{
		UpdateJson->SetBoolField(TEXT("replayPreserveCached"), Update.bReplayPreserveCached);
	}

	return JsonToString(UpdateJson);
}

void UWebUIBridge::ScheduleStreamFlush()
{
	TWeakObjectPtr<UWebUIBridge> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis]()
	{
		if (UWebUIBridge* Self = WeakThis.Get())
		{
			if (!Self->StreamFlushTickerHandle.IsValid())
			{
				Self->StreamFlushTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateWeakLambda(Self, [Self](float)
					{
						Self->FlushPendingStreamUpdates(/*bFromTicker=*/true);
						return false;
					}),
					WebUIStreamFlushIntervalSeconds);
			}
		}
	});
}

void UWebUIBridge::FlushPendingStreamUpdates(bool bFromTicker)
{
	TArray<FPendingStreamUpdate> Pending;
	{
		FScopeLock Lock(&PendingStreamLock);
		for (const FString& Key : PendingStreamOrder)
		{
			if (FPendingStreamUpdate* Update = PendingStreamUpdates.Find(Key))
			{
				Pending.Add(*Update);
			}
		}
		PendingStreamUpdates.Empty();
		PendingStreamOrder.Empty();
		bStreamFlushScheduled = false;
	}

	if (!bFromTicker && StreamFlushTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StreamFlushTickerHandle);
	}
	StreamFlushTickerHandle.Reset();

	for (const FPendingStreamUpdate& PendingUpdate : Pending)
	{
		FACPSessionUpdate Update;
		Update.UpdateType = PendingUpdate.UpdateType;
		Update.TextChunk = PendingUpdate.Text;
		Update.bIsSystemStatus = PendingUpdate.bIsSystemStatus;
		Update.SystemStatus = PendingUpdate.SystemStatus;

		if (OnMessageCallback.IsValid())
		{
			OnMessageCallback(PendingUpdate.SessionId, SerializeAgentUpdate(PendingUpdate.AgentName, Update));
		}
	}
}

void UWebUIBridge::QueueOrDispatchAgentUpdate(const FString& SessionId, const FString& AgentName, const FACPSessionUpdate& Update)
{
	if (Update.UpdateType == EACPUpdateType::HistoryReplayStarted)
	{
		if (Update.bReplayPreserveCached)
		{
			PreserveCachedReplaySessions.Add(SessionId);
		}
		else
		{
			PreserveCachedReplaySessions.Remove(SessionId);
		}
	}
	else if (Update.UpdateType == EACPUpdateType::HistoryReplayFinished)
	{
		PreserveCachedReplaySessions.Remove(SessionId);
	}
	else if (PreserveCachedReplaySessions.Contains(SessionId))
	{
		return;
	}

	const bool bCoalescable =
		Update.ToolCallId.IsEmpty()
		&& Update.ErrorMessage.IsEmpty()
		&& (Update.UpdateType == EACPUpdateType::AgentMessageChunk || Update.UpdateType == EACPUpdateType::AgentThoughtChunk);

	if (bCoalescable)
	{
		bool bShouldScheduleFlush = false;
		const FString Key = FString::Printf(TEXT("%s|%s|%d|%s"),
			*SessionId,
			*AgentName,
			static_cast<int32>(Update.UpdateType),
			*Update.SystemStatus);

		{
			FScopeLock Lock(&PendingStreamLock);
			const bool bIsNewKey = !PendingStreamUpdates.Contains(Key);
			FPendingStreamUpdate& Pending = PendingStreamUpdates.FindOrAdd(Key);
			if (bIsNewKey)
			{
				PendingStreamOrder.Add(Key);
			}
			Pending.SessionId = SessionId;
			Pending.AgentName = AgentName;
			Pending.UpdateType = Update.UpdateType;
			Pending.bIsSystemStatus = Update.bIsSystemStatus;
			Pending.SystemStatus = Update.SystemStatus;
			Pending.Text += Update.TextChunk;

			if (!bStreamFlushScheduled)
			{
				bStreamFlushScheduled = true;
				bShouldScheduleFlush = true;
			}
		}

		if (bShouldScheduleFlush)
		{
			ScheduleStreamFlush();
		}
		return;
	}

	const FString CapturedJson = SerializeAgentUpdate(AgentName, Update);
	TWeakObjectPtr<UWebUIBridge> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson]()
	{
		if (UWebUIBridge* Self = WeakThis.Get())
		{
			Self->FlushPendingStreamUpdates(/*bFromTicker=*/false);
			if (Self->OnMessageCallback.IsValid())
			{
				Self->OnMessageCallback(CapturedSessionId, CapturedJson);
			}
		}
	});
}

void UWebUIBridge::BindOnInstallProgress(FWebJSFunction Callback)
{
	OnInstallProgressCallback = Callback;
}

void UWebUIBridge::BindOnInstallComplete(FWebJSFunction Callback)
{
	OnInstallCompleteCallback = Callback;
}

void UWebUIBridge::BindOnModelsAvailable(FWebJSFunction Callback)
{
	OnModelsAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnCommandsAvailable(FWebJSFunction Callback)
{
	OnCommandsAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnPlanUpdate(FWebJSFunction Callback)
{
	OnPlanUpdateCallback = Callback;
	BindDelegates();
}

// ── Streaming Callbacks ──────────────────────────────────────────────

void UWebUIBridge::BindOnMessage(FWebJSFunction Callback)
{
	OnMessageCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnStateChanged(FWebJSFunction Callback)
{
	OnStateChangedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnPermissionRequest(FWebJSFunction Callback)
{
	OnPermissionRequestCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnModesAvailable(FWebJSFunction Callback)
{
	OnModesAvailableCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::BindOnModeChanged(FWebJSFunction Callback)
{
	OnModeChangedCallback = Callback;
	BindDelegates();
}

void UWebUIBridge::RespondToPermission(const FString& AgentName, const FString& RequestId, const FString& OptionId, const FString& OutcomeMetaJson)
{
	if (AgentName.IsEmpty() || RequestId.IsEmpty() || OptionId.IsEmpty()) return;

	TSharedPtr<FJsonObject> OutcomeMeta;
	if (!OutcomeMetaJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutcomeMetaJson);
		FJsonSerializer::Deserialize(Reader, OutcomeMeta);
	}

	FACPAgentManager::Get().RespondToPermissionRequest(AgentName, RequestId, OptionId, OutcomeMeta);
}

void UWebUIBridge::RespondToPermissionForSession(
	const FString& SessionId,
	const FString& AgentName,
	const FString& RequestId,
	const FString& OptionId,
	const FString& OutcomeMetaJson)
{
	if (SessionId.IsEmpty() || AgentName.IsEmpty() || RequestId.IsEmpty() || OptionId.IsEmpty()) return;

	TSharedPtr<FJsonObject> OutcomeMeta;
	if (!OutcomeMetaJson.IsEmpty())
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutcomeMetaJson);
		FJsonSerializer::Deserialize(Reader, OutcomeMeta);
	}

	FACPAgentManager::Get().RespondToPermissionRequestForSession(SessionId, AgentName, RequestId, OptionId, OutcomeMeta);
}

void UWebUIBridge::BindDelegates()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	// Only bind once
	if (!AgentMessageHandle.IsValid())
	{
		AgentMessageHandle = AgentMgr.OnAgentMessage.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionUpdate& Update)
			{
				// Persistence to FACPSessionManager is owned by FAgentService::BindDelegates.
				// This lambda only forwards the update to the JavaScript callback;
				// adding a parallel persistence path here would double-create messages
				// in the session manager because both lambdas would call StartAssistantMessage.

				if (!OnMessageCallback.IsValid()) return;
				QueueOrDispatchAgentUpdate(SessionId, AgentName, Update);

			}
		);
	}

	if (!AgentStateHandle.IsValid())
	{
		AgentStateHandle = AgentMgr.OnAgentStateChanged.AddWeakLambda(this,
			[this](const FString& SessionId, const FString& AgentName, EACPClientState State, const FString& Message)
			{
				// Streaming-message finalization is owned by FAgentService::BindDelegates.

				// Detect prompting → ready/in_session transition for completion notifications
				EACPClientState* PrevPtr = PreviousSessionStates.Find(SessionId);
				EACPClientState PrevState = PrevPtr ? *PrevPtr : EACPClientState::Disconnected;
				PreviousSessionStates.FindOrAdd(SessionId) = State;

				if (PrevState == EACPClientState::Prompting
					&& (State == EACPClientState::Ready || State == EACPClientState::InSession))
				{
					FireCompletionNotifications(/*bSuccess=*/ true);
				}
				else if (PrevState == EACPClientState::Prompting && State == EACPClientState::Error)
				{
					FireCompletionNotifications(/*bSuccess=*/ false);
				}

				if (!OnStateChangedCallback.IsValid()) return;

				FString StateStr;
				switch (State)
				{
				case EACPClientState::Disconnected:  StateStr = TEXT("disconnected"); break;
				case EACPClientState::Connecting:     StateStr = TEXT("connecting"); break;
				case EACPClientState::Initializing:   StateStr = TEXT("initializing"); break;
				case EACPClientState::Ready:          StateStr = TEXT("ready"); break;
				case EACPClientState::InSession:      StateStr = TEXT("in_session"); break;
				case EACPClientState::Prompting:
					if (Message.Contains(TEXT("Queued"), ESearchCase::IgnoreCase))
					{
						StateStr = TEXT("prompting_queued_tool");
					}
					else if (Message.Contains(TEXT("tool"), ESearchCase::IgnoreCase)
						|| Message.Contains(TEXT("Running tools"), ESearchCase::IgnoreCase))
					{
						StateStr = TEXT("prompting_executing_tool");
					}
					else
					{
						StateStr = TEXT("prompting_streaming");
					}
					break;
				case EACPClientState::Error:          StateStr = TEXT("error"); break;
				default:                              StateStr = TEXT("unknown"); break;
				}

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, SessionId, AgentName, StateStr, Message, State]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnStateChangedCallback.IsValid())
						{
							Self->OnStateChangedCallback(SessionId, AgentName, StateStr, Message);
						}

						// Gate chat input until MCP tools are discovered by the ACP agent.
						// ACP agents get MCP config in session/new but discover tools asynchronously.
						if (State == EACPClientState::InSession
							&& FMCPServer::Get().IsRunning()
							&& !FACPAgentManager::Get().IsChatGatewayAgent(AgentName)
							&& Self->McpWaitingSessionId.IsEmpty())  // Not already waiting
						{
							Self->McpWaitingSessionId = SessionId;

							// Fire "waiting" status to JS
							if (Self->OnMcpStatusCallback.IsValid())
							{
								Self->OnMcpStatusCallback(SessionId, TEXT("waiting"));
							}

							// Check if MCP tools were already discovered (race: tools/list
							// can complete before we register the listener)
							if (FMCPServer::Get().HasClientDiscoveredTools())
							{
								Self->NotifyMcpStatus(Self->McpWaitingSessionId, TEXT("ready"));
								// NotifyMcpStatus clears McpWaitingSessionId, skip listener/timeout
							}
							else
							{
							// Listen for MCP client tools/list completion
							if (!Self->McpToolsDiscoveredHandle.IsValid())
							{
								Self->McpToolsDiscoveredHandle = FMCPServer::Get().OnClientToolsDiscovered.AddLambda(
									[WeakThis]()
									{
										if (UWebUIBridge* S = WeakThis.Get())
										{
											if (!S->McpWaitingSessionId.IsEmpty())
											{
												S->NotifyMcpStatus(S->McpWaitingSessionId, TEXT("ready"));
											}
										}
									});
							}

							// Timeout fallback — unblock after 15 seconds
							Self->McpTimeoutTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
								FTickerDelegate::CreateLambda([WeakThis](float) -> bool
								{
									if (UWebUIBridge* S = WeakThis.Get())
									{
										if (!S->McpWaitingSessionId.IsEmpty())
										{
											S->NotifyMcpStatus(S->McpWaitingSessionId, TEXT("timeout"));
										}
									}
									return false; // one-shot
								}), 5.0f);

							UE_LOG(LogNeoStackAI, Log,
								TEXT("WebUIBridge: Waiting for MCP tools discovery (session %s, agent %s)"),
								*SessionId, *AgentName);
							} // else (tools not yet discovered)
						}
					}
				});
			}
		);
	}

		if (!AgentErrorHandle.IsValid())
		{
			AgentErrorHandle = AgentMgr.OnAgentError.AddLambda(
				[this](const FString& SessionId, const FString& AgentName, int32 ErrorCode, const FString& ErrorMessage)
				{
					FString RoutedSessionId = SessionId;
					if (RoutedSessionId.IsEmpty())
					{
						RoutedSessionId = FACPAgentManager::Get().GetActiveSessionForAgent(AgentName);
					}

					// Streaming-message finalization on errors is owned by FAgentService::BindDelegates.

					if (!OnMessageCallback.IsValid()) return;

				// Construct a JSON error update matching the streaming update format
				TSharedRef<FJsonObject> UpdateJson = MakeShared<FJsonObject>();
				UpdateJson->SetStringField(TEXT("agentName"), AgentName);
				UpdateJson->SetStringField(TEXT("type"), TEXT("error"));
				UpdateJson->SetStringField(TEXT("text"), ErrorMessage);
				UpdateJson->SetStringField(TEXT("errorMessage"), ErrorMessage);
				UpdateJson->SetNumberField(TEXT("errorCode"), ErrorCode);

				FString JsonStr = JsonToString(UpdateJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = RoutedSessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnMessageCallback.IsValid())
						{
							Self->OnMessageCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!PermissionRequestHandle.IsValid())
	{
		PermissionRequestHandle = AgentMgr.OnAgentPermissionRequest.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPPermissionRequest& Request)
			{
				if (!OnPermissionRequestCallback.IsValid()) return;

				TSharedRef<FJsonObject> ReqJson = MakeShared<FJsonObject>();
				ReqJson->SetStringField(TEXT("agentName"), AgentName);
				ReqJson->SetStringField(TEXT("requestId"), Request.RequestId);
				ReqJson->SetBoolField(TEXT("isAskUserQuestion"), Request.bIsAskUserQuestion);

				// Tool call info
				TSharedPtr<FJsonObject> ToolCallObj = MakeShared<FJsonObject>();
				ToolCallObj->SetStringField(TEXT("toolCallId"), Request.ToolCall.ToolCallId);
				ToolCallObj->SetStringField(TEXT("title"), Request.ToolCall.Title);
				ToolCallObj->SetStringField(TEXT("rawInput"), Request.ToolCall.RawInput);
				ReqJson->SetObjectField(TEXT("toolCall"), ToolCallObj);

				// Permission options
				TArray<TSharedPtr<FJsonValue>> OptionsArr;
				for (const FACPPermissionOption& Opt : Request.Options)
				{
					TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
					OptObj->SetStringField(TEXT("optionId"), Opt.OptionId);
					OptObj->SetStringField(TEXT("name"), Opt.Name);
					OptObj->SetStringField(TEXT("kind"), Opt.Kind);
					OptionsArr.Add(MakeShared<FJsonValueObject>(OptObj));
				}
				ReqJson->SetArrayField(TEXT("options"), OptionsArr);

				// Questions (for AskUserQuestion)
				TArray<TSharedPtr<FJsonValue>> QuestionsArr;
				for (const FACPQuestion& Q : Request.Questions)
				{
					TSharedPtr<FJsonObject> QObj = MakeShared<FJsonObject>();
					QObj->SetStringField(TEXT("question"), Q.Question);
					QObj->SetStringField(TEXT("header"), Q.Header);
					QObj->SetBoolField(TEXT("multiSelect"), Q.bMultiSelect);

					TArray<TSharedPtr<FJsonValue>> QOptsArr;
					for (const FACPQuestionOption& QOpt : Q.Options)
					{
						TSharedPtr<FJsonObject> QOptObj = MakeShared<FJsonObject>();
						QOptObj->SetStringField(TEXT("label"), QOpt.Label);
						QOptObj->SetStringField(TEXT("description"), QOpt.Description);
						QOptsArr.Add(MakeShared<FJsonValueObject>(QOptObj));
					}
					QObj->SetArrayField(TEXT("options"), QOptsArr);
					QuestionsArr.Add(MakeShared<FJsonValueObject>(QObj));
				}
				ReqJson->SetArrayField(TEXT("questions"), QuestionsArr);

				FString JsonStr = JsonToString(ReqJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						Self->FirePermissionRequestNotification();
						if (Self->OnPermissionRequestCallback.IsValid())
						{
							Self->OnPermissionRequestCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!ModesAvailableHandle.IsValid())
	{
		ModesAvailableHandle = AgentMgr.OnAgentModesAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionModeState& ModeState)
			{
				if (!OnModesAvailableCallback.IsValid()) return;

				TArray<TSharedPtr<FJsonValue>> ModesArr;
				for (const FACPSessionMode& Mode : ModeState.AvailableModes)
				{
					TSharedPtr<FJsonObject> ModeObj = MakeShared<FJsonObject>();
					ModeObj->SetStringField(TEXT("id"), Mode.ModeId);
					ModeObj->SetStringField(TEXT("name"), Mode.Name);
					ModeObj->SetStringField(TEXT("description"), Mode.Description);
					ModesArr.Add(MakeShared<FJsonValueObject>(ModeObj));
				}

				TSharedRef<FJsonObject> ResultJson = MakeShared<FJsonObject>();
				ResultJson->SetArrayField(TEXT("modes"), ModesArr);
				ResultJson->SetStringField(TEXT("currentModeId"), ModeState.CurrentModeId);

				FString JsonStr = JsonToString(ResultJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModesAvailableCallback.IsValid())
						{
							Self->OnModesAvailableCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!ModeChangedHandle.IsValid())
	{
		ModeChangedHandle = AgentMgr.OnAgentModeChanged.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FString& ModeId)
			{
				if (!OnModeChangedCallback.IsValid()) return;

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, AgentName, ModeId]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModeChangedCallback.IsValid())
						{
							Self->OnModeChangedCallback(AgentName, ModeId);
						}
					}
				});
			}
		);
	}

	if (!ModelsAvailableHandle.IsValid())
	{
		ModelsAvailableHandle = AgentMgr.OnAgentModelsAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPSessionModelState& ModelState)
			{
				if (!OnModelsAvailableCallback.IsValid()) return;

				// Check if ACP client has reasoning effort options
					bool bACPHasReasoning = false;
					FACPAgentManager& Mgr = FACPAgentManager::Get();
					TSharedPtr<FACPClient> ACPClient = Mgr.GetClient(AgentName);
					if (ACPClient.IsValid() && ACPClient->SupportsReasoningEffortControl())
					{
						bACPHasReasoning = true;
					}

				TSharedRef<FJsonObject> ResultJson = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> ModelsArr;
				for (const FACPModelInfo& Model : ModelState.AvailableModels)
				{
					TSharedRef<FJsonObject> MObj = MakeShared<FJsonObject>();
					MObj->SetStringField(TEXT("id"), Model.ModelId);
					MObj->SetStringField(TEXT("name"), Model.Name);
					MObj->SetStringField(TEXT("description"), Model.Description);
					MObj->SetBoolField(TEXT("supportsReasoning"), Model.SupportsReasoning() || bACPHasReasoning);
					if (!Model.ProviderId.IsEmpty()) MObj->SetStringField(TEXT("provider"), Model.ProviderId);
					if (!Model.ProviderDisplayName.IsEmpty()) MObj->SetStringField(TEXT("providerDisplayName"), Model.ProviderDisplayName);
					ModelsArr.Add(MakeShared<FJsonValueObject>(MObj));
				}
				ResultJson->SetArrayField(TEXT("models"), ModelsArr);
				ResultJson->SetStringField(TEXT("currentModelId"), ModelState.CurrentModelId);

				FString JsonStr = JsonToString(ResultJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnModelsAvailableCallback.IsValid())
						{
							Self->OnModelsAvailableCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	// Bridge the new chat model registry into OnModelsAvailableCallback so the
	// built-in chat path produces the same picker updates as ACP agents.
	// Only bind once per UWebUIBridge instance.
	static bool bChatRegistryBound = false;
	if (!bChatRegistryBound)
	{
		bChatRegistryBound = true;
		FChatModelRegistry::Get().OnChanged.AddLambda(
		[this](const FChatModelRegistryChange& /*Change*/)
		{
			if (!OnModelsAvailableCallback.IsValid()) return;

			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("currentModelId"), FChatModelRegistry::Get().GetSelectedModel());

			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FChatModelInfo& M : FChatModelRegistry::Get().GetAllModelsFlat())
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("id"), M.GetPrefixedId());
				Obj->SetStringField(TEXT("name"), M.DisplayName);
				Obj->SetStringField(TEXT("description"), M.Description);
				Obj->SetBoolField(TEXT("supportsReasoning"), M.bSupportsReasoning);
				Obj->SetStringField(TEXT("provider"), M.ProviderId);
				Obj->SetStringField(TEXT("providerDisplayName"), M.ProviderDisplayName);
				Arr.Add(MakeShared<FJsonValueObject>(Obj));
			}
			Root->SetArrayField(TEXT("models"), Arr);

			FString JsonStr = JsonToString(Root);
			TWeakObjectPtr<UWebUIBridge> WeakThis(this);
			AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedJson = MoveTemp(JsonStr)]()
			{
				if (UWebUIBridge* Self = WeakThis.Get())
				{
					if (Self->OnModelsAvailableCallback.IsValid())
					{
						Self->OnModelsAvailableCallback(TEXT("Local & BYOK Chat"), CapturedJson);
					}
				}
			});
		});
	}

	if (!CommandsAvailableHandle.IsValid())
	{
		CommandsAvailableHandle = AgentMgr.OnAgentCommandsAvailable.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const TArray<FACPSlashCommand>& Commands)
			{
				if (!OnCommandsAvailableCallback.IsValid()) return;

				TArray<TSharedPtr<FJsonValue>> CmdsArr;
				for (const FACPSlashCommand& Cmd : Commands)
				{
					TSharedRef<FJsonObject> CmdObj = MakeShared<FJsonObject>();
					CmdObj->SetStringField(TEXT("name"), Cmd.Name);
					CmdObj->SetStringField(TEXT("description"), Cmd.Description);
					CmdObj->SetStringField(TEXT("inputHint"), Cmd.InputHint);
					CmdsArr.Add(MakeShared<FJsonValueObject>(CmdObj));
				}

				FString JsonStr = JsonArrayToString(CmdsArr);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnCommandsAvailableCallback.IsValid())
						{
							Self->OnCommandsAvailableCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!PlanUpdateHandle.IsValid())
	{
		PlanUpdateHandle = AgentMgr.OnAgentPlanUpdate.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, const FACPPlan& Plan)
			{
				if (!OnPlanUpdateCallback.IsValid()) return;

				TSharedRef<FJsonObject> PlanJson = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> EntriesArr;
				for (const FACPPlanEntry& Entry : Plan.Entries)
				{
					TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
					EntryObj->SetStringField(TEXT("content"), Entry.Content);
					EntryObj->SetStringField(TEXT("activeForm"), Entry.ActiveForm);

					FString PriorityStr;
					switch (Entry.Priority)
					{
					case EACPPlanEntryPriority::High:   PriorityStr = TEXT("high"); break;
					case EACPPlanEntryPriority::Medium: PriorityStr = TEXT("medium"); break;
					case EACPPlanEntryPriority::Low:    PriorityStr = TEXT("low"); break;
					}
					EntryObj->SetStringField(TEXT("priority"), PriorityStr);

					FString StatusStr;
					switch (Entry.Status)
					{
					case EACPPlanEntryStatus::Pending:    StatusStr = TEXT("pending"); break;
					case EACPPlanEntryStatus::InProgress: StatusStr = TEXT("in_progress"); break;
					case EACPPlanEntryStatus::Completed:  StatusStr = TEXT("completed"); break;
					}
					EntryObj->SetStringField(TEXT("status"), StatusStr);

					EntriesArr.Add(MakeShared<FJsonValueObject>(EntryObj));
				}
				PlanJson->SetArrayField(TEXT("entries"), EntriesArr);
				PlanJson->SetNumberField(TEXT("completedCount"), Plan.GetCompletedCount());
				PlanJson->SetNumberField(TEXT("totalCount"), Plan.Entries.Num());

				FString JsonStr = JsonToString(PlanJson);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSessionId = SessionId, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnPlanUpdateCallback.IsValid())
						{
							Self->OnPlanUpdateCallback(CapturedSessionId, CapturedJson);
						}
					}
				});
			}
		);
	}

	// Usage data updates (from FAgentUsageMonitor, not FACPAgentManager)
	if (!UsageUpdatedHandle.IsValid())
	{
		UsageUpdatedHandle = FAgentUsageMonitor::Get().OnUsageDataUpdated.AddLambda(
			[this](const FString& AgentName, const FAgentRateLimitData& Data)
			{
				if (!OnUsageUpdatedCallback.IsValid()) return;

				FString JsonStr = RateLimitDataToJsonString(Data);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedAgentName = AgentName, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnUsageUpdatedCallback.IsValid())
						{
							Self->OnUsageUpdatedCallback(CapturedAgentName, CapturedJson);
						}
					}
				});
			}
		);
	}

	// Meshy balance updates — re-push usage data so UI sees updated Meshy fields
	if (!MeshyBalanceHandle.IsValid())
	{
		MeshyBalanceHandle = FAgentUsageMonitor::Get().OnMeshyBalanceUpdated.AddLambda(
			[this](bool /*bSuccess*/, int32 /*Balance*/)
			{
				if (!OnUsageUpdatedCallback.IsValid()) return;

				// Re-serialize usage data for all cached agents so Meshy fields are fresh.
				// We push an update with agent name "_meshy" so the UI knows to refresh its cached data.
				// The RateLimitDataToJsonString reads Meshy state from the monitor singleton.
				FAgentRateLimitData DummyData;
				DummyData.AgentName = TEXT("_meshy");
				FString JsonStr = RateLimitDataToJsonString(DummyData);

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedJson = MoveTemp(JsonStr)]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnUsageUpdatedCallback.IsValid())
						{
							Self->OnUsageUpdatedCallback(TEXT("_meshy"), CapturedJson);
						}
					}
				});
			}
		);
	}

	if (!AgentAuthCompleteHandle.IsValid())
	{
		AgentAuthCompleteHandle = AgentMgr.OnAgentAuthComplete.AddLambda(
			[this](const FString& SessionId, const FString& AgentName, bool bSuccess, const FString& Error)
			{
				if (!OnLoginCompleteCallback.IsValid()) return;

				TWeakObjectPtr<UWebUIBridge> WeakThis(this);
				AsyncTask(ENamedThreads::GameThread, [WeakThis, AgentName, bSuccess, Error]()
				{
					if (UWebUIBridge* Self = WeakThis.Get())
					{
						if (Self->OnLoginCompleteCallback.IsValid())
						{
							Self->OnLoginCompleteCallback(AgentName, bSuccess, Error);
						}
					}
				});
			}
		);
	}

	if (!SessionListUpdatedHandle.IsValid())
	{
		SessionListUpdatedHandle = AgentMgr.OnAgentSessionListReceived.AddLambda(
			[this](const FString& AgentName, const TArray<FACPRemoteSessionEntry>& Sessions)
			{
				if (!OnSessionListUpdatedCallback.IsValid()) return;

				// Build a map from agent session ID → Unreal session ID for active sessions.
				// When a remote session matches an active session, we replace the agent ID
				// with the Unreal ID so the JS merge logic can match and update titles.
				FACPSessionManager& SessionMgr = FACPSessionManager::Get();
				TMap<FString, FString> AgentIdToUnrealId;
				TArray<FString> ActiveIds = SessionMgr.GetActiveSessionIds();
				for (const FString& Id : ActiveIds)
				{
					const FACPActiveSession* Active = SessionMgr.GetActiveSession(Id);
					if (Active && !Active->Metadata.AgentSessionId.IsEmpty())
					{
						AgentIdToUnrealId.Add(Active->Metadata.AgentSessionId, Id);
					}
				}

				// Serialize the session list to JSON
				TArray<TSharedPtr<FJsonValue>> SessionsArray;
				FACPAgentManager& AgentMgrForReg = FACPAgentManager::Get();
				for (const FACPRemoteSessionEntry& Entry : Sessions)
				{
					TSharedRef<FJsonObject> SessionObj = MakeShared<FJsonObject>();

					// If this remote session maps to an active Unreal session,
					// use the Unreal ID so JS dedup/merge works correctly
					FString UseSessionId = Entry.SessionId;
					if (const FString* UnrealId = AgentIdToUnrealId.Find(Entry.SessionId))
					{
						UseSessionId = *UnrealId;

						// Also update the active session's title in the session manager
						if (!Entry.Title.IsEmpty())
						{
							SessionMgr.UpdateSessionTitle(*UnrealId, Entry.Title);
						}
					}

					SessionObj->SetStringField(TEXT("sessionId"), UseSessionId);
					SessionObj->SetStringField(TEXT("title"), Entry.Title);

					// Apply persisted custom title if available (survives editor restarts)
					if (const FString* Persisted = SessionMgr.GetPersistedCustomTitle(UseSessionId))
					{
						SessionObj->SetStringField(TEXT("title"), *Persisted);
						SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
					}
					else if (const FString* PersistedByAgent = SessionMgr.GetPersistedCustomTitle(Entry.SessionId))
					{
						SessionObj->SetStringField(TEXT("title"), *PersistedByAgent);
						SessionObj->SetBoolField(TEXT("hasCustomTitle"), true);
					}

					if (Entry.UpdatedAt.GetTicks() > 0)
					{
						SessionObj->SetStringField(TEXT("lastModifiedAt"), Entry.UpdatedAt.ToIso8601());
					}

					SetSessionAgentRegistryFields(AgentMgrForReg, *SessionObj, AgentName);

					SessionsArray.Add(MakeShared<FJsonValueObject>(SessionObj));
				}
				FString SessionsJson = JsonArrayToString(SessionsArray);

				// This delegate fires on the game thread (ProcessLine dispatches there).
				// Call the JS callback directly — no need for a second AsyncTask dispatch
				// which can race with GC and cause the callback to silently drop.
				OnSessionListUpdatedCallback(AgentName, SessionsJson);
			}
		);
	}
}

void UWebUIBridge::UnbindDelegates()
{
	FACPAgentManager& AgentMgr = FACPAgentManager::Get();

	if (AgentMessageHandle.IsValid())
	{
		AgentMgr.OnAgentMessage.Remove(AgentMessageHandle);
		AgentMessageHandle.Reset();
	}
	if (AgentStateHandle.IsValid())
	{
		AgentMgr.OnAgentStateChanged.Remove(AgentStateHandle);
		AgentStateHandle.Reset();
	}
	if (AgentErrorHandle.IsValid())
	{
		AgentMgr.OnAgentError.Remove(AgentErrorHandle);
		AgentErrorHandle.Reset();
	}
	if (PermissionRequestHandle.IsValid())
	{
		AgentMgr.OnAgentPermissionRequest.Remove(PermissionRequestHandle);
		PermissionRequestHandle.Reset();
	}
	if (ModesAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentModesAvailable.Remove(ModesAvailableHandle);
		ModesAvailableHandle.Reset();
	}
	if (ModeChangedHandle.IsValid())
	{
		AgentMgr.OnAgentModeChanged.Remove(ModeChangedHandle);
		ModeChangedHandle.Reset();
	}
	if (CommandsAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentCommandsAvailable.Remove(CommandsAvailableHandle);
		CommandsAvailableHandle.Reset();
	}
	if (PlanUpdateHandle.IsValid())
	{
		AgentMgr.OnAgentPlanUpdate.Remove(PlanUpdateHandle);
		PlanUpdateHandle.Reset();
	}
	if (ModelsAvailableHandle.IsValid())
	{
		AgentMgr.OnAgentModelsAvailable.Remove(ModelsAvailableHandle);
		ModelsAvailableHandle.Reset();
	}
	if (UsageUpdatedHandle.IsValid())
	{
		FAgentUsageMonitor::Get().OnUsageDataUpdated.Remove(UsageUpdatedHandle);
		UsageUpdatedHandle.Reset();
	}
	if (MeshyBalanceHandle.IsValid())
	{
		FAgentUsageMonitor::Get().OnMeshyBalanceUpdated.Remove(MeshyBalanceHandle);
		MeshyBalanceHandle.Reset();
	}
	if (AttachmentsChangedHandle.IsValid())
	{
		FACPAttachmentManager::Get().OnAttachmentsChanged.Remove(AttachmentsChangedHandle);
		AttachmentsChangedHandle.Reset();
	}
	if (AgentAuthCompleteHandle.IsValid())
	{
		AgentMgr.OnAgentAuthComplete.Remove(AgentAuthCompleteHandle);
		AgentAuthCompleteHandle.Reset();
	}
	if (McpToolsDiscoveredHandle.IsValid())
	{
		FMCPServer::Get().OnClientToolsDiscovered.Remove(McpToolsDiscoveredHandle);
		McpToolsDiscoveredHandle.Reset();
	}
	if (McpTimeoutTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(McpTimeoutTickerHandle);
		McpTimeoutTickerHandle.Reset();
	}
	if (SessionListUpdatedHandle.IsValid())
	{
		AgentMgr.OnAgentSessionListReceived.Remove(SessionListUpdatedHandle);
		SessionListUpdatedHandle.Reset();
	}

	// Terminal delegates
	if (TerminalOutputHandle.IsValid())
	{
		FTerminalManager::Get().OnTerminalOutput.Remove(TerminalOutputHandle);
		TerminalOutputHandle.Reset();
	}
	if (TerminalExitHandle.IsValid())
	{
		FTerminalManager::Get().OnTerminalExit.Remove(TerminalExitHandle);
		TerminalExitHandle.Reset();
	}
}

// ── JSON Serialization Helpers ───────────────────────────────────────

TSharedPtr<FJsonObject> UWebUIBridge::ContentBlockToJson(const FACPContentBlock& Block)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	FString TypeStr;
	switch (Block.Type)
	{
	case EACPContentBlockType::Text:       TypeStr = TEXT("text"); break;
	case EACPContentBlockType::Thought:    TypeStr = TEXT("thought"); break;
	case EACPContentBlockType::ToolCall:   TypeStr = TEXT("tool_call"); break;
	case EACPContentBlockType::ToolResult: TypeStr = TEXT("tool_result"); break;
	case EACPContentBlockType::Image:      TypeStr = TEXT("image"); break;
	case EACPContentBlockType::Error:      TypeStr = TEXT("error"); break;
	case EACPContentBlockType::System:     TypeStr = TEXT("system"); break;
	default:                               TypeStr = TEXT("unknown"); break;
	}
	Obj->SetStringField(TEXT("type"), TypeStr);
	Obj->SetStringField(TEXT("text"), Block.Text);
	Obj->SetBoolField(TEXT("isStreaming"), Block.bIsStreaming);

	if (Block.Type == EACPContentBlockType::ToolCall)
	{
		Obj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
		Obj->SetStringField(TEXT("toolName"), Block.ToolName);
		Obj->SetStringField(TEXT("toolArguments"), Block.ToolArguments);
		if (!Block.ParentToolCallId.IsEmpty())
		{
			Obj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
		}
	}

	if (Block.Type == EACPContentBlockType::ToolResult)
	{
		Obj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
		Obj->SetStringField(TEXT("toolResult"), Block.ToolResultContent);
		Obj->SetBoolField(TEXT("toolSuccess"), Block.bToolSuccess);
		if (!Block.ParentToolCallId.IsEmpty())
		{
			Obj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
		}

		// Serialize tool result images (base64 + metadata)
		if (Block.ToolResultImages.Num() > 0)
		{
			Obj->SetNumberField(TEXT("imageCount"), Block.ToolResultImages.Num());

			TArray<TSharedPtr<FJsonValue>> ImagesArr;
			for (const FACPToolResultImage& Img : Block.ToolResultImages)
			{
				TSharedRef<FJsonObject> ImgObj = MakeShared<FJsonObject>();
				ImgObj->SetStringField(TEXT("base64"), Img.Base64Data);
				ImgObj->SetStringField(TEXT("mimeType"), Img.MimeType);
				ImgObj->SetNumberField(TEXT("width"), Img.Width);
				ImgObj->SetNumberField(TEXT("height"), Img.Height);
				ImagesArr.Add(MakeShared<FJsonValueObject>(ImgObj));
			}
			Obj->SetArrayField(TEXT("images"), ImagesArr);
		}
	}

	return Obj;
}

TSharedPtr<FJsonObject> UWebUIBridge::MessageToJson(const FACPChatMessage& Message)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("messageId"), Message.MessageId.ToString());

	FString RoleStr;
	switch (Message.Role)
	{
	case EACPMessageRole::User:      RoleStr = TEXT("user"); break;
	case EACPMessageRole::Assistant: RoleStr = TEXT("assistant"); break;
	case EACPMessageRole::System:    RoleStr = TEXT("system"); break;
	default:                         RoleStr = TEXT("unknown"); break;
	}
	Obj->SetStringField(TEXT("role"), RoleStr);
	Obj->SetBoolField(TEXT("isStreaming"), Message.bIsStreaming);
	Obj->SetStringField(TEXT("timestamp"), Message.Timestamp.ToIso8601());

	// Content blocks
	TArray<TSharedPtr<FJsonValue>> BlocksArray;
	for (const FACPContentBlock& Block : Message.ContentBlocks)
	{
		TSharedPtr<FJsonObject> BlockJson = ContentBlockToJson(Block);
		if (BlockJson.IsValid())
		{
			BlocksArray.Add(MakeShared<FJsonValueObject>(BlockJson));
		}
	}
	Obj->SetArrayField(TEXT("contentBlocks"), BlocksArray);

	return Obj;
}
