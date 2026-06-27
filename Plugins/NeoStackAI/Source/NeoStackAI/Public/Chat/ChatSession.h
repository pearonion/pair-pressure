// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/ChatTypes.h"
#include "Chat/IChatProvider.h"
#include "Chat/IChatEventSink.h"
#include "Chat/IChatStreamHandle.h"
#include "Tools/NeoStackToolRegistry.h"

class FChatSessionSink;

/**
 * State of a chat session. Only one stream can be in flight at a time; the
 * state machine enforces this invariant.
 */
enum class EChatSessionState : uint8
{
	Idle,              // Ready to accept a new prompt.
	Streaming,         // A provider stream is in flight for the current turn.
	ExecutingTools,    // Stream finished with tool uses; running tools on game thread.
	Error              // Terminal; NewSession() is required to recover.
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChatSessionStateChanged, EChatSessionState, const FString& /*Message*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatSessionEvent, const FChatEvent&);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatSessionToolCallStart, const FChatContentBlock& /*ToolUseBlock*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChatSessionToolCallResult, const FString& /*ToolUseId*/, const FChatContentBlock& /*ToolResultBlock*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnChatSessionUsageUpdated, const FChatUsage&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnChatSessionError, int32 /*Code*/, const FString& /*Message*/);

/**
 * A single chat session, bound to a provider resolved from FChatModelRegistry.
 *
 * Responsibilities:
 *   - Own the canonical FChatMessage[] history.
 *   - Turn UI prompts into FChatRequest and stream them through a provider.
 *   - Run the tool-execution loop: drain ToolUse blocks against the local
 *     tool registries (NeoStack + MCP), append ToolResult messages, and
 *     continue the conversation until no more tool calls remain.
 *   - Broadcast delegate events for state changes, streamed text, tool
 *     activity, usage, and errors.
 *
 * The session does not know about HTTP, SSE, or any wire format. All of
 * that lives inside the provider.
 *
 * Thread contract: all public methods must be called on the game thread.
 * Events arriving from the provider are marshalled back to the game thread
 * by the internal sink before touching session state or broadcasting.
 *
 * Lifecycle: construct via MakeShared<FChatSession>(), hold via TSharedRef.
 * Session extends TSharedFromThis so it can hand out a weak pointer to the
 * sink, which survives provider callbacks even if the session is released.
 */
class NEOSTACKAI_API FChatSession : public TSharedFromThis<FChatSession>
{
public:
	FChatSession();
	~FChatSession();

	// Non-copyable (shared ownership only)
	FChatSession(const FChatSession&) = delete;
	FChatSession& operator=(const FChatSession&) = delete;

	// ── Lifecycle ──────────────────────────────────────────────────────

	/**
	 * Reset conversation state, seed a fresh system message, and transition
	 * to Idle. Also resets session usage.
	 */
	void NewSession();

	/**
	 * Restore a previously saved provider-facing history and transition to Idle.
	 * The restored history is used for the next provider request, unlike the
	 * UI-only ACP projection stored in FACPSessionManager.
	 */
	void RestoreHistory(TArray<FChatMessage>&& RestoredHistory);

	/** Send a user prompt. Must be called in Idle state. */
	void SendPrompt(const FString& Text);

	/**
	 * Abort the current turn.
	 *   Streaming      -> cancel the stream immediately.
	 *   ExecutingTools -> cancel the in-flight tool and drop any queued tools.
	 *   other          -> no-op.
	 */
	void CancelPrompt();

	// ── Configuration ──────────────────────────────────────────────────

	void SetReasoningEffort(EReasoningEffort Effort) { ReasoningEffort = Effort; }
	EReasoningEffort GetReasoningEffort() const { return ReasoningEffort; }

	/**
	 * Freeze the provider/model selection for this session. The input is the
	 * registry's normal "<providerId>:<modelId>" form. Empty clears the override.
	 */
	void SetModelSelection(const FString& PrefixedModelId);

