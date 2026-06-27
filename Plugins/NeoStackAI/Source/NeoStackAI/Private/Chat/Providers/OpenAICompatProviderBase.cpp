// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Chat/Providers/OpenAICompatProviderBase.h"
#include "Chat/Streaming/HttpStreamHandle.h"
#include "Chat/Streaming/SseLineReader.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================
// Per-stream state
// ============================================================================

struct FOpenAICompatProviderBase::FStreamContext
{
	// Destination for events. Held as TSharedRef so it lives until the stream
	// finishes, regardless of who holds the session.
	TSharedRef<IChatEventSink> Sink;

	// The handle we handed back to the caller. Used to mark complete and to
	// skip event emission after cancellation.
	TSharedRef<FHttpStreamHandle> Handle;

	// Incremental SSE buffer. Fed in Progress64 callbacks.
	FSseLineReader Reader;

	// Tracks the length of response bytes already consumed, so Progress64
	// only feeds newly arrived bytes to the reader.
	int32 LastProcessedLength = 0;

	// Open tool calls, indexed by the OpenAI `tool_calls[].index` field.
	// Entries are created on first sighting and closed (emitting ToolUseEnd)
	// on stream completion.
	TMap<int32, FChatContentBlock> OpenToolCallsByIndex;

	// Track which indices we've already emitted ToolUseStart for (so we
	// don't re-emit it on every subsequent delta for the same index).
	TSet<int32> StartedToolCallIndices;

	// Set to true the first time Done or Error is emitted, preventing
	// double-emission from overlapping progress + completion callbacks.
	bool bTerminated = false;

	// Total reasoning text we've already emitted as ReasoningDelta events on
	// this stream. Used to deduplicate two ways at once:
	//   1. OpenRouter streams `reasoning` AND `reasoning_details[].text` with
	//      the same content per chunk — without this we'd double-emit.
	//   2. OpenRouter's reasoning_details[].text is *cumulative* (each chunk
	//      contains the full reasoning so far) so we have to subtract what
	//      we've already emitted and only forward the new tail.
	FString EmittedReasoning;

	// Human-readable provider label used in log messages.
	FString ProviderLogName;

	FStreamContext(TSharedRef<IChatEventSink> InSink, TSharedRef<FHttpStreamHandle> InHandle)
		: Sink(InSink)
		, Handle(InHandle)
	{}
};

// ============================================================================
// Defaults
// ============================================================================

FChatProviderCapabilities FOpenAICompatProviderBase::GetCapabilities() const
{
	FChatProviderCapabilities Caps;
	Caps.bSupportsTools = true;
	Caps.bSupportsStreaming = true;
	Caps.bSupportsReasoning = false;
	Caps.bSupportsImages = false;
	return Caps;
}

bool FOpenAICompatProviderBase::ValidateConfig(FString& OutError) const
{
	if (RequiresApiKey() && GetSettingsApiKey().IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s requires an API key"), *GetDisplayName());
		return false;
	}
	return true;
}

FString FOpenAICompatProviderBase::GetSettingsApiKey() const
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->GetChatProviderApiKey(GetId());
	}
	return FString();
}

FString FOpenAICompatProviderBase::GetSettingsBaseUrlOverride() const
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->GetChatProviderBaseUrlOverride(GetId());
	}
	return FString();
}

void FOpenAICompatProviderBase::ConfigureHeaders(
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
	const FString& ApiKey) const
{
	HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
}

