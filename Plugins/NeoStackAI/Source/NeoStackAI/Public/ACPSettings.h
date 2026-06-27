// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ACPTypes.h"
#include "ACPSettings.generated.h"

/**
 * Agent configuration stored in settings
 */
USTRUCT(BlueprintType)
struct FACPAgentSettingsEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString AgentName;

	UPROPERTY(EditAnywhere, Category = "Agent", meta = (FilePathFilter = "Executable files (*.exe)|*.exe|All files (*.*)|*.*"))
	FFilePath ExecutablePath;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TArray<FString> Arguments;

	UPROPERTY(EditAnywhere, Category = "Agent", meta = (RelativeToGameDir))
	FDirectoryPath WorkingDirectory;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TMap<FString, FString> EnvironmentVariables;

	// For agents that need API keys
	UPROPERTY(EditAnywhere, Category = "Agent", meta = (PasswordField = true))
	FString ApiKey;

	// Model ID for agents that support model selection
	UPROPERTY(EditAnywhere, Category = "Agent")
	FString ModelId;
};

// ============================================================================
// Chat provider settings (replaces legacy FCustomProviderDefinition /
// EnabledModels / ProviderApiKeys / ProviderBaseUrls / ProviderPriority).
// ============================================================================

/**
 * Per-provider configuration row used by the new chat provider layer.
 * One row per enabled provider (built-in or user-defined), keyed by
 * FChatProviderSettings::ProviderId which matches IChatProvider::GetId().
 */
USTRUCT()
struct FChatProviderSettings
{
	GENERATED_BODY()

	/** Provider id, e.g. "openrouter", "anthropic", "userprovider_<guid>". */
	UPROPERTY(config)
	FString ProviderId;

	/** API key (password-field in the UI). Empty if not yet configured. */
	UPROPERTY(config)
	FString ApiKey;

	/** Base URL override; empty = provider default. */
	UPROPERTY(config)
	FString BaseUrlOverride;

	/** Whether this provider participates in the registry and model picker. */
	UPROPERTY(config)
	bool bEnabled = true;
};

/**
 * A single model entry stored on a user-defined chat provider.
 */
USTRUCT()
struct FChatModelEntry
{
	GENERATED_BODY()

	UPROPERTY(config)
	FString ModelId;

	UPROPERTY(config)
	FString DisplayName;

	UPROPERTY(config)
	FString Description;
};

/**
 * A user-defined chat provider (any OpenAI-compatible endpoint). Each becomes
 * a first-class IChatProvider instance with its own models.
 */
USTRUCT()
struct FUserChatProvider
{
	GENERATED_BODY()

	/** Stable id, auto-generated as "userprovider_<guid>" on creation. */
	UPROPERTY(config)
	FString Id;

	/** User-facing display name shown in the picker's group header. */
	UPROPERTY(config)
	FString DisplayName;

	/** Base URL for the OpenAI-compatible API. */
	UPROPERTY(config)
	FString BaseUrl;

	/** Most endpoints need an API key; set to false for local servers. */
	UPROPERTY(config)
	bool bRequiresApiKey = true;

	/** Also attempt /models endpoint discovery in addition to the manual list. */
	UPROPERTY(config)
	bool bEnableDiscovery = false;

	/** Manually configured model list for this provider. */
	UPROPERTY(config)
	TArray<FChatModelEntry> StaticModels;
};

/**
 * Settings for the NeoStack AI plugin
 * Accessible via Project Settings > Plugins > NeoStack AI
 */
