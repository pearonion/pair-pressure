// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "ACPClient.h"
#include "Containers/Ticker.h"

// Session-aware delegates: (SessionId, AgentName, ...)
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentMessageReceived, const FString& /*SessionId*/, const FString& /*AgentName*/, const FACPSessionUpdate&);
DECLARE_MULTICAST_DELEGATE_FourParams(FOnAgentStateChangedWithName, const FString& /*SessionId*/, const FString& /*AgentName*/, EACPClientState, const FString&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentModelsAvailable, const FString& /*SessionId*/, const FString& /*AgentName*/, const FACPSessionModelState&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPermissionRequest, const FString& /*SessionId*/, const FString& /*AgentName*/, const FACPPermissionRequest&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentModesAvailable, const FString& /*SessionId*/, const FString& /*AgentName*/, const FACPSessionModeState&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentModeChanged, const FString& /*SessionId*/, const FString& /*AgentName*/, const FString& /*ModeId*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FOnAgentError, const FString& /*SessionId*/, const FString& /*AgentName*/, int32, const FString&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPlanUpdate, const FString& /*SessionId*/, const FString& /*AgentName*/, const FACPPlan&);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentCommandsAvailable, const FString& /*SessionId*/, const FString& /*AgentName*/, const TArray<FACPSlashCommand>&);
DECLARE_MULTICAST_DELEGATE_FourParams(FOnAgentAuthComplete, const FString& /*SessionId*/, const FString& /*AgentName*/, bool /*bSuccess*/, const FString& /*Error*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAgentSessionListReceived, const FString& /*AgentName*/, const TArray<FACPRemoteSessionEntry>& /*Sessions*/);

/**
 * Session context for tracking parallel conversations
 */
struct FAgentSessionContext
{
	FString SessionId;
	FString AgentName;
	bool bIsStreaming = false;
};

/**
 * A live ACP runtime bound to exactly one NeoStack session.
 */
struct FACPManagedSessionClient
{
	FString SessionId;
	FString AgentName;
	TSharedPtr<FACPClient> Client;
	bool bSessionStarted = false;
};

/**
 * Manages multiple ACP agent connections
 * Singleton accessible throughout the editor
 */
class NEOSTACKAI_API FACPAgentManager
{
public:
	static FACPAgentManager& Get();

	// Agent configuration management
	void AddAgentConfig(const FACPAgentConfig& Config);
	void RemoveAgentConfig(const FString& AgentName);
	TArray<FACPAgentConfig> GetAllAgentConfigs() const;
	FACPAgentConfig* GetAgentConfig(const FString& AgentName);

	// Get list of available agent names
	TArray<FString> GetAvailableAgentNames() const;

	// Connect to a specific agent
	bool ConnectToAgent(const FString& AgentName);
	void DisconnectFromAgent(const FString& AgentName);
	void DisconnectAll();

	// Get active client for an agent
	TSharedPtr<FACPClient> GetClient(const FString& AgentName);
	TSharedPtr<FACPClient> GetClientForSession(const FString& SessionId);

	// Get all active ACP clients
	const TMap<FString, TSharedPtr<FACPClient>>& GetActiveClients() const { return ActiveClients; }

	// Check if connected to agent
	bool IsConnectedToAgent(const FString& AgentName) const;

	// Send prompt to an agent (legacy - uses default session)
	void SendPromptToAgent(const FString& AgentName, const FString& PromptText);

	// Session-aware prompt sending
	void SendPromptToSession(const FString& SessionId, const FString& AgentName, const FString& PromptText);

	// Register/unregister session for an agent
	void RegisterSession(const FString& SessionId, const FString& AgentName);
	void UnregisterSession(const FString& SessionId);
	FString GetSessionAgent(const FString& SessionId) const;
	FString GetActiveSessionForAgent(const FString& AgentName) const;

	// Cancel prompt for a specific session
	void CancelSessionPrompt(const FString& SessionId);