TArray<FChatModelInfo> FOpenAICompatProviderBase::ParseDiscoveryResponse(const FString& ResponseBody) const
{
	TArray<FChatModelInfo> Models;

	TSharedPtr<FJsonObject> JsonRoot;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
	{
		return Models;
	}

	const TArray<TSharedPtr<FJsonValue>>* DataArray;
	if (!JsonRoot->TryGetArrayField(TEXT("data"), DataArray))
	{
		return Models;
	}

	for (const TSharedPtr<FJsonValue>& ModelVal : *DataArray)
	{
		const TSharedPtr<FJsonObject> ModelObj = ModelVal->AsObject();
		if (!ModelObj.IsValid()) continue;

		FChatModelInfo Info;
		ModelObj->TryGetStringField(TEXT("id"), Info.ModelId);
		if (Info.ModelId.IsEmpty()) continue;

		if (!ModelObj->TryGetStringField(TEXT("name"), Info.DisplayName) || Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = Info.ModelId;
		}
		ModelObj->TryGetStringField(TEXT("description"), Info.Description);

		Info.ProviderId = GetId();
		Info.ProviderDisplayName = GetDisplayName();
		Info.bSupportsReasoning = ModelSupportsReasoning(Info.ModelId);

		Models.Add(MoveTemp(Info));
	}

	Models.Sort([](const FChatModelInfo& A, const FChatModelInfo& B)
	{
		return A.DisplayName < B.DisplayName;
	});

	return Models;
}

FString FOpenAICompatProviderBase::FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody) const
{
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
	if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
	{
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if (Json->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && ErrorObj->IsValid())
		{
			FString InnerMsg;
			if ((*ErrorObj)->TryGetStringField(TEXT("message"), InnerMsg) && !InnerMsg.IsEmpty())
			{
				return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *InnerMsg);
			}
		}

		FString TopMsg;
		if (Json->TryGetStringField(TEXT("error"), TopMsg) && !TopMsg.IsEmpty())
		{
			return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *TopMsg);
		}
		if (Json->TryGetStringField(TEXT("message"), TopMsg) && !TopMsg.IsEmpty())
		{
			return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *TopMsg);
		}
	}

	return FString::Printf(TEXT("%s API error %d: %s"), *GetDisplayName(), ResponseCode, *ResponseBody.Left(800));
}

// ============================================================================
// StreamCompletion
// ============================================================================