UCLASS(config = NeoStackAI, defaultconfig, meta = (DisplayName = "NeoStack AI"))
class NEOSTACKAI_API UACPSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UACPSettings();

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName(TEXT("Plugins")); }
	virtual FName GetSectionName() const override { return FName(TEXT("NeoStack AI")); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// Get singleton instance
	static UACPSettings* Get();

	// ============================================
	// General
	// ============================================

	/** Last agent used — automatically saved when creating a session, used as default next time */
	UPROPERTY(config)
	FString LastUsedAgentName;

	/** Check for newer versions of NeoStack AI when the editor starts */
	UPROPERTY(config, EditAnywhere, Category = "General", meta = (DisplayName = "Check for Updates",
		ToolTip = "Automatically check for newer versions of NeoStack AI when the editor starts. Shows a banner in the chat window if an update is available."))
	bool bCheckForUpdates = true;

	// ============================================
	// Auto-Update
	// ============================================

	/** Automatically download updates when available (still requires manual editor restart to install) */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Auto-Download Updates",
		ToolTip = "When enabled and an API token is set, updates are automatically downloaded in the background. You will still be prompted before installation."))
	bool bAutoDownloadUpdates = false;

	/** When the core plugin updates, also fold any out-of-date NeoStack extensions installed
	 *  in this project into the same update so they all reach the new release version together.
	 *  Prevents version-skew between core and extensions, which can cause missing-symbol crashes
	 *  on the binary tier where extensions can't be recompiled locally. */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Update Extensions Alongside Core",
		ToolTip = "When the core plugin updates, also update any out-of-date NeoStack extensions in this project so everything stays on the same release version. Disable to keep core updates separate from extensions."))
	bool bAlsoUpdateExtensions = true;

	/** Opt into beta channel to receive pre-release updates before they go to stable */
	UPROPERTY(config, EditAnywhere, Category = "Auto-Update", meta = (DisplayName = "Beta Channel",
		ToolTip = "When enabled, version checks will include beta/pre-release versions. Useful for testing updates before they go live to all users."))
	bool bUseBetaChannel = false;

	// ============================================
	// Analytics (anonymous, privacy-first)
	// ============================================

	/** Send anonymous usage analytics to help improve the plugin.
	 *  No personal data is collected — only tool/agent usage counts and error rates.
	 *  A random anonymous ID is generated per install (no machine fingerprint). */
	UPROPERTY(config, EditAnywhere, Category = "Analytics", meta = (DisplayName = "Enable Anonymous Analytics",
		ToolTip = "Help improve NeoStack AI by sending anonymous usage data (which tools and agents are used, error rates). No personal information, file paths, or code content is ever sent. You can disable this at any time."))
	bool bEnableAnalytics = true;

	// ============================================
	// Crash Reporting
	// ============================================

	/** When enabled and a crash involving AIK is detected on next launch, a basic crash
	 *  report (error message, crash type, callstack summary) is sent automatically.
	 *  This respects the Enable Analytics toggle above — if analytics are off, no crash
	 *  data is sent either. */
	UPROPERTY(config, EditAnywhere, Category = "Crash Reporting", meta = (DisplayName = "Enable Crash Reporting",
		ToolTip = "Automatically send a basic crash report (error type and callstack summary) when a crash involving NeoStack AI is detected. No log files or project content are included unless you explicitly choose to send them."))
	bool bEnableCrashReporting = true;

	/** When enabled, the full editor log is sent along with crash reports without prompting.
	 *  If disabled, you will be asked each time a crash is detected whether to include the log. */
	UPROPERTY(config, EditAnywhere, Category = "Crash Reporting", meta = (DisplayName = "Always Send Full Logs",
		ToolTip = "Skip the crash report prompt and always include the full editor log with crash reports. You can change this at any time."))
	bool bAlwaysSendCrashLogs = false;

	/** Upload URL used to send crash reports.
	 *  When empty, full crash uploads are disabled. */
	UPROPERTY(config, EditAnywhere, Category = "Crash Reporting", meta = (DisplayName = "Crash Report URL",
		ToolTip = "Upload URL used to upload crash reports. When empty, full crash uploads are disabled."))
	FString CrashReportUrl;

	// ============================================
	// Agent Feedback
	// ============================================

	/** When disabled, the in-editor `report_issue()` Lua binding short-circuits before
	 *  any HTTP call and returns {accepted=false, reason="disabled_locally"} to the
	 *  agent. Default off (reports allowed) — toggle from the WebUI Settings panel. */
	UPROPERTY(config, EditAnywhere, Category = "Agent Feedback", meta = (DisplayName = "Disable Agent Issue Reports",
		ToolTip = "Block agents from submitting issue reports to neostack.dev via the report_issue() Lua binding. The plugin won't even make the HTTP call when this is on."))
	bool bDisableAgentIssueReports = false;

	// ============================================
	// ACP Agents (External CLI Agents)
	// ============================================

	/** Registry agent IDs that the user has "installed" (managed via Web UI agent registry) */
	UPROPERTY(config)
	TArray<FString> InstalledAgentIds;

	/** Custom ACP agent definitions. Each entry spawns an external process that communicates via ACP (JSON-RPC over stdio). */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "Custom Agents",
		ToolTip = "Define custom ACP-compatible agents. Each agent is an external process that communicates over stdin/stdout using the Agent Client Protocol."))
	TArray<FACPAgentSettingsEntry> CustomAgents;

	/** Generic agent executable path overrides, keyed by ACP registry ID (e.g., "claude-acp", "gemini").
	 *  Used by the registry-based config generator. Takes precedence over auto-detection. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "Agent Path Overrides (Registry ID → Path)",
		ToolTip = "Custom executable paths for specific agents. Key = registry agent ID (e.g., 'claude-acp', 'gemini'), Value = path to executable.",
		AdvancedDisplay))
	TMap<FString, FString> AgentPathOverrides;

	// ============================================
	// AI Generation
	// ============================================

	/** Meshy API key for AI 3D model generation (used by generate_asset with asset_type=model_3d). Get one at meshy.ai */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Meshy)", meta = (PasswordField = true, DisplayName = "Meshy API Key",
		ToolTip = "Your Meshy API key. In NeoStack Cloud mode this is BYOK passthrough — set it and we use your key (billed by Meshy directly); leave empty and we use NeoStack's caps. In Direct mode this is required. Get a key at https://meshy.ai"))
	FString MeshyApiKey;

	/** Route Meshy requests through NeoStack Cloud (recommended). When disabled, the editor connects directly to Meshy's API. */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Meshy)", meta = (DisplayName = "Route through NeoStack Cloud",
		ToolTip = "Route Meshy requests through NeoStack Cloud (recommended). Lets you bill via NeoStack caps without a Meshy account, get unified usage analytics on neostack.dev, and seamlessly fall back to your own key (set above) if you have one. Disable to send requests straight to Meshy's API instead."))
	bool bMeshyUseCloud = true;

	/** Tripo API key for AI 3D model generation. Get one at tripo3d.ai */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Tripo)", meta = (PasswordField = true, DisplayName = "Tripo API Key",
		ToolTip = "Your Tripo API key. In NeoStack Cloud mode this is BYOK passthrough — set it and we use your key (billed by Tripo directly); leave empty and we use NeoStack's caps. In Direct mode this is required. Get a key at https://tripo3d.ai"))
	FString TripoApiKey;

	/** Route Tripo requests through NeoStack Cloud (recommended). When disabled, the editor connects directly to Tripo's API. */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (Tripo)", meta = (DisplayName = "Route through NeoStack Cloud",
		ToolTip = "Route Tripo requests through NeoStack Cloud (recommended). Lets you bill via NeoStack caps without a Tripo account, get unified usage analytics on neostack.dev, and seamlessly fall back to your own key (set above) if you have one. Disable to send requests straight to Tripo's API instead."))
	bool bTripoUseCloud = true;

	/** ElevenLabs API key for AI audio generation (TTS, sound effects, music, STT). Get one at elevenlabs.io */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | Audio (ElevenLabs)", meta = (PasswordField = true, DisplayName = "ElevenLabs API Key",
		ToolTip = "Your ElevenLabs API key. In NeoStack Cloud mode this is BYOK passthrough — set it and we use your key (billed by ElevenLabs directly); leave empty and we use NeoStack's caps. In Direct mode this is required. Get a key at https://elevenlabs.io"))
	FString ElevenLabsApiKey;

	/** Route ElevenLabs requests through NeoStack Cloud (recommended). When disabled, the editor connects directly to ElevenLabs' API. */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | Audio (ElevenLabs)", meta = (DisplayName = "Route through NeoStack Cloud",
		ToolTip = "Route ElevenLabs requests through NeoStack Cloud (recommended). Lets you bill via NeoStack caps without an ElevenLabs account, get unified usage analytics on neostack.dev, and seamlessly fall back to your own key (set above) if you have one. Disable to send requests straight to ElevenLabs' API instead."))
	bool bElevenLabsUseCloud = true;

	/** fal.ai API key for AI 3D model generation (used by generate_3d_model with provider='fal'). */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | 3D Models (fal.ai)", meta = (PasswordField = true, DisplayName = "fal.ai API Key",
		ToolTip = "Your fal.ai API key for direct BYOK 3D generation (for example Hunyuan3D models)."))
	FString FalApiKey;

	/** OpenAI API key for direct GPT Image generation. */
	UPROPERTY(config, EditAnywhere, Category = "AI Generation | Images (OpenAI)", meta = (PasswordField = true, DisplayName = "OpenAI API Key",
		ToolTip = "Your OpenAI API key for direct GPT Image 2 generation. Used by generate({provider='openai', action='text_to_image'})."))
	FString OpenAIApiKey;

	// ============================================
	// MCP Server
	// ============================================

	/** Allow browser-based access to the MCP server. When disabled (default), requests with an Origin header
	 *  are rejected. Browsers always send Origin on cross-origin requests; CLI tools do not.
	 *  This prevents malicious websites from calling your MCP server while you browse the web. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (DisplayName = "Allow Browser Requests",
		ToolTip = "When disabled, HTTP requests with an Origin header are rejected. This blocks browser-based cross-origin requests (CSRF protection). CLI agents (Claude Code, Gemini, Codex) never send Origin headers and are unaffected. Only enable if you use a browser-based MCP client."))
	bool bAllowBrowserMCPRequests = false;

	/** Preferred MCP server port. If occupied, the server automatically tries subsequent ports. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (DisplayName = "Server Port", ClampMin = 1, ClampMax = 65535,
		ToolTip = "Preferred local TCP port for the built-in MCP server. If this port is already in use, NeoStack AI will scan a few higher ports automatically."))
	int32 MCPServerPort = 9315;

	// ============================================
	// Remote Access
	// ============================================

	/** Enable remote access to this instance via the relay server.
	 *  When enabled, the editor connects to the relay and becomes accessible from neostack.dev. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", meta = (DisplayName = "Enable Remote Access",
		ToolTip = "Connect this instance to the relay server so you can control it remotely from neostack.dev. Requires a NeoStack API key (neostack_...)."))
	bool bEnableRemoteAccess = false;

	// NeoStackApiKey lived here historically as a standalone field used only
	// by the relay client. It now lives in the "neostack" row of
	// ChatProviderSettings (canonical store, paired with the cloud-chat
	// provider key). RelayClient + EntitlementClient + NSAIAnalytics +
	// FGenerativeProviderBase all read via GetChatProviderApiKey("neostack")
	// today. One-shot migrator at UACPSettings::Get() lifts the old .ini line
	// into the chat-provider row so existing installs keep working.

	/** A friendly name for this instance (shown on the website when picking which instance to control). */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", meta = (DisplayName = "Instance Name",
		ToolTip = "Human-readable name shown on neostack.dev when you have multiple instances connected. Defaults to 'ComputerName - ProjectName' if left empty."))
	FString InstanceName;

	/** Stable local identifier for this editor instance. Auto-generated when remote access first connects. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", AdvancedDisplay, meta = (DisplayName = "Remote Instance ID",
		ToolTip = "Stable ID used by neostack.dev to route remote web and mobile clients to this editor instance. Leave empty to auto-generate."))
	FString RemoteInstanceId;

	/** Timeout for streaming OpenAI-compatible chat requests. Large contexts can take a while, but hung transports should eventually release the UI. */
	UPROPERTY(config, EditAnywhere, Category = "Chat & Agents", meta = (DisplayName = "Chat HTTP Timeout Seconds", ClampMin = 30, ClampMax = 1800,
		ToolTip = "Timeout for streaming chat completion HTTP requests. Increase for very large contexts; decrease to fail faster on broken networks."))
	float ChatHttpTimeoutSeconds = 300.0f;

	/** The relay server URL. Only change this if you're running a custom relay. */
	UPROPERTY(config, EditAnywhere, Category = "Remote Access", AdvancedDisplay, meta = (DisplayName = "Relay Server URL",
		ToolTip = "WebSocket base URL of the AIK relay server. The instance ID is appended automatically."))
	FString RelayServerUrl = TEXT("wss://neostack.dev/api/relay/instance");

	// ============================================
	// IDE Connection
	// ============================================

	/** Automatically connect to the NeoStack IDE when it's running locally.
	 *  The plugin discovers the IDE via ~/.neostack/server.json. */
	UPROPERTY(config, EditAnywhere, Category = "IDE Connection", meta = (DisplayName = "Enable IDE Connection",
		ToolTip = "Automatically connect to the NeoStack IDE desktop app when it's running on the same machine. No API key required — works offline."))
	bool bEnableIDEConnection = true;

	/** Override the default discovery file path (~/.neostack/server.json). */
	UPROPERTY(config, EditAnywhere, Category = "IDE Connection", AdvancedDisplay, meta = (DisplayName = "Discovery File Path Override",
		ToolTip = "Override the default path to the IDE discovery file. Leave empty to use ~/.neostack/server.json."))
	FString IDEDiscoveryPathOverride;

	// ============================================
	// Debug
	// ============================================

	/** Enable verbose logging for ACP/MCP communication (logged to Output Log under LogNeoStackAI) */
	UPROPERTY(config, EditAnywhere, Category = "Debug", meta = (DisplayName = "Verbose Logging",
		ToolTip = "Logs all ACP/MCP JSON messages to the Output Log. Useful for debugging agent communication issues. Can produce a lot of output."))
	bool bVerboseLogging = false;

	// ============================================
	// Internal (not shown in UI)
	// ============================================

	/** Per-provider settings rows for the chat layer. One entry per provider id. */
	UPROPERTY(config)
	TArray<FChatProviderSettings> ChatProviders;

	/** User-defined chat providers (any OpenAI-compatible endpoint). */
	UPROPERTY(config)
	TArray<FUserChatProvider> UserChatProviders;

	/** Selected chat model in prefixed form "<providerId>:<modelId>". Empty = use default. */
	UPROPERTY(config)
	FString SelectedChatModelId;

	/** Per-agent saved model selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedModelPerAgent;

	/** Per-agent saved mode selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedModePerAgent;

	/** Per-agent saved reasoning effort selections (persisted across editor sessions) */
	UPROPERTY(config)
	TMap<FString, FString> SelectedReasoningPerAgent;

	/** Per-agent system prompt delivery method. Key = agent name (e.g. "Open Code", "Codex CLI").
	 *  Agents not listed here default to SessionMeta. */
	UPROPERTY(config, EditAnywhere, Category = "ACP Agents", meta = (DisplayName = "System Prompt Delivery Per Agent",
		ToolTip = "How to deliver the custom system prompt to each agent. SessionMeta uses _meta.systemPrompt (Claude Code). FirstUserMessage/EveryUserMessage prepend it to user messages (for agents like Open Code that ignore _meta)."))
	TMap<FString, ESystemPromptDelivery> SystemPromptDeliveryPerAgent;

	/** Whether the first-launch onboarding wizard has been completed or skipped */
	UPROPERTY(config)
	bool bOnboardingCompleted = false;

	/** Whether the user has confirmed/selected a UI language. Independent of bOnboardingCompleted
	 *  so existing users (auto-completed via LastUsedAgentName) still see the language step once
	 *  on the first launch after this is shipped. */
	UPROPERTY(config)
	bool bLanguageOnboardingCompleted = false;

	// Convert settings to agent configs
	TArray<FACPAgentConfig> GetAgentConfigs() const;

	// Model selection persistence helpers
	FString GetSavedModelForAgent(const FString& AgentName) const;
	void SaveModelForAgent(const FString& AgentName, const FString& ModelId);

	// Mode selection persistence helpers
	FString GetSavedModeForAgent(const FString& AgentName) const;
	void SaveModeForAgent(const FString& AgentName, const FString& ModeId);

	// Reasoning selection persistence helpers
	FString GetSavedReasoningForAgent(const FString& AgentName) const;
	void SaveReasoningForAgent(const FString& AgentName, const FString& ReasoningLevel);

	// Direct provider auth/routing helpers
	bool HasOpenRouterAuth() const;
	bool HasMeshyAuth() const;
	bool HasFalAuth() const;
	FString GetOpenRouterAuthToken() const;
	FString GetMeshyAuthToken() const;
	FString GetFalAuthToken() const;
	FString GetOpenRouterChatCompletionsUrl() const;
	FString GetOpenRouterImageGenerationUrl() const;
	FString GetOpenRouterModelsUrl() const;
	FString GetMeshyBaseUrl() const;
	FString GetFalSubmitUrl() const;

	/** Returns the custom system prompt text (from UUserPreferencesSubsystem) that gets appended
	 *  to each agent's system message. Name kept for call-site compatibility. */
	FString GetProfileSystemPromptAppend() const;

	/** Returns the system prompt delivery method for a given agent (legacy overload — name only). */
	ESystemPromptDelivery GetSystemPromptDeliveryForAgent(const FString& AgentName) const;

	/** Returns the system prompt delivery method for a given agent.
	 *  Resolution order: user per-agent override (AgentName) → agent quirks override (RegistryId) → SessionMeta.
	 *  Prefer this overload when you have the full FACPAgentConfig — it honors hard per-agent
	 *  constraints encoded in quirks (e.g. Codex doesn't read _meta.systemPrompt). */
	ESystemPromptDelivery GetSystemPromptDeliveryForAgent(const FACPAgentConfig& Config) const;

	/** Ensures built-in defaults exist in SystemPromptDeliveryPerAgent so they're visible in the UI */
	void EnsureBuiltInSystemPromptDeliveryDefaults();

	// ────────────────────────────────────────────────────────────────
	// Chat provider accessors. Read/write the ChatProviders /
	// UserChatProviders / SelectedChatModelId fields above.
	// ────────────────────────────────────────────────────────────────

	/** Get the per-provider settings row, creating a default one if missing. */
	FChatProviderSettings& GetOrCreateChatProviderSettings(const FString& ProviderId);

	/** Read-only lookup; returns nullptr if the provider has no row. */
	const FChatProviderSettings* FindChatProviderSettings(const FString& ProviderId) const;

	/** Convenience: get the API key for a provider, or empty if not configured. */
	FString GetChatProviderApiKey(const FString& ProviderId) const;

	/** Convenience: get the base URL override for a provider, or empty if none. */
	FString GetChatProviderBaseUrlOverride(const FString& ProviderId) const;

	/** Convenience: whether a provider row exists, is enabled, and has valid auth if required. */
	bool IsChatProviderConfigured(const FString& ProviderId, bool bRequiresApiKey) const;

	/** Update an API key; creates the row if missing. Saves to disk. */
	void SetChatProviderApiKey(const FString& ProviderId, const FString& ApiKey);

	/** Update the base URL override; creates the row if missing. Saves to disk. */
	void SetChatProviderBaseUrlOverride(const FString& ProviderId, const FString& BaseUrl);

	/** Update the enabled flag; creates the row if missing. Saves to disk. */
	void SetChatProviderEnabled(const FString& ProviderId, bool bEnabled);

	/** User-defined chat provider CRUD. */
	FUserChatProvider* FindUserChatProvider(const FString& ProviderId);
	const FUserChatProvider* FindUserChatProvider(const FString& ProviderId) const;
	FString CreateUserChatProvider(const FString& DisplayName, const FString& BaseUrl);
	void    DeleteUserChatProvider(const FString& ProviderId);
	void    UpdateUserChatProvider(const FString& ProviderId, TFunctionRef<void(FUserChatProvider&)> Mutator);
	void    AddUserChatProviderModel(const FString& ProviderId, const FChatModelEntry& Entry);
	void    RemoveUserChatProviderModel(const FString& ProviderId, const FString& ModelId);

	// Unified preferences persistence — writes to ~/.agentintegrationkit/preferences.json
	// Bypasses UE's broken SaveConfig() which writes to the wrong INI file.
	void SavePreferences();
	void LoadPreferences();

	// Legacy wrappers (delegate to SavePreferences/LoadPreferences)
	void SaveInstalledAgentIds();
	void LoadInstalledAgentIds();

	// Agent status cache management
	void RefreshAgentStatus();
	void InvalidateAgentStatusCache();
	bool IsAgentStatusStale() const;

private:
	mutable TMap<FString, EACPAgentStatus> CachedAgentStatus;
	mutable FDateTime LastStatusRefresh;
	mutable FCriticalSection StatusCacheLock;
	static constexpr double StatusCacheTTLSeconds = 300.0;
};
