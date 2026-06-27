// Copyright 2026 Betide Studio. All Rights Reserved.

#include "NSAIAnalytics.h"
#include "ACPSettings.h"
#include "NeoStackAIModule.h"

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

static FString ResolveAnalyticsEndpoint()
{
	FString Base = TEXT("https://neostack.dev");
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
		if (!Override.IsEmpty())
		{
			FString Root = Override;
			Root.RemoveFromEnd(TEXT("/"));
			Root.RemoveFromEnd(TEXT("/v1"));
			Root.RemoveFromEnd(TEXT("/"));
			Base = Root;
		}
	}
	return Base + TEXT("/api/analytics/v1/events");
}

static FString ResolveAnalyticsApiKey()
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		return Settings->GetChatProviderApiKey(TEXT("neostack"));
	}
	return FString();
}

// Strip asset paths from error messages to avoid leaking project structure
// "/Game/MyProject/Characters/BP_Hero" → "<path>"
FString FNSAIAnalytics::SanitizeErrorForAnalytics(const FString& Msg)
{
	FString Sanitized = Msg;

	// Replace /Game/... paths
	int32 Idx = 0;
	while ((Idx = Sanitized.Find(TEXT("/Game/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx)) != INDEX_NONE)
	{
		// Find end of path (space, quote, single quote, paren, comma, or end of string)
		int32 End = Idx + 6;
		while (End < Sanitized.Len())
		{
			TCHAR C = Sanitized[End];
			if (C == TEXT(' ') || C == TEXT('"') || C == TEXT('\'') || C == TEXT(')') || C == TEXT(',') || C == TEXT('\n'))
				break;
			End++;
		}
		Sanitized = Sanitized.Left(Idx) + TEXT("<path>") + Sanitized.Mid(End);
		Idx += 6; // length of "<path>"
	}

	// Also strip /Engine/ paths
	Idx = 0;
	while ((Idx = Sanitized.Find(TEXT("/Engine/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Idx)) != INDEX_NONE)
	{
		int32 End = Idx + 8;
		while (End < Sanitized.Len())
		{
			TCHAR C = Sanitized[End];
			if (C == TEXT(' ') || C == TEXT('"') || C == TEXT('\'') || C == TEXT(')') || C == TEXT(',') || C == TEXT('\n'))
				break;
			End++;
		}
		Sanitized = Sanitized.Left(Idx) + TEXT("<path>") + Sanitized.Mid(End);
		Idx += 6;
	}

	return Sanitized.Left(2000); // Truncate
}

// Per-user install ID file. Lives outside the project so a single user
// across multiple projects produces one stable identifier.
static FString GetInstallIdFilePath()
{
	return FPaths::Combine(
		FPlatformProcess::UserSettingsDir(),
		TEXT("NeoStackAI"),
		TEXT("analytics_id.txt"));
}

static FString LegacyInstallIdFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AIK"), TEXT("analytics_id.txt"));
}

FNSAIAnalytics& FNSAIAnalytics::Get()
{
	static FNSAIAnalytics Instance;
	return Instance;
}

void FNSAIAnalytics::Initialize()
{
	if (bInitialized) return;
	bInitialized = true;
	bHadActiveSession = false;

	if (!IsEnabled()) return;

	bHadActiveSession = true;

	// Record plugin loaded event
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	RecordEvent(TEXT("plugin_loaded"), Props);

	// Start periodic flush ticker
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FNSAIAnalytics::OnTick),
		FlushIntervalSeconds);
}

void FNSAIAnalytics::Shutdown()
{
	if (!bInitialized) return;

	// Remove ticker
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// Do not call IsEnabled() / UACPSettings::Get() here — UObject config may already be invalid
	// during editor teardown (same class of issue as WebUIBridge cleanup in ShutdownModule).
	if (bHadActiveSession)
	{
		RecordEventWithoutOptInCheck(TEXT("plugin_unloaded"));
		Flush(/*bSynchronous=*/ true);
	}

	bInitialized = false;
	bHadActiveSession = false;
}

bool FNSAIAnalytics::IsEnabled() const
{
	const UACPSettings* Settings = UACPSettings::Get();
	return Settings ? Settings->bEnableAnalytics : true;
}

void FNSAIAnalytics::RecordEvent(const FString& EventName, TSharedPtr<FJsonObject> Properties)
{
	if (!IsEnabled()) return;
	RecordEventWithoutOptInCheck(EventName, Properties);
}

void FNSAIAnalytics::RecordEventWithoutOptInCheck(const FString& EventName, TSharedPtr<FJsonObject> Properties)
{
	FAnalyticsEvent Evt;
	Evt.EventName = EventName;
	Evt.Properties = Properties;
	Evt.ClientTimestamp = FDateTime::UtcNow().ToUnixTimestamp() * 1000.0;

	bool bShouldFlush = false;
	{
		FScopeLock Lock(&BufferLock);
		EventBuffer.Add(MoveTemp(Evt));

		// Force flush if buffer is getting large
		if (EventBuffer.Num() >= MaxBufferSize)
		{
			bShouldFlush = true;
		}
	}

	if (bShouldFlush)
	{
		Flush(/*bSynchronous=*/ false);
	}
}

void FNSAIAnalytics::RecordToolExecution(const FString& ToolName, bool bSuccess, double DurationMs, const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("tool"), ToolName);
	Props->SetBoolField(TEXT("success"), bSuccess);
	Props->SetNumberField(TEXT("duration_ms"), DurationMs);
	if (!bSuccess && !ErrorMessage.IsEmpty())
	{
		Props->SetStringField(TEXT("error"), SanitizeErrorForAnalytics(ErrorMessage));
	}
	RecordEvent(TEXT("tool_executed"), Props);
}

void FNSAIAnalytics::RecordAgentSelected(const FString& AgentName)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("agent"), AgentName);
	RecordEvent(TEXT("agent_selected"), Props);
}

void FNSAIAnalytics::RecordLuaError(const FString& BindingName, const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("binding"), BindingName);
	Props->SetStringField(TEXT("error"), SanitizeErrorForAnalytics(ErrorMessage));
	RecordEvent(TEXT("lua_error"), Props);
}

FString FNSAIAnalytics::GetInstallId()
{
	if (!CachedInstallId.IsEmpty())
	{
		return CachedInstallId;
	}

	const FString FilePath = GetInstallIdFilePath();
	if (FFileHelper::LoadFileToString(CachedInstallId, *FilePath))
	{
		CachedInstallId.TrimStartAndEndInline();
		if (!CachedInstallId.IsEmpty())
		{
			return CachedInstallId;
		}
	}

	const FString LegacyPath = LegacyInstallIdFilePath();
	if (FFileHelper::LoadFileToString(CachedInstallId, *LegacyPath))
	{
		CachedInstallId.TrimStartAndEndInline();
	}

	if (CachedInstallId.IsEmpty())
	{
		CachedInstallId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		UE_LOG(LogNeoStackAI, Log, TEXT("[AIK Analytics] Generated new anonymous install ID"));
	}

	const FString Dir = FPaths::GetPath(FilePath);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(CachedInstallId, *FilePath);

	return CachedInstallId;
}

bool FNSAIAnalytics::OnTick(float DeltaTime)
{
	Flush(/*bSynchronous=*/ false);
	return true; // Keep ticking
}

void FNSAIAnalytics::Flush(bool bSynchronous)
{
	TArray<FAnalyticsEvent> EventsToSend;
	{
		FScopeLock Lock(&BufferLock);
		if (EventBuffer.Num() == 0) return;
		EventsToSend = MoveTemp(EventBuffer);
		EventBuffer.Reset();
	}

	// Build JSON payload
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("installId"), GetInstallId());

	// Plugin version from .uplugin
	FString PluginVersion = TEXT("unknown");
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NeoStackAI"));
	if (Plugin.IsValid())
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	Payload->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Payload->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString(EVersionComponent::Minor));

