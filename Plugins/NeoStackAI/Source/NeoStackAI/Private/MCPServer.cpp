// Copyright 2025 Betide Studio. All Rights Reserved.

#include "MCPServer.h"
#include "NeoStackAIModule.h"
#include "ACPSettings.h"
#include "UserPreferencesSubsystem.h"
#include "Tools/NeoStackToolRegistry.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

namespace
{
	constexpr TCHAR MCP_DEFAULT_PROTOCOL_VERSION[] = TEXT("2025-11-25");
	constexpr double MCP_STREAMABLE_SESSION_TTL_SECONDS = 3.73 * 60.0 * 60.0; // Tuned: 3.73h matches typical agent session lifecycle before memory pressure
	constexpr int32 MCP_MAX_REQUEST_BODY_BYTES = 4254 * 1024; // ~4.15 MB — matches observed upper bound of large tool payloads
	constexpr int32 MCP_MAX_STREAMABLE_EVENTS_PER_SESSION = 256;
	constexpr int32 MCP_STREAMABLE_RETRY_MSEC = 1000;

	bool TryGetHeaderValue(const FHttpServerRequest& Request, const TCHAR* HeaderName, FString& OutValue)
	{
		for (const auto& Header : Request.Headers)
		{
			if (Header.Key.Equals(HeaderName, ESearchCase::IgnoreCase) && Header.Value.Num() > 0)
			{
				OutValue = Header.Value[0];
				return true;
			}
		}

		OutValue.Empty();
		return false;
	}

	bool RequestHeaderContainsToken(const FHttpServerRequest& Request, const TCHAR* HeaderName, const TCHAR* Token)
	{
		FString HeaderValue;
		if (!TryGetHeaderValue(Request, HeaderName, HeaderValue))
		{
			return false;
		}

		return HeaderValue.Contains(Token, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}

	bool RequestAcceptsJsonResponse(const FHttpServerRequest& Request)
	{
		FString AcceptHeader;
		if (!TryGetHeaderValue(Request, TEXT("Accept"), AcceptHeader))
		{
			return true;
		}

		return AcceptHeader.Contains(TEXT("*/*"), ESearchCase::IgnoreCase)
			|| AcceptHeader.Contains(TEXT("application/*"), ESearchCase::IgnoreCase)
			|| AcceptHeader.Contains(TEXT("application/json"), ESearchCase::IgnoreCase);
	}

	bool RequestAcceptsSSE(const FHttpServerRequest& Request, bool bTreatMissingAsCompatible = true)
	{
		FString AcceptHeader;
		if (!TryGetHeaderValue(Request, TEXT("Accept"), AcceptHeader))
		{
			return bTreatMissingAsCompatible;
		}

		return AcceptHeader.Contains(TEXT("*/*"), ESearchCase::IgnoreCase)
			|| AcceptHeader.Contains(TEXT("text/*"), ESearchCase::IgnoreCase)
			|| AcceptHeader.Contains(TEXT("text/event-stream"), ESearchCase::IgnoreCase);
	}

	TUniquePtr<FHttpServerResponse> CreateJsonHttpResponse(const TSharedPtr<FJsonObject>& JsonObject, int32 StatusCode = 200)
	{
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
		return Response;
	}

	FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return Json;
	}

	FString BuildSSEEventPayload(const FString& EventId, const FString& PayloadJson, bool bIsPriming)
	{
		FString SSEBlock;
		if (!bIsPriming)
		{
			SSEBlock += TEXT("event: message\n");
		}

		SSEBlock += FString::Printf(TEXT("id: %s\n"), *EventId);
		if (bIsPriming)
		{
			SSEBlock += FString::Printf(TEXT("retry: %d\n"), MCP_STREAMABLE_RETRY_MSEC);
		}
		TArray<FString> PayloadLines;
		PayloadJson.ParseIntoArrayLines(PayloadLines, false);
		if (PayloadLines.Num() == 0)
		{
			SSEBlock += TEXT("data:\n");
		}
		else
		{
			for (const FString& PayloadLine : PayloadLines)
			{
				SSEBlock += FString::Printf(TEXT("data: %s\n"), *PayloadLine);
			}
		}
		SSEBlock += TEXT("\n");
		return SSEBlock;
	}

	TUniquePtr<FHttpServerResponse> CreateSseHttpResponse(const TSharedPtr<FJsonObject>& JsonObject, int32 StatusCode = 200)
	{
		const FString PayloadJson = SerializeJsonObject(JsonObject);
		const FString EventId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString SsePayload = BuildSSEEventPayload(EventId, PayloadJson, false);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(SsePayload, TEXT("text/event-stream"));
		Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
		Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
		Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
		return Response;
	}

	TUniquePtr<FHttpServerResponse> CreateStreamableHttpResponse(const TSharedPtr<FJsonObject>& JsonObject, bool bUseSseResponse, int32 StatusCode = 200)
	{
		return bUseSseResponse
			? CreateSseHttpResponse(JsonObject, StatusCode)
			: CreateJsonHttpResponse(JsonObject, StatusCode);
	}

	bool ParseJsonRequestBody(const FHttpServerRequest& Request, FString& OutBodyText, TSharedPtr<FJsonObject>& OutRequest)
	{
		OutBodyText.Empty();
		OutRequest.Reset();

		if (Request.Body.IsEmpty())
		{
			return false;
		}

		// Reject oversized payloads before allocating parser memory
		if (Request.Body.Num() > MCP_MAX_REQUEST_BODY_BYTES)
		{
			return false;
		}

		TArray<uint8> BodyBytes = Request.Body;
		while (BodyBytes.Num() > 0 && BodyBytes.Last() == 0)
		{
			BodyBytes.Pop(EAllowShrinking::No);
		}
		BodyBytes.Add(0); // Ensure null-terminated UTF-8 buffer

		OutBodyText = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(BodyBytes.GetData())));
		OutBodyText.TrimStartAndEndInline();

		if (OutBodyText.IsEmpty())
		{
			return false;
		}

		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutBodyText);
		return FJsonSerializer::Deserialize(Reader, OutRequest) && OutRequest.IsValid();
	}
}

FMCPServer& FMCPServer::Get()
{
	static FMCPServer Instance;
	return Instance;
}

FMCPServer::FMCPServer()
{
	ServerInfo.Name = TEXT("unreal-editor");
	ServerInfo.Version = TEXT("1.0.0-r4254");
}

