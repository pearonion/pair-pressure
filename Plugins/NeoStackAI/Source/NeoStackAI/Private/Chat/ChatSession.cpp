// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/ChatSession.h"
#include "Chat/ChatModelRegistry.h"
#include "Chat/ChatSessionSink.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"
#include "MCPServer.h"
#include "MCPTypes.h"
#include "Tools/NeoStackToolBase.h"
#include "Tools/NeoStackToolRegistry.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FChatMessage BuildSystemMessage(const FString& SystemPrompt)
{
	FChatMessage Sys;
	Sys.Role = EChatRole::System;

	FChatContentBlock Block;
	Block.Kind = EChatContentKind::Text;
	Block.Text = SystemPrompt.IsEmpty()
		? TEXT("You are a helpful AI assistant integrated into Unreal Engine. Help the user with their game development tasks.")
		: SystemPrompt;

	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		const FString Append = Settings->GetProfileSystemPromptAppend();
		if (!Append.IsEmpty())
		{
			Block.Text += TEXT("\n\n") + Append;
		}
	}

	Sys.Content.Add(MoveTemp(Block));
	return Sys;
}

struct FChatToolCompletionState
{
	FNeoStackToolHandle Handle;
	bool bCompleted = false;
};
}

// ============================================================================
// Lifecycle
// ============================================================================

FChatSession::FChatSession()
{
}

FChatSession::~FChatSession()
{
	// Drop the sink's weak link explicitly so any in-flight events become no-ops.
	Sink.Reset();

	if (CurrentStream.IsValid())
	{
		CurrentStream->Cancel();
		CurrentStream.Reset();
	}
}

void FChatSession::NewSession()
{
	check(IsInGameThread());

	History.Empty();
	InProgressAssistant = FChatMessage{};
	bHasInProgressAssistant = false;
	PendingToolUses.Empty();
	CancelOwnedToolHandles();
	AdvanceTurnGeneration();
	bCancelAfterCurrentTool = false;
	SessionUsage = FChatUsage{};

	History.Add(BuildSystemMessage(SystemPrompt));

	ResolveProvider();
	SetState(EChatSessionState::Idle, TEXT("Session started"));
}

void FChatSession::RestoreHistory(TArray<FChatMessage>&& RestoredHistory)
{
	check(IsInGameThread());

	History = MoveTemp(RestoredHistory);
	if (History.Num() == 0 || History[0].Role != EChatRole::System)
	{
		History.Insert(BuildSystemMessage(SystemPrompt), 0);
	}

	InProgressAssistant = FChatMessage{};
	bHasInProgressAssistant = false;
	PendingToolUses.Empty();
	CancelOwnedToolHandles();
	AdvanceTurnGeneration();
	bCancelAfterCurrentTool = false;
	CurrentStream.Reset();
	SessionUsage = FChatUsage{};

	ResolveProvider();
	SetState(EChatSessionState::Idle, TEXT("Session restored"));
}

void FChatSession::SendPrompt(const FString& Text)
{
	check(IsInGameThread());

	if (State != EChatSessionState::Idle)
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("ChatSession: SendPrompt called in state %d (ignored)"), (int32)State);
		return;
	}

	if (History.Num() == 0)
	{
		// Implicit NewSession if the caller forgot
		NewSession();
	}

	ResolveProvider();
	if (!CachedProvider.IsValid())
	{
		OnError.Broadcast(-1, TEXT("No chat provider is configured. Add an API key in Settings."));
		SetState(EChatSessionState::Error, TEXT("No provider"));
		return;
	}

	FString Err;
	if (!CachedProvider->ValidateConfig(Err))
	{
		OnError.Broadcast(-1, Err);
		SetState(EChatSessionState::Error, Err);
		return;
	}

	// Append user message
	FChatMessage User;
	User.Role = EChatRole::User;
	FChatContentBlock Block;
	Block.Kind = EChatContentKind::Text;
	Block.Text = Text;
	User.Content.Add(MoveTemp(Block));
	History.Add(MoveTemp(User));

	AdvanceTurnGeneration();
	StartStream();
}

