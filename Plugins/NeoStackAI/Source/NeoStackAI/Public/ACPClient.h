// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/SingleThreadRunnable.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnACPStateChanged, EACPClientState, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPSessionUpdate, const FACPSessionUpdate&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPResponse, const TSharedPtr<FJsonObject>&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnACPError, int32, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPModelsAvailable, const FACPSessionModelState&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPPermissionRequest, const FACPPermissionRequest&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPModesAvailable, const FACPSessionModeState&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPModeChanged, const FString&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPCommandsAvailable, const TArray<FACPSlashCommand>&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnACPAuthComplete, bool /*bSuccess*/, const FString& /*Error*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPSessionListReceived, const TArray<FACPRemoteSessionEntry>& /*Sessions*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnACPSessionInfoUpdated, const FString& /*SessionId*/, const FString& /*Title*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnACPPromptComplete, const FString& /*StopReason*/, const FACPUsageData& /*Usage*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnACPElicitationRequest, const FACPElicitationRequest&);

/**
 * ACP Client - handles communication with an ACP agent via JSON-RPC over stdio
 */
class NEOSTACKAI_API FACPClient : public FRunnable, public TSharedFromThis<FACPClient>
{
public:
	FACPClient();
	virtual ~FACPClient();

	// Connect to an agent by spawning its process
	bool Connect(const FACPAgentConfig& Config);

	// Disconnect from the agent
	void Disconnect();

	// Check connection status
	bool IsConnected() const { return State != EACPClientState::Disconnected && State != EACPClientState::Error; }
	EACPClientState GetState() const { return State; }

	// ACP Protocol Methods
	void Initialize();
	void NewSession(const FString& WorkingDirectory);
	void LoadSession(const FString& SessionId, const FString& WorkingDirectory);
	void ResumeSession(const FString& SessionId, const FString& WorkingDirectory);
	void SendPrompt(const FString& PromptText);
	void CancelPrompt();
	void CloseSession(const FString& SessionId);
	void DeleteSession(const FString& SessionId);
	void SetMode(const FString& Mode);
	void SetModel(const FString& ModelId);
	void SetConfigOption(const FString& ConfigId, const FString& Value);
	void Authenticate(const FString& MethodId);

	// Session listing (ACP session/list with pagination)
	void ListSessions(const FString& WorkingDirectory, const FString& Cursor = TEXT(""));

	// Session state access
	FString GetCurrentSessionId() const { return CurrentSessionId; }

	// Unreal session tracking (for multi-chat support)
	void SetUnrealSessionId(const FString& SessionId) { UnrealSessionId = SessionId; }
	FString GetUnrealSessionId() const { return UnrealSessionId; }

	// Respond to a permission request (with optional _meta for AskUserQuestion answers).
	// RequestId is the JSON-RPC id from the agent's request, preserved as a string —
	// the underlying JSON type (string vs number) is round-tripped via a stored JsonValue.
	void RespondToPermissionRequest(const FString& RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta = nullptr);

	// Respond to a permission request with outcome=cancelled. Used internally on session/cancel
	// to satisfy the spec MUST (docs/protocol/prompt-turn.mdx: "Client MUST respond to all
	// pending session/request_permission requests with the cancelled outcome").
	void RespondToPermissionCancelled(const FString& RequestId);

	// Respond to an elicitation request (ACP spec: elicitation/create).
	// See RespondToPermissionRequest for RequestId semantics.
	void RespondToElicitation(const FString& RequestId, const FString& Action, TSharedPtr<FJsonObject> Content = nullptr);

