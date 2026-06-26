// Copyright 2026 Betide Studio. All Rights Reserved.

#include "RelayClient.h"
#include "AgentService.h"
#include "ACPSettings.h"
#include "EntitlementClient.h"
#include "NeoStackAIModule.h"
#include "WebSocketsModule.h"
#include "Json.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Interfaces/IPluginManager.h"

// ── Singleton ──────────────────────────────────────────────────────────

FRelayClient& FRelayClient::Get()
{
	static FRelayClient Instance;
	return Instance;
}

FRelayClient::~FRelayClient()
{
	Shutdown();
}

// ── Lifecycle ──────────────────────────────────────────────────────────

void FRelayClient::Initialize()
{
	// Reset any deferred startup from a previous Initialize() call before
	// checking current settings; if remote access was toggled off, that stale
	// callback must not connect later.
	bShutdown = false;
	if (EntitlementResolvedHandle.IsValid())
	{
		FEntitlementClient::Get().RemoveWhenResolved(EntitlementResolvedHandle);
		EntitlementResolvedHandle.Reset();
	}

	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings || !Settings->bEnableRemoteAccess)
	{
		return;
	}

	const FString ApiKey = Settings->GetChatProviderApiKey(TEXT("neostack"));
	if (ApiKey.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[RelayClient] Remote access enabled but no NeoStack API key configured."));
		return;
	}

	// Defer the actual connect until the per-launch entitlement check has
	// resolved — opening the WebSocket and sending auth before the gate
	// decides means binary builds either leak relay traffic for lapsed
	// subscribers (if SendAuth runs early) or reject legit subscribers
	// (if we suppress on the false-during-pending state). WhenResolved
	// fires synchronously on source builds (gate is open immediately) and
	// after the HTTP response on binary builds.
	//
	// `this` is safe to capture by raw pointer — FRelayClient is a static
	// singleton with program-lifetime storage. The bShutdown guard handles
	// the late-arrival case (HTTP completes after Shutdown).
	EntitlementResolvedHandle = FEntitlementClient::Get().WhenResolved([this]()
	{
		EntitlementResolvedHandle.Reset();
		if (bShutdown)
		{
			// Editor/module went down while the entitlement HTTP was in
			// flight. Don't bring up the WebSocket on torn-down state.
			return;
		}
		if (!FEntitlementClient::Get().IsEntitled())
		{
			UE_LOG(LogTemp, Log,
				TEXT("[RelayClient] Skipped remote-access connect — no active entitlement."));
			return;
		}

		bEnabled = true;
		UE_LOG(LogTemp, Log, TEXT("[RelayClient] Initializing remote access..."));

		FModuleManager::Get().LoadModuleChecked<FWebSocketsModule>("WebSockets");
		Connect();
	});
}

void FRelayClient::Shutdown()
{
	bShutdown = true;
	bEnabled = false;
	bAuthenticated = false;
	if (EntitlementResolvedHandle.IsValid())
	{
		FEntitlementClient::Get().RemoveWhenResolved(EntitlementResolvedHandle);
		EntitlementResolvedHandle.Reset();
	}

	UnbindDelegates();

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

bool FRelayClient::IsConnected() const
{
	return WebSocket.IsValid() && WebSocket->IsConnected() && bAuthenticated;
}

FString FRelayClient::GetInstanceId() const
{
	return InstanceId;
}

// ── Connection ─────────────────────────────────────────────────────────

void FRelayClient::Connect()
{
	if (WebSocket.IsValid()) Disconnect();

	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	InstanceId = EnsureLocalInstanceId();
	if (InstanceId.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("[RelayClient] Could not create a remote instance ID."));
		return;
	}

	FString Url = BuildRelayUrl(Settings->RelayServerUrl, InstanceId);

	UE_LOG(LogTemp, Log, TEXT("[RelayClient] Connecting to %s"), *Url);

	WebSocket = FWebSocketsModule::Get().CreateWebSocket(Url, TEXT(""), TMap<FString, FString>());
	WebSocket->OnConnected().AddRaw(this, &FRelayClient::OnConnected);
	WebSocket->OnConnectionError().AddRaw(this, &FRelayClient::OnConnectionError);
	WebSocket->OnClosed().AddRaw(this, &FRelayClient::OnClosed);
	WebSocket->OnMessage().AddRaw(this, &FRelayClient::OnMessage);
	WebSocket->Connect();
}