TSharedRef<IChatStreamHandle> FOpenAICompatProviderBase::StreamCompletion(
	const FChatRequest& Request,
	TSharedRef<IChatEventSink> Sink)
{
	// Resolve URL and auth from settings, falling back to provider defaults.
	FString ApiKey = GetSettingsApiKey();
	FString BaseUrl = GetSettingsBaseUrlOverride();
	if (BaseUrl.IsEmpty())
	{
		BaseUrl = GetDefaultBaseUrl();
	}
	const FString FullUrl = BaseUrl + GetChatCompletionsPath();

	// Build request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(FullUrl);
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		HttpRequest->SetTimeout(FMath::Max(30.0f, Settings->ChatHttpTimeoutSeconds));
	}
	ConfigureHeaders(HttpRequest, ApiKey);
	HttpRequest->SetContentAsString(BuildRequestBody(Request));

	// Stream handle and per-stream context
	TSharedRef<FHttpStreamHandle> Handle = MakeShared<FHttpStreamHandle>(HttpRequest);
	TSharedRef<FStreamContext> Context = MakeShared<FStreamContext>(Sink, Handle);
	Context->ProviderLogName = GetDisplayName();

	{
		const FString PostMsg = FString::Printf(
			TEXT("ChatProvider [%s]: POST %s (model=%s)"),
			*Context->ProviderLogName, *FullUrl, *Request.ModelId);
		UE_LOG(LogNeoStackAI, Log, TEXT("%s"), *PostMsg);
	}

	// Progress callback: feed new bytes to the reader, process lines
	HttpRequest->OnRequestProgress64().BindLambda(
		[Context](FHttpRequestPtr Req, uint64 /*BytesSent*/, uint64 /*BytesReceived*/)
		{
			if (Context->bTerminated || !Context->Handle->IsActive()) return;

			FHttpResponsePtr Response = Req->GetResponse();
			if (!Response.IsValid()) return;

			const FString Content = Response->GetContentAsString();
			if (Content.Len() > Context->LastProcessedLength)
			{
				const FString NewBytes = Content.Mid(Context->LastProcessedLength);
				Context->LastProcessedLength = Content.Len();
				ProcessSseChunk(Context, NewBytes);
			}
		});

	// Completion callback: handle error, finalize open tool calls, emit Done
	HttpRequest->OnProcessRequestComplete().BindLambda(
		[this, Context](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
		{
			if (Context->bTerminated) return;
			if (!Context->Handle->IsActive())
			{
				Context->bTerminated = true;
				Context->Handle->MarkComplete();
				return;
			}

			// Flush any remaining buffered data
			if (Resp.IsValid())
			{
				const FString Content = Resp->GetContentAsString();
				if (Content.Len() > Context->LastProcessedLength)
				{
					const FString NewBytes = Content.Mid(Context->LastProcessedLength);
					Context->LastProcessedLength = Content.Len();
					ProcessSseChunk(Context, NewBytes);
				}
			}

			// Error path — do NOT finalize partial tool calls here. Emitting
			// ToolUseEnd on a dead transport queues tools that never run and
			// leaves the UI stuck on an in-progress execute_script spinner.
			if (!bSuccess || !Resp.IsValid())
			{
				FChatEvent Err;
				Err.Kind = EChatEventKind::Error;
				Err.ErrorMessage = TEXT("HTTP transport failed");
				if (Req.IsValid())
				{
					const EHttpFailureReason FailureReason = Req->GetFailureReason();
					if (FailureReason != EHttpFailureReason::None)
					{
						Err.ErrorMessage = FString::Printf(
							TEXT("HTTP transport failed (%s)"),
							LexToString(FailureReason));
					}
				}
				Err.ErrorCode = -1;
				Emit(Context, Err);

				Context->bTerminated = true;
				Context->Handle->MarkComplete();
				return;
			}

			const int32 Code = Resp->GetResponseCode();
			if (Code != 200)
			{
				FChatEvent Err;
				Err.Kind = EChatEventKind::Error;
				Err.ErrorMessage = FormatErrorMessage(Code, Resp->GetContentAsString());
				Err.ErrorCode = Code;
				Emit(Context, Err);

				Context->bTerminated = true;
				Context->Handle->MarkComplete();
				return;
			}

			// Close any still-open tool calls on successful completion only.
			FinalizeOpenToolCalls(Context);

			// Clean completion
			FChatEvent DoneEvent;
			DoneEvent.Kind = EChatEventKind::Done;
			Emit(Context, DoneEvent);

			Context->bTerminated = true;
			Context->Handle->MarkComplete();
		});

	if (!HttpRequest->ProcessRequest())
	{
		// Synthesize an immediate error event on the caller's thread
		FChatEvent Err;
		Err.Kind = EChatEventKind::Error;
		Err.ErrorMessage = FString::Printf(TEXT("Failed to initiate request to %s"), *GetDisplayName());
		Err.ErrorCode = -1;
		Sink->OnEvent(Err);
		Handle->MarkComplete();
	}

	return Handle;
}

// ============================================================================
// Request body construction
// ============================================================================

FString FOpenAICompatProviderBase::BuildRequestBody(const FChatRequest& Request) const
{
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();

	Body->SetStringField(TEXT("model"), Request.ModelId);
	Body->SetBoolField(TEXT("stream"), true);
	Body->SetNumberField(TEXT("temperature"), Request.Temperature);
	Body->SetNumberField(TEXT("max_tokens"), Request.MaxTokens);

	Body->SetArrayField(TEXT("messages"), BuildMessagesArray(Request.Messages));

	TArray<TSharedPtr<FJsonValue>> ToolsArray = BuildToolsArray(Request.Tools);
	if (ToolsArray.Num() > 0)
	{
		Body->SetArrayField(TEXT("tools"), ToolsArray);
	}

	// Reasoning (provider-specific format)
	if (Request.Reasoning != EReasoningEffort::None && ModelSupportsReasoning(Request.ModelId))
	{
		FormatReasoningParams(Body, Request.Reasoning);
	}

	// Include usage in stream chunks (OpenRouter / OpenAI honor this)
	TSharedRef<FJsonObject> StreamOptions = MakeShared<FJsonObject>();
	StreamOptions->SetBoolField(TEXT("include_usage"), true);
	Body->SetObjectField(TEXT("stream_options"), StreamOptions);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Body, Writer);
	return Out;
}

