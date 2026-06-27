// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatStore.h"

#include "NeoStackAIModule.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FString RoleToString(EACPMessageRole Role)
{
	switch (Role)
	{
	case EACPMessageRole::User: return TEXT("user");
	case EACPMessageRole::Assistant: return TEXT("assistant");
	case EACPMessageRole::System: return TEXT("system");
	default: return TEXT("unknown");
	}
}

EACPMessageRole RoleFromString(const FString& Role)
{
	if (Role == TEXT("assistant")) return EACPMessageRole::Assistant;
	if (Role == TEXT("system")) return EACPMessageRole::System;
	return EACPMessageRole::User;
}

FString BlockTypeToString(EACPContentBlockType Type)
{
	switch (Type)
	{
	case EACPContentBlockType::Text: return TEXT("text");
	case EACPContentBlockType::Thought: return TEXT("thought");
	case EACPContentBlockType::ToolCall: return TEXT("tool_call");
	case EACPContentBlockType::ToolResult: return TEXT("tool_result");
	case EACPContentBlockType::Image: return TEXT("image");
	case EACPContentBlockType::Error: return TEXT("error");
	case EACPContentBlockType::System: return TEXT("system");
	default: return TEXT("text");
	}
}

EACPContentBlockType BlockTypeFromString(const FString& Type)
{
	if (Type == TEXT("thought")) return EACPContentBlockType::Thought;
	if (Type == TEXT("tool_call")) return EACPContentBlockType::ToolCall;
	if (Type == TEXT("tool_result")) return EACPContentBlockType::ToolResult;
	if (Type == TEXT("image")) return EACPContentBlockType::Image;
	if (Type == TEXT("error")) return EACPContentBlockType::Error;
	if (Type == TEXT("system")) return EACPContentBlockType::System;
	return EACPContentBlockType::Text;
}

FString FirstTextFromMessages(const TArray<FACPChatMessage>& Messages)
{
	for (int32 MessageIndex = Messages.Num() - 1; MessageIndex >= 0; --MessageIndex)
	{
		for (const FACPContentBlock& Block : Messages[MessageIndex].ContentBlocks)
		{
			if (!Block.Text.IsEmpty())
			{
				return Block.Text.Left(240);
			}
		}
	}
	return FString();
}
}

FNeoStackChatStore& FNeoStackChatStore::Get()
{
	static FNeoStackChatStore Instance;
	return Instance;
}

FNeoStackChatStore::~FNeoStackChatStore() = default;

void FNeoStackChatStore::Shutdown()
{
	FScopeLock Scope(&Lock);
	if (Database.IsValid())
	{
		if (Database->IsValid())
		{
			Database->Close();
		}
		Database.Reset();
	}
	bInitialized = false;
}

bool FNeoStackChatStore::Initialize()
{
	FScopeLock Scope(&Lock);
	return OpenIfNeeded();
}

bool FNeoStackChatStore::IsAvailable()
{
	FScopeLock Scope(&Lock);
	return OpenIfNeeded();
}

bool FNeoStackChatStore::OpenIfNeeded()
{
	if (bInitialized && Database.IsValid() && Database->IsValid())
	{
		return true;
	}

	const FString DbPath = GetDatabasePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DbPath), true);

	Database = MakeUnique<FSQLiteDatabase>();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("ChatStore: failed to open %s: %s"), *DbPath, *Database->GetLastError());
		Database.Reset();
		bInitialized = false;
		return false;
	}

	bInitialized = CreateSchema();
	if (!bInitialized)
	{
		Database.Reset();
	}
	return bInitialized;
}