void FChatSession::CancelPrompt()
{
	check(IsInGameThread());

	if (State == EChatSessionState::Streaming)
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("ChatSession: cancelling active stream"));
		AdvanceTurnGeneration();
		if (CurrentStream.IsValid())
		{
			CurrentStream->Cancel();
		}
		CurrentStream.Reset();
		bHasInProgressAssistant = false;
		InProgressAssistant = FChatMessage{};
		PendingToolUses.Empty();
		CancelOwnedToolHandles();
		bCancelAfterCurrentTool = false;
		SetState(EChatSessionState::Idle, TEXT("Cancelled"));
	}
	else if (State == EChatSessionState::ExecutingTools)
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("ChatSession: cancelling tool execution"));
		AdvanceTurnGeneration();
		bCancelAfterCurrentTool = true;
		PendingToolUses.Empty();
		CancelOwnedToolHandles();
		bCancelAfterCurrentTool = false;
		SetState(EChatSessionState::Idle, TEXT("Cancelled"));
	}
}

void FChatSession::SetModelSelection(const FString& PrefixedModelId)
{
	FString ProviderId;
	FString ModelId;
	if (ChatTypes::SplitPrefixedModelId(PrefixedModelId, ProviderId, ModelId))
	{
		SessionProviderId = ProviderId;
		SessionModelId = ModelId;
	}
	else
	{
		SessionProviderId.Empty();
		SessionModelId.Empty();
	}

	CachedProvider.Reset();
}

// ============================================================================
// Provider resolution
// ============================================================================

void FChatSession::ResolveProvider()
{
	if (!SessionProviderId.IsEmpty())
	{
		CachedProvider = FChatModelRegistry::Get().FindProvider(SessionProviderId);
	}

	if (!CachedProvider.IsValid())
	{
		CachedProvider = FChatModelRegistry::Get().GetSelectedProvider();
	}
}

// ============================================================================
// Request building
// ============================================================================

FChatRequest FChatSession::BuildRequest() const
{
	FChatRequest Request;
	Request.ModelId = SessionModelId;
	if (Request.ModelId.IsEmpty())
	{
		Request.ModelId = FChatModelRegistry::Get().GetSelectedBareModelId();
	}
	if (Request.ModelId.IsEmpty() && CachedProvider.IsValid())
	{
		// Fall back to provider default if nothing selected
		TArray<FChatModelInfo> Static = CachedProvider->GetStaticModels();
		if (Static.Num() > 0)
		{
			Request.ModelId = Static[0].ModelId;
		}
	}

	Request.Messages = History;
	Request.Tools = CollectTools();
	Request.Reasoning = ReasoningEffort;
	return Request;
}

TArray<FChatTool> FChatSession::CollectTools() const
{
	TArray<FChatTool> Result;

	// MCP-registered tools
	const TMap<FString, FMCPToolDefinition>& MCPTools = FMCPServer::Get().GetRegisteredTools();
	for (const auto& Pair : MCPTools)
	{
		FChatTool T;
		T.Name = Pair.Value.Name;
		T.Description = Pair.Value.Description;
		T.InputSchema = Pair.Value.InputSchema;
		Result.Add(MoveTemp(T));
	}

	// NeoStack registry tools (skip duplicates)
	FNeoStackToolRegistry& Registry = FNeoStackToolRegistry::Get();
	for (const FString& Name : Registry.GetToolNames())
	{
		if (MCPTools.Contains(Name)) continue;

		FNeoStackToolBase* Tool = Registry.GetTool(Name);
		if (!Tool) continue;

		FChatTool T;
		T.Name = Tool->GetName();
		T.Description = Tool->GetDescription();
		T.InputSchema = Tool->GetInputSchema();
		if (!T.InputSchema.IsValid())
		{
			T.InputSchema = MakeShared<FJsonObject>();
			T.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			T.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		}
		Result.Add(MoveTemp(T));
	}

	return Result;
}

// ============================================================================
// Streaming
// ============================================================================

void FChatSession::StartStream()
{
	check(IsInGameThread());
	check(CachedProvider.IsValid());

	const uint64 StreamTurnGeneration = ActiveTurnGeneration;
	Sink = MakeShared<FChatSessionSink>(TWeakPtr<FChatSession>(AsShared()), StreamTurnGeneration);

	// Fresh in-progress assistant message for this turn
	InProgressAssistant = FChatMessage{};
	InProgressAssistant.Role = EChatRole::Assistant;
	bHasInProgressAssistant = true;

	const FChatRequest Request = BuildRequest();
	if (Request.Messages.Num() >= 200)
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("ChatSession: large provider request (%d messages). If transport errors repeat, start a fresh session or compact context."),
			Request.Messages.Num());
	}

	UE_LOG(LogNeoStackAI, Log,
		TEXT("ChatSession: streaming completion via %s (model=%s, %d messages, %d tools)"),
		*CachedProvider->GetDisplayName(), *Request.ModelId, Request.Messages.Num(), Request.Tools.Num());

	SetState(EChatSessionState::Streaming, TEXT("Processing..."));

	TSharedRef<IChatEventSink> SinkRef = StaticCastSharedRef<IChatEventSink>(Sink.ToSharedRef());
	CurrentStream = CachedProvider->StreamCompletion(Request, SinkRef);
}

