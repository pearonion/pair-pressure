// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPSessionTypes.h"
#include "Chat/ChatSession.h"

struct NEOSTACKAI_API FChatSessionAttachOptions
{
	TArray<FACPChatMessage> RestoredMessages;
	FString PrefixedModelId;
	FString ReasoningLevel;
};

/**
 * Singleton that owns FChatSession instances keyed by session id, and forwards
 * their events into the existing FAgentService delegate surface (OnMessage,
 * OnStateChanged, OnModelsAvailable, etc.) so the WebUI + relay clients receive
 * them without knowing that the built-in chat agent has a new backing.
 *
 * "Built-in Chat" sessions live here. ACP subprocess agent sessions continue
 * to live inside FACPAgentManager. AgentService routes each session id to the
 * correct owner via the agent-name stored alongside it.
 *
 * Thread contract: all public methods are called on the game thread.
 */
class NEOSTACKAI_API FChatSessionManager
{
public:
	static FChatSessionManager& Get();

	/**
	 * Attach a FChatSession to an existing session id (created upstream by
	 * FACPSessionManager). Returns true on success; false if a session with
	 * this id already exists here.
	 */
	bool AttachSession(const FString& SessionId);
	bool AttachSession(const FString& SessionId, const TArray<FACPChatMessage>& RestoredMessages);
	bool AttachSession(const FString& SessionId, const FChatSessionAttachOptions& Options);

	/** Close and delete a session. Returns true if it existed. */
	bool CloseSession(const FString& SessionId);

	/** Whether the given session id belongs to a built-in chat session. */
	bool HasSession(const FString& SessionId) const;

	/** Look up a session by id; nullptr if not managed here. */
	TSharedPtr<FChatSession> FindSession(const FString& SessionId) const;

	/** Convenience: send a prompt to a managed session. No-op if not found. */
	void SendPrompt(const FString& SessionId, const FString& Text);

	/** Convenience: cancel a managed session. No-op if not found. */
	void CancelPrompt(const FString& SessionId);

	/** Set the active model for all new sessions (and optionally the running one). */
	void SetSelectedModel(const FString& PrefixedModelId);

	/** Set reasoning effort for all new sessions (and optionally the running one). */
	void SetReasoningEffort(const FString& Level);

	/** Called by FAgentService::Shutdown to drop everything. */
	void Shutdown();

private:
	FChatSessionManager() = default;

	/** Wire a freshly created session's delegates into FACPAgentManager. */
	void BindSession(const FString& SessionId, TSharedRef<FChatSession> Session);

	TMap<FString, TSharedPtr<FChatSession>> Sessions;
};