bool FNeoStackChatStore::CreateSchema()
{
	check(Database.IsValid());

	return Database->Execute(TEXT("PRAGMA journal_mode=WAL;"))
		&& Database->Execute(TEXT("PRAGMA foreign_keys=ON;"))
		&& Database->Execute(TEXT(
			"CREATE TABLE IF NOT EXISTS chat_sessions ("
			"session_id TEXT PRIMARY KEY,"
			"agent_name TEXT NOT NULL,"
			"agent_session_id TEXT,"
			"title TEXT,"
			"created_at TEXT,"
			"last_modified_at TEXT,"
			"message_count INTEGER NOT NULL DEFAULT 0,"
			"model_id TEXT,"
			"has_custom_title INTEGER NOT NULL DEFAULT 0,"
			"is_deleted INTEGER NOT NULL DEFAULT 0"
			");"))
		&& Database->Execute(TEXT("CREATE INDEX IF NOT EXISTS idx_chat_sessions_modified ON chat_sessions(last_modified_at DESC);"))
		&& Database->Execute(TEXT(
			"CREATE TABLE IF NOT EXISTS chat_messages ("
			"session_id TEXT PRIMARY KEY,"
			"messages_json TEXT NOT NULL,"
			"preview TEXT,"
			"updated_at TEXT,"
			"FOREIGN KEY(session_id) REFERENCES chat_sessions(session_id) ON DELETE CASCADE"
			");"))
		&& Database->Execute(TEXT(
			"CREATE TABLE IF NOT EXISTS provider_bindings ("
			"session_id TEXT PRIMARY KEY,"
			"provider_id TEXT,"
			"transport TEXT,"
			"agent_config_id TEXT,"
			"resume_cursor_json TEXT,"
			"runtime_payload_json TEXT,"
			"status TEXT,"
			"last_seen_at TEXT,"
			"FOREIGN KEY(session_id) REFERENCES chat_sessions(session_id) ON DELETE CASCADE"
			");"));
}

FString FNeoStackChatStore::GetDatabasePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NeoStackAI"), TEXT("chat_history.sqlite"));
}

bool FNeoStackChatStore::UpsertSession(const FACPSessionMetadata& Metadata)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT(
		"INSERT INTO chat_sessions "
		"(session_id, agent_name, agent_session_id, title, created_at, last_modified_at, message_count, model_id, has_custom_title, is_deleted) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 0) "
		"ON CONFLICT(session_id) DO UPDATE SET "
		"agent_name=excluded.agent_name,"
		"agent_session_id=excluded.agent_session_id,"
		"title=excluded.title,"
		"created_at=excluded.created_at,"
		"last_modified_at=excluded.last_modified_at,"
		"message_count=excluded.message_count,"
		"model_id=excluded.model_id,"
		"has_custom_title=excluded.has_custom_title,"
		"is_deleted=0;"));
	if (!Statement.IsValid()) return false;

	Statement.SetBindingValueByIndex(1, Metadata.SessionId);
	Statement.SetBindingValueByIndex(2, Metadata.AgentName);
	Statement.SetBindingValueByIndex(3, Metadata.AgentSessionId);
	Statement.SetBindingValueByIndex(4, Metadata.Title);
	Statement.SetBindingValueByIndex(5, Metadata.CreatedAt.ToIso8601());
	Statement.SetBindingValueByIndex(6, Metadata.LastModifiedAt.ToIso8601());
	Statement.SetBindingValueByIndex(7, Metadata.MessageCount);
	Statement.SetBindingValueByIndex(8, Metadata.ModelId);
	Statement.SetBindingValueByIndex(9, Metadata.bHasCustomTitle ? 1 : 0);
	return Statement.Execute();
}

bool FNeoStackChatStore::MarkSessionDeleted(const FString& SessionId)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT("UPDATE chat_sessions SET is_deleted=1 WHERE session_id=?1;"));
	if (!Statement.IsValid()) return false;
	Statement.SetBindingValueByIndex(1, SessionId);
	return Statement.Execute();
}