// ============================================================================
// Event handling (game thread)
// ============================================================================

void FChatSession::HandleEvent(const FChatEvent& Event, uint64 EventTurnGeneration)
{
	check(IsInGameThread());

	if (!IsCurrentTurn(EventTurnGeneration))
	{
		return;
	}

	// Pass through to any external listeners for raw events.
	OnEvent.Broadcast(Event);

	switch (Event.Kind)
	{
		case EChatEventKind::TextDelta:
		{
			if (!bHasInProgressAssistant) break;

			// Append to an existing trailing Text block, or create a new one.
			FChatContentBlock* TailText = nullptr;
			if (InProgressAssistant.Content.Num() > 0)
			{
				FChatContentBlock& Last = InProgressAssistant.Content.Last();
				if (Last.Kind == EChatContentKind::Text)
				{
					TailText = &Last;
				}
			}
			if (!TailText)
			{
				FChatContentBlock NewBlock;
				NewBlock.Kind = EChatContentKind::Text;
				InProgressAssistant.Content.Add(MoveTemp(NewBlock));
				TailText = &InProgressAssistant.Content.Last();
			}
			TailText->Text += Event.TextChunk;
			break;
		}

		case EChatEventKind::ReasoningDelta:
			// DeepSeek V4 in thinking mode 400s on follow-up turns if the
			// prior assistant message is missing its reasoning_content, so we
			// buffer it here and the OpenAI-compat serializer re-emits it
			// when building the next request.
			if (bHasInProgressAssistant)
			{
				InProgressAssistant.Reasoning += Event.TextChunk;
			}
			break;

		case EChatEventKind::ToolUseStart:
		{
			if (!bHasInProgressAssistant) break;

			FChatContentBlock Block;
			Block.Kind = EChatContentKind::ToolUse;
			Block.ToolUseId = Event.ToolUseId;
			Block.ToolName = Event.ToolName;
			InProgressAssistant.Content.Add(MoveTemp(Block));
			break;
		}

		case EChatEventKind::ToolUseArgsDelta:
		{
			if (!bHasInProgressAssistant) break;

			// Find the matching ToolUse block
			for (int32 i = InProgressAssistant.Content.Num() - 1; i >= 0; --i)
			{
				FChatContentBlock& Block = InProgressAssistant.Content[i];
				if (Block.Kind == EChatContentKind::ToolUse && Block.ToolUseId == Event.ToolUseId)
				{
					Block.RawArgsBuffer += Event.TextChunk;
					break;
				}
			}
			break;
		}

		case EChatEventKind::ToolUseEnd:
		{
			if (!bHasInProgressAssistant) break;

			// Attach parsed args to the matching block and queue for execution.
			for (int32 i = InProgressAssistant.Content.Num() - 1; i >= 0; --i)
			{
				FChatContentBlock& Block = InProgressAssistant.Content[i];
				if (Block.Kind == EChatContentKind::ToolUse && Block.ToolUseId == Event.ToolUseId)
				{
					Block.ToolArgs = Event.ToolArgs;
					PendingToolUses.Add(Block);
					OnToolCallStart.Broadcast(Block);
					break;
				}
			}
			break;
		}

		case EChatEventKind::UsageUpdate:
		{
			SessionUsage.InputTokens     += Event.Usage.InputTokens;
			SessionUsage.OutputTokens    += Event.Usage.OutputTokens;
			SessionUsage.TotalTokens     += Event.Usage.TotalTokens;
			SessionUsage.CachedTokens    += Event.Usage.CachedTokens;
			SessionUsage.ReasoningTokens += Event.Usage.ReasoningTokens;
			SessionUsage.CostAmount      += Event.Usage.CostAmount;
			if (!Event.Usage.CostCurrency.IsEmpty())
			{
				SessionUsage.CostCurrency = Event.Usage.CostCurrency;
			}
			OnUsageUpdated.Broadcast(SessionUsage);
			break;
		}

		case EChatEventKind::Error:
			OnStreamError(Event.ErrorCode, Event.ErrorMessage);
			break;

		case EChatEventKind::Done:
			OnStreamDone();
			break;
	}
}

