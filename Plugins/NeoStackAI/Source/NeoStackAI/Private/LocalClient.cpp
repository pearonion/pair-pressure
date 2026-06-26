// Copyright 2026 Betide Studio. All Rights Reserved.

#include "LocalClient.h"
#include "AgentService.h"
#include "ACPSettings.h"
#include "MCPServer.h"
#include "Tools/NeoStackToolRegistry.h"
#include "Tools/NeoStackToolBase.h"
#include "WebSocketsModule.h"
#include "Json.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"

DECLARE_LOG_CATEGORY_EXTERN(LogLocalClient, Log, All);
DEFINE_LOG_CATEGORY(LogLocalClient);

// ── Singleton ──────────────────────────────────────────────────────────

FLocalClient& FLocalClient::Get()
{
	static FLocalClient Instance;
	return Instance;
}

FLocalClient::~FLocalClient()
{
	Shutdown();
}

// ── Discovery ──────────────────────────────────────────────────────────

FString FLocalClient::GetDiscoveryFilePath()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && !Settings->IDEDiscoveryPathOverride.IsEmpty())
	{
		return Settings->IDEDiscoveryPathOverride;
	}

	// The IDE writes this into the open project's Saved folder.
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NeoStack"), TEXT("ide-bridge.json"));
}

FLocalClient::FIdeDiscovery FLocalClient::ReadDiscovery() const
{
	FIdeDiscovery Result;
	const FString Path = GetDiscoveryFilePath();

	FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
	if (!Stat.bIsValid || Stat.bIsDirectory)
	{
		return Result;
	}

	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *Path))
	{
		return Result;
	}

	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return Result;
	}

	// Liveness: if the IDE that wrote this file is gone, treat it as stale. The IDE
	// writes the file once on project open (it is not refreshed), so an mtime window
	// would wrongly expire a healthy connection — use the pid instead.
	int32 Pid = 0;
	if (Json->TryGetNumberField(TEXT("pid"), Pid) && Pid > 0
		&& !FPlatformProcess::IsApplicationRunning(static_cast<uint32>(Pid)))
	{
		return Result;
	}

	// Accept `url` (current), `wsUrl` (alias), or a bare `wsPort` (older IDEs).
	FString Url;
	if (!Json->TryGetStringField(TEXT("url"), Url))
	{
		Json->TryGetStringField(TEXT("wsUrl"), Url);
	}
	if (Url.IsEmpty())
	{
		int32 Port = 0;
		if (Json->TryGetNumberField(TEXT("wsPort"), Port) && Port > 0 && Port <= 65535)
		{
			Url = FString::Printf(TEXT("ws://127.0.0.1:%d"), Port);
		}
	}
	if (Url.IsEmpty())
	{
		return Result;
	}

	// Loopback-only: never dial an arbitrary host even if the file names one.
	if (!Url.StartsWith(TEXT("ws://127.0.0.1:"))
		&& !Url.StartsWith(TEXT("ws://localhost:"))
		&& !Url.StartsWith(TEXT("ws://[::1]:")))
	{
		UE_LOG(LogLocalClient, Warning, TEXT("Ignoring non-loopback IDE url: %s"), *Url);
		return Result;
	}

	Json->TryGetStringField(TEXT("token"), Result.Token);
	Result.Url = Url;
	Result.bValid = true;
	return Result;
}

// ── Lifecycle ──────────────────────────────────────────────────────────

void FLocalClient::Initialize()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (Settings && !Settings->bEnableIDEConnection)
	{
		return;
	}

	bEnabled = true;
	UE_LOG(LogLocalClient, Log, TEXT("Initializing IDE connection..."));

	FModuleManager::Get().LoadModuleChecked<FWebSocketsModule>("WebSockets");

	// Try to connect immediately if IDE is running
	FIdeDiscovery Discovery = ReadDiscovery();
	if (Discovery.bValid)
	{
		DiscoveredToken = Discovery.Token;
		Connect(Discovery.Url);
	}
	else
	{
		UE_LOG(LogLocalClient, Log, TEXT("IDE not detected. Will poll for discovery file..."));
	}

	// Poll for IDE discovery file every 5 seconds
	DiscoveryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			if (!bEnabled) return false;
			if (IsConnected()) return true; // Already connected

			FIdeDiscovery Discovery = ReadDiscovery();
			if (Discovery.bValid && Discovery.Url != LastKnownUrl)
			{
				UE_LOG(LogLocalClient, Log, TEXT("IDE discovered at %s"), *Discovery.Url);
				DiscoveredToken = Discovery.Token;
				Connect(Discovery.Url);
			}
			return true;
		}),
		5.0f);
}

