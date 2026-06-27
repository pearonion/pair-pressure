// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ACPTypes.generated.h"

// ============================================================================
// JSON-RPC Base Types (internal use only, not exposed to Blueprints)
// ============================================================================

/** Starting request ID — offset from 0 to avoid collision with system-reserved IDs on some providers */
constexpr int32 ACP_INITIAL_REQUEST_ID = 4254;

/** Lowest ACP protocol version this client can speak. Mirrors Zed's MINIMUM_SUPPORTED_VERSION.
 *  If an agent's initialize response reports a lower version we abort the session — talking
 *  v0 to a v1+ client would silently drop fields the agent doesn't know about. */
constexpr int32 ACP_MINIMUM_PROTOCOL_VERSION = 1;

/** Highest ACP protocol version this client claims to speak in initialize requests. */
constexpr int32 ACP_CLIENT_PROTOCOL_VERSION = 1;

struct FACPJsonRpcRequest
{
	FString JsonRpc = TEXT("2.0");
	int32 Id = ACP_INITIAL_REQUEST_ID;
	FString Method;
	TSharedPtr<FJsonObject> Params;
};

struct FACPJsonRpcResponse
{
	FString JsonRpc = TEXT("2.0");
	int32 Id = 0;
	TSharedPtr<FJsonObject> Result;
	TSharedPtr<FJsonObject> Error;
};

struct FACPJsonRpcNotification
{
	FString JsonRpc = TEXT("2.0");
	FString Method;
	TSharedPtr<FJsonObject> Params;
};

// ============================================================================
// ACP Content Block Types (for streaming UI)
// ============================================================================

UENUM(BlueprintType)
enum class EACPContentBlockType : uint8
{
	Text,
	Thought,
	ToolCall,
	ToolResult,
	Image,
	Error,
	System          // Inline status messages (compaction, etc.)
};

USTRUCT(BlueprintType)
struct FACPToolResultImage
{
	GENERATED_BODY()

	UPROPERTY()
	FString Base64Data;

	UPROPERTY()
	FString MimeType;

	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;
};

/** ACP spec tool_call.locations[] entry — file the agent is reading/editing.
 *  See docs/protocol/tool-calls.mdx: "Each location MUST have a path. Optional
 *  line is 1-based. Clients use these to render jump-to-file links and to
 *  drive a follow-the-agent cursor mode." Both tool_call and tool_call_update
 *  notifications can carry locations. */
USTRUCT(BlueprintType)
struct FACPToolCallLocation
{
	GENERATED_BODY()

	/** Absolute or workspace-relative file path. Required per spec. */
	UPROPERTY()
	FString Path;

	/** 1-based line number. INDEX_NONE (-1) when the agent did not supply one. */
	UPROPERTY()
	int32 Line = INDEX_NONE;
};

USTRUCT(BlueprintType)
struct FACPContentBlock
{
	GENERATED_BODY()

	UPROPERTY()
	EACPContentBlockType Type = EACPContentBlockType::Text;

	// For Text/Thought blocks
	UPROPERTY()
	FString Text;

	// For ToolCall blocks
	UPROPERTY()
	FString ToolCallId;

	UPROPERTY()
	FString ToolName;

	UPROPERTY()
	FString ToolArguments; // JSON string

	// Parent tool call ID — set when this tool call was made inside a subagent (Task)
	UPROPERTY()
	FString ParentToolCallId;

	// For ToolResult blocks
	UPROPERTY()
	FString ToolResultContent;

	UPROPERTY()
	bool bToolSuccess = true;

	// For ToolResult blocks with images (e.g., screenshot)
	UPROPERTY()
	TArray<FACPToolResultImage> ToolResultImages;

	// Timestamp for ordering
	UPROPERTY()
	FDateTime Timestamp;

	// Whether this block is still streaming (for text/thought)
	UPROPERTY()
	bool bIsStreaming = false;

	FACPContentBlock()
		: Timestamp(FDateTime::Now())
	{
	}
};

// ============================================================================
// Message Context (resolved @ mentions)
// ============================================================================

UENUM(BlueprintType)
enum class EACPContextType : uint8
{
	Blueprint,
	CppFile,
	Asset,
	Folder,
	Unknown
};