FString FRelayClient::EnsureLocalInstanceId()
{
	UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return FString();

	FString LocalInstanceId = Settings->RemoteInstanceId.TrimStartAndEnd();
	if (!LocalInstanceId.IsEmpty())
	{
		return LocalInstanceId;
	}

	LocalInstanceId = TEXT("nsinst_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Settings->RemoteInstanceId = LocalInstanceId;
	Settings->SaveConfig();

	UE_LOG(LogTemp, Log, TEXT("[RelayClient] Generated remote instance ID: %s"), *LocalInstanceId);
	return LocalInstanceId;
}

FString FRelayClient::BuildRelayUrl(const FString& BaseUrl, const FString& LocalInstanceId) const
{
	FString Url = BaseUrl.TrimStartAndEnd();
	if (Url.IsEmpty())
	{
		Url = TEXT("wss://neostack.dev/api/relay/instance");
	}

	if (Url.Contains(TEXT("{instanceId}")))
	{
		Url = Url.Replace(TEXT("{instanceId}"), *LocalInstanceId, ESearchCase::CaseSensitive);
	}
	else if (!Url.EndsWith(TEXT("/")) && !Url.EndsWith(LocalInstanceId))
	{
		Url += TEXT("/");
		Url += LocalInstanceId;
	}
	else if (Url.EndsWith(TEXT("/")))
	{
		Url += LocalInstanceId;
	}

	return Url;
}

void FRelayClient::Disconnect()
{
	if (WebSocket.IsValid())
	{
		if (WebSocket->IsConnected()) WebSocket->Close();
		WebSocket.Reset();
	}
}

void FRelayClient::ScheduleReconnect()
{
	if (!bEnabled) return;

	float Delay = FMath::Min(FMath::Pow(2.0f, static_cast<float>(ReconnectAttempt)), 30.0f);
	ReconnectAttempt++;

	UE_LOG(LogTemp, Log, TEXT("[RelayClient] Reconnecting in %.0fs (attempt %d)"), Delay, ReconnectAttempt);

	if (ReconnectTickerHandle.IsValid())
		FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTickerHandle);

	ReconnectTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool { Connect(); return false; }),
		Delay);
}

// ── Auth ───────────────────────────────────────────────────────────────

void FRelayClient::SendAuth()
{
	const UACPSettings* Settings = UACPSettings::Get();
	if (!Settings) return;

	// Subscription gate — relay is a paid feature; no point burning the
	// connection for a lapsed subscriber. Logging-only because Initialize
	// may have already run before the entitlement check returned.
	if (!FEntitlementClient::Get().IsEntitled())
	{
		UE_LOG(LogNeoStackAI, Verbose,
			TEXT("[NSAI] RelayClient::SendAuth skipped — no active entitlement."));
		return;
	}

	TSharedRef<FJsonObject> AuthMsg = MakeShared<FJsonObject>();
	AuthMsg->SetStringField(TEXT("type"), TEXT("auth"));
	AuthMsg->SetNumberField(TEXT("version"), 1);
	AuthMsg->SetStringField(TEXT("apiKey"), Settings->GetChatProviderApiKey(TEXT("neostack")));
	AuthMsg->SetStringField(TEXT("instanceId"), InstanceId);

	const TSharedRef<FJsonObject> Metadata = GetInstanceMetadata();
	for (const auto& Pair : Metadata->Values)
	{
		AuthMsg->SetField(Pair.Key, Pair.Value);
	}
	SendMessage(AuthMsg);
}

TSharedRef<FJsonObject> FRelayClient::GetInstanceMetadata() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();

	FString Name = Settings ? Settings->InstanceName : FString();
	if (Name.IsEmpty()) Name = GetDefaultInstanceName();
	M->SetStringField(TEXT("name"), Name);
	M->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	M->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString(EVersionComponent::Minor));

#if PLATFORM_MAC
	M->SetStringField(TEXT("platform"), TEXT("Mac"));
#elif PLATFORM_WINDOWS
	M->SetStringField(TEXT("platform"), TEXT("Win64"));
#elif PLATFORM_LINUX
	M->SetStringField(TEXT("platform"), TEXT("Linux"));
#else
	M->SetStringField(TEXT("platform"), TEXT("Unknown"));
#endif

	if (IPluginManager::Get().FindPlugin(TEXT("NeoStackAI")))
	{
		M->SetStringField(TEXT("pluginVersion"),
			IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"))->GetDescriptor().VersionName);
	}

	return M;
}

FString FRelayClient::GetDefaultInstanceName()
{
	return FString::Printf(TEXT("%s - %s"), FPlatformProcess::ComputerName(), FApp::GetProjectName());
}

// ── WebSocket Events ───────────────────────────────────────────────────

void FRelayClient::OnConnected()
{
	UE_LOG(LogTemp, Log, TEXT("[RelayClient] WebSocket connected, sending auth..."));
	ReconnectAttempt = 0;
	SendAuth();
}

void FRelayClient::OnConnectionError(const FString& Error)
{
	UE_LOG(LogTemp, Warning, TEXT("[RelayClient] Connection error: %s"), *Error);
	bAuthenticated = false;
	ScheduleReconnect();
}

