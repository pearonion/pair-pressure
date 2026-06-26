// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatSessionManager.h"
#include "Chat/ChatModelRegistry.h"

#include "ACPAgentManager.h"
#include "ACPTypes.h"
#include "NeoStackAIModule.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Guid.h"

namespace
{
	/**
	 * Agent name used for built-in chat sessions. Historically this was
	 * "OpenRouter" when it was the only backend; the manager still accepts
	 * that legacy name as an alias for saved sessions.
	 */
	static const FString BuiltInChatAgentName = TEXT("Local & BYOK Chat");

	static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
	{
		if (Json.IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	static FChatContentBlock ConvertTextBlock(const FString& Text)
	{
		FChatContentBlock Block;
		Block.Kind = EChatContentKind::Text;
		Block.Text = Text;
		return Block;
	}

	static void FlushAssistantMessage(TArray<FChatMessage>& OutHistory, FChatMessage& PendingAssistant)
	{
		if (PendingAssistant.Content.Num() > 0 || !PendingAssistant.Reasoning.IsEmpty())
		{
			OutHistory.Add(MoveTemp(PendingAssistant));
			PendingAssistant = FChatMessage{};
			PendingAssistant.Role = EChatRole::Assistant;
		}
	}

	static TArray<FChatMessage> ConvertAcpMessagesToChatHistory(const TArray<FACPChatMessage>& Messages)
	{
		TArray<FChatMessage> History;

		for (const FACPChatMessage& AcpMessage : Messages)
		{
			if (AcpMessage.Role == EACPMessageRole::User)
			{
				FChatMessage User;
				User.Role = EChatRole::User;
				const FString ProviderText = AcpMessage.ProviderText.IsEmpty()
					? FString()
					: AcpMessage.ProviderText;
				for (const FACPContentBlock& Block : AcpMessage.ContentBlocks)
				{
					if (Block.Type == EACPContentBlockType::Text && !Block.Text.IsEmpty())
					{
						User.Content.Add(ConvertTextBlock(ProviderText.IsEmpty() ? Block.Text : ProviderText));
					}
				}
				if (User.Content.Num() > 0)
				{
					History.Add(MoveTemp(User));
				}
				continue;
			}

			if (AcpMessage.Role == EACPMessageRole::System)
			{
				FChatMessage System;
				System.Role = EChatRole::System;
				for (const FACPContentBlock& Block : AcpMessage.ContentBlocks)
				{
					if (!Block.Text.IsEmpty())
					{
						System.Content.Add(ConvertTextBlock(Block.Text));
					}
				}
				if (System.Content.Num() > 0)
				{
					History.Add(MoveTemp(System));
				}
				continue;
			}

			FChatMessage Assistant;
			Assistant.Role = EChatRole::Assistant;
			for (const FACPContentBlock& Block : AcpMessage.ContentBlocks)
			{
				switch (Block.Type)
				{
				case EACPContentBlockType::Text:
				case EACPContentBlockType::System:
					if (!Block.Text.IsEmpty())
					{
						Assistant.Content.Add(ConvertTextBlock(Block.Text));
					}
					break;

				case EACPContentBlockType::Thought:
					Assistant.Reasoning += Block.Text;
					break;

				case EACPContentBlockType::ToolCall:
				{
					FChatContentBlock ToolUse;
					ToolUse.Kind = EChatContentKind::ToolUse;
					ToolUse.ToolUseId = Block.ToolCallId;
					ToolUse.ToolName = Block.ToolName;
					ToolUse.RawArgsBuffer = Block.ToolArguments;
					ToolUse.ToolArgs = ParseJsonObject(Block.ToolArguments);
					Assistant.Content.Add(MoveTemp(ToolUse));
					break;
				}

				case EACPContentBlockType::ToolResult:
				{
					FlushAssistantMessage(History, Assistant);

					FChatMessage ToolMessage;
					ToolMessage.Role = EChatRole::Tool;
					FChatContentBlock ToolResult;
					ToolResult.Kind = EChatContentKind::ToolResult;
					ToolResult.ToolUseId = Block.ToolCallId;
					ToolResult.Text = Block.ToolResultContent;
					ToolResult.bToolError = !Block.bToolSuccess;
					ToolResult.ResultImages = Block.ToolResultImages;
					ToolMessage.Content.Add(MoveTemp(ToolResult));
					History.Add(MoveTemp(ToolMessage));
					break;
				}

				case EACPContentBlockType::Image:
				{
					FChatContentBlock Image;
					Image.Kind = EChatContentKind::Image;
					Image.Text = Block.Text;
					Assistant.Content.Add(MoveTemp(Image));
					break;
				}

				case EACPContentBlockType::Error:
					if (!Block.Text.IsEmpty())
					{
						Assistant.Content.Add(ConvertTextBlock(Block.Text));
					}
					break;
				}
			}

			FlushAssistantMessage(History, Assistant);
		}

		return History;
	}
}

FChatSessionManager& FChatSessionManager::Get()
{
	static FChatSessionManager Instance;
	return Instance;
}

bool FChatSessionManager::AttachSession(const FString& SessionId)
{
	return AttachSession(SessionId, FChatSessionAttachOptions());
}

bool FChatSessionManager::AttachSession(const FString& SessionId, const TArray<FACPChatMessage>& RestoredMessages)
{
	FChatSessionAttachOptions Options;
	Options.RestoredMessages = RestoredMessages;
	return AttachSession(SessionId, Options);
}

bool FChatSessionManager::AttachSession(const FString& SessionId, const FChatSessionAttachOptions& Options)
{
	if (SessionId.IsEmpty()) return false;
	if (Sessions.Contains(SessionId)) return true;

	TSharedRef<FChatSession> Session = MakeShared<FChatSession>();
	Session->SetSessionId(SessionId);
	Session->SetModelSelection(Options.PrefixedModelId);
	Session->SetReasoningEffort(Options.ReasoningLevel.IsEmpty()
		? EReasoningEffort::None
		: ChatTypes::ReasoningEffortFromString(Options.ReasoningLevel));
	if (Options.RestoredMessages.Num() > 0)
	{
		Session->RestoreHistory(ConvertAcpMessagesToChatHistory(Options.RestoredMessages));
	}
	else
	{
		Session->NewSession();
	}

	Sessions.Add(SessionId, Session);
	BindSession(SessionId, Session);

	UE_LOG(LogNeoStackAI, Log,
		TEXT("ChatSessionManager: attached session %s"), *SessionId);

	return true;
}

bool FChatSessionManager::CloseSession(const FString& SessionId)
{
	if (TSharedPtr<FChatSession> Session = FindSession(SessionId))
	{
		Session->CancelPrompt();
		Sessions.Remove(SessionId);
		return true;
	}
	return false;
}

bool FChatSessionManager::HasSession(const FString& SessionId) const
{
	return Sessions.Contains(SessionId);
}

TSharedPtr<FChatSession> FChatSessionManager::FindSession(const FString& SessionId) const
{
	const TSharedPtr<FChatSession>* Found = Sessions.Find(SessionId);
	return Found ? *Found : nullptr;
}

void FChatSessionManager::SendPrompt(const FString& SessionId, const FString& Text)
{
	if (TSharedPtr<FChatSession> Session = FindSession(SessionId))
	{
		Session->SendPrompt(Text);
	}
}

void FChatSessionManager::CancelPrompt(const FString& SessionId)
{
	if (TSharedPtr<FChatSession> Session = FindSession(SessionId))
	{
		Session->CancelPrompt();
	}
}

void FChatSessionManager::SetSelectedModel(const FString& PrefixedModelId)
{
	FChatModelRegistry::Get().SetSelectedModel(PrefixedModelId);
}

void FChatSessionManager::SetReasoningEffort(const FString& Level)
{
	const EReasoningEffort Effort = ChatTypes::ReasoningEffortFromString(Level);
	for (auto& Pair : Sessions)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->SetReasoningEffort(Effort);
		}
	}
}