void FLocalClient::Shutdown()
{
	bEnabled = false;
	bAuthenticated = false;
	LastKnownUrl.Reset();
	DiscoveredToken.Reset();

	UnbindDelegates();

	if (DiscoveryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DiscoveryTickerHandle);
		DiscoveryTickerHandle.Reset();
	}
	if (ReconnectTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);
		ReconnectTickerHandle.Reset();
	}
	if (HeartbeatTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle.Reset();
	}

	Disconnect();
}

bool FLocalClient::IsConnected() const
{
	return WebSocket.IsValid() && WebSocket->IsConnected() && bAuthenticated;
}

// ── Connection ─────────────────────────────────────────────────────────

void FLocalClient::Connect(const FString& Url)
{
	if (WebSocket.IsValid()) Disconnect();

	LastKnownUrl = Url;
	UE_LOG(LogLocalClient, Log, TEXT("Connecting to IDE at %s"), *Url);

	WebSocket = FWebSocketsModule::Get().CreateWebSocket(Url, TEXT(""), TMap<FString, FString>());
	WebSocket->OnConnected().AddRaw(this, &FLocalClient::OnConnected);
	WebSocket->OnConnectionError().AddRaw(this, &FLocalClient::OnConnectionError);
	WebSocket->OnClosed().AddRaw(this, &FLocalClient::OnClosed);
	WebSocket->OnMessage().AddRaw(this, &FLocalClient::OnMessage);
	WebSocket->Connect();
}

void FLocalClient::Disconnect()
{
	for (const auto& Pair : ActiveToolHandlesByRequestId)
	{
		FNeoStackToolRegistry::Get().CancelExecute(Pair.Value);
	}
	ActiveToolHandlesByRequestId.Empty();

	if (WebSocket.IsValid())
	{
		if (WebSocket->IsConnected()) WebSocket->Close();
		WebSocket.Reset();
	}
}

void FLocalClient::ScheduleReconnect()
{
	if (!bEnabled) return;

	// If the IDE's discovery file is gone (IDE quit), go idle rather than spinning
	// the backoff — the 5s discovery poll reconnects when the file reappears.
	if (!ReadDiscovery().bValid)
	{
		ReconnectAttempt = 0;
		LastKnownUrl.Reset();
		return;
	}

	float Delay = FMath::Min(FMath::Pow(2.0f, static_cast<float>(ReconnectAttempt)), 30.0f);
	ReconnectAttempt++;

	UE_LOG(LogLocalClient, Log, TEXT("Reconnecting in %.0fs (attempt %d)"), Delay, ReconnectAttempt);

	if (ReconnectTickerHandle.IsValid())
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);

	ReconnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			// Re-discover in case the IDE restarted on a different port.
			FIdeDiscovery Discovery = ReadDiscovery();
			if (Discovery.bValid)
			{
				DiscoveredToken = Discovery.Token;
				Connect(Discovery.Url);
			}
			return false;
		}),
		Delay);
}

// ── Auth ───────────────────────────────────────────────────────────────