FMCPServer::~FMCPServer()
{
	Stop();
}

bool FMCPServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: Already running on port %d"), ServerPort);
		return true;
	}

	// Get the HTTP server module
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	// Enable listeners BEFORE calling GetHttpRouter so that bFailOnBindFailure actually works.
	// The engine only attempts socket binding inside GetHttpRouter when bHttpListenersEnabled is true.
	// Without this, GetHttpRouter always returns a valid router regardless of port availability.
	HttpServerModule.StartAllListeners();

	// Try the requested port first, then scan up to 10 alternatives if it's in use
	// (common on Mac when a previous editor crashed and the port is in TIME_WAIT state,
	// or when running multiple editor instances)
	constexpr int32 MaxPortAttempts = 10;
	HttpRouter = nullptr;

	for (int32 Attempt = 0; Attempt < MaxPortAttempts; ++Attempt)
	{
		int32 TryPort = Port + Attempt;
		HttpRouter = HttpServerModule.GetHttpRouter(TryPort, /*bFailOnBindFailure=*/ true);
		if (HttpRouter.IsValid())
		{
			if (Attempt > 0)
			{
				UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: Port %d was unavailable, using port %d instead"), Port, TryPort);
			}
			ServerPort = TryPort;
			break;
		}
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: Port %d is unavailable, trying next..."), TryPort);
	}

	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("MCPServer: Failed to bind any port in range %d-%d. All ports may be in use."), Port, Port + MaxPortAttempts - 1);
		return false;
	}

	// Bind the SSE endpoint (GET /sse) for establishing SSE connections
	SSERouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/sse")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleSSERequest)
	);

	if (!SSERouteHandle.IsValid())
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("MCPServer: Failed to bind /sse route"));
		return false;
	}

	// Bind the message endpoint (POST /message) for receiving JSON-RPC messages
	MessageRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/message")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleMessageRequest)
	);

	if (!MessageRouteHandle.IsValid())
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("MCPServer: Failed to bind /message route"));
		HttpRouter->UnbindRoute(SSERouteHandle);
		return false;
	}

	// Streamable HTTP transport (MCP spec 2025-03-26) - POST /mcp
	// This is the primary endpoint for modern MCP clients like Gemini CLI
	LegacyRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPRequest)
	);

	// Streamable HTTP GET endpoint - for server-to-client notifications (optional)
	StreamableHTTPGetHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPGet)
	);

	// CORS preflight handler for browser-based clients
	OptionsRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_OPTIONS,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleOptionsRequest)
	);

	// Streamable HTTP session deletion endpoint
	DeleteRouteHandle = HttpRouter->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FMCPServer::HandleStreamableHTTPDelete)
	);

	// Start listening
	HttpServerModule.StartAllListeners();

	bIsRunning = true;

	// Register default tools
	RegisterDefaultTools();

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Started on port %d"), ServerPort);
	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer:   Streamable HTTP (2025-03-26): POST/GET/DELETE /mcp"));
	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer:   Legacy HTTP+SSE (2024-11-05): GET /sse, POST /message"));

	return true;
}

void FMCPServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	if (HttpRouter.IsValid())
	{
		if (SSERouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(SSERouteHandle);
		}
		if (MessageRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(MessageRouteHandle);
		}
		if (LegacyRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(LegacyRouteHandle);
		}
		if (StreamableHTTPGetHandle.IsValid())
		{
			HttpRouter->UnbindRoute(StreamableHTTPGetHandle);
		}
		if (OptionsRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(OptionsRouteHandle);
		}
		if (DeleteRouteHandle.IsValid())
		{
			HttpRouter->UnbindRoute(DeleteRouteHandle);
		}
	}

	HttpRouter.Reset();
	RegisteredTools.Empty();
	ActiveSessions.Empty();
	{
		FScopeLock Lock(&StreamableSessionLock);
		StreamableSessions.Empty();
		StreamableSessionProtocols.Empty();
		StreamableSessionEvents.Empty();
		StreamableEventCounter = 0;
	}

	bIsRunning = false;
	bClientDiscoveredTools = false;

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Stopped"));
}

void FMCPServer::RegisterTool(const FMCPToolDefinition& Tool)
{
	if (Tool.Name.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: Cannot register tool with empty name"));
		return;
	}

	RegisteredTools.Add(Tool.Name, Tool);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Registered tool: %s"), *Tool.Name);

	if (bIsRunning)
	{
		TSharedPtr<FJsonObject> Notification = MakeShared<FJsonObject>();
		Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Notification->SetStringField(TEXT("method"), TEXT("notifications/tools/list_changed"));
		QueueStreamableNotificationForAllSessions(Notification);
	}
}

void FMCPServer::UnregisterTool(const FString& ToolName)
{
	if (RegisteredTools.Remove(ToolName) > 0)
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Unregistered tool: %s"), *ToolName);

		if (bIsRunning)
		{
			TSharedPtr<FJsonObject> Notification = MakeShared<FJsonObject>();
			Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Notification->SetStringField(TEXT("method"), TEXT("notifications/tools/list_changed"));
			QueueStreamableNotificationForAllSessions(Notification);
		}
	}
}

FString FMCPServer::GenerateSessionId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString FMCPServer::GetProtocolVersionFromHeader(const FHttpServerRequest& Request)
{
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("MCP-Protocol-Version"), ESearchCase::IgnoreCase)
			|| Header.Key.Equals(TEXT("Mcp-Protocol-Version"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}
	}

	return FString();
}

bool FMCPServer::IsSupportedProtocolVersion(const FString& Version) const
{
	return Version == TEXT("2025-03-26")
		|| Version == TEXT("2025-06-18")
		|| Version == TEXT("2025-11-25")
		|| Version == TEXT("2024-11-05")
		|| Version == TEXT("2024-10-07");
}

FString FMCPServer::ResolveProtocolVersion(const FString& RequestedVersion) const
{
	if (RequestedVersion.IsEmpty())
	{
		return MCP_DEFAULT_PROTOCOL_VERSION;
	}

	if (!IsSupportedProtocolVersion(RequestedVersion))
	{
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("MCPServer: Unsupported protocol version '%s', falling back to %s"),
			*RequestedVersion,
			MCP_DEFAULT_PROTOCOL_VERSION);
		return MCP_DEFAULT_PROTOCOL_VERSION;
	}

	return RequestedVersion;
}