	// Start a new conversation for a session
	void StartSessionConversation(const FString& SessionId, const FString& AgentName);
	TSharedPtr<FACPClient> EnsureSessionClient(const FString& SessionId, const FString& AgentName);
	bool StartConversationOnSessionClient(const FString& SessionId, const FString& WorkingDirectory, bool bForceNewSession);

	// Model selection
	FACPSessionModelState GetAgentModelState(const FString& AgentName) const;
	void SetAgentModel(const FString& AgentName, const FString& ModelId);
	TArray<FACPModelInfo> GetAgentFullModelList(const FString& AgentName) const;
	void AddAgentRecentModel(const FString& AgentName, const FACPModelInfo& Model);
	void RefreshAgentModels(const FString& AgentName);

	// Mode selection
	FACPSessionModeState GetAgentModeState(const FString& AgentName) const;
	void SetAgentMode(const FString& AgentName, const FString& ModeId);

	// Cancel current operation
	void CancelAgentPrompt(const FString& AgentName);

	// Start a new session (resets conversation context)
	void StartNewSession(const FString& AgentName);

	// Delegates
	FOnAgentMessageReceived OnAgentMessage;
	FOnAgentStateChangedWithName OnAgentStateChanged;
	FOnAgentModelsAvailable OnAgentModelsAvailable;
	FOnAgentPermissionRequest OnAgentPermissionRequest;
	FOnAgentModesAvailable OnAgentModesAvailable;
	FOnAgentModeChanged OnAgentModeChanged;
	FOnAgentError OnAgentError;
	FOnAgentPlanUpdate OnAgentPlanUpdate;
	FOnAgentCommandsAvailable OnAgentCommandsAvailable;
	FOnAgentAuthComplete OnAgentAuthComplete;

	// Authentication
	TArray<FACPAuthMethod> GetAuthMethods(const FString& AgentName) const;
	void AuthenticateAgent(const FString& AgentName, const FString& MethodId);

	// Respond to a permission request (with optional _meta for AskUserQuestion answers).
	// RequestId is the JSON-RPC id from the agent's request, as a string — see FACPClient
	// for the wire-type preservation contract.
	void RespondToPermissionRequest(const FString& AgentName, const FString& RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta = nullptr);
	void RespondToPermissionRequestForSession(const FString& SessionId, const FString& AgentName, const FString& RequestId, const FString& OptionId, TSharedPtr<FJsonObject> OutcomeMeta = nullptr);

	// Load/Save configurations
	void LoadConfigFromSettings();
	void SaveConfigToSettings();

	// Initialize with default agents
	void InitializeDefaultAgents();

	/** Queue a NewSession call for when the agent becomes Ready.
	 *  Used by WebUI when CreateSession is called before the agent finishes connecting. */
	void QueuePendingNewSession(const FString& AgentName, const FString& SessionId);

	/** For agents that resume at process launch (Gemini/Copilot), apply this to the next Connect(). */
	void SetLaunchResumeSession(const FString& AgentName, const FString& SessionId);

	// Check if agent uses the Chat Gateway client (not ACP subprocess)
	bool IsChatGatewayAgent(const FString& AgentName) const;

	UE_DEPRECATED(5.7, "Use IsChatGatewayAgent instead")
	bool IsOpenRouterAgent(const FString& AgentName) const { return IsChatGatewayAgent(AgentName); }

	// Session listing via ACP protocol
	void RequestSessionList(const FString& AgentName);
	TArray<FACPRemoteSessionEntry> GetCachedSessionList(const FString& AgentName) const;
	FOnAgentSessionListReceived OnAgentSessionListReceived;

	// Delete a session via ACP session/delete (if supported by agent)
	void DeleteRemoteSession(const FString& AgentName, const FString& AgentSessionId);

	// Whether an agent supports ACP session/delete
	bool AgentSupportsDeleteSession(const FString& AgentName) const;

private:
	FACPAgentManager();
	~FACPAgentManager();

	// Prevent copying
	FACPAgentManager(const FACPAgentManager&) = delete;
	FACPAgentManager& operator=(const FACPAgentManager&) = delete;