void FLocalClient::SendAuth()
{
	TSharedRef<FJsonObject> AuthMsg = MakeShared<FJsonObject>();
	AuthMsg->SetStringField(TEXT("type"), TEXT("auth"));
	AuthMsg->SetNumberField(TEXT("version"), 1);

	// Local-link auth: echo the discovery-file token so the IDE can verify us.
	if (!DiscoveredToken.IsEmpty())
	{
		AuthMsg->SetStringField(TEXT("token"), DiscoveredToken);
	}

	// Instance metadata (same fields as relay, minus API key)
	FString InstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	AuthMsg->SetStringField(TEXT("instanceId"), InstanceId);
	AuthMsg->SetStringField(TEXT("projectPath"),
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	AuthMsg->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	AuthMsg->SetStringField(TEXT("engineVersion"),
		FEngineVersion::Current().ToString(EVersionComponent::Minor));

#if PLATFORM_MAC
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Mac"));
#elif PLATFORM_WINDOWS
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Win64"));
#elif PLATFORM_LINUX
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Linux"));
#else
	AuthMsg->SetStringField(TEXT("platform"), TEXT("Unknown"));
#endif

	if (IPluginManager::Get().FindPlugin(TEXT("NeoStackAI")))
	{
		AuthMsg->SetStringField(TEXT("pluginVersion"),
			IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"))->GetDescriptor().VersionName);
	}

	SendMessage(AuthMsg);
}

// ── WebSocket Events ───────────────────────────────────────────────────

void FLocalClient::OnConnected()
{
	UE_LOG(LogLocalClient, Log, TEXT("WebSocket connected to IDE, sending auth..."));
	ReconnectAttempt = 0;
	SendAuth();
}

void FLocalClient::OnConnectionError(const FString& Error)
{
	UE_LOG(LogLocalClient, Warning, TEXT("IDE connection error: %s"), *Error);
	bAuthenticated = false;
	ScheduleReconnect();
}

void FLocalClient::OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogLocalClient, Log, TEXT("IDE connection closed (code=%d, reason=%s)"), StatusCode, *Reason);
	bAuthenticated = false;
	UnbindDelegates();
	ScheduleReconnect();
}

void FLocalClient::OnMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type)) return;

	if (Type == TEXT("auth_ok"))
	{
		bAuthenticated = true;
		UE_LOG(LogLocalClient, Log, TEXT("Authenticated with IDE!"));

		BindDelegates();

		// Start heartbeat
		if (HeartbeatTickerHandle.IsValid())
			FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float) -> bool
			{
				if (!IsConnected()) return true;
				TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
				Pong->SetStringField(TEXT("type"), TEXT("pong"));
				SendMessage(Pong);
				return true;
			}),
			30.0f);
		return;
	}

	if (Type == TEXT("auth_error"))
	{
		FString Msg;
		Json->TryGetStringField(TEXT("message"), Msg);
		UE_LOG(LogLocalClient, Error, TEXT("IDE auth failed: %s"), *Msg);
		return;
	}

	if (Type == TEXT("ping"))
	{
		TSharedRef<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(TEXT("type"), TEXT("pong"));
		SendMessage(Pong);
		return;
	}

	if (Type == TEXT("rpc_request"))
	{
		HandleRpcRequest(Json);
		return;
	}
}

// ── Message Sending ────────────────────────────────────────────────────

void FLocalClient::SendMessage(const TSharedRef<FJsonObject>& Message)
{
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Message, Writer);
	SendRawMessage(JsonString);
}

void FLocalClient::SendRawMessage(const FString& JsonString)
{
	if (!WebSocket.IsValid() || !WebSocket->IsConnected()) return;

	if (IsInGameThread())
	{
		WebSocket->Send(JsonString);
	}
	else
	{
		FString Copy = JsonString;
		AsyncTask(ENamedThreads::GameThread, [this, Copy]()
		{
			if (WebSocket.IsValid() && WebSocket->IsConnected())
				WebSocket->Send(Copy);
		});
	}
}

void FLocalClient::SendEvent(const FString& EventName, const TSharedRef<FJsonObject>& Data)
{
	if (!IsConnected()) return;
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("event"));
	Msg->SetStringField(TEXT("event"), EventName);
	Msg->SetObjectField(TEXT("data"), Data);
	SendMessage(Msg);
}

// ── RPC Response Helpers ───────────────────────────────────────────────

void FLocalClient::SendRpcResponse(const FString& RequestId, const FString& ResultJson)
{
	TSharedPtr<FJsonValue> ResultValue;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	FJsonSerializer::Deserialize(Reader, ResultValue);

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("type"), TEXT("rpc_response"));
	Response->SetStringField(TEXT("id"), RequestId);
	if (ResultValue.IsValid())
		Response->SetField(TEXT("result"), ResultValue);
	else
		Response->SetStringField(TEXT("result"), ResultJson);
	SendMessage(Response);
}

void FLocalClient::SendRpcError(const FString& RequestId, const FString& Error)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("type"), TEXT("rpc_response"));
	Response->SetStringField(TEXT("id"), RequestId);
	Response->SetStringField(TEXT("error"), Error);
	SendMessage(Response);
}

// ── RPC Dispatch ───────────────────────────────────────────────────────