TArray<TSharedPtr<FJsonValue>> FOpenAICompatProviderBase::BuildMessagesArray(
	const TArray<FChatMessage>& Messages) const
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Messages.Num());

	for (const FChatMessage& Msg : Messages)
	{
		// Tool role: canonical [ToolResult] block -> {role:"tool", tool_call_id, content}
		if (Msg.Role == EChatRole::Tool)
		{
			for (const FChatContentBlock& Block : Msg.Content)
			{
				if (Block.Kind != EChatContentKind::ToolResult) continue;

				TSharedRef<FJsonObject> ToolMsg = MakeShared<FJsonObject>();
				ToolMsg->SetStringField(TEXT("role"), TEXT("tool"));
				ToolMsg->SetStringField(TEXT("tool_call_id"), Block.ToolUseId);
				ToolMsg->SetStringField(TEXT("content"), Block.Text);
				Result.Add(MakeShared<FJsonValueObject>(ToolMsg));
			}
			continue;
		}

		// Assistant role with tool calls: {role, content, tool_calls}
		if (Msg.Role == EChatRole::Assistant)
		{
			FString TextContent;
			TArray<TSharedPtr<FJsonValue>> ToolCallsArr;

			for (const FChatContentBlock& Block : Msg.Content)
			{
				if (Block.Kind == EChatContentKind::Text)
				{
					TextContent += Block.Text;
				}
				else if (Block.Kind == EChatContentKind::ToolUse)
				{
					TSharedRef<FJsonObject> Call = MakeShared<FJsonObject>();
					Call->SetStringField(TEXT("id"), Block.ToolUseId);
					Call->SetStringField(TEXT("type"), TEXT("function"));

					TSharedRef<FJsonObject> Func = MakeShared<FJsonObject>();
					Func->SetStringField(TEXT("name"), Block.ToolName);

					FString ArgsString = Block.RawArgsBuffer;
					if (ArgsString.IsEmpty() && Block.ToolArgs.IsValid())
					{
						TSharedRef<TJsonWriter<>> ArgWriter = TJsonWriterFactory<>::Create(&ArgsString);
						FJsonSerializer::Serialize(Block.ToolArgs.ToSharedRef(), ArgWriter);
					}
					if (ArgsString.IsEmpty()) ArgsString = TEXT("{}");
					Func->SetStringField(TEXT("arguments"), ArgsString);
					Call->SetObjectField(TEXT("function"), Func);

					ToolCallsArr.Add(MakeShared<FJsonValueObject>(Call));
				}
			}

			TSharedRef<FJsonObject> AsstMsg = MakeShared<FJsonObject>();
			AsstMsg->SetStringField(TEXT("role"), TEXT("assistant"));

			if (!TextContent.IsEmpty())
			{
				AsstMsg->SetStringField(TEXT("content"), TextContent);
			}
			else if (ToolCallsArr.Num() > 0)
			{
				AsstMsg->SetField(TEXT("content"), MakeShared<FJsonValueNull>());
			}
			else
			{
				AsstMsg->SetStringField(TEXT("content"), TEXT(""));
			}

			if (ToolCallsArr.Num() > 0)
			{
				AsstMsg->SetArrayField(TEXT("tool_calls"), ToolCallsArr);
			}

			// Re-emit reasoning for providers that require it on follow-up
			// turns (DeepSeek V4 in thinking mode: drops the conversation with
			// HTTP 400 if this is missing). We send both `reasoning_content`
			// (DeepSeek / OpenAI-compat name) and `reasoning_details[]`
			// (OpenRouter's wrapper shape) so downstream translators recognize
			// whichever one they expect. Providers that don't understand
			// either field ignore the extra keys.
			if (!Msg.Reasoning.IsEmpty())
			{
				AsstMsg->SetStringField(TEXT("reasoning_content"), Msg.Reasoning);

				TArray<TSharedPtr<FJsonValue>> ReasoningDetails;
				TSharedRef<FJsonObject> DetailObj = MakeShared<FJsonObject>();
				DetailObj->SetStringField(TEXT("type"), TEXT("reasoning.text"));
				DetailObj->SetStringField(TEXT("text"), Msg.Reasoning);
				ReasoningDetails.Add(MakeShared<FJsonValueObject>(DetailObj));
				AsstMsg->SetArrayField(TEXT("reasoning_details"), ReasoningDetails);
			}

			Result.Add(MakeShared<FJsonValueObject>(AsstMsg));
			continue;
		}

		// System or User: check for multimodal (image blocks)
		bool bHasImage = false;
		for (const FChatContentBlock& Block : Msg.Content)
		{
			if (Block.Kind == EChatContentKind::Image) { bHasImage = true; break; }
		}

		TSharedRef<FJsonObject> WireMsg = MakeShared<FJsonObject>();
		WireMsg->SetStringField(TEXT("role"), ChatTypes::RoleToOpenAIString(Msg.Role));

		if (bHasImage)
		{
			TArray<TSharedPtr<FJsonValue>> Parts;
			for (const FChatContentBlock& Block : Msg.Content)
			{
				if (Block.Kind == EChatContentKind::Text && !Block.Text.IsEmpty())
				{
					TSharedRef<FJsonObject> TextPart = MakeShared<FJsonObject>();
					TextPart->SetStringField(TEXT("type"), TEXT("text"));
					TextPart->SetStringField(TEXT("text"), Block.Text);
					Parts.Add(MakeShared<FJsonValueObject>(TextPart));
				}
				else if (Block.Kind == EChatContentKind::Image)
				{
					TSharedRef<FJsonObject> ImagePart = MakeShared<FJsonObject>();
					ImagePart->SetStringField(TEXT("type"), TEXT("image_url"));

					TSharedRef<FJsonObject> Url = MakeShared<FJsonObject>();
					const FString Mime = Block.ImageMime.IsEmpty() ? TEXT("image/png") : Block.ImageMime;
					Url->SetStringField(TEXT("url"),
						FString::Printf(TEXT("data:%s;base64,%s"), *Mime, *Block.ImageBase64));
					ImagePart->SetObjectField(TEXT("image_url"), Url);

					Parts.Add(MakeShared<FJsonValueObject>(ImagePart));
				}
			}
			WireMsg->SetArrayField(TEXT("content"), Parts);
		}
		else
		{
			FString FlatText;
			for (const FChatContentBlock& Block : Msg.Content)
			{
				if (Block.Kind == EChatContentKind::Text)
				{
					FlatText += Block.Text;
				}
			}
			WireMsg->SetStringField(TEXT("content"), FlatText);
		}

		Result.Add(MakeShared<FJsonValueObject>(WireMsg));
	}

	return Result;
}