void FRelayClient::OnClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogTemp, Log, TEXT("[RelayClient] Connection closed (code=%d, reason=%s)"), StatusCode, *Reason);
	bAuthenticated = false;
	UnbindDelegates();

	if (StatusCode >= 4001 && StatusCode <= 4003)
	{
		UE_LOG(LogTemp, Error, TEXT("[RelayClient] Auth rejected. Not reconnecting."));
		bEnabled = false;
		return;
	}

	ScheduleReconnect();
}

void FRelayClient::OnMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type)) return;

	if (Type == TEXT("auth_ok"))
	{
		Json->TryGetStringField(TEXT("instanceId"), InstanceId);
		bAuthenticated = true;
		UE_LOG(LogTemp, Log, TEXT("[RelayClient] Authenticated! Instance ID: %s"), *InstanceId);

		BindDelegates();

		if (HeartbeatTickerHandle.IsValid())
			FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([this](float) -> bool { SendPing(); return true; }),
			30.0f);
		return;
	}

	if (Type == TEXT("auth_error"))
	{
		FString Msg;
		Json->TryGetStringField(TEXT("message"), Msg);
		UE_LOG(LogTemp, Error, TEXT("[RelayClient] Auth failed: %s"), *Msg);
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

// ── Heartbeat ──────────────────────────────────────────────────────────

void FRelayClient::SendPing()
{
	if (!IsConnected()) return;
	TSharedRef<FJsonObject> Ping = MakeShared<FJsonObject>();
	Ping->SetStringField(TEXT("type"), TEXT("ping"));
	Ping->SetNumberField(TEXT("t"), FPlatformTime::Seconds());
	SendMessage(Ping);
}

// ── Message Sending ────────────────────────────────────────────────────

void FRelayClient::SendMessage(const TSharedRef<FJsonObject>& Message)
{
	FString JsonString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
	FJsonSerializer::Serialize(Message, Writer);
	SendRawMessage(JsonString);
}

void FRelayClient::SendRawMessage(const FString& JsonString)
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

void FRelayClient::SendEvent(const FString& EventName, const TSharedRef<FJsonObject>& Data)
{
	if (!IsConnected()) return;
	TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
	Msg->SetStringField(TEXT("type"), TEXT("event"));
	Msg->SetStringField(TEXT("event"), EventName);
	Msg->SetObjectField(TEXT("data"), Data);
	SendMessage(Msg);
}

// ── RPC Response Helpers ───────────────────────────────────────────────

void FRelayClient::SendRpcResponse(const FString& RequestId, const FString& ResultJson)
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

void FRelayClient::SendRpcError(const FString& RequestId, const FString& Error)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("type"), TEXT("rpc_response"));
	Response->SetStringField(TEXT("id"), RequestId);
	Response->SetStringField(TEXT("error"), Error);
	SendMessage(Response);
}

// ── RPC Dispatch (all calls go through FAgentService) ──────────────────

void FRelayClient::HandleRpcRequest(const TSharedPtr<FJsonObject>& Request)
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

	FAgentService& Svc = FAgentService::Get();

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

	SendRpcError(RequestId, FString::Printf(TEXT("Method '%s' not implemented"), *Method));
}

// ── Delegate Binding (subscribe to FAgentService, forward to relay) ────

void FRelayClient::BindDelegates()
{
	UnbindDelegates();

	FAgentService& Svc = FAgentService::Get();

	// Forward all service events to web clients via the relay WebSocket
	OnAgentMessageHandle = Svc.OnMessage.AddLambda(
		[this](const FString& SessionId, const FString& AgentName, const FString& UpdateJson)
		{
			if (!IsConnected()) return;
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("sessionId"), SessionId);

			// Parse the update JSON and embed it
			TSharedPtr<FJsonObject> UpdateObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UpdateJson);
			if (FJsonSerializer::Deserialize(Reader, UpdateObj) && UpdateObj.IsValid())
			{
				Data->SetObjectField(TEXT("update"), UpdateObj);
			}

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

	UE_LOG(LogTemp, Log, TEXT("[RelayClient] Bound to FAgentService delegates for event forwarding"));
}

void FRelayClient::UnbindDelegates()
{
	FAgentService& Svc = FAgentService::Get();

	if (OnAgentMessageHandle.IsValid()) { Svc.OnMessage.Remove(OnAgentMessageHandle); OnAgentMessageHandle.Reset(); }
	if (OnAgentStateChangedHandle.IsValid()) { Svc.OnStateChanged.Remove(OnAgentStateChangedHandle); OnAgentStateChangedHandle.Reset(); }
	if (OnPermissionRequestHandle.IsValid()) { Svc.OnPermissionRequest.Remove(OnPermissionRequestHandle); OnPermissionRequestHandle.Reset(); }
	if (OnModelsAvailableHandle.IsValid()) { Svc.OnModelsAvailable.Remove(OnModelsAvailableHandle); OnModelsAvailableHandle.Reset(); }
	if (OnPlanUpdateHandle.IsValid()) { Svc.OnPlanUpdate.Remove(OnPlanUpdateHandle); OnPlanUpdateHandle.Reset(); }
}