	/** System prompt for new sessions. If empty, a default is used. */
	void SetSystemPrompt(const FString& Prompt) { SystemPrompt = Prompt; }
	void SetSessionId(const FString& InSessionId) { SessionId = InSessionId; }

	// ── State access ───────────────────────────────────────────────────

	EChatSessionState GetState() const { return State; }
	const TArray<FChatMessage>& GetHistory() const { return History; }
	const FChatUsage& GetSessionUsage() const { return SessionUsage; }
	TSharedPtr<IChatProvider> GetProvider() const { return CachedProvider; }

	// ── Delegates (all broadcast on game thread) ───────────────────────

	FOnChatSessionStateChanged  OnStateChanged;
	FOnChatSessionEvent         OnEvent;          // raw pass-through for streamed deltas
	FOnChatSessionToolCallStart OnToolCallStart;  // after a ToolUse block fully arrives
	FOnChatSessionToolCallResult OnToolCallResult; // after local execution
	FOnChatSessionUsageUpdated  OnUsageUpdated;
	FOnChatSessionError         OnError;

private:
	friend class FChatSessionSink;

	// Called by the sink on the game thread for each event from the provider.
	void HandleEvent(const FChatEvent& Event, uint64 EventTurnGeneration);

	// Transition helper (always game-thread).
	void SetState(EChatSessionState NewState, const FString& Message);

	// Build an FChatRequest from current history + registered tools.
	FChatRequest BuildRequest() const;

	// Harvest FChatTool list from NeoStack + MCP registries, filtered by profile.
	TArray<FChatTool> CollectTools() const;

	// Start a new stream against the currently cached provider.
	void StartStream();

	// Handle stream termination: commit history, run tools or go Idle, handle cancel.
	void OnStreamDone();
	void OnStreamError(int32 Code, const FString& Message);

	// Drain PendingToolUses one at a time. Runs on game thread.
	void ExecuteNextTool(uint64 ExpectedTurnGeneration);

	// Resolve the provider to use for this session. Called by NewSession() and SendPrompt().
	void ResolveProvider();

	// Invalidate late stream events, queued tool ticks, and tool callbacks from older turns.
	uint64 AdvanceTurnGeneration();
	bool IsCurrentTurn(uint64 ExpectedTurnGeneration) const { return ExpectedTurnGeneration == ActiveTurnGeneration; }
	void CancelOwnedToolHandles();

private:
	// Canonical conversation.
	TArray<FChatMessage> History;

	// Assistant message currently being streamed. Committed to History on Done.
	FChatMessage InProgressAssistant;
	bool bHasInProgressAssistant = false;

	// Tool calls queued for execution after the current stream ends.
	TArray<FChatContentBlock> PendingToolUses;

	// State machine
	EChatSessionState State = EChatSessionState::Idle;

	// Current stream handle (valid iff State == Streaming).
	TSharedPtr<IChatStreamHandle> CurrentStream;

	// Sink wrapping this session; held here to keep it alive for the stream.
	TSharedPtr<FChatSessionSink> Sink;

	// Flag: stop the tool loop once the current in-flight tool completes.
	bool bCancelAfterCurrentTool = false;

	// Monotonic guard for dropping late callbacks after cancel/error/new turn.
	uint64 ActiveTurnGeneration = 0;

	// Tool handles issued by this session for the active turn (running or queued).
	TSet<FNeoStackToolHandle> OwnedToolHandles;

	// Cached provider (resolved from registry). Re-resolved when model changes.
	TSharedPtr<IChatProvider> CachedProvider;

	// Per-session configuration
	EReasoningEffort ReasoningEffort = EReasoningEffort::None;
	FString SystemPrompt;
	FString SessionProviderId;
	FString SessionModelId;
	FString SessionId;

	// Cumulative usage for the session.
	FChatUsage SessionUsage;
};