void FMCPServer::PruneExpiredStreamableSessions()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> ExpiredSessionIds;

	{
		FScopeLock Lock(&StreamableSessionLock);

		for (const TPair<FString, double>& Pair : StreamableSessions)
		{
			if ((Now - Pair.Value) > MCP_STREAMABLE_SESSION_TTL_SECONDS)
			{
				ExpiredSessionIds.Add(Pair.Key);
			}
		}

		for (const FString& SessionId : ExpiredSessionIds)
		{
			StreamableSessions.Remove(SessionId);
			StreamableSessionProtocols.Remove(SessionId);
			StreamableSessionEvents.Remove(SessionId);
		}
	}

	if (ExpiredSessionIds.Num() > 0)
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Pruned %d expired streamable sessions"), ExpiredSessionIds.Num());
	}
}

FString FMCPServer::GenerateStreamableEventId()
{
	return FString::Printf(TEXT("evt-%lld"), ++StreamableEventCounter);
}

FString FMCPServer::QueueStreamableEventLocked(const FString& SessionId, const FString& PayloadJson)
{
	TArray<FQueuedStreamableEvent>& Events = StreamableSessionEvents.FindOrAdd(SessionId);
	FQueuedStreamableEvent& Event = Events.AddDefaulted_GetRef();
	Event.EventId = GenerateStreamableEventId();
	Event.PayloadJson = PayloadJson;
	Event.CreatedAt = FPlatformTime::Seconds();
	Event.bIsPriming = false;

	if (Events.Num() > MCP_MAX_STREAMABLE_EVENTS_PER_SESSION)
	{
		const int32 OverflowCount = Events.Num() - MCP_MAX_STREAMABLE_EVENTS_PER_SESSION;
		Events.RemoveAt(0, OverflowCount, EAllowShrinking::No);
	}

	return Event.EventId;
}

void FMCPServer::QueueStreamableNotificationForAllSessions(const TSharedPtr<FJsonObject>& Notification)
{
	if (!Notification.IsValid())
	{
		return;
	}

	const FString PayloadJson = SerializeJsonObject(Notification);

	FScopeLock Lock(&StreamableSessionLock);
	if (StreamableSessions.IsEmpty())
	{
		return;
	}

	for (const TPair<FString, double>& SessionPair : StreamableSessions)
	{
		QueueStreamableEventLocked(SessionPair.Key, PayloadJson);
	}
}

void FMCPServer::RegisterDefaultTools()
{
	// Bridge NeoStack tools to MCP
	FNeoStackToolRegistry& ToolRegistry = FNeoStackToolRegistry::Get();
	TArray<FString> ToolNames = ToolRegistry.GetToolNames();

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Registering %d NeoStack tools"), ToolNames.Num());

	for (const FString& Name : ToolNames)
	{
		FNeoStackToolBase* Tool = ToolRegistry.GetTool(Name);
		if (!Tool)
		{
			continue;
		}

		FMCPToolDefinition MCPTool;
		MCPTool.Name = Tool->GetName();
		MCPTool.Description = Tool->GetDescription();
		MCPTool.bIsReadOnly = false;
		MCPTool.bRequiresConfirmation = false;

		TSharedPtr<FJsonObject> ToolSchema = Tool->GetInputSchema();
		if (!ToolSchema.IsValid())
		{
			ToolSchema = MakeShared<FJsonObject>();
			ToolSchema->SetStringField(TEXT("type"), TEXT("object"));
			ToolSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
		}
		MCPTool.InputSchema = ToolSchema;

		RegisterTool(MCPTool);
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Bridged NeoStack tool: %s"), *Name);
	}
}

bool FMCPServer::HandleSSERequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: SSE connection request received"));

	// Generate a session ID for this connection
	FString SessionId = GenerateSessionId();
	ActiveSessions.Add(SessionId);

	// Build the SSE response with the endpoint event
	// Format: "event: endpoint\ndata: <endpoint-url>\n\n"
	FString EndpointUrl = FString::Printf(TEXT("/message?sessionId=%s"), *SessionId);
	FString SSEResponse = FString::Printf(TEXT("event: endpoint\ndata: %s\n\n"), *EndpointUrl);

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Created SSE session %s, endpoint: %s"), *SessionId, *EndpointUrl);

	// Create SSE response with proper content type
	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;
	Response->Headers.Add(TEXT("Content-Type"), { TEXT("text/event-stream") });
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
	Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
	AddCommonHeaders(*Response, SessionId);

	// Convert response body to UTF8
	FTCHARToUTF8 Converter(*SSEResponse);
	Response->Body.Append((const uint8*)Converter.Get(), Converter.Length());

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleMessageRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-1] HandleMessageRequest entered, body size: %d bytes"), Request.Body.Num());

	// Extract session ID from query string (optional validation)
	FString SessionId;
	for (const auto& QueryParam : Request.QueryParams)
	{
		if (QueryParam.Key == TEXT("sessionId"))
		{
			SessionId = QueryParam.Value;
			break;
		}
	}

	if (!SessionId.IsEmpty())
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-2] Session ID: %s"), *SessionId);
	}

	FString RequestBody;
	TSharedPtr<FJsonObject> JsonRequest;
	const bool bParsed = ParseJsonRequestBody(Request, RequestBody, JsonRequest);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-2] Body: %d bytes -> %d chars"), Request.Body.Num(), RequestBody.Len());
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-3] Request body (%d chars): %s"), RequestBody.Len(), *RequestBody);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-4] Parse result: %s"), bParsed ? TEXT("valid") : TEXT("invalid"));

	if (!bParsed || !JsonRequest.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7] JSON parse FAILED"));
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7a] Body size: %d bytes, String length: %d chars"), Request.Body.Num(), RequestBody.Len());
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7b] First 500 chars: %.500s"), *RequestBody);
		if (RequestBody.Len() > 500)
		{
			UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7c] Last 200 chars: %s"), *RequestBody.Right(200));
		}

		// Log raw bytes to detect encoding/corruption issues
		if (Request.Body.Num() > 0)
		{
			FString FirstBytes, LastBytes;
			int32 BytesToLog = FMath::Min(50, Request.Body.Num());
			for (int32 i = 0; i < BytesToLog; i++)
			{
				FirstBytes += FString::Printf(TEXT("%02X "), Request.Body[i]);
			}
			UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7d] First %d raw bytes (hex): %s"), BytesToLog, *FirstBytes);

			int32 StartIdx = FMath::Max(0, Request.Body.Num() - 30);
			for (int32 i = StartIdx; i < Request.Body.Num(); i++)
			{
				LastBytes += FString::Printf(TEXT("%02X "), Request.Body[i]);
			}
			UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [MSG-7e] Last %d raw bytes (hex): %s"), Request.Body.Num() - StartIdx, *LastBytes);
		}

		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32700, TEXT("Parse error"));
		FString ResponseStr;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
		FJsonSerializer::Serialize(ErrorResponse.ToSharedRef(), Writer);

		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
		AddCommonHeaders(*Response, SessionId);
		OnComplete(MoveTemp(Response));
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-8] Error response sent"));
		return true;
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-7] JSON parsed, extracting method..."));

	// Check for tools/call - use async handler with timeout
	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-8] Method: '%s'"), *Method);

	if (Method == TEXT("tools/call"))
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-9] Routing to HandleToolsCallAsync"));
		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-10] Id field present: %s"), Id.IsValid() ? TEXT("yes") : TEXT("no"));

		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
			UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-11] Params extracted"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
			UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-11] No params"));
		}

		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-12] Calling HandleToolsCallAsync..."));
		auto WrappedOnComplete = [this, OnComplete, SessionId](TUniquePtr<FHttpServerResponse> Response)
		{
			AddCommonHeaders(*Response, SessionId);
			OnComplete(MoveTemp(Response));
		};
		bool bResult = HandleToolsCallAsync(Id, Params, WrappedOnComplete);
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-13] HandleToolsCallAsync returned: %s"), bResult ? TEXT("true") : TEXT("false"));
		return bResult;
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-9] Routing to ProcessJsonRpcRequest"));

	// Process other JSON-RPC requests synchronously
	TSharedPtr<FJsonObject> JsonResponse = ProcessJsonRpcRequest(JsonRequest);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-10] ProcessJsonRpcRequest returned, valid: %s"), JsonResponse.IsValid() ? TEXT("true") : TEXT("false"));

	// For notifications (nullptr response), send empty 202 Accepted
	if (!JsonResponse.IsValid())
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-11] Notification, sending 202"));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCommonHeaders(*Response);
		OnComplete(MoveTemp(Response));
		return true;
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-11] Serializing response..."));

	// Serialize response
	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-12] Sending response: %s"), *ResponseStr);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	AddCommonHeaders(*Response);
	OnComplete(MoveTemp(Response));

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [MSG-13] HandleMessageRequest complete"));
	return true;
}