UENUM(BlueprintType)
enum class EACPContextStatus : uint8
{
	Resolved,
	NotFound,
	Truncated
};

USTRUCT(BlueprintType)
struct FACPMessageContext
{
	GENERATED_BODY()

	UPROPERTY()
	FString Path;

	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	EACPContextType Type = EACPContextType::Unknown;

	UPROPERTY()
	EACPContextStatus Status = EACPContextStatus::Resolved;

	UPROPERTY()
	int32 LineCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString ErrorMessage;
};

// ============================================================================
// Chat Message (contains multiple content blocks)
// ============================================================================

UENUM(BlueprintType)
enum class EACPMessageRole : uint8
{
	User,
	Assistant,
	System
};

USTRUCT(BlueprintType)
struct FACPChatMessage
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid MessageId;

	UPROPERTY()
	EACPMessageRole Role = EACPMessageRole::User;

	// Inline content blocks in arrival order
	UPROPERTY()
	TArray<FACPContentBlock> ContentBlocks;

	UPROPERTY()
	FDateTime Timestamp;

	// Whether this message is still receiving content
	UPROPERTY()
	bool bIsStreaming = false;

	// Resolved @ mention contexts (for user messages)
	UPROPERTY()
	TArray<FACPMessageContext> Contexts;

	// Full text sent to the provider when it differs from the display text
	// in ContentBlocks, e.g. WebUI @mention context prefixes.
	UPROPERTY()
	FString ProviderText;

	FACPChatMessage()
		: MessageId(FGuid::NewGuid())
		, Timestamp(FDateTime::Now())
	{
	}
};

// ============================================================================
// Plan/Todo Types (for task tracking)
// ============================================================================

UENUM(BlueprintType)
enum class EACPPlanEntryStatus : uint8
{
	Pending,
	InProgress,
	Completed
};

UENUM(BlueprintType)
enum class EACPPlanEntryPriority : uint8
{
	High,
	Medium,
	Low
};

USTRUCT(BlueprintType)
struct FACPPlanEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString Content;

	UPROPERTY()
	FString ActiveForm;

	UPROPERTY()
	EACPPlanEntryPriority Priority = EACPPlanEntryPriority::Medium;

	UPROPERTY()
	EACPPlanEntryStatus Status = EACPPlanEntryStatus::Pending;
};

USTRUCT(BlueprintType)
struct FACPPlan
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FACPPlanEntry> Entries;

	int32 GetCompletedCount() const
	{
		int32 Count = 0;
		for (const auto& Entry : Entries)
		{
			if (Entry.Status == EACPPlanEntryStatus::Completed)
			{
				Count++;
			}
		}
		return Count;
	}

	int32 GetInProgressIndex() const
	{
		for (int32 i = 0; i < Entries.Num(); i++)
		{
			if (Entries[i].Status == EACPPlanEntryStatus::InProgress)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	float GetProgress() const
	{
		if (Entries.Num() == 0) return 0.0f;
		return (float)GetCompletedCount() / (float)Entries.Num();
	}
};

// ============================================================================
// Slash Commands (available from agent)
// ============================================================================

USTRUCT(BlueprintType)
struct FACPSlashCommand
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString InputHint;

	bool HasInput() const { return !InputHint.IsEmpty(); }
};

// ============================================================================
// ACP Session Update Types (from agent)
// ============================================================================

UENUM(BlueprintType)
enum class EACPUpdateType : uint8
{
	AgentMessageChunk,    // Text response chunk
	AgentThoughtChunk,    // Reasoning/thinking chunk
	ToolCall,             // Tool invocation announcement
	ToolCallUpdate,       // Tool execution result
	Plan,                 // Multi-step plan outline
	Error,                // Error notification
	UsageUpdate,          // Token/cost usage update
	UserMessageChunk,     // User message replay during session/load
	HistoryReplayStarted, // Control event: session/load replay is rebuilding history
	HistoryReplayFinished // Control event: session/load replay completed
};

// ============================================================================
// Usage and Cost Tracking
// ============================================================================