	// Delegates
	FOnACPStateChanged OnStateChanged;
	FOnACPSessionUpdate OnSessionUpdate;
	FOnACPResponse OnResponse;
	FOnACPError OnError;
	FOnACPModelsAvailable OnModelsAvailable;
	FOnACPPermissionRequest OnPermissionRequest;
	FOnACPModesAvailable OnModesAvailable;
	FOnACPModeChanged OnModeChanged;
	FOnACPCommandsAvailable OnCommandsAvailable;
	FOnACPAuthComplete OnAuthComplete;
	FOnACPSessionListReceived OnSessionListReceived;
	FOnACPSessionInfoUpdated OnSessionInfoUpdated;
	FOnACPPromptComplete OnPromptComplete;
	FOnACPElicitationRequest OnElicitationRequest;

	// Get capabilities
	const FACPAgentCapabilities& GetAgentCapabilities() const { return AgentCapabilities; }

	// Get available models for the current session
	const FACPSessionModelState& GetModelState() const { return SessionModelState; }

	// Get available modes for the current session
	const FACPSessionModeState& GetModeState() const { return SessionModeState; }

	// Get available slash commands for the current session
	const TArray<FACPSlashCommand>& GetAvailableCommands() const { return AvailableCommands; }

	// Usage tracking
	const FACPUsageData& GetSessionUsage() const { return SessionUsage; }
	void ResetSessionUsage() { SessionUsage = FACPUsageData(); }

	// Thinking token budget (0 = no thinking/default)
	// Sends session/set_config_option to the adapter when a session is active
	void SetMaxThinkingTokens(int32 Tokens);
	int32 GetMaxThinkingTokens() const { return MaxThinkingTokens; }

	// Whether this agent uses unified config options (session/set_config_option) vs old pattern
	bool UsesConfigOptions() const { return bUsesConfigOptions; }

	// Reasoning effort (from config_option_update, for agents like Codex)
	const FString& GetCurrentReasoningEffort() const { return CurrentReasoningEffort; }
	const TArray<FString>& GetAvailableReasoningEfforts() const { return AvailableReasoningEfforts; }
	bool HasReasoningEffortOptions() const { return AvailableReasoningEfforts.Num() > 0; }
	bool SupportsReasoningEffortControl() const { return HasReasoningEffortOptions() && !ReasoningConfigOptionId.IsEmpty(); }
	void SetReasoningEffort(const FString& Value);

	// Per-process MCP config temp files (set by ACPAgentManager, cleaned up on Disconnect)
	FString CopilotAdditionalMcpConfigPath;
	FString GeminiSystemSettingsPath;

protected:
	// FRunnable interface - for reading from agent stdout
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	// Send a JSON-RPC request
	FString SendRequest(const FString& Method, TSharedPtr<FJsonObject> Params = nullptr);

	// Send a JSON-RPC notification
	void SendNotification(const FString& Method, TSharedPtr<FJsonObject> Params = nullptr);

	// Send raw JSON message to agent stdin
	void SendRawMessage(const FString& JsonMessage);

	// Process a line of JSON from agent stdout
	void ProcessLine(const FString& Line);

	// Handle incoming messages
	void HandleResponse(FString Id, TSharedPtr<FJsonObject> Result);
	void HandleError(FString Id, int32 Code, const FString& Message);
	void HandleNotification(const FString& Method, TSharedPtr<FJsonObject> Params);
	void HandleServerRequest(FString Id, const FString& Method, TSharedPtr<FJsonObject> Params);

	// Process session/update notifications
	void ProcessSessionUpdate(TSharedPtr<FJsonObject> Params);

	// Refresh inactivity deadlines for in-flight session/prompt requests.
	void RefreshSessionPromptDeadlines();

	// Set state and broadcast
	void SetState(EACPClientState NewState, const FString& Message = TEXT(""));

private:
	// Process handles
	FProcHandle ProcessHandle;
	void* StdinWritePipe = nullptr;
	void* StdoutReadPipe = nullptr;
	void* StderrReadPipe = nullptr;

	// Reading threads
	FRunnableThread* ReadThread = nullptr;
	FRunnableThread* StderrThread = nullptr;
	void StderrReaderLoop();
	TAtomic<bool> bStopRequested;