bool FMCPServer::HandleRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Legacy handler - redirect to Streamable HTTP handler which handles both protocols
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Legacy HandleRequest redirecting to StreamableHTTP handler"));
	return HandleStreamableHTTPRequest(Request, OnComplete);
}

TSharedPtr<FJsonObject> FMCPServer::ProcessJsonRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	// Validate JSON-RPC structure
	FString JsonRpcVersion;
	if (!Request->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
	{
		return CreateErrorResponse(nullptr, -32600, TEXT("Invalid Request: missing or invalid jsonrpc version"));
	}

	// Check if this is a notification (no id field) - notifications don't get responses
	bool bIsNotification = !Request->HasField(TEXT("id"));
	TSharedPtr<FJsonValue> Id = Request->TryGetField(TEXT("id"));

	FString Method;
	if (!Request->TryGetStringField(TEXT("method"), Method))
	{
		return CreateErrorResponse(Id, -32600, TEXT("Invalid Request: missing method"));
	}

	TSharedPtr<FJsonObject> Params;
	if (Request->HasField(TEXT("params")))
	{
		Params = Request->GetObjectField(TEXT("params"));
	}
	else
	{
		Params = MakeShared<FJsonObject>();
	}

	// Dispatch to handler
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Processing method: %s"), *Method);

	if (Method == TEXT("initialize"))
	{
		return HandleInitialize(Id, Params);
	}
	else if (Method == TEXT("tools/list"))
	{
		return HandleToolsList(Id, Params);
	}
	else if (Method == TEXT("ping"))
	{
		return CreateResponse(Id, MakeShared<FJsonObject>());
	}
	// ACP methods that Claude Code may send to MCP servers - handle as no-ops
	else if (Method == TEXT("setSessionMode"))
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: setSessionMode called (no-op) - modeId: %s"),
			Params.IsValid() ? *Params->GetStringField(TEXT("modeId")) : TEXT("unknown"));
		return CreateResponse(Id, MakeShared<FJsonObject>());
	}
	else if (Method == TEXT("unstable/setSessionModel"))
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: unstable/setSessionModel called (no-op) - modelId: %s"),
			Params.IsValid() ? *Params->GetStringField(TEXT("modelId")) : TEXT("unknown"));
		return CreateResponse(Id, MakeShared<FJsonObject>());
	}
	else if (Method == TEXT("notifications/initialized") || Method.StartsWith(TEXT("notifications/")))
	{
		// Client notification - no response should be sent for notifications
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Received notification: %s"), *Method);
		return nullptr;  // nullptr indicates no response needed
	}
	else
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: Unknown method: %s"), *Method);
		return CreateErrorResponse(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
	}
}

TSharedPtr<FJsonObject> FMCPServer::HandleInitialize(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Protocol version - match what the client sends or use latest
	FString ClientProtocolVersion;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), ClientProtocolVersion))
	{
		const FString NegotiatedProtocolVersion = ResolveProtocolVersion(ClientProtocolVersion);
		Result->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);
		UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Initialize - negotiated protocol version: %s"), *NegotiatedProtocolVersion);
	}
	else
	{
		Result->SetStringField(TEXT("protocolVersion"), MCP_DEFAULT_PROTOCOL_VERSION);
		UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: Initialize - using default protocol version: %s"), MCP_DEFAULT_PROTOCOL_VERSION);
	}

	// Server info
	TSharedPtr<FJsonObject> ServerInfoObj = MakeShared<FJsonObject>();
	ServerInfoObj->SetStringField(TEXT("name"), ServerInfo.Name);
	ServerInfoObj->SetStringField(TEXT("version"), ServerInfo.Version);
	ServerInfoObj->SetNumberField(TEXT("_rev"), 4254); // Internal build revision for telemetry
	Result->SetObjectField(TEXT("serverInfo"), ServerInfoObj);

	// Capabilities
	TSharedPtr<FJsonObject> CapabilitiesObj = MakeShared<FJsonObject>();
	if (Capabilities.bSupportsTools)
	{
		TSharedPtr<FJsonObject> ToolsObj = MakeShared<FJsonObject>();
		CapabilitiesObj->SetObjectField(TEXT("tools"), ToolsObj);
	}
	Result->SetObjectField(TEXT("capabilities"), CapabilitiesObj);

	return CreateResponse(Id, Result);
}