/** Per-model usage breakdown (from SDK modelUsage) */
USTRUCT()
struct FModelUsageEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString ModelName;

	UPROPERTY()
	int32 InputTokens = 0;

	UPROPERTY()
	int32 OutputTokens = 0;

	UPROPERTY()
	int32 CacheReadTokens = 0;

	UPROPERTY()
	int32 CacheCreationTokens = 0;

	UPROPERTY()
	double CostUSD = 0.0;

	UPROPERTY()
	int32 ContextWindow = 0;

	UPROPERTY()
	int32 MaxOutputTokens = 0;

	UPROPERTY()
	int32 WebSearchRequests = 0;
};

USTRUCT(BlueprintType)
struct FACPUsageData
{
	GENERATED_BODY()

	// Token counts
	UPROPERTY()
	int32 TotalTokens = 0;

	UPROPERTY()
	int32 InputTokens = 0;

	UPROPERTY()
	int32 OutputTokens = 0;

	/** Legacy combined cached tokens (read + creation) */
	UPROPERTY()
	int32 CachedTokens = 0;

	/** Tokens read from prompt cache */
	UPROPERTY()
	int32 CacheReadTokens = 0;

	/** Tokens written to prompt cache */
	UPROPERTY()
	int32 CacheCreationTokens = 0;

	UPROPERTY()
	int32 ReasoningTokens = 0;

	// Cost information (cumulative session total)
	UPROPERTY()
	double CostAmount = 0.0;

	UPROPERTY()
	FString CostCurrency = TEXT("USD");

	/** Cost for just the last prompt turn */
	UPROPERTY()
	double TurnCostUSD = 0.0;

	// Context window (for ACP agents)
	UPROPERTY()
	int32 ContextUsed = 0;

	UPROPERTY()
	int32 ContextSize = 0;

	/** Number of agentic turns in last prompt */
	UPROPERTY()
	int32 NumTurns = 0;

	/** Duration of last prompt in milliseconds */
	UPROPERTY()
	int32 DurationMs = 0;

	/** Per-model usage breakdown */
	UPROPERTY()
	TArray<FModelUsageEntry> ModelUsage;

	bool HasCost() const { return CostAmount > 0.0; }
	bool HasTokens() const { return TotalTokens > 0 || InputTokens > 0 || OutputTokens > 0; }
	bool HasContext() const { return ContextSize > 0; }
	float GetContextPercent() const { return ContextSize > 0 ? FMath::Clamp((float)ContextUsed / (float)ContextSize, 0.f, 1.f) : 0.f; }

	FString GetFormattedCost() const
	{
		if (CostAmount <= 0.0)
		{
			return FString();
		}
		if (CostCurrency == TEXT("USD"))
		{
			return FString::Printf(TEXT("$%.4f"), CostAmount);
		}
		return FString::Printf(TEXT("%.4f %s"), CostAmount, *CostCurrency);
	}

	FString GetFormattedTokens(int32 Count) const
	{
		if (Count >= 1000000) return FString::Printf(TEXT("%.1fM"), Count / 1000000.0);
		if (Count >= 1000) return FString::Printf(TEXT("%.1fK"), Count / 1000.0);
		return FString::Printf(TEXT("%d"), Count);
	}
};

// ============================================================================
// Agent Rate Limit / Usage Quota Tracking
// ============================================================================

USTRUCT()
struct FAgentRateLimitWindow
{
	GENERATED_BODY()

	/** Usage percentage (0-100) within this window */
	UPROPERTY()
	double UsedPercent = 0.0;

	/** When this rate limit window resets */
	UPROPERTY()
	FDateTime ResetsAt;

	/** Window duration in minutes (e.g. 300 for 5h, 10080 for 7d) */
	UPROPERTY()
	int32 WindowDurationMinutes = 0;

	bool HasData() const { return WindowDurationMinutes > 0 || UsedPercent > 0.0; }
};

USTRUCT()
struct FAgentExtraUsage
{
	GENERATED_BODY()

	/** Whether Claude Extra / overage spending is enabled */
	UPROPERTY()
	bool bIsEnabled = false;

	/** Amount used this period (major currency units, e.g. dollars) */
	UPROPERTY()
	double UsedAmount = 0.0;

	/** Spending limit for this period (major currency units) */
	UPROPERTY()
	double LimitAmount = 0.0;