void FLocalClient::HandleRpcRequest(const TSharedPtr<FJsonObject>& Request)
{
	FString RequestId, Method;
	Request->TryGetStringField(TEXT("id"), RequestId);
	Request->TryGetStringField(TEXT("method"), Method);
	if (RequestId.IsEmpty() || Method.IsEmpty()) return;

	const TArray<TSharedPtr<FJsonValue>>* Args = nullptr;
	Request->TryGetArrayField(TEXT("args"), Args);

	auto GetStringArg = [&](int32 Index) -> FString
	{
		if (Args && Args->IsValidIndex(Index) && (*Args)[Index]->Type == EJson::String)
			return (*Args)[Index]->AsString();
		return FString();
	};

	auto GetObjectArg = [&](int32 Index) -> TSharedPtr<FJsonObject>
	{
		if (Args && Args->IsValidIndex(Index) && (*Args)[Index]->Type == EJson::Object)
			return (*Args)[Index]->AsObject();
		return nullptr;
	};

	FAgentService& Svc = FAgentService::Get();

	// ── Standard relay methods (same as FRelayClient) ──────────────────
	if (Method == TEXT("getAgents"))           { SendRpcResponse(RequestId, Svc.GetAgents()); return; }
	if (Method == TEXT("getLastUsedAgent"))     { SendRpcResponse(RequestId, FString::Printf(TEXT("\"%s\""), *Svc.GetLastUsedAgent())); return; }
	if (Method == TEXT("getSessions"))          { SendRpcResponse(RequestId, Svc.GetAllSessions()); return; }
	if (Method == TEXT("getSessionMessages"))   { SendRpcResponse(RequestId, Svc.GetSessionMessages(GetStringArg(0))); return; }
	if (Method == TEXT("resumeSession"))        { SendRpcResponse(RequestId, Svc.ResumeSession(GetStringArg(0))); return; }
	if (Method == TEXT("deleteSession"))        { SendRpcResponse(RequestId, Svc.DeleteSession(GetStringArg(0))); return; }
	if (Method == TEXT("renameSession"))        { SendRpcResponse(RequestId, Svc.RenameSession(GetStringArg(0), GetStringArg(1))); return; }
	if (Method == TEXT("getModels"))            { SendRpcResponse(RequestId, Svc.GetModels(GetStringArg(0))); return; }
	if (Method == TEXT("getModes"))             { SendRpcResponse(RequestId, Svc.GetModes(GetStringArg(0))); return; }
	if (Method == TEXT("cancelPrompt"))         { Svc.CancelPrompt(GetStringArg(0)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }

	if (Method == TEXT("createSession"))
	{
		FString AgentName = GetStringArg(0);
		if (AgentName.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing agentName")); return; }
		SendRpcResponse(RequestId, Svc.CreateSession(AgentName));
		return;
	}

	if (Method == TEXT("sendPrompt"))
	{
		FString SessionId = GetStringArg(0), Text = GetStringArg(1);
		if (SessionId.IsEmpty() || Text.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing sessionId or text")); return; }
		SendRpcResponse(RequestId, Svc.SendPrompt(SessionId, Text));
		return;
	}

	if (Method == TEXT("setModel"))  { Svc.SetModel(GetStringArg(0), GetStringArg(1)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }
	if (Method == TEXT("setMode"))   { Svc.SetMode(GetStringArg(0), GetStringArg(1)); SendRpcResponse(RequestId, TEXT("\"ok\"")); return; }

	if (Method == TEXT("respondToPermission"))
	{
		// RequestId is a string round-tripped from the agent's JSON-RPC id (see
		// FACPClient::PendingServerRequestIdValues). Accept both String (modern clients)
		// and Number (legacy clients that pre-date the FString migration) for compat.
		FString PermReqId;
		if (Args && Args->IsValidIndex(1))
		{
			const TSharedPtr<FJsonValue>& V = (*Args)[1];
			if (V->Type == EJson::String)      PermReqId = V->AsString();
			else if (V->Type == EJson::Number) PermReqId = FString::Printf(TEXT("%lld"), static_cast<int64>(V->AsNumber()));
		}
		Svc.RespondToPermission(GetStringArg(0), PermReqId, GetStringArg(2));
		SendRpcResponse(RequestId, TEXT("\"ok\""));
		return;
	}

	// ── IDE-specific methods (new for LocalClient) ─────────────────────

	if (Method == TEXT("executeScript"))
	{
		FString Script = GetStringArg(0);
		if (Script.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing script argument")); return; }

		TSharedPtr<FJsonObject> ToolArgs = MakeShared<FJsonObject>();
		ToolArgs->SetStringField(TEXT("script"), Script);
		DispatchTool(RequestId, TEXT("execute_script"), ToolArgs);
		return;
	}

	// MCP-style tool call from the IDE's agent: args[0] = { name, arguments }.
	if (Method == TEXT("tools/call"))
	{
		TSharedPtr<FJsonObject> Call = GetObjectArg(0);
		if (!Call.IsValid()) { SendRpcError(RequestId, TEXT("Missing tools/call params")); return; }
		FString ToolName;
		Call->TryGetStringField(TEXT("name"), ToolName);
		// `arguments` is optional; DispatchTool substitutes an empty object if absent.
		TSharedPtr<FJsonObject> ToolArgs;
		const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
		if (Call->TryGetObjectField(TEXT("arguments"), ArgsObj) && ArgsObj)
		{
			ToolArgs = *ArgsObj;
		}
		DispatchTool(RequestId, ToolName, ToolArgs);
		return;
	}

	if (Method == TEXT("getEditorState"))
	{
		TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		State->SetStringField(TEXT("projectPath"),
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		State->SetStringField(TEXT("engineVersion"),
			FEngineVersion::Current().ToString());

		// Current level
		if (GEditor && GEditor->GetEditorWorldContext().World())
		{
			State->SetStringField(TEXT("currentLevel"),
				GEditor->GetEditorWorldContext().World()->GetMapName());
		}

		// PIE state
		State->SetBoolField(TEXT("isPlaying"), GEditor ? GEditor->IsPlaySessionInProgress() : false);

		FString StateJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&StateJson);
		FJsonSerializer::Serialize(State, Writer);
		SendRpcResponse(RequestId, StateJson);
		return;
	}

	if (Method == TEXT("tools/list") || Method == TEXT("getToolsList"))
	{
		TArray<TSharedPtr<FJsonValue>> ToolsArray;
		for (const FString& ToolName : FNeoStackToolRegistry::Get().GetToolNames())
		{
			FNeoStackToolBase* ToolDef = FNeoStackToolRegistry::Get().GetTool(ToolName);
			if (!ToolDef) continue;

			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), ToolDef->GetName());
			ToolObj->SetStringField(TEXT("description"), ToolDef->GetDescription());

			TSharedPtr<FJsonObject> Schema = ToolDef->GetInputSchema();
			if (Schema.IsValid())
				ToolObj->SetObjectField(TEXT("inputSchema"), Schema);

			ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), ToolsArray);

		FString ResultJson;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultJson);
		FJsonSerializer::Serialize(Result, Writer);
		SendRpcResponse(RequestId, ResultJson);
		return;
	}

	if (Method == TEXT("executeTool"))
	{
		DispatchTool(RequestId, GetStringArg(0), GetObjectArg(1));
		return;
	}

	SendRpcError(RequestId, FString::Printf(TEXT("Method '%s' not implemented"), *Method));
}

// ── Tool Dispatch ──────────────────────────────────────────────────────

void FLocalClient::DispatchTool(const FString& RequestId, const FString& ToolName,
	const TSharedPtr<FJsonObject>& ToolArgsIn)
{
	if (ToolName.IsEmpty()) { SendRpcError(RequestId, TEXT("Missing tool name")); return; }
	if (!FNeoStackToolRegistry::Get().HasTool(ToolName))
	{
		SendRpcError(RequestId, FString::Printf(TEXT("Tool '%s' not registered"), *ToolName));
		return;
	}
	const TSharedPtr<FJsonObject> ToolArgs = ToolArgsIn.IsValid() ? ToolArgsIn : MakeShared<FJsonObject>();

	// Async dispatch — SendRpcResponse fires from inside the tool's OnComplete, so
	// the RPC reply is naturally deferred until the script finishes (which may
	// include coroutine yields for async generation work). The registry marshals
	// execution to the game thread.
	TSharedRef<bool> bCompleted = MakeShared<bool>(false);
	FNeoStackToolHandle Handle = FNeoStackToolRegistry::Get().Execute(ToolName, ToolArgs,
		[this, RequestId, bCompleted](const FToolResult& Result)
		{
			*bCompleted = true;
			ActiveToolHandlesByRequestId.Remove(RequestId);
			TSharedRef<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), Result.bSuccess);
			ResultObj->SetBoolField(TEXT("isError"), !Result.bSuccess);

			TArray<TSharedPtr<FJsonValue>> ContentArray;
			TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
			TextContent->SetStringField(TEXT("type"), TEXT("text"));
			TextContent->SetStringField(TEXT("text"), Result.Output);
			ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));

			for (const auto& Img : Result.Images)
			{
				TSharedPtr<FJsonObject> ImgContent = MakeShared<FJsonObject>();
				ImgContent->SetStringField(TEXT("type"), TEXT("image"));
				ImgContent->SetStringField(TEXT("data"), Img.Base64Data);
				ImgContent->SetStringField(TEXT("mimeType"), Img.MimeType);
				ContentArray.Add(MakeShared<FJsonValueObject>(ImgContent));
			}

			ResultObj->SetArrayField(TEXT("content"), ContentArray);

			FString ResponseJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseJson);
			FJsonSerializer::Serialize(ResultObj, Writer);
			SendRpcResponse(RequestId, ResponseJson);
		});
	if (Handle.IsValid() && !*bCompleted)
	{
		ActiveToolHandlesByRequestId.Add(RequestId, Handle);
	}
}