	// Handle session updates from clients (routes to correct session)
	void OnClientSessionUpdate(const FString& AgentName, const FACPSessionUpdate& Update);
	void OnSessionClientSessionUpdate(const FString& SessionId, const FString& AgentName, const FACPSessionUpdate& Update);
	void OnClientStateChanged(const FString& AgentName, EACPClientState State, const FString& Message);
	void OnSessionClientStateChanged(const FString& SessionId, const FString& AgentName, EACPClientState State, const FString& Message);
	void OnClientModelsAvailable(const FString& AgentName, const FACPSessionModelState& ModelState);
	void OnSessionClientModelsAvailable(const FString& SessionId, const FString& AgentName, const FACPSessionModelState& ModelState);
	void OnClientPermissionRequest(const FString& AgentName, const FACPPermissionRequest& Request);
	void OnSessionClientPermissionRequest(const FString& SessionId, const FString& AgentName, const FACPPermissionRequest& Request);
	void OnClientModesAvailable(const FString& AgentName, const FACPSessionModeState& ModeState);
	void OnSessionClientModesAvailable(const FString& SessionId, const FString& AgentName, const FACPSessionModeState& ModeState);
	void OnClientModeChanged(const FString& AgentName, const FString& ModeId);
	void OnSessionClientModeChanged(const FString& SessionId, const FString& AgentName, const FString& ModeId);
	void OnClientCommandsAvailable(const FString& AgentName, const TArray<FACPSlashCommand>& Commands);
	void OnSessionClientCommandsAvailable(const FString& SessionId, const FString& AgentName, const TArray<FACPSlashCommand>& Commands);

	void BindSessionClientDelegates(const FString& SessionId, const FString& AgentName, const TSharedPtr<FACPClient>& Client);

	// Find session ID for an agent's update (uses active streaming session)
	FString FindSessionForAgent(const FString& AgentName) const;

	// Agent configurations
	TMap<FString, FACPAgentConfig> AgentConfigs;

	// Active ACP clients (for external agents like Claude Code, Gemini CLI)
	TMap<FString, TSharedPtr<FACPClient>> ActiveClients;

	// Live ACP clients keyed by NeoStack session id. These own user prompt traffic.
	TMap<FString, FACPManagedSessionClient> ActiveSessionClients;

	// Pending prompts waiting for session to be created.
	// Key is session ID (or agent name for legacy path), value is prompt text.
	TMultiMap<FString, FString> PendingPrompts;
	TSet<FString> PendingCancelledSessions;

	// Pending NewSession calls — AgentName → SessionId (multi-value queue)
	// When CreateSession is called but the client isn't Ready yet, we queue
	// the NewSession call here so it fires automatically when Ready.
	TMultiMap<FString, FString> PendingNewSessions;

	// One-shot launch-time resume requests for native CLIs that need resume flags at process start.
	TMap<FString, FString> PendingLaunchResumeSessions;

	// Session tracking for parallel chat support
	// Maps SessionId -> FAgentSessionContext
	TMap<FString, FAgentSessionContext> ActiveSessions;
	mutable FCriticalSection SessionLock;

	// Thread safety
	mutable FCriticalSection ConfigLock;
	mutable FCriticalSection ClientLock;

	// Terminal-auth process management
	void SpawnTerminalAuth(const FString& AgentName, const FACPAuthMethod& Method);
	bool LaunchExternalAuthTerminal(const FString& AgentName, const FString& CommandLine, FString& OutError) const;
	FProcHandle AuthProcessHandle;
	FTSTicker::FDelegateHandle AuthTickerHandle;
	FString AuthenticatingAgentName;

	// Agents currently downloading managed binaries (prevents re-entrant download loops)
	TSet<FString> AgentsDownloadingBinary;

	// Session list cache — AgentName → cached list from unstable_listSessions
	TMap<FString, TArray<FACPRemoteSessionEntry>> CachedSessionLists;
	void OnClientSessionListReceived(const FString& AgentName, const TArray<FACPRemoteSessionEntry>& Sessions);

};