#if PLATFORM_WINDOWS
	Payload->SetStringField(TEXT("platform"), TEXT("Win64"));
#elif PLATFORM_MAC
	Payload->SetStringField(TEXT("platform"), TEXT("Mac"));
#elif PLATFORM_LINUX
	Payload->SetStringField(TEXT("platform"), TEXT("Linux"));
#else
	Payload->SetStringField(TEXT("platform"), TEXT("Other"));
#endif

	// Events array
	TArray<TSharedPtr<FJsonValue>> EventsArray;
	for (const FAnalyticsEvent& Evt : EventsToSend)
	{
		TSharedPtr<FJsonObject> EvtObj = MakeShared<FJsonObject>();
		EvtObj->SetStringField(TEXT("event"), Evt.EventName);
		EvtObj->SetNumberField(TEXT("clientTimestamp"), Evt.ClientTimestamp);
		if (Evt.Properties.IsValid())
		{
			EvtObj->SetObjectField(TEXT("properties"), Evt.Properties);
		}
		EventsArray.Add(MakeShared<FJsonValueObject>(EvtObj));
	}
	Payload->SetArrayField(TEXT("events"), EventsArray);

	// Serialize to string
	FString PayloadString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
	FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

	// Send HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	HttpRequest->SetURL(ResolveAnalyticsEndpoint());
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	const FString ApiKey = ResolveAnalyticsApiKey();
	if (!ApiKey.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
	}

	HttpRequest->SetContentAsString(PayloadString);

	HttpRequest->OnProcessRequestComplete().BindLambda(
		[](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			if (!bConnectedSuccessfully || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				UE_LOG(LogNeoStackAI, Verbose,
					TEXT("[AIK Analytics] Failed to send analytics (code: %d)"),
					Response.IsValid() ? Response->GetResponseCode() : 0);
			}
		});

	HttpRequest->ProcessRequest();

	// For synchronous flush (shutdown), block until complete
	if (bSynchronous)
	{
		// Give it up to 3 seconds to complete
		double StartTime = FPlatformTime::Seconds();
		while (HttpRequest->GetStatus() == EHttpRequestStatus::Processing)
		{
			if (FPlatformTime::Seconds() - StartTime > 3.0)
			{
				HttpRequest->CancelRequest();
				break;
			}
			FHttpModule::Get().GetHttpManager().Tick(0.01f);
			FPlatformProcess::Sleep(0.01f);
		}
	}

	UE_LOG(LogNeoStackAI, Verbose,
		TEXT("[AIK Analytics] Flushed %d events"), EventsToSend.Num());
}