void FChatSession::OnStreamDone()
{
	check(IsInGameThread());

	// Commit the in-progress assistant message to history
	if (bHasInProgressAssistant && InProgressAssistant.Content.Num() > 0)
	{
		History.Add(InProgressAssistant);
	}
	bHasInProgressAssistant = false;
	InProgressAssistant = FChatMessage{};

	CurrentStream.Reset();

	if (bCancelAfterCurrentTool)
	{
		// Should only happen if cancel was requested before tool execution started
		bCancelAfterCurrentTool = false;
		PendingToolUses.Empty();
		CancelOwnedToolHandles();
		SetState(EChatSessionState::Idle, TEXT("Cancelled"));
		return;
	}

	if (PendingToolUses.Num() > 0)
	{
		SetState(EChatSessionState::ExecutingTools, TEXT("Running tools..."));
		// Defer to next game-thread tick so the UI can render the Done state first
		TWeakPtr<FChatSession> Weak(AsShared());
		const uint64 ToolTurnGeneration = ActiveTurnGeneration;
		AsyncTask(ENamedThreads::GameThread, [Weak, ToolTurnGeneration]()
		{
			if (TSharedPtr<FChatSession> Pinned = Weak.Pin())
			{
				Pinned->ExecuteNextTool(ToolTurnGeneration);
			}
		});
	}
	else
	{
		SetState(EChatSessionState::Idle, TEXT("Ready"));
	}
}

void FChatSession::OnStreamError(int32 Code, const FString& Message)
{
	check(IsInGameThread());

	if (bHasInProgressAssistant)
	{
		FChatMessage ErrorAssistant;
		ErrorAssistant.Role = EChatRole::Assistant;
		FChatContentBlock ErrorBlock;
		ErrorBlock.Kind = EChatContentKind::Text;
		ErrorBlock.Text = FString::Printf(
			TEXT("The previous assistant response was interrupted before completion: %s"),
			*Message);
		ErrorAssistant.Content.Add(MoveTemp(ErrorBlock));
		History.Add(MoveTemp(ErrorAssistant));
	}

	// Discard the partial in-progress assistant message
	bHasInProgressAssistant = false;
	InProgressAssistant = FChatMessage{};
	CurrentStream.Reset();
	PendingToolUses.Empty();
	CancelOwnedToolHandles();
	bCancelAfterCurrentTool = false;

	UE_LOG(LogNeoStackAI, Error,
		TEXT("ChatSession: stream error (%d) %s"), Code, *Message);

	OnError.Broadcast(Code, Message);
	AdvanceTurnGeneration();
	// Return to Idle so the user can retry; Error state is reserved for
	// unrecoverable cases (missing provider, bad config, etc.).
	SetState(EChatSessionState::Idle, Message);
}

// ============================================================================
// Tool execution loop
// ============================================================================