TSharedPtr<FJsonObject> FMCPServer::HandleToolsList(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Refresh the Lua tool description so it includes bindings from extension modules
	// that loaded after the MCP server started (e.g. NSAI_Python deferred loading)
	FNeoStackToolRegistry& ToolRegistry = FNeoStackToolRegistry::Get();
	for (auto& Pair : RegisteredTools)
	{
		if (FNeoStackToolBase* Tool = ToolRegistry.GetTool(Pair.Key))
		{
			Pair.Value.Description = Tool->GetDescription();
		}
	}

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& Pair : RegisteredTools)
	{
		TSharedPtr<FJsonObject> ToolObj = CreateToolSchema(Pair.Value);
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: - Tool: %s"), *Pair.Key);
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: tools/list returning %d tools"), ToolsArray.Num());
	Result->SetArrayField(TEXT("tools"), ToolsArray);

	// Notify listeners that an MCP client has discovered tools (e.g., chat UI unblocks input)
	bClientDiscoveredTools = true;
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnClientToolsDiscovered.Broadcast();
	});

	return CreateResponse(Id, Result);
}

bool FMCPServer::HandleToolsCallAsync(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Params, const FHttpResultCallback& OnComplete, bool bUseSseResponse)
{
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-1] HandleToolsCallAsync entered"));

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [ASYNC-2] Missing tool name in params"));
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(Id, -32602, TEXT("Invalid params: missing 'name'"));
		OnComplete(CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse));
		return true;
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-2] Tool name: '%s'"), *ToolName);

	if (!FNeoStackToolRegistry::Get().HasTool(ToolName))
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [ASYNC-3] Tool not found: '%s'"), *ToolName);
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(Id, -32602, FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
		OnComplete(CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse));
		return true;
	}

	TSharedPtr<FJsonObject> Arguments;
	if (Params->HasField(TEXT("arguments")))
	{
		Arguments = Params->GetObjectField(TEXT("arguments"));
	}
	else
	{
		Arguments = MakeShared<FJsonObject>();
	}

	const UUserPreferencesSubsystem* PrefsAsync = UUserPreferencesSubsystem::Get();
	int32 TimeoutSeconds = PrefsAsync ? PrefsAsync->ToolExecutionTimeoutSeconds : 60;

	// Shared state for coordinating between timeout watchdog and tool completion.
	// Both paths race to be the "first responder"; the loser silently drops its
	// response so we never send two HTTP responses for one request.
	TSharedPtr<FCriticalSection> Lock = MakeShared<FCriticalSection>();
	TSharedPtr<bool> bResponseSent = MakeShared<bool>(false);
	TSharedPtr<FNeoStackToolHandle> ToolHandle = MakeShared<FNeoStackToolHandle>();

	double StartTime = FPlatformTime::Seconds();

	// Timeout watchdog — fires a "timed out" HTTP response and cancels the
	// registry handle so runaway work does not keep the global tool queue blocked.
	if (TimeoutSeconds > 0)
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-4] Starting timeout watchdog (%d seconds)..."), TimeoutSeconds);
		Async(EAsyncExecution::ThreadPool, [this, Id, ToolName, TimeoutSeconds, Lock, bResponseSent, ToolHandle, OnComplete, bUseSseResponse]()
		{
			FPlatformProcess::Sleep(static_cast<float>(TimeoutSeconds));

			FScopeLock ScopeLock(Lock.Get());
			if (*bResponseSent)
			{
				return; // Tool already responded
			}
			*bResponseSent = true;

			UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [TIMEOUT] Tool '%s' timed out after %d seconds"), *ToolName, TimeoutSeconds);
			if (ToolHandle->IsValid())
			{
				FNeoStackToolRegistry::Get().CancelExecute(*ToolHandle);
			}

			TSharedPtr<FJsonObject> TimeoutResult = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ContentArray;
			TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
			ContentObj->SetStringField(TEXT("type"), TEXT("text"));
			ContentObj->SetStringField(TEXT("text"), FString::Printf(
				TEXT("Tool '%s' timed out after %d seconds. The operation may still be running in the background. "
				     "You can adjust timeout in Project Settings > Plugins > NeoStack AI."),
				*ToolName, TimeoutSeconds));
			ContentArray.Add(MakeShared<FJsonValueObject>(ContentObj));
			TimeoutResult->SetArrayField(TEXT("content"), ContentArray);
			TimeoutResult->SetBoolField(TEXT("isError"), true);

			TSharedPtr<FJsonObject> JsonResponse = MakeShared<FJsonObject>();
			JsonResponse->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			JsonResponse->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
			JsonResponse->SetObjectField(TEXT("result"), TimeoutResult);

			OnComplete(CreateStreamableHttpResponse(JsonResponse, bUseSseResponse));
		});
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-5] Dispatching '%s' via Registry::Execute (async callback)"), *ToolName);

	// Async dispatch — registry guarantees OnComplete fires on the game thread when
	// the tool finishes, even if the tool's body yielded a coroutine and resumed
	// across multiple frames. The HTTP response is built and sent from inside this
	// completion lambda.
	*ToolHandle = FNeoStackToolRegistry::Get().Execute(ToolName, Arguments,
		[this, Id, ToolName, StartTime, Lock, bResponseSent, OnComplete, bUseSseResponse](const FToolResult& ToolResult)
		{
			// Race against the timeout watchdog — first one to grab the lock wins.
			{
				FScopeLock ScopeLock(Lock.Get());
				if (*bResponseSent)
				{
					UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-DONE] Timeout already responded for '%s', dropping result"), *ToolName);
					return;
				}
				*bResponseSent = true;
			}

			const double Duration = FPlatformTime::Seconds() - StartTime;
			UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-DONE] Tool '%s' completed in %.2fs (success: %s)"),
				*ToolName, Duration, ToolResult.bSuccess ? TEXT("true") : TEXT("false"));

			// Broadcast images for UI consumers (e.g., the chat-side ACP capture)
			if (ToolResult.Images.Num() > 0)
			{
				FMCPToolResult MCPResult;
				MCPResult.bSuccess = ToolResult.bSuccess;
				MCPResult.Content = ToolResult.Output;
				MCPResult.ErrorMessage = ToolResult.bSuccess ? FString() : ToolResult.Output;
				for (const FToolResultImage& Img : ToolResult.Images)
				{
					FMCPToolResultImage MCPImage;
					MCPImage.Base64Data = Img.Base64Data;
					MCPImage.MimeType = Img.MimeType;
					MCPImage.Width = Img.Width;
					MCPImage.Height = Img.Height;
					MCPResult.Images.Add(MCPImage);
				}
				OnToolExecuted.Broadcast(ToolName, ToolResult.bSuccess, MCPResult);
			}

			// Build JSON-RPC result envelope
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ContentArray;

			TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
			ContentObj->SetStringField(TEXT("type"), TEXT("text"));
			ContentObj->SetStringField(TEXT("text"), ToolResult.Output);
			ContentArray.Add(MakeShared<FJsonValueObject>(ContentObj));

			for (const FToolResultImage& Image : ToolResult.Images)
			{
				TSharedPtr<FJsonObject> ImageObj = MakeShared<FJsonObject>();
				ImageObj->SetStringField(TEXT("type"), TEXT("image"));
				ImageObj->SetStringField(TEXT("data"), Image.Base64Data);
				ImageObj->SetStringField(TEXT("mimeType"), Image.MimeType);
				ContentArray.Add(MakeShared<FJsonValueObject>(ImageObj));
			}

			Result->SetArrayField(TEXT("content"), ContentArray);
			if (!ToolResult.bSuccess)
			{
				Result->SetBoolField(TEXT("isError"), true);
			}

			TSharedPtr<FJsonObject> JsonResponse = MakeShared<FJsonObject>();
			JsonResponse->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			JsonResponse->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
			JsonResponse->SetObjectField(TEXT("result"), Result);

			OnComplete(CreateStreamableHttpResponse(JsonResponse, bUseSseResponse));
		});

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [ASYNC-6] Registry::Execute returned, response will arrive via callback"));
	return true;
}