bool FNeoStackChatStore::LoadSession(const FString& SessionId, FACPSessionMetadata& OutMetadata)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT(
		"SELECT session_id, agent_name, agent_session_id, title, created_at, last_modified_at, message_count, model_id, has_custom_title "
		"FROM chat_sessions WHERE session_id=?1 AND is_deleted=0;"));
	if (!Statement.IsValid()) return false;

	Statement.SetBindingValueByIndex(1, SessionId);
	bool bFound = false;
	Statement.Execute([&](const FSQLitePreparedStatement& Row)
	{
		FString CreatedAt;
		FString LastModifiedAt;
		int32 HasCustomTitle = 0;
		Row.GetColumnValueByIndex(0, OutMetadata.SessionId);
		Row.GetColumnValueByIndex(1, OutMetadata.AgentName);
		Row.GetColumnValueByIndex(2, OutMetadata.AgentSessionId);
		Row.GetColumnValueByIndex(3, OutMetadata.Title);
		Row.GetColumnValueByIndex(4, CreatedAt);
		Row.GetColumnValueByIndex(5, LastModifiedAt);
		Row.GetColumnValueByIndex(6, OutMetadata.MessageCount);
		Row.GetColumnValueByIndex(7, OutMetadata.ModelId);
		Row.GetColumnValueByIndex(8, HasCustomTitle);
		FDateTime::ParseIso8601(*CreatedAt, OutMetadata.CreatedAt);
		FDateTime::ParseIso8601(*LastModifiedAt, OutMetadata.LastModifiedAt);
		OutMetadata.bHasCustomTitle = HasCustomTitle != 0;
		bFound = true;
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	});
	return bFound;
}

TArray<FNeoStackStoredSession> FNeoStackChatStore::ListSessions()
{
	FScopeLock Scope(&Lock);
	TArray<FNeoStackStoredSession> Result;
	if (!OpenIfNeeded()) return Result;

	Database->Execute(TEXT(
		"SELECT session_id, agent_name, agent_session_id, title, created_at, last_modified_at, message_count, model_id, has_custom_title, is_deleted "
		"FROM chat_sessions WHERE is_deleted=0 ORDER BY last_modified_at DESC;"),
		[&](const FSQLitePreparedStatement& Row)
		{
			FNeoStackStoredSession Stored;
			FString CreatedAt;
			FString LastModifiedAt;
			int32 HasCustomTitle = 0;
			int32 IsDeleted = 0;
			Row.GetColumnValueByIndex(0, Stored.Metadata.SessionId);
			Row.GetColumnValueByIndex(1, Stored.Metadata.AgentName);
			Row.GetColumnValueByIndex(2, Stored.Metadata.AgentSessionId);
			Row.GetColumnValueByIndex(3, Stored.Metadata.Title);
			Row.GetColumnValueByIndex(4, CreatedAt);
			Row.GetColumnValueByIndex(5, LastModifiedAt);
			Row.GetColumnValueByIndex(6, Stored.Metadata.MessageCount);
			Row.GetColumnValueByIndex(7, Stored.Metadata.ModelId);
			Row.GetColumnValueByIndex(8, HasCustomTitle);
			Row.GetColumnValueByIndex(9, IsDeleted);
			FDateTime::ParseIso8601(*CreatedAt, Stored.Metadata.CreatedAt);
			FDateTime::ParseIso8601(*LastModifiedAt, Stored.Metadata.LastModifiedAt);
			Stored.Metadata.bHasCustomTitle = HasCustomTitle != 0;
			Stored.bIsDeleted = IsDeleted != 0;
			Result.Add(MoveTemp(Stored));
			return ESQLitePreparedStatementExecuteRowResult::Continue;
		});
	return Result;
}

bool FNeoStackChatStore::SaveMessages(const FString& SessionId, const TArray<FACPChatMessage>& Messages)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT(
		"INSERT INTO chat_messages (session_id, messages_json, preview, updated_at) VALUES (?1, ?2, ?3, ?4) "
		"ON CONFLICT(session_id) DO UPDATE SET messages_json=excluded.messages_json, preview=excluded.preview, updated_at=excluded.updated_at;"));
	if (!Statement.IsValid()) return false;

	Statement.SetBindingValueByIndex(1, SessionId);
	Statement.SetBindingValueByIndex(2, MessagesToJson(Messages));
	Statement.SetBindingValueByIndex(3, FirstTextFromMessages(Messages));
	Statement.SetBindingValueByIndex(4, FDateTime::Now().ToIso8601());
	return Statement.Execute();
}