TArray<TSharedPtr<FJsonValue>> FOpenAICompatProviderBase::BuildToolsArray(
	const TArray<FChatTool>& Tools) const
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Tools.Num());

	for (const FChatTool& Tool : Tools)
	{
		TSharedRef<FJsonObject> Wrapper = MakeShared<FJsonObject>();
		Wrapper->SetStringField(TEXT("type"), TEXT("function"));

		TSharedRef<FJsonObject> Func = MakeShared<FJsonObject>();
		Func->SetStringField(TEXT("name"), Tool.Name);
		Func->SetStringField(TEXT("description"), Tool.Description);

		if (Tool.InputSchema.IsValid())
		{
			Func->SetObjectField(TEXT("parameters"), Tool.InputSchema);
		}
		else
		{
			TSharedRef<FJsonObject> Empty = MakeShared<FJsonObject>();
			Empty->SetStringField(TEXT("type"), TEXT("object"));
			Empty->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			Func->SetObjectField(TEXT("parameters"), Empty);
		}

		Wrapper->SetObjectField(TEXT("function"), Func);
		Result.Add(MakeShared<FJsonValueObject>(Wrapper));
	}

	return Result;
}

// ============================================================================
// SSE parsing
// ============================================================================

void FOpenAICompatProviderBase::ProcessSseChunk(TSharedRef<FStreamContext> Context, const FString& NewBytes)
{
	Context->Reader.Feed(NewBytes);

	FString Line;
	while (Context->Reader.NextLine(Line))
	{
		if (Line.IsEmpty() || Line.StartsWith(TEXT(":"))) continue; // blank / comment
		if (!Line.StartsWith(TEXT("data: "))) continue;

		const FString Data = Line.Mid(6);
		if (Data == TEXT("[DONE]")) continue; // terminal handled by HTTP completion

		ProcessSseDataLine(Context, Data);
	}
}