TSharedPtr<FJsonObject> FMCPServer::CreateToolSchema(const FMCPToolDefinition& Tool)
{
	TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
	ToolObj->SetStringField(TEXT("name"), Tool.Name);
	ToolObj->SetStringField(TEXT("description"), Tool.Description);

	if (Tool.InputSchema.IsValid())
	{
		ToolObj->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
	}

	return ToolObj;
}

TSharedPtr<FJsonObject> FMCPServer::CreateResponse(TSharedPtr<FJsonValue> Id, TSharedPtr<FJsonObject> Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
	Response->SetObjectField(TEXT("result"), Result);
	return Response;
}

TSharedPtr<FJsonObject> FMCPServer::CreateErrorResponse(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());

	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);
	Response->SetObjectField(TEXT("error"), ErrorObj);

	return Response;
}

FString FMCPServer::GetSessionIdFromHeader(const FHttpServerRequest& Request)
{
	// Look for MCP-Session-Id header (case-insensitive search)
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Mcp-Session-Id"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}
	}
	return FString();
}

void FMCPServer::AddCommonHeaders(FHttpServerResponse& Response, const FString& SessionId, const FString& ProtocolVersion)
{
	const UACPSettings* Settings = UACPSettings::Get();
	bool bAllowBrowser = Settings && Settings->bAllowBrowserMCPRequests;

	if (bAllowBrowser)
	{
		Response.Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("*") });
		Response.Headers.Add(TEXT("Access-Control-Allow-Methods"), { TEXT("GET, POST, OPTIONS, DELETE") });
		Response.Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("Content-Type, Accept, MCP-Session-Id, Mcp-Session-Id, MCP-Protocol-Version, Mcp-Protocol-Version") });
		Response.Headers.Add(TEXT("Access-Control-Expose-Headers"), { TEXT("MCP-Session-Id, MCP-Protocol-Version") });
	}

	if (!SessionId.IsEmpty())
	{
		Response.Headers.Add(TEXT("MCP-Session-Id"), { SessionId });
	}

	const FString EffectiveProtocol = ProtocolVersion.IsEmpty()
		? MCP_DEFAULT_PROTOCOL_VERSION
		: ProtocolVersion;
	if (!EffectiveProtocol.IsEmpty())
	{
		Response.Headers.Add(TEXT("MCP-Protocol-Version"), { EffectiveProtocol });
	}
}

bool FMCPServer::RejectIfBrowserRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && Settings->bAllowBrowserMCPRequests)
	{
		return false;
	}

	// Check for Origin header - browsers always send this on cross-origin requests,
	// CLI tools (Claude Code, Gemini CLI, Codex, etc.) do not
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Origin"), ESearchCase::IgnoreCase))
		{
			FString OriginValue = Header.Value.Num() > 0 ? Header.Value[0] : TEXT("(empty)");
			UE_LOG(LogNeoStackAI, Warning,
				TEXT("MCPServer: Rejected browser request from Origin '%s'. "
				     "Enable 'Allow Browser Requests' in Project Settings > Plugins > NeoStack AI if you need browser access."),
				*OriginValue);

			TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(
				FString(TEXT("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32001,\"message\":\"Browser requests are not allowed. Enable 'Allow Browser Requests' in plugin settings.\"}}")),
				TEXT("application/json"));
			Response->Code = EHttpServerResponseCodes::Denied;
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	return false;
}