bool FNeoStackChatStore::LoadMessages(const FString& SessionId, TArray<FACPChatMessage>& OutMessages)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT("SELECT messages_json FROM chat_messages WHERE session_id=?1;"));
	if (!Statement.IsValid()) return false;

	Statement.SetBindingValueByIndex(1, SessionId);
	FString Json;
	const bool bFound = Statement.Execute([&](const FSQLitePreparedStatement& Row)
	{
		Row.GetColumnValueByIndex(0, Json);
		return ESQLitePreparedStatementExecuteRowResult::Stop;
	}) > 0;
	return bFound && JsonToMessages(Json, OutMessages);
}

bool FNeoStackChatStore::UpsertProviderBinding(
	const FString& SessionId,
	const FString& ProviderId,
	const FString& Transport,
	const FString& AgentConfigId,
	const FString& ResumeCursorJson,
	const FString& RuntimePayloadJson,
	const FString& Status)
{
	FScopeLock Scope(&Lock);
	if (!OpenIfNeeded()) return false;

	FSQLitePreparedStatement Statement = Database->PrepareStatement(TEXT(
		"INSERT INTO provider_bindings (session_id, provider_id, transport, agent_config_id, resume_cursor_json, runtime_payload_json, status, last_seen_at) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
		"ON CONFLICT(session_id) DO UPDATE SET "
		"provider_id=excluded.provider_id, transport=excluded.transport, agent_config_id=excluded.agent_config_id, "
		"resume_cursor_json=excluded.resume_cursor_json, runtime_payload_json=excluded.runtime_payload_json, status=excluded.status, last_seen_at=excluded.last_seen_at;"));
	if (!Statement.IsValid()) return false;
	Statement.SetBindingValueByIndex(1, SessionId);
	Statement.SetBindingValueByIndex(2, ProviderId);
	Statement.SetBindingValueByIndex(3, Transport);
	Statement.SetBindingValueByIndex(4, AgentConfigId);
	Statement.SetBindingValueByIndex(5, ResumeCursorJson);
	Statement.SetBindingValueByIndex(6, RuntimePayloadJson);
	Statement.SetBindingValueByIndex(7, Status);
	Statement.SetBindingValueByIndex(8, FDateTime::Now().ToIso8601());
	return Statement.Execute();
}