void FChatSession::ExecuteNextTool(uint64 ExpectedTurnGeneration)
{
	check(IsInGameThread());

	if (!IsCurrentTurn(ExpectedTurnGeneration))
	{
		return;
	}

	if (bCancelAfterCurrentTool)
	{
		bCancelAfterCurrentTool = false;
		PendingToolUses.Empty();
		CancelOwnedToolHandles();
		SetState(EChatSessionState::Idle, TEXT("Cancelled"));
		return;
	}

	if (PendingToolUses.Num() == 0)
	{
		if (State == EChatSessionState::ExecutingTools)
		{
			// Tool cycle complete - continue the conversation
			StartStream();
		}
		return;
	}

	const FChatContentBlock Call = PendingToolUses[0];
	PendingToolUses.RemoveAt(0);

	UE_LOG(LogNeoStackAI, Log,
		TEXT("ChatSession: executing tool '%s' (id=%s)"), *Call.ToolName, *Call.ToolUseId);

	FNeoStackToolRegistry& Registry = FNeoStackToolRegistry::Get();
	const TSharedPtr<FJsonObject> Args = Call.ToolArgs.IsValid()
		? Call.ToolArgs
		: MakeShared<FJsonObject>();

	// Async dispatch: register OnComplete that finalizes the result block,
	// appends to history, broadcasts, and schedules the next iteration. If the
	// tool yields (Lua coroutines), the coroutine resumes asynchronously; the
	// game thread stays interactive throughout. OnComplete fires on the game
	// thread (registry guarantees this).
	TWeakPtr<FChatSession> Weak(AsShared());
	const uint64 ToolTurnGeneration = ExpectedTurnGeneration;
	TSharedRef<FChatToolCompletionState> CompletionState = MakeShared<FChatToolCompletionState>();

	auto FinishTool =
		[Weak, ToolUseId = Call.ToolUseId, ToolTurnGeneration, CompletionState](const FToolResult& Result, bool bFound)
		{
			TSharedPtr<FChatSession> Self = Weak.Pin();
			if (!Self)
			{
				return;
			}

			CompletionState->bCompleted = true;
			if (CompletionState->Handle.IsValid())
			{
				Self->OwnedToolHandles.Remove(CompletionState->Handle);
			}

			if (!Self->IsCurrentTurn(ToolTurnGeneration))
			{
				return;
			}

			FChatContentBlock ResultBlock;
			ResultBlock.Kind = EChatContentKind::ToolResult;
			ResultBlock.ToolUseId = ToolUseId;

			if (bFound)
			{
				ResultBlock.Text = Result.Output;
				ResultBlock.bToolError = !Result.bSuccess;
				for (const FToolResultImage& Img : Result.Images)
				{
					FACPToolResultImage AcpImg;
					AcpImg.Base64Data = Img.Base64Data;
					AcpImg.MimeType = Img.MimeType;
					AcpImg.Width = Img.Width;
					AcpImg.Height = Img.Height;
					ResultBlock.ResultImages.Add(MoveTemp(AcpImg));
				}
			}
			else
			{
				ResultBlock.Text = Result.Output;  // already populated with the "not found" message
				ResultBlock.bToolError = true;
			}

			// Append tool-role message to history
			FChatMessage ToolMsg;
			ToolMsg.Role = EChatRole::Tool;
			ToolMsg.Content.Add(ResultBlock);
			Self->History.Add(MoveTemp(ToolMsg));

			Self->OnToolCallResult.Broadcast(ToolUseId, ResultBlock);

			// Continue draining on the next tick to avoid deep stack recursion
			// when tools complete inline (sync tools fire OnComplete from inside
			// Execute, which would chain DrainPendingToolUses calls in the same
			// stack frame otherwise).
			AsyncTask(ENamedThreads::GameThread, [Weak, ToolTurnGeneration]()
			{
				if (TSharedPtr<FChatSession> Pinned = Weak.Pin())
				{
					Pinned->ExecuteNextTool(ToolTurnGeneration);
				}
			});
		};

	if (Registry.HasTool(Call.ToolName))
	{
		FNeoStackToolExecuteOptions ExecuteOptions;
		ExecuteOptions.OriginatingSessionId = SessionId;
		FNeoStackToolHandle Handle = Registry.Execute(Call.ToolName, Args,
			[FinishTool](const FToolResult& Result)
			{
				FinishTool(Result, /*bFound*/ true);
			},
			ExecuteOptions);
		CompletionState->Handle = Handle;
		if (!CompletionState->bCompleted && Handle.IsValid() && IsCurrentTurn(ToolTurnGeneration))
		{
			OwnedToolHandles.Add(Handle);
			OnStateChanged.Broadcast(
				EChatSessionState::ExecutingTools,
				Registry.IsHandleQueued(Handle) ? TEXT("Queued tool...") : TEXT("Running tool..."));
		}
	}
	else
	{
		// Tool not registered. (The MCP server mirrors the registry's tools, so
		// there's no fallback pool to consult — if it's not in the registry,
		// it doesn't exist.) Build a "not found" result and feed the same path.
		FToolResult NotFound;
		NotFound.bSuccess = false;
		NotFound.Output = FString::Printf(TEXT("Tool '%s' not found"), *Call.ToolName);
		FinishTool(NotFound, /*bFound*/ false);
	}
}

uint64 FChatSession::AdvanceTurnGeneration()
{
	return ++ActiveTurnGeneration;
}

void FChatSession::CancelOwnedToolHandles()
{
	if (OwnedToolHandles.Num() == 0)
	{
		return;
	}

	TArray<FNeoStackToolHandle> Handles;
	Handles.Reserve(OwnedToolHandles.Num());
	for (const FNeoStackToolHandle& Handle : OwnedToolHandles)
	{
		Handles.Add(Handle);
	}
	OwnedToolHandles.Empty();

	for (const FNeoStackToolHandle& Handle : Handles)
	{
		FNeoStackToolRegistry::Get().CancelExecute(Handle);
	}
}

// ============================================================================
// State transitions
// ============================================================================

void FChatSession::SetState(EChatSessionState NewState, const FString& Message)
{
	if (State == NewState) return;
	State = NewState;
	OnStateChanged.Broadcast(NewState, Message);
}