	/** Currency code (e.g. "USD", "EUR") */
	UPROPERTY()
	FString CurrencyCode = TEXT("USD");

	bool HasData() const { return bIsEnabled && LimitAmount > 0.0; }
};

USTRUCT()
struct FAgentRateLimitData
{
	GENERATED_BODY()

	/** Whether we have successfully fetched data at least once */
	UPROPERTY()
	bool bHasData = false;

	/** Whether a fetch is currently in-flight */
	UPROPERTY()
	bool bIsLoading = false;

	/** Error message from last fetch attempt (empty = no error) */
	UPROPERTY()
	FString ErrorMessage;

	/** Primary rate limit window (session/5-hour for Claude, primary for Codex) */
	UPROPERTY()
	FAgentRateLimitWindow Primary;

	/** Secondary rate limit window (weekly/7-day all models) */
	UPROPERTY()
	FAgentRateLimitWindow Secondary;

	/** Model-specific weekly window (e.g. seven_day_opus or seven_day_sonnet) */
	UPROPERTY()
	FAgentRateLimitWindow ModelSpecific;

	/** Label for the model-specific window (e.g. "Opus", "Sonnet") */
	UPROPERTY()
	FString ModelSpecificLabel;

	/** Extra usage / cost tracking (Claude Extra) */
	UPROPERTY()
	FAgentExtraUsage ExtraUsage;

	/** Plan type string (e.g. "Pro", "Max", "Team") */
	UPROPERTY()
	FString PlanType;

	/** Agent name this data belongs to (e.g. "Claude Code", "Codex") */
	UPROPERTY()
	FString AgentName;

	/** When this data was last successfully fetched */
	UPROPERTY()
	FDateTime LastUpdated;
};

USTRUCT(BlueprintType)
struct FACPSessionUpdate
{
	GENERATED_BODY()

	// ACP session ID from params.sessionId (or id for some legacy formats)
	// Used by manager-side routing when one agent serves multiple sessions.
	UPROPERTY()
	FString SessionId;

	UPROPERTY()
	EACPUpdateType UpdateType = EACPUpdateType::AgentMessageChunk;

	// For message/thought chunks
	UPROPERTY()
	FString TextChunk;

	// For system status messages (compaction, etc.)
	UPROPERTY()
	bool bIsSystemStatus = false;

	UPROPERTY()
	FString SystemStatus; // "compacting", "compacted", etc.

	// For tool calls
	UPROPERTY()
	FString ToolCallId;

	UPROPERTY()
	FString ToolName;

	UPROPERTY()
	FString ToolArguments;

	// Parent tool call ID — set when this tool call was made inside a subagent (Task)
	UPROPERTY()
	FString ParentToolCallId;

	// ACP spec: tool call kind — "read"|"edit"|"delete"|"move"|"search"|"execute"|"think"|"fetch"|"switch_mode"|"other"
	UPROPERTY()
	FString ToolCallKind;

	// ACP spec: tool call status — "pending"|"in_progress"|"completed"|"failed"
	UPROPERTY()
	FString ToolCallStatus;

	// For tool results
	UPROPERTY()
	FString ToolResult;

	UPROPERTY()
	bool bToolSuccess = true;

	// For tool results with images (e.g., screenshot)
	UPROPERTY()
	TArray<FACPToolResultImage> ToolResultImages;

	// ACP spec: diff content from tool_call_update (type: "diff")
	UPROPERTY()
	bool bHasDiff = false;

	UPROPERTY()
	FString DiffPath;

	UPROPERTY()
	FString DiffOldText;

	UPROPERTY()
	FString DiffNewText;

	// ACP spec: tool_call.locations[] — file/line pointers for "what the agent is touching".
	// Populated for both initial tool_call notifications and tool_call_update follow-ups.
	// Empty (Num()==0) when the agent did not provide locations. Lets the UI render
	// clickable file:line jump targets and (eventually) drive a follow-the-agent cursor.
	UPROPERTY()
	TArray<FACPToolCallLocation> Locations;

	// For errors
	UPROPERTY()
	int32 ErrorCode = 0;

	UPROPERTY()
	FString ErrorMessage;

	// For plan updates
	UPROPERTY()
	FACPPlan Plan;