void FOpenAICompatProviderBase::ProcessSseDataLine(TSharedRef<FStreamContext> Context, const FString& Json)
{
	TSharedPtr<FJsonObject> Chunk;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Chunk) || !Chunk.IsValid())
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("ChatProvider [%s]: Malformed SSE chunk (skipped)"),
			*Context->ProviderLogName);
		return;
	}

	// Usage object is usually on a standalone chunk with empty choices
	ParseAndEmitUsage(Context, Chunk);

	const TArray<TSharedPtr<FJsonValue>>* ChoicesArr;
	if (!Chunk->TryGetArrayField(TEXT("choices"), ChoicesArr) || ChoicesArr->Num() == 0)
	{
		return;
	}

	const TSharedPtr<FJsonObject> Choice = (*ChoicesArr)[0]->AsObject();
	if (!Choice.IsValid()) return;

	const TSharedPtr<FJsonObject> Delta = Choice->GetObjectField(TEXT("delta"));
	if (!Delta.IsValid()) return;

	// Regular text content
	FString Content;
	if (Delta->TryGetStringField(TEXT("content"), Content) && !Content.IsEmpty())
	{
		FChatEvent Ev;
		Ev.Kind = EChatEventKind::TextDelta;
		Ev.TextChunk = Content;
		Emit(Context, Ev);
	}

	// Reasoning content. OpenRouter emits both `reasoning` (flat string) and
	// `reasoning_details[].text` per chunk with the same content, AND each
	// chunk's text is cumulative (contains everything so far, not a delta).
	// We pick one source per chunk (prefer the structured array when present)
	// and route through EmitReasoningIncremental so we only forward the new
	// tail. This handles both cumulative streams (subtract what we've seen)
	// and delta streams (append) without provider-specific branching.
	FString FullReasoning;
	const TArray<TSharedPtr<FJsonValue>>* ReasoningArr;
	if (Delta->TryGetArrayField(TEXT("reasoning_details"), ReasoningArr))
	{
		for (const TSharedPtr<FJsonValue>& DetailVal : *ReasoningArr)
		{
			const TSharedPtr<FJsonObject> DetailObj = DetailVal->AsObject();
			if (!DetailObj.IsValid()) continue;

			FString Type, Text;
			DetailObj->TryGetStringField(TEXT("type"), Type);
			if (Type == TEXT("reasoning.text"))
			{
				DetailObj->TryGetStringField(TEXT("text"), Text);
			}
			else if (Type == TEXT("reasoning.summary"))
			{
				DetailObj->TryGetStringField(TEXT("summary"), Text);
			}
			if (!Text.IsEmpty()) FullReasoning += Text;
		}
	}
	if (FullReasoning.IsEmpty())
	{
		Delta->TryGetStringField(TEXT("reasoning_content"), FullReasoning);
	}
	if (FullReasoning.IsEmpty())
	{
		// Fall back to the flat field for providers that don't ship reasoning_details.
		Delta->TryGetStringField(TEXT("reasoning"), FullReasoning);
	}
	if (!FullReasoning.IsEmpty())
	{
		EmitReasoningIncremental(Context, FullReasoning);
	}

	// Tool call deltas
	const TArray<TSharedPtr<FJsonValue>>* ToolCallsArr;
	if (Delta->TryGetArrayField(TEXT("tool_calls"), ToolCallsArr))
	{
		for (const TSharedPtr<FJsonValue>& TcVal : *ToolCallsArr)
		{
			const TSharedPtr<FJsonObject> TcObj = TcVal->AsObject();
			if (!TcObj.IsValid()) continue;

			int32 Index = 0;
			TcObj->TryGetNumberField(TEXT("index"), Index);

			FChatContentBlock& Block = Context->OpenToolCallsByIndex.FindOrAdd(Index);
			Block.Kind = EChatContentKind::ToolUse;

			FString IdField;
			if (TcObj->TryGetStringField(TEXT("id"), IdField) && !IdField.IsEmpty())
			{
				Block.ToolUseId = IdField;
			}

			const TSharedPtr<FJsonObject> Func = TcObj->GetObjectField(TEXT("function"));
			if (Func.IsValid())
			{
				FString NameField;
				if (Func->TryGetStringField(TEXT("name"), NameField) && !NameField.IsEmpty())
				{
					Block.ToolName = NameField;
				}

				// Once we have both id and name, emit ToolUseStart exactly once
				if (!Context->StartedToolCallIndices.Contains(Index)
					&& !Block.ToolUseId.IsEmpty()
					&& !Block.ToolName.IsEmpty())
				{
					Context->StartedToolCallIndices.Add(Index);

					FChatEvent Ev;
					Ev.Kind = EChatEventKind::ToolUseStart;
					Ev.ToolUseId = Block.ToolUseId;
					Ev.ToolName = Block.ToolName;
					Emit(Context, Ev);
				}

				FString ArgsFragment;
				if (Func->TryGetStringField(TEXT("arguments"), ArgsFragment) && !ArgsFragment.IsEmpty())
				{
					Block.RawArgsBuffer += ArgsFragment;

					if (Context->StartedToolCallIndices.Contains(Index))
					{
						FChatEvent Ev;
						Ev.Kind = EChatEventKind::ToolUseArgsDelta;
						Ev.ToolUseId = Block.ToolUseId;
						Ev.TextChunk = ArgsFragment;
						Emit(Context, Ev);
					}
				}
			}
		}
	}
}