	// State
	EACPClientState State = EACPClientState::Disconnected;
	FACPAgentConfig CurrentConfig;
	FACPAgentCapabilities AgentCapabilities;
	FACPClientCapabilities ClientCapabilities;

	// Request tracking — deadline lets us sweep hung requests so a stuck agent doesn't
	// permanently block queued user prompts. Sweep runs from the stdout reader heartbeat
	// (~5s tick). On expiry we synthesize a -32099 error response so the regular
	// HandleError path runs and the UI gets an actionable message.
	struct FPendingRequest
	{
		FString Method;
		double DeadlineSeconds = 0.0; // FPlatformTime::Seconds() basis; 0 = no timeout
	};
	TMap<FString, FPendingRequest> PendingRequests;

	// Server-initiated request IDs awaiting our response (permission, elicitation, etc.).
	// Keyed by stringified id; the stored JsonValue preserves the original wire type
	// (string vs number) so the response echoes back the same shape — JSON-RPC §4.2 requires
	// the response id be the same type as the request id. Without this, agents using
	// string ids (UUIDs in claude-agent-acp, codex-acp, gemini) silently drop our responses.
	TMap<FString, TSharedPtr<FJsonValue>> PendingServerRequestIdValues;

	// Subset of PendingServerRequestIdValues — IDs that came from session/request_permission
	// specifically. Tracked separately so CancelPrompt can sweep them with outcome=cancelled
	// without touching unrelated server requests (elicitation, fs/*, etc.). Spec MUST:
	// docs/protocol/prompt-turn.mdx — "Client MUST respond to all pending
	// session/request_permission requests with the cancelled outcome".
	TSet<FString> PendingPermissionRequestIds;

	// Current session
	FString CurrentSessionId;

	// Unreal session ID this client is currently serving (for multi-chat support)
	FString UnrealSessionId;

	// Model state
	FACPSessionModelState SessionModelState;

	// Mode state
	FACPSessionModeState SessionModeState;

	// Available slash commands
	TArray<FACPSlashCommand> AvailableCommands;

	// Usage tracking (cumulative for session)
	FACPUsageData SessionUsage;

	// Analytics: timestamp when the last prompt was sent (for response duration tracking)
	double AnalyticsPromptStartTime = 0.0;

	// Thinking token budget for ACP sessions (0 = no thinking)
	int32 MaxThinkingTokens = 0;

	// Whether agent uses config_option_update pattern (Codex) vs old mode/model pattern (Claude Code)
	bool bUsesConfigOptions = false;

	// System prompt delivery tracking
	bool bFirstPromptSent = false;

	// Reasoning effort config (from config_option_update with category "thought_level")
	FString CurrentReasoningEffort;
	TArray<FString> AvailableReasoningEfforts;
	FString ReasoningConfigOptionId;

	// Boolean config options (ACP spec: type "boolean", UNSTABLE)
	TMap<FString, bool> BooleanConfigOptions;

	// Session list pagination: accumulated sessions across pages
	TArray<FACPRemoteSessionEntry> PaginatedSessionAccumulator;
	FString PaginatedSessionCwd;
	FString LastPaginationCursor;

	// Thread safety
	FCriticalSection StateLock;
	FCriticalSection WriteLock;

	// Byte buffer for partial lines from stdout. UTF-8 safe: scanned for byte 0x0A (\n)
	// at the byte level so multi-byte sequences crossing read-call boundaries stay intact.
	// Was FString — that path goes through FPlatformProcess::ReadPipe(FString&) which
	// drops every byte ≥ 0x80 on POSIX/Windows (ANSI conversion). Net effect with the
	// old code: emoji, accented chars, CJK silently corrupted to '?'.
	TArray<uint8> ReadBuffer;

	// Future from Async(EAsyncExecution::Thread) that runs StderrReaderLoop. Disconnect
	// terminates the child process before joining this future so a blocked pipe read
	// cannot pin editor shutdown forever.
	TFuture<void> StderrReaderFuture;
};