void FChatSessionManager::Shutdown()
{
	for (auto& Pair : Sessions)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->CancelPrompt();
		}
	}
	Sessions.Empty();
}

// ============================================================================
// Delegate wiring — forwards FChatSession events into FACPAgentManager
// delegates. WebUIBridge subscribes to FACPAgentManager::OnAgentMessage and
// OnAgentStateChanged, so going through those channels keeps the existing
// chat UI streaming/persistence logic working without changes.
// ============================================================================

void FChatSessionManager::BindSession(const FString& SessionId, TSharedRef<FChatSession> Session)
{
	const FString CapturedId = SessionId;

	// TextDelta / ReasoningDelta / Error → FACPSessionUpdate broadcast
	Session->OnEvent.AddLambda([CapturedId](const FChatEvent& Event)
	{
		// Tool-call start/result and Done are handled by their own delegates
		// so we don't double-broadcast here.
		if (Event.Kind == EChatEventKind::ToolUseStart
			|| Event.Kind == EChatEventKind::ToolUseEnd
			|| Event.Kind == EChatEventKind::ToolUseArgsDelta
			|| Event.Kind == EChatEventKind::Done
			|| Event.Kind == EChatEventKind::UsageUpdate)
		{
			return;
		}

		FACPSessionUpdate Update;
		Update.SessionId = CapturedId;

		switch (Event.Kind)
		{
			case EChatEventKind::TextDelta:
				Update.UpdateType = EACPUpdateType::AgentMessageChunk;
				Update.TextChunk = Event.TextChunk;
				break;

			case EChatEventKind::ReasoningDelta:
				Update.UpdateType = EACPUpdateType::AgentThoughtChunk;
				Update.TextChunk = Event.TextChunk;
				break;

			// Terminal errors are forwarded via Session->OnError only.
			case EChatEventKind::Error:
				return;

			default:
				return;
		}

		FACPAgentManager::Get().OnAgentMessage.Broadcast(CapturedId, BuiltInChatAgentName, Update);
	});

	// State changes → OnAgentStateChanged broadcast
	Session->OnStateChanged.AddLambda([CapturedId](EChatSessionState NewState, const FString& Message)
	{
		EACPClientState AcpState = EACPClientState::Ready;
		switch (NewState)
		{
			case EChatSessionState::Idle:           AcpState = EACPClientState::Ready; break;
			case EChatSessionState::Streaming:      AcpState = EACPClientState::Prompting; break;
			case EChatSessionState::ExecutingTools: AcpState = EACPClientState::Prompting; break;
			case EChatSessionState::Error:          AcpState = EACPClientState::Error; break;
		}
		FACPAgentManager::Get().OnAgentStateChanged.Broadcast(CapturedId, BuiltInChatAgentName, AcpState, Message);
	});

	// Tool call start → ToolCall update
	Session->OnToolCallStart.AddLambda([CapturedId](const FChatContentBlock& ToolUseBlock)
	{
		FString ArgsJson;
		if (ToolUseBlock.ToolArgs.IsValid())
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsJson);
			FJsonSerializer::Serialize(ToolUseBlock.ToolArgs.ToSharedRef(), Writer);
		}
		if (ArgsJson.IsEmpty()) ArgsJson = TEXT("{}");

		FACPSessionUpdate Update;
		Update.SessionId = CapturedId;
		Update.UpdateType = EACPUpdateType::ToolCall;
		Update.ToolCallId = ToolUseBlock.ToolUseId;
		Update.ToolName = ToolUseBlock.ToolName;
		Update.ToolArguments = ArgsJson;
		Update.ToolCallStatus = TEXT("in_progress");

		FACPAgentManager::Get().OnAgentMessage.Broadcast(CapturedId, BuiltInChatAgentName, Update);
	});

	// Tool call result → ToolCallUpdate update
	Session->OnToolCallResult.AddLambda([CapturedId](const FString& ToolUseId, const FChatContentBlock& ResultBlock)
	{
		FACPSessionUpdate Update;
		Update.SessionId = CapturedId;
		Update.UpdateType = EACPUpdateType::ToolCallUpdate;
		Update.ToolCallId = ToolUseId;
		Update.ToolResult = ResultBlock.Text;
		Update.bToolSuccess = !ResultBlock.bToolError;
		Update.ToolCallStatus = ResultBlock.bToolError ? TEXT("failed") : TEXT("completed");
		Update.ToolResultImages = ResultBlock.ResultImages;

		FACPAgentManager::Get().OnAgentMessage.Broadcast(CapturedId, BuiltInChatAgentName, Update);
	});

	// Usage updates → UsageUpdate update
	Session->OnUsageUpdated.AddLambda([CapturedId](const FChatUsage& Usage)
	{
		FACPSessionUpdate Update;
		Update.SessionId = CapturedId;
		Update.UpdateType = EACPUpdateType::UsageUpdate;
		Update.Usage.InputTokens = Usage.InputTokens;
		Update.Usage.OutputTokens = Usage.OutputTokens;
		Update.Usage.TotalTokens = Usage.TotalTokens;
		Update.Usage.CacheReadTokens = Usage.CachedTokens;
		Update.Usage.ReasoningTokens = Usage.ReasoningTokens;
		Update.Usage.CostAmount = Usage.CostAmount;
		Update.Usage.CostCurrency = Usage.CostCurrency;
		Update.Usage.TurnCostUSD = Usage.CostAmount;

		FACPAgentManager::Get().OnAgentMessage.Broadcast(CapturedId, BuiltInChatAgentName, Update);
	});

	// Errors → Error update
	Session->OnError.AddLambda([CapturedId](int32 Code, const FString& Message)
	{
		FACPSessionUpdate Update;
		Update.SessionId = CapturedId;
		Update.UpdateType = EACPUpdateType::Error;
		Update.ErrorCode = Code;
		Update.ErrorMessage = Message;

		FACPAgentManager::Get().OnAgentMessage.Broadcast(CapturedId, BuiltInChatAgentName, Update);
	});
}