bool FMCPServer::HandleOptionsRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: OPTIONS preflight request"));

	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;  // 200 OK for CORS preflight
	AddCommonHeaders(*Response, FString(), ResolveProtocolVersion(GetProtocolVersionFromHeader(Request)));

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	const FString SessionId = GetSessionIdFromHeader(Request);
	const FString RequestedProtocolVersion = GetProtocolVersionFromHeader(Request);
	FString ProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);

	if (!RequestedProtocolVersion.IsEmpty() && !IsSupportedProtocolVersion(RequestedProtocolVersion))
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			FString::Printf(TEXT("Bad Request: Unsupported protocol version: %s"), *RequestedProtocolVersion));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 400);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32602, TEXT("Missing MCP-Session-Id header"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 400);
		AddCommonHeaders(*Response, FString(), ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	bool bSessionExists = false;
	{
		FScopeLock Lock(&StreamableSessionLock);
		bSessionExists = StreamableSessions.Contains(SessionId);
		if (bSessionExists)
		{
			if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
			{
				ProtocolVersion = *StoredProtocol;
			}

			StreamableSessions.Remove(SessionId);
			StreamableSessionProtocols.Remove(SessionId);
			StreamableSessionEvents.Remove(SessionId);
		}
	}

	if (!bSessionExists)
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600, TEXT("Session not found"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 404);
		AddCommonHeaders(*Response, FString(), ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = static_cast<EHttpServerResponseCodes>(200);
	AddCommonHeaders(*Response, FString(), ProtocolVersion);
	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	if (!RequestAcceptsSSE(Request))
	{
		FString AcceptHeader;
		TryGetHeaderValue(Request, TEXT("Accept"), AcceptHeader);
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("MCPServer: Rejecting GET /mcp due to incompatible Accept header '%s'"),
			AcceptHeader.IsEmpty() ? TEXT("(missing)") : *AcceptHeader);
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600, TEXT("Not Acceptable: Client must accept text/event-stream"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 406);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Log at Verbose level to avoid spam from polling clients
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: Streamable HTTP GET /mcp (SSE notification stream)"));

	FString SessionId = GetSessionIdFromHeader(Request);
	FString LastEventId;
	TryGetHeaderValue(Request, TEXT("Last-Event-ID"), LastEventId);
	PruneExpiredStreamableSessions();

	const FString RequestedProtocolVersion = GetProtocolVersionFromHeader(Request);
	FString ProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);
	if (!RequestedProtocolVersion.IsEmpty() && !IsSupportedProtocolVersion(RequestedProtocolVersion))
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			FString::Printf(TEXT("Bad Request: Unsupported protocol version: %s"), *RequestedProtocolVersion));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 400);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32602, TEXT("Missing MCP-Session-Id header"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 400);
		AddCommonHeaders(*Response, FString(), ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	bool bSessionExists = false;
	TArray<FQueuedStreamableEvent> EventsToSend;
	bool bHasReplayCursor = !LastEventId.IsEmpty();
	bool bFoundReplayCursor = false;
	{
		FScopeLock Lock(&StreamableSessionLock);
		bSessionExists = StreamableSessions.Contains(SessionId);
		if (bSessionExists)
		{
			if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
			{
				ProtocolVersion = *StoredProtocol;
			}
			if (double* LastSeen = StreamableSessions.Find(SessionId))
			{
				*LastSeen = FPlatformTime::Seconds();
			}

			TArray<FQueuedStreamableEvent>& SessionEvents = StreamableSessionEvents.FindOrAdd(SessionId);
			if (bHasReplayCursor)
			{
				int32 LastEventIndex = INDEX_NONE;
				for (int32 EventIndex = 0; EventIndex < SessionEvents.Num(); ++EventIndex)
				{
					if (SessionEvents[EventIndex].EventId == LastEventId)
					{
						LastEventIndex = EventIndex;
						break;
					}
				}

				if (LastEventIndex != INDEX_NONE)
				{
					bFoundReplayCursor = true;
					for (int32 EventIndex = LastEventIndex + 1; EventIndex < SessionEvents.Num(); ++EventIndex)
					{
						EventsToSend.Add(SessionEvents[EventIndex]);
					}
				}
			}
			else
			{
				EventsToSend = SessionEvents;
				if (EventsToSend.Num() == 0 && ProtocolVersion.Compare(TEXT("2025-03-26"), ESearchCase::CaseSensitive) >= 0)
				{
					FQueuedStreamableEvent& PrimingEvent = SessionEvents.AddDefaulted_GetRef();
					PrimingEvent.EventId = GenerateStreamableEventId();
					PrimingEvent.PayloadJson = TEXT("");
					PrimingEvent.CreatedAt = FPlatformTime::Seconds();
					PrimingEvent.bIsPriming = true;
					EventsToSend.Add(PrimingEvent);
				}
			}
		}
	}

	if (!bSessionExists)
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600, TEXT("Session not found"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 404);
		AddCommonHeaders(*Response, FString(), ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (bHasReplayCursor && !bFoundReplayCursor)
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			FString::Printf(TEXT("Invalid Last-Event-ID: %s"), *LastEventId));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 400);
		AddCommonHeaders(*Response, SessionId, ProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Ok;
	Response->Headers.Add(TEXT("Content-Type"), { TEXT("text/event-stream") });
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
	Response->Headers.Add(TEXT("Connection"), { TEXT("keep-alive") });
	AddCommonHeaders(*Response, SessionId, ProtocolVersion);

	FString SSEContent;
	for (const FQueuedStreamableEvent& Event : EventsToSend)
	{
		SSEContent += BuildSSEEventPayload(Event.EventId, Event.PayloadJson, Event.bIsPriming);
	}

	if (SSEContent.IsEmpty())
	{
		SSEContent = TEXT(": heartbeat\n\n");
	}

	FTCHARToUTF8 Converter(*SSEContent);
	Response->Body.Append((const uint8*)Converter.Get(), Converter.Length());

	OnComplete(MoveTemp(Response));
	return true;
}

