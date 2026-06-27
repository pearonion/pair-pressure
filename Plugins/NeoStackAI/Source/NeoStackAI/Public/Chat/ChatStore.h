// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPSessionTypes.h"
#include "SQLiteDatabase.h"

struct NEOSTACKAI_API FNeoStackStoredSession
{
	FACPSessionMetadata Metadata;
	bool bIsDeleted = false;
};

/**
 * Local SQLite projection for NeoStack chat history.
 *
 * This store is a NeoStack-owned local projection: it makes session list and
 * message detail loading local and fast for WebUI, relay, and future clients,
 * while ACP/native provider IDs remain the source of truth for live continuation.
 */
class NEOSTACKAI_API FNeoStackChatStore
{
public:
	static FNeoStackChatStore& Get();

	bool Initialize();
	bool IsAvailable();

	// Closes the underlying SQLite handle. Must be called during module shutdown —
	// the Meyers-singleton destructor would otherwise run at C++ static teardown,
	// after SQLiteCore is gone, and FSQLiteDatabase's destructor asserts unless
	// Close() has been called first.
	void Shutdown();

	bool UpsertSession(const FACPSessionMetadata& Metadata);
	bool MarkSessionDeleted(const FString& SessionId);
	bool LoadSession(const FString& SessionId, FACPSessionMetadata& OutMetadata);
	TArray<FNeoStackStoredSession> ListSessions();

	bool SaveMessages(const FString& SessionId, const TArray<FACPChatMessage>& Messages);
	bool LoadMessages(const FString& SessionId, TArray<FACPChatMessage>& OutMessages);

	bool UpsertProviderBinding(
		const FString& SessionId,
		const FString& ProviderId,
		const FString& Transport,
		const FString& AgentConfigId,
		const FString& ResumeCursorJson,
		const FString& RuntimePayloadJson,
		const FString& Status);

private:
	FNeoStackChatStore() = default;
	~FNeoStackChatStore();

	bool OpenIfNeeded();
	bool CreateSchema();
	FString GetDatabasePath() const;

	static FString MessagesToJson(const TArray<FACPChatMessage>& Messages);
	static bool JsonToMessages(const FString& Json, TArray<FACPChatMessage>& OutMessages);

	bool bInitialized = false;
	TUniquePtr<FSQLiteDatabase> Database;
	FCriticalSection Lock;
};