// ── Delegate Binding ───────────────────────────────────────────────────

void FLocalClient::BindDelegates()
{
	UnbindDelegates();

	FAgentService& Svc = FAgentService::Get();

	OnAgentMessageHandle = Svc.OnMessage.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& UpdateJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);

			TSharedPtr<FJsonObject> UpdateObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UpdateJson);
			if (FJsonSerializer::Deserialize(Reader, UpdateObj) && UpdateObj.IsValid())
				Data->SetObjectField(TEXT("update"), UpdateObj);

			SendEvent(TEXT("onMessage"), Data);
		});

	OnAgentStateChangedHandle = Svc.OnStateChanged.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& StateStr, const FString& Message)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);
			Data->SetStringField(TEXT("agentName"), AgentName);
			Data->SetStringField(TEXT("state"), StateStr);
			Data->SetStringField(TEXT("message"), Message);
			SendEvent(TEXT("onStateChanged"), Data);
		});

	OnPermissionRequestHandle = Svc.OnPermissionRequest.AddLambda(
		[this](const FString& SessionId, const FString& RequestJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);

			TSharedPtr<FJsonObject> ReqObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
			if (FJsonSerializer::Deserialize(Reader, ReqObj) && ReqObj.IsValid())
			{
				for (const auto& Pair : ReqObj->Values)
					Data->SetField(Pair.Key, Pair.Value);
			}

			SendEvent(TEXT("onPermissionRequest"), Data);
		});

	OnModelsAvailableHandle = Svc.OnModelsAvailable.AddLambda(
		[this](const FString& AgentName, const FString& ModelsJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("agentName"), AgentName);
			Data->SetStringField(TEXT("modelsJson"), ModelsJson);
			SendEvent(TEXT("onModelsAvailable"), Data);
		});

	OnPlanUpdateHandle = Svc.OnPlanUpdate.AddLambda(
		[this](const FString& SessionId, const FString& PlanJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);
			Data->SetStringField(TEXT("planJson"), PlanJson);
			SendEvent(TEXT("onPlanUpdate"), Data);
		});

	UE_LOG(LogLocalClient, Log, TEXT("Bound to FAgentService delegates for IDE event forwarding"));
}

void FLocalClient::UnbindDelegates()
{
	FAgentService& Svc = FAgentService::Get();

	if (OnAgentMessageHandle.IsValid()) { Svc.OnMessage.Remove(OnAgentMessageHandle); OnAgentMessageHandle.Reset(); }
	if (OnAgentStateChangedHandle.IsValid()) { Svc.OnStateChanged.Remove(OnAgentStateChangedHandle); OnAgentStateChangedHandle.Reset(); }
	if (OnPermissionRequestHandle.IsValid()) { Svc.OnPermissionRequest.Remove(OnPermissionRequestHandle); OnPermissionRequestHandle.Reset(); }
	if (OnModelsAvailableHandle.IsValid()) { Svc.OnModelsAvailable.Remove(OnModelsAvailableHandle); OnModelsAvailableHandle.Reset(); }
	if (OnPlanUpdateHandle.IsValid()) { Svc.OnPlanUpdate.Remove(OnPlanUpdateHandle); OnPlanUpdateHandle.Reset(); }
}