void FOpenAICompatProviderBase::ParseAndEmitUsage(
	TSharedRef<FStreamContext> Context,
	const TSharedPtr<FJsonObject>& Chunk)
{
	const TSharedPtr<FJsonObject>* UsageObjPtr = nullptr;
	if (!Chunk->TryGetObjectField(TEXT("usage"), UsageObjPtr) || !UsageObjPtr || !UsageObjPtr->IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject>& UsageObj = *UsageObjPtr;

	FChatUsage Usage;
	UsageObj->TryGetNumberField(TEXT("prompt_tokens"),     Usage.InputTokens);
	UsageObj->TryGetNumberField(TEXT("completion_tokens"), Usage.OutputTokens);
	UsageObj->TryGetNumberField(TEXT("total_tokens"),      Usage.TotalTokens);

	const TSharedPtr<FJsonObject>* PromptDetails = nullptr;
	if (UsageObj->TryGetObjectField(TEXT("prompt_tokens_details"), PromptDetails) && PromptDetails && PromptDetails->IsValid())
	{
		(*PromptDetails)->TryGetNumberField(TEXT("cached_tokens"), Usage.CachedTokens);
	}

	const TSharedPtr<FJsonObject>* CompletionDetails = nullptr;
	if (UsageObj->TryGetObjectField(TEXT("completion_tokens_details"), CompletionDetails) && CompletionDetails && CompletionDetails->IsValid())
	{
		(*CompletionDetails)->TryGetNumberField(TEXT("reasoning_tokens"), Usage.ReasoningTokens);
	}

	double Cost = 0.0;
	if (UsageObj->TryGetNumberField(TEXT("cost"), Cost) && Cost > 0.0)
	{
		Usage.CostAmount = Cost;
		Usage.CostCurrency = TEXT("USD");
	}
	else
	{
		const TSharedPtr<FJsonObject>* CostDetails = nullptr;
		if (UsageObj->TryGetObjectField(TEXT("cost_details"), CostDetails) && CostDetails && CostDetails->IsValid())
		{
			double Upstream = 0.0;
			if ((*CostDetails)->TryGetNumberField(TEXT("upstream_inference_cost"), Upstream) && Upstream > 0.0)
			{
				Usage.CostAmount = Upstream;
				Usage.CostCurrency = TEXT("USD");
			}
		}
	}

	FChatEvent Ev;
	Ev.Kind = EChatEventKind::UsageUpdate;
	Ev.Usage = Usage;
	Emit(Context, Ev);
}

void FOpenAICompatProviderBase::FinalizeOpenToolCalls(TSharedRef<FStreamContext> Context)
{
	for (auto& Pair : Context->OpenToolCallsByIndex)
	{
		FChatContentBlock& Block = Pair.Value;

		// Parse the accumulated args buffer into a JSON object
		if (!Block.RawArgsBuffer.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Block.RawArgsBuffer);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				Block.ToolArgs = Parsed;
			}
		}

		if (!Block.ToolArgs.IsValid())
		{
			Block.ToolArgs = MakeShared<FJsonObject>();
		}

		FChatEvent Ev;
		Ev.Kind = EChatEventKind::ToolUseEnd;
		Ev.ToolUseId = Block.ToolUseId;
		Ev.ToolName = Block.ToolName;
		Ev.ToolArgs = Block.ToolArgs;
		Emit(Context, Ev);
	}

	Context->OpenToolCallsByIndex.Empty();
	Context->StartedToolCallIndices.Empty();
}