FString FNeoStackChatStore::MessagesToJson(const TArray<FACPChatMessage>& Messages)
{
	TArray<TSharedPtr<FJsonValue>> MessageArray;
	for (const FACPChatMessage& Message : Messages)
	{
		TSharedRef<FJsonObject> MessageObj = MakeShared<FJsonObject>();
		MessageObj->SetStringField(TEXT("messageId"), Message.MessageId.ToString(EGuidFormats::DigitsWithHyphens));
		MessageObj->SetStringField(TEXT("role"), RoleToString(Message.Role));
		MessageObj->SetStringField(TEXT("timestamp"), Message.Timestamp.ToIso8601());
		MessageObj->SetBoolField(TEXT("isStreaming"), Message.bIsStreaming);
		MessageObj->SetStringField(TEXT("providerText"), Message.ProviderText);

		TArray<TSharedPtr<FJsonValue>> Contexts;
		for (const FACPMessageContext& Context : Message.Contexts)
		{
			TSharedRef<FJsonObject> ContextObj = MakeShared<FJsonObject>();
			ContextObj->SetStringField(TEXT("path"), Context.Path);
			ContextObj->SetStringField(TEXT("displayName"), Context.DisplayName);
			ContextObj->SetNumberField(TEXT("type"), static_cast<int32>(Context.Type));
			ContextObj->SetNumberField(TEXT("status"), static_cast<int32>(Context.Status));
			ContextObj->SetNumberField(TEXT("lineCount"), Context.LineCount);
			ContextObj->SetBoolField(TEXT("truncated"), Context.bTruncated);
			ContextObj->SetStringField(TEXT("errorMessage"), Context.ErrorMessage);
			Contexts.Add(MakeShared<FJsonValueObject>(ContextObj));
		}
		MessageObj->SetArrayField(TEXT("contexts"), Contexts);

		TArray<TSharedPtr<FJsonValue>> Blocks;
		for (const FACPContentBlock& Block : Message.ContentBlocks)
		{
			TSharedRef<FJsonObject> BlockObj = MakeShared<FJsonObject>();
			BlockObj->SetStringField(TEXT("type"), BlockTypeToString(Block.Type));
			BlockObj->SetStringField(TEXT("text"), Block.Text);
			BlockObj->SetStringField(TEXT("toolCallId"), Block.ToolCallId);
			BlockObj->SetStringField(TEXT("toolName"), Block.ToolName);
			BlockObj->SetStringField(TEXT("toolArguments"), Block.ToolArguments);
			BlockObj->SetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
			BlockObj->SetStringField(TEXT("toolResult"), Block.ToolResultContent);
			BlockObj->SetBoolField(TEXT("toolSuccess"), Block.bToolSuccess);
			BlockObj->SetStringField(TEXT("timestamp"), Block.Timestamp.ToIso8601());
			BlockObj->SetBoolField(TEXT("isStreaming"), Block.bIsStreaming);
			TArray<TSharedPtr<FJsonValue>> Images;
			for (const FACPToolResultImage& Image : Block.ToolResultImages)
			{
				TSharedRef<FJsonObject> ImageObj = MakeShared<FJsonObject>();
				ImageObj->SetStringField(TEXT("base64"), Image.Base64Data);
				ImageObj->SetStringField(TEXT("mimeType"), Image.MimeType);
				ImageObj->SetNumberField(TEXT("width"), Image.Width);
				ImageObj->SetNumberField(TEXT("height"), Image.Height);
				Images.Add(MakeShared<FJsonValueObject>(ImageObj));
			}
			BlockObj->SetArrayField(TEXT("images"), Images);
			Blocks.Add(MakeShared<FJsonValueObject>(BlockObj));
		}
		MessageObj->SetArrayField(TEXT("contentBlocks"), Blocks);
		MessageArray.Add(MakeShared<FJsonValueObject>(MessageObj));
	}

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(MessageArray, Writer);
	return Out;
}