	// For usage updates
	UPROPERTY()
	FACPUsageData Usage;

	// For history replay lifecycle controls
	UPROPERTY()
	int32 ReplayMessageCount = 0;

	UPROPERTY()
	bool bReplayEmpty = false;

	UPROPERTY()
	bool bReplayPreserveCached = false;

	// ACP spec: stop reason from session/prompt response
	// "end_turn"|"max_tokens"|"max_turn_requests"|"refusal"|"cancelled"
	UPROPERTY()
	FString StopReason;
};

// ============================================================================
// System Prompt Delivery Method
// ============================================================================

UENUM(BlueprintType)
enum class ESystemPromptDelivery : uint8
{
	/** Send via _meta.systemPrompt in session/new (default, works with Claude Code) */
	SessionMeta        UMETA(DisplayName = "Session Meta (_meta.systemPrompt)"),

	/** Prepend to the first user message of each session */
	FirstUserMessage   UMETA(DisplayName = "First User Message"),

	/** Prepend to every user message */
	EveryUserMessage   UMETA(DisplayName = "Every User Message"),
};

// ============================================================================
// Agent Configuration
// ============================================================================

UENUM(BlueprintType)
enum class EACPAgentStatus : uint8
{
	Available,           // Executable found and ready
	NotInstalled,        // Executable not found in PATH or at path
	MissingApiKey,       // Agent requires API key but none provided
	Unknown              // Status not yet checked
};

USTRUCT(BlueprintType)
struct FACPAgentConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString AgentName;

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString ExecutablePath;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TArray<FString> Arguments;

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString WorkingDirectory;

	UPROPERTY(EditAnywhere, Category = "Agent")
	TMap<FString, FString> EnvironmentVariables;

	// For agents that resume an existing session at process launch instead of via ACP session/load.
	UPROPERTY()
	FString LaunchResumeSessionId;

	// For built-in chat gateway provider authentication
	UPROPERTY(EditAnywhere, Category = "Agent")
	FString ApiKey;

	UPROPERTY(EditAnywhere, Category = "Agent")
	FString ModelId;

	UPROPERTY()
	bool bIsBuiltIn = false;

	/** ACP Registry ID (e.g., "claude-acp", "gemini"). Empty for custom/legacy agents. */
	UPROPERTY()
	FString RegistryId;

	/**
	 * For ChatGateway agents only. When set, the model picker only shows models
	 * from this IChatProvider (matched by IChatProvider::GetId()). Empty (the
	 * Local & BYOK Chat default) means show every chat provider's models.
	 */
	UPROPERTY()
	FString GatewayProviderId;

	// Availability status (set by CheckAgentAvailability)
	UPROPERTY()
	EACPAgentStatus Status = EACPAgentStatus::Unknown;

	// Human-readable status message
	UPROPERTY()
	FString StatusMessage;

	// Instructions for installing this agent
	UPROPERTY()
	FString InstallInstructions;

	/**
	 * Optional icon override. When non-empty, takes precedence over the
	 * registry-supplied icon. Used for synthetic/built-in agents that don't
	 * have a remote registry entry (e.g. the "NeoStack Cloud" gateway agent).
	 * Should be a black-on-transparent SVG — the WebUI applies `invert opacity-70`
	 * to render it as neutral grey alongside the other brand glyphs.
	 */
	UPROPERTY()
	FString IconUrl;
};

// ============================================================================
// Agent Identity Helpers
//
// Background: the public agent registry (cdn.agentclientprotocol.com) returns
// display names "Claude Agent" / "Codex CLI", while older NeoStack hardcoded
// configs used "Claude Code" / "Codex". RegistryId is the stable identifier
// but is empty for legacy/custom configs. These helpers normalize the matching.
// ============================================================================