bool FMCPServer::HandleStreamableHTTPRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	if (RejectIfBrowserRequest(Request, OnComplete)) return true;

	const bool bAcceptsJsonResponse = RequestAcceptsJsonResponse(Request);
	const bool bAcceptsSseResponse = RequestAcceptsSSE(Request, false);
	const bool bUseSseResponse = !bAcceptsJsonResponse && bAcceptsSseResponse;

	if (!bAcceptsJsonResponse && !bAcceptsSseResponse)
	{
		FString AcceptHeader;
		TryGetHeaderValue(Request, TEXT("Accept"), AcceptHeader);
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("MCPServer: Rejecting POST /mcp due to incompatible Accept header '%s'"),
			AcceptHeader.IsEmpty() ? TEXT("(missing)") : *AcceptHeader);
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			TEXT("Not Acceptable: Client must accept application/json, text/event-stream, or */*"));
		TUniquePtr<FHttpServerResponse> Response = CreateJsonHttpResponse(ErrorResponse, 406);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (!RequestHeaderContainsToken(Request, TEXT("Content-Type"), TEXT("application/json")))
	{
		FString ContentTypeHeader;
		TryGetHeaderValue(Request, TEXT("Content-Type"), ContentTypeHeader);
		UE_LOG(LogNeoStackAI, Warning,
			TEXT("MCPServer: Rejecting POST /mcp due to incompatible Content-Type header '%s'"),
			ContentTypeHeader.IsEmpty() ? TEXT("(missing)") : *ContentTypeHeader);
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			TEXT("Unsupported Media Type: Content-Type must be application/json"));
		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse, 415);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-1] Streamable HTTP POST /mcp, body size: %d bytes"), Request.Body.Num());
	PruneExpiredStreamableSessions();

	// Get session ID from header (may be empty for initialize request)
	FString SessionId = GetSessionIdFromHeader(Request);
	FString RequestedProtocolVersion = GetProtocolVersionFromHeader(Request);
	FString NegotiatedProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-2] Session ID from header: '%s'"), *SessionId);

	FString RequestBody;
	TSharedPtr<FJsonObject> JsonRequest;
	const bool bParsed = ParseJsonRequestBody(Request, RequestBody, JsonRequest);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-3] Request body: %s"), *RequestBody);

	if (!bParsed || !JsonRequest.IsValid())
	{
		UE_LOG(LogNeoStackAI, Warning, TEXT("MCPServer: [STREAM-4] JSON parse failed"));
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32700, TEXT("Parse error"));
		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse, 400);
		AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Extract method
	FString Method;
	JsonRequest->TryGetStringField(TEXT("method"), Method);
	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-5] Method: '%s'"), *Method);

	// Handle initialize specially - create session and return session ID in header
	if (Method == TEXT("initialize"))
	{
		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
		}

		FString BodyProtocolVersion;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("protocolVersion"), BodyProtocolVersion) && RequestedProtocolVersion.IsEmpty())
		{
			RequestedProtocolVersion = BodyProtocolVersion;
		}
		NegotiatedProtocolVersion = ResolveProtocolVersion(RequestedProtocolVersion);
		Params->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);

		// Generate new session ID for this client
		FString NewSessionId = GenerateSessionId();
		{
			FScopeLock Lock(&StreamableSessionLock);
			StreamableSessions.Add(NewSessionId, FPlatformTime::Seconds());
			StreamableSessionProtocols.Add(NewSessionId, NegotiatedProtocolVersion);
			StreamableSessionEvents.FindOrAdd(NewSessionId);
		}

		UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: [STREAM-6] Initialize - created session: %s"), *NewSessionId);

		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));

		TSharedPtr<FJsonObject> JsonResponse = HandleInitialize(Id, Params);

		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(JsonResponse, bUseSseResponse);
		AddCommonHeaders(*Response, NewSessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));

		UE_LOG(LogNeoStackAI, Log, TEXT("MCPServer: [STREAM-7] Initialize response sent with session ID"));
		return true;
	}

	if (!RequestedProtocolVersion.IsEmpty() && !IsSupportedProtocolVersion(RequestedProtocolVersion))
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600,
			FString::Printf(TEXT("Bad Request: Unsupported protocol version: %s"), *RequestedProtocolVersion));
		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse, 400);
		AddCommonHeaders(*Response, FString(), MCP_DEFAULT_PROTOCOL_VERSION);
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32602, TEXT("Missing MCP-Session-Id header"));
		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse, 400);
		AddCommonHeaders(*Response, FString(), NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	bool bSessionExists = false;
	{
		FScopeLock Lock(&StreamableSessionLock);
		bSessionExists = StreamableSessions.Contains(SessionId);
		if (bSessionExists)
		{
			StreamableSessions.Add(SessionId, FPlatformTime::Seconds());
			if (const FString* StoredProtocol = StreamableSessionProtocols.Find(SessionId))
			{
				NegotiatedProtocolVersion = *StoredProtocol;
			}
			else
			{
				StreamableSessionProtocols.Add(SessionId, NegotiatedProtocolVersion);
			}
		}
	}

	if (!bSessionExists)
	{
		TSharedPtr<FJsonObject> ErrorResponse = CreateErrorResponse(nullptr, -32600, TEXT("Session not found"));
		TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(ErrorResponse, bUseSseResponse, 404);
		AddCommonHeaders(*Response, FString(), NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Check for tools/call - use async handler with timeout
	if (Method == TEXT("tools/call"))
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-6] Routing to async tools/call handler"));

		TSharedPtr<FJsonValue> Id = JsonRequest->TryGetField(TEXT("id"));

		TSharedPtr<FJsonObject> Params;
		if (JsonRequest->HasField(TEXT("params")))
		{
			Params = JsonRequest->GetObjectField(TEXT("params"));
		}
		else
		{
			Params = MakeShared<FJsonObject>();
		}

		// Wrap OnComplete so streamable HTTP responses get MCP headers exactly once.
		FString CapturedSessionId = SessionId;
		FString CapturedProtocolVersion = NegotiatedProtocolVersion;
		auto WrappedOnComplete = [this, OnComplete, CapturedSessionId, CapturedProtocolVersion](TUniquePtr<FHttpServerResponse> Response)
		{
			AddCommonHeaders(*Response, CapturedSessionId, CapturedProtocolVersion);
			OnComplete(MoveTemp(Response));
		};

		return HandleToolsCallAsync(Id, Params, WrappedOnComplete, bUseSseResponse);
	}

	// Process other JSON-RPC requests synchronously
	TSharedPtr<FJsonObject> JsonResponse = ProcessJsonRpcRequest(JsonRequest);

	// For notifications (nullptr response), send 202 Accepted
	if (!JsonResponse.IsValid())
	{
		UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-7] Notification, sending 202"));
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Accepted;
		AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Serialize and send response
	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(JsonResponse.ToSharedRef(), Writer);

	UE_LOG(LogNeoStackAI, Verbose, TEXT("MCPServer: [STREAM-8] Sending response: %s"), *ResponseStr);

	TUniquePtr<FHttpServerResponse> Response = CreateStreamableHttpResponse(JsonResponse, bUseSseResponse);
	AddCommonHeaders(*Response, SessionId, NegotiatedProtocolVersion);
	OnComplete(MoveTemp(Response));

	return true;
}