void FOpenAICompatProviderBase::Emit(TSharedRef<FStreamContext> Context, const FChatEvent& Event)
{
	if (Context->bTerminated || !Context->Handle->IsActive()) return;
	Context->Sink->OnEvent(Event);
}

void FOpenAICompatProviderBase::EmitReasoningIncremental(
	TSharedRef<FStreamContext> Context, const FString& FullReasoning)
{
	if (FullReasoning.IsEmpty()) return;

	const FString& Prior = Context->EmittedReasoning;
	FString DeltaText;

	if (FullReasoning.Equals(Prior, ESearchCase::CaseSensitive))
	{
		// Identical chunk — nothing new to forward.
		return;
	}
	if (FullReasoning.StartsWith(Prior, ESearchCase::CaseSensitive)
		&& FullReasoning.Len() > Prior.Len())
	{
		// Cumulative stream: provider re-sent everything-so-far. Emit only
		// the new tail.
		DeltaText = FullReasoning.Mid(Prior.Len());
		Context->EmittedReasoning = FullReasoning;
	}
	else
	{
		// Delta stream (or unrelated text — provider reset / new turn).
		// Emit as-is and append; that way both modes converge correctly.
		DeltaText = FullReasoning;
		Context->EmittedReasoning += FullReasoning;
	}

	if (DeltaText.IsEmpty()) return;

	FChatEvent Ev;
	Ev.Kind = EChatEventKind::ReasoningDelta;
	Ev.TextChunk = DeltaText;
	Emit(Context, Ev);
}