namespace ACPAgentIdentity
{
	/** True if the AgentName matches any Claude display-name variant. */
	inline bool IsClaudeName(const FString& AgentName)
	{
		return AgentName.Equals(TEXT("Claude Agent"), ESearchCase::IgnoreCase)
			|| AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase);
	}

	/** True if the AgentName matches any Codex display-name variant. */
	inline bool IsCodexName(const FString& AgentName)
	{
		return AgentName.Equals(TEXT("Codex CLI"), ESearchCase::IgnoreCase)
			|| AgentName.Equals(TEXT("Codex"), ESearchCase::IgnoreCase);
	}

	/** True if the agent is Claude — prefer stable RegistryId, fall back to display name. */
	inline bool IsClaude(const FACPAgentConfig& Config)
	{
		return Config.RegistryId == TEXT("claude-acp") || IsClaudeName(Config.AgentName);
	}

	/** True if the agent is Codex — prefer stable RegistryId, fall back to display name. */
	inline bool IsCodex(const FACPAgentConfig& Config)
	{
		return Config.RegistryId == TEXT("codex-acp") || IsCodexName(Config.AgentName);
	}
}

// ============================================================================
// ACP Client State
// ============================================================================

UENUM(BlueprintType)
enum class EACPClientState : uint8
{
	Disconnected,
	Connecting,
	Initializing,
	Ready,
	InSession,
	Prompting,
	Error
};

// ============================================================================
// ACP Capabilities
// ============================================================================

USTRUCT(BlueprintType)
struct FACPClientCapabilities
{
	GENERATED_BODY()

	/** ACP spec: capabilities.fs.readTextFile / writeTextFile */
	UPROPERTY()
	bool bSupportsFileSystem = true;

	/** ACP spec: capabilities.terminal (boolean) */
	UPROPERTY()
	bool bSupportsTerminal = false;

	/** ACP spec: capabilities.auth.terminal — client can spawn terminal-based auth flows */
	UPROPERTY()
	bool bSupportsAuthTerminal = true;
};

USTRUCT()
struct FACPAuthMethod
{
	GENERATED_BODY()

	UPROPERTY()
	FString Id;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	/** ACP spec: auth method type — "agent" (default), "terminal", or "env_var" */
	UPROPERTY()
	FString Type;

	/** If true, the client must spawn this command externally instead of calling agent/authenticate */
	UPROPERTY()
	bool bIsTerminalAuth = false;

	UPROPERTY()
	FString TerminalAuthCommand;

	UPROPERTY()
	TArray<FString> TerminalAuthArgs;

	UPROPERTY()
	FString TerminalAuthLabel;
};

USTRUCT(BlueprintType)
struct FACPAgentCapabilities
{
	GENERATED_BODY()

	UPROPERTY()
	bool bSupportsNewSession = true;

	UPROPERTY()
	bool bSupportsLoadSession = false;

	UPROPERTY()
	bool bSupportsResumeSession = false;

	UPROPERTY()
	bool bSupportsListSessions = false;

	UPROPERTY()
	bool bSupportsCloseSession = false;

	UPROPERTY()
	bool bSupportsDeleteSession = false;

	UPROPERTY()
	bool bSupportsAudio = false;

	UPROPERTY()
	bool bSupportsImage = false;

	/** ACP spec: agentCapabilities.mcpCapabilities.http — agent can connect to HTTP MCP servers.
	 *  When false, including HTTP entries in session/new mcpServers gets rejected with -32602.
	 *  Default false matches spec ("MCP HTTP transport is opt-in"). */
	UPROPERTY()
	bool bSupportsHttpMcp = false;

	/** ACP spec: agentCapabilities.mcpCapabilities.sse — agent can connect to SSE MCP servers. */
	UPROPERTY()
	bool bSupportsSseMcp = false;

	UPROPERTY()
	TArray<FString> SupportedModels;

	UPROPERTY()
	TArray<FACPAuthMethod> AuthMethods;
};

// ============================================================================
// Model Selection Types
// ============================================================================

USTRUCT(BlueprintType)
struct FACPModelInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString ModelId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;

	/** Provider this model came from (e.g. "openrouter", "openai"). Empty = unknown/default. */
	UPROPERTY()
	FString ProviderId;

	/** Display name of the provider (e.g. "OpenRouter", "OpenAI"). */
	UPROPERTY()
	FString ProviderDisplayName;

	/** All provider IDs that can serve this model (for priority-based routing). */
	UPROPERTY()
	TArray<FString> ServableByProviders;

	// Supported parameters (from OpenRouter API)
	UPROPERTY()
	TArray<FString> SupportedParameters;

	// Helper to check if model supports reasoning
	bool SupportsReasoning() const
	{
		return SupportedParameters.Contains(TEXT("reasoning")) ||
		       SupportedParameters.Contains(TEXT("include_reasoning"));
	}
};