bool FNeoStackChatStore::JsonToMessages(const FString& Json, TArray<FACPChatMessage>& OutMessages)
{
	TArray<TSharedPtr<FJsonValue>> MessageArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, MessageArray))
	{
		return false;
	}

	OutMessages.Reset();
	for (const TSharedPtr<FJsonValue>& MessageValue : MessageArray)
	{
		const TSharedPtr<FJsonObject> MessageObj = MessageValue.IsValid() ? MessageValue->AsObject() : nullptr;
		if (!MessageObj.IsValid())
		{
			continue;
		}
		FACPChatMessage Message;
		FString MessageId;
		if (MessageObj->TryGetStringField(TEXT("messageId"), MessageId))
		{
			FGuid::Parse(MessageId, Message.MessageId);
		}
		FString Role;
		MessageObj->TryGetStringField(TEXT("role"), Role);
		Message.Role = RoleFromString(Role);
		FString Timestamp;
		if (MessageObj->TryGetStringField(TEXT("timestamp"), Timestamp))
		{
			FDateTime::ParseIso8601(*Timestamp, Message.Timestamp);
		}
		MessageObj->TryGetBoolField(TEXT("isStreaming"), Message.bIsStreaming);
		MessageObj->TryGetStringField(TEXT("providerText"), Message.ProviderText);

		const TArray<TSharedPtr<FJsonValue>>* Contexts = nullptr;
		if (MessageObj->TryGetArrayField(TEXT("contexts"), Contexts) && Contexts)
		{
			for (const TSharedPtr<FJsonValue>& ContextValue : *Contexts)
			{
				const TSharedPtr<FJsonObject> ContextObj = ContextValue.IsValid() ? ContextValue->AsObject() : nullptr;
				if (!ContextObj.IsValid())
				{
					continue;
				}
				FACPMessageContext Context;
				ContextObj->TryGetStringField(TEXT("path"), Context.Path);
				ContextObj->TryGetStringField(TEXT("displayName"), Context.DisplayName);
				double Type = static_cast<double>(static_cast<int32>(EACPContextType::Unknown));
				double Status = static_cast<double>(static_cast<int32>(EACPContextStatus::Resolved));
				double LineCount = 0.0;
				ContextObj->TryGetNumberField(TEXT("type"), Type);
				ContextObj->TryGetNumberField(TEXT("status"), Status);
				ContextObj->TryGetNumberField(TEXT("lineCount"), LineCount);
				Context.Type = static_cast<EACPContextType>(static_cast<int32>(Type));
				Context.Status = static_cast<EACPContextStatus>(static_cast<int32>(Status));
				Context.LineCount = static_cast<int32>(LineCount);
				ContextObj->TryGetBoolField(TEXT("truncated"), Context.bTruncated);
				ContextObj->TryGetStringField(TEXT("errorMessage"), Context.ErrorMessage);
				Message.Contexts.Add(MoveTemp(Context));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
		if (MessageObj->TryGetArrayField(TEXT("contentBlocks"), Blocks) && Blocks)
		{
			for (const TSharedPtr<FJsonValue>& BlockValue : *Blocks)
			{
				const TSharedPtr<FJsonObject> BlockObj = BlockValue.IsValid() ? BlockValue->AsObject() : nullptr;
				if (!BlockObj.IsValid())
				{
					continue;
				}
				FACPContentBlock Block;
				FString Type;
				BlockObj->TryGetStringField(TEXT("type"), Type);
				Block.Type = BlockTypeFromString(Type);
				BlockObj->TryGetStringField(TEXT("text"), Block.Text);
				BlockObj->TryGetStringField(TEXT("toolCallId"), Block.ToolCallId);
				BlockObj->TryGetStringField(TEXT("toolName"), Block.ToolName);
				BlockObj->TryGetStringField(TEXT("toolArguments"), Block.ToolArguments);
				BlockObj->TryGetStringField(TEXT("parentToolCallId"), Block.ParentToolCallId);
				BlockObj->TryGetStringField(TEXT("toolResult"), Block.ToolResultContent);
				Block.bToolSuccess = true;
				BlockObj->TryGetBoolField(TEXT("toolSuccess"), Block.bToolSuccess);
				FString BlockTimestamp;
				if (BlockObj->TryGetStringField(TEXT("timestamp"), BlockTimestamp))
				{
					FDateTime::ParseIso8601(*BlockTimestamp, Block.Timestamp);
				}
				BlockObj->TryGetBoolField(TEXT("isStreaming"), Block.bIsStreaming);
				const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
				if (BlockObj->TryGetArrayField(TEXT("images"), Images) && Images)
				{
					for (const TSharedPtr<FJsonValue>& ImageValue : *Images)
					{
						const TSharedPtr<FJsonObject> ImageObj = ImageValue.IsValid() ? ImageValue->AsObject() : nullptr;
						if (!ImageObj.IsValid())
						{
							continue;
						}
						FACPToolResultImage Image;
						ImageObj->TryGetStringField(TEXT("base64"), Image.Base64Data);
						ImageObj->TryGetStringField(TEXT("mimeType"), Image.MimeType);
						double Width = 0.0;
						double Height = 0.0;
						ImageObj->TryGetNumberField(TEXT("width"), Width);
						ImageObj->TryGetNumberField(TEXT("height"), Height);
						Image.Width = static_cast<int32>(Width);
						Image.Height = static_cast<int32>(Height);
						Block.ToolResultImages.Add(MoveTemp(Image));
					}
				}
				Message.ContentBlocks.Add(MoveTemp(Block));
			}
		}

		OutMessages.Add(MoveTemp(Message));
	}
	return true;
}