USTRUCT(BlueprintType)
struct FACPSessionModelState
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FACPModelInfo> AvailableModels;

	UPROPERTY()
	FString CurrentModelId;
};

// ============================================================================
// AskUserQuestion Types (structured Q&A from agent)
// ============================================================================

USTRUCT()
struct FACPQuestionOption
{
	GENERATED_BODY()

	UPROPERTY()
	FString Label;

	UPROPERTY()
	FString Description;
};

USTRUCT()
struct FACPQuestion
{
	GENERATED_BODY()

	UPROPERTY()
	FString Question;

	UPROPERTY()
	FString Header;

	UPROPERTY()
	TArray<FACPQuestionOption> Options;

	UPROPERTY()
	bool bMultiSelect = false;
};

// ============================================================================
// Permission Request Types
// ============================================================================

USTRUCT(BlueprintType)
struct FACPPermissionOption
{
	GENERATED_BODY()

	UPROPERTY()
	FString OptionId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Kind; // "allow_always", "allow_once", "reject_once"
};

USTRUCT(BlueprintType)
struct FACPPermissionToolCall
{
	GENERATED_BODY()

	UPROPERTY()
	FString ToolCallId;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FString RawInput; // JSON string of the tool arguments
};

USTRUCT(BlueprintType)
struct FACPPermissionRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString RequestId;

	UPROPERTY()
	FString SessionId;

	UPROPERTY()
	TArray<FACPPermissionOption> Options;

	UPROPERTY()
	FACPPermissionToolCall ToolCall;

	// AskUserQuestion support
	UPROPERTY()
	bool bIsAskUserQuestion = false;

	UPROPERTY()
	TArray<FACPQuestion> Questions;
};

// ============================================================================
// Session Mode Types
// ============================================================================

USTRUCT(BlueprintType)
struct FACPSessionMode
{
	GENERATED_BODY()

	UPROPERTY()
	FString ModeId;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Description;
};

USTRUCT(BlueprintType)
struct FACPSessionModeState
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FACPSessionMode> AvailableModes;

	UPROPERTY()
	FString CurrentModeId;
};

// ============================================================================
// Remote Session Entry (from ACP unstable_listSessions)
// ============================================================================

/** Session entry returned by the agent's unstable_listSessions API */
struct FACPRemoteSessionEntry
{
	FString SessionId;
	FString Title;
	FString Cwd;
	FDateTime UpdatedAt;
};

// ============================================================================
// Context Attachment Types
// ============================================================================

UENUM(BlueprintType)
enum class EACPAttachmentType : uint8
{
	BlueprintNode,    // Single node from a Blueprint graph
	Blueprint,        // Full blueprint structure overview
	ImageAsset,       // Texture2D or image file
	FileAsset,        // Generic file attachment (pdf, txt, etc.)
	Actor,            // Actor from the level (selected in Details panel)
	GenericObject     // Any UObject (Material, Texture, DataAsset, etc.)
};

USTRUCT(BlueprintType)
struct FACPNodeAttachment
{
	GENERATED_BODY()

	// The blueprint containing this node
	UPROPERTY()
	FString BlueprintPath;

	// Graph name within the blueprint
	UPROPERTY()
	FString GraphName;

	// Node GUID for precise identification
	UPROPERTY()
	FGuid NodeGuid;

	// Human-readable node title for display
	UPROPERTY()
	FString NodeTitle;

	// Serialized node context (full detail with pins, connections)
	UPROPERTY()
	FString SerializedContext;
};

USTRUCT(BlueprintType)
struct FACPBlueprintAttachment
{
	GENERATED_BODY()

	// Asset path
	UPROPERTY()
	FString AssetPath;

	// Display name
	UPROPERTY()
	FString DisplayName;

	// Serialized structure overview
	UPROPERTY()
	FString SerializedContext;
};

USTRUCT(BlueprintType)
struct FACPImageAttachment
{
	GENERATED_BODY()

	// Asset path or file path
	UPROPERTY()
	FString Path;

	// Display name
	UPROPERTY()
	FString DisplayName;

	// Base64 encoded image data
	UPROPERTY()
	FString ImageBase64;

	// MIME type (e.g., "image/png")
	UPROPERTY()
	FString MimeType;

	// Dimensions for display
	UPROPERTY()
	int32 Width = 0;

	UPROPERTY()
	int32 Height = 0;
};

USTRUCT(BlueprintType)
struct FACPFileAttachment
{
	GENERATED_BODY()

	// Absolute source file path
	UPROPERTY()
	FString Path;

	// Human-readable display name (typically filename)
	UPROPERTY()
	FString DisplayName;

	// MIME type (e.g., "application/pdf")
	UPROPERTY()
	FString MimeType;

	// File size in bytes
	UPROPERTY()
	int64 SizeBytes = 0;

	// Optional extracted text for text-based formats (truncated)
	UPROPERTY()
	FString ExtractedText;

	// Whether ExtractedText contains useful content
	UPROPERTY()
	bool bHasExtractedText = false;
};

USTRUCT(BlueprintType)
struct FACPActorAttachment
{
	GENERATED_BODY()

	// Actor label in the level
	UPROPERTY()
	FString ActorLabel;

	// Actor class name (e.g., "StaticMeshActor", "BP_Enemy_C")
	UPROPERTY()
	FString ClassName;

	// Full asset path if this is a Blueprint-spawned actor
	UPROPERTY()
	FString BlueprintPath;

	// Serialized actor context (class, transform, components, properties)
	UPROPERTY()
	FString SerializedContext;
};

USTRUCT(BlueprintType)
struct FACPGenericObjectAttachment
{
	GENERATED_BODY()

	// Object display name
	UPROPERTY()
	FString DisplayName;

	// Object class name (e.g., "Material", "Texture2D", "DataTable")
	UPROPERTY()
	FString ClassName;

	// Asset path (for assets that live on disk)
	UPROPERTY()
	FString AssetPath;

	// Serialized object context (class, properties)
	UPROPERTY()
	FString SerializedContext;
};

USTRUCT(BlueprintType)
struct FACPContextAttachment
{
	GENERATED_BODY()

	UPROPERTY()
	EACPAttachmentType Type = EACPAttachmentType::BlueprintNode;

	// Unique ID for this attachment (for removal)
	UPROPERTY()
	FGuid AttachmentId;

	// Node attachment data (used when Type == BlueprintNode)
	UPROPERTY()
	FACPNodeAttachment NodeAttachment;

	// Blueprint attachment data (used when Type == Blueprint)
	UPROPERTY()
	FACPBlueprintAttachment BlueprintAttachment;

	// Image attachment data (used when Type == ImageAsset)
	UPROPERTY()
	FACPImageAttachment ImageAttachment;

	// File attachment data (used when Type == FileAsset)
	UPROPERTY()
	FACPFileAttachment FileAttachment;

	// Actor attachment data (used when Type == Actor)
	UPROPERTY()
	FACPActorAttachment ActorAttachment;

	// Generic object attachment data (used when Type == GenericObject)
	UPROPERTY()
	FACPGenericObjectAttachment GenericObjectAttachment;

	// Timestamp when attached
	UPROPERTY()
	FDateTime AttachedAt;

	FACPContextAttachment()
		: AttachmentId(FGuid::NewGuid())
		, AttachedAt(FDateTime::Now())
	{
	}
};

// ============================================================================
// Elicitation Types (ACP spec: session/elicitation server request)
// ============================================================================

USTRUCT()
struct FACPElicitationRequest
{
	GENERATED_BODY()

	/** JSON-RPC request ID (for responding) */
	UPROPERTY()
	FString RequestId;

	UPROPERTY()
	FString SessionId;

	/** "form" or "url" */
	UPROPERTY()
	FString Mode;

	/** Human-readable message/question from the agent */
	UPROPERTY()
	FString Message;

	// For mode == "form": JSON schema string describing expected input
	UPROPERTY()
	FString RequestedSchema;

	// For mode == "url": URL to open and elicitation tracking ID
	UPROPERTY()
	FString Url;

	UPROPERTY()
	FString ElicitationId;
};
