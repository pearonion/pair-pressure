// Copyright 2025 Betide Studio. All Rights Reserved.

#include "NeoStackDeviceAuth.h"

#include "ACPSettings.h"
#include "EntitlementClient.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "NeoStackAIModule.h"

namespace
{
	FString UrlEncode(const FString& In)
	{
		return FGenericPlatformHttp::UrlEncode(In);
	}

	TSharedPtr<FJsonObject> ParseJson(FHttpResponsePtr Response)
	{
		TSharedPtr<FJsonObject> Json;
		if (!Response.IsValid()) return Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		FJsonSerializer::Deserialize(Reader, Json);
		return Json;
	}
}

FNeoStackDeviceAuth& FNeoStackDeviceAuth::Get()
{
	static FNeoStackDeviceAuth Instance;
	return Instance;
}

FString FNeoStackDeviceAuth::ResolveBaseUrl()
{
	if (const UACPSettings* Settings = UACPSettings::Get())
	{
		const FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
		if (!Override.IsEmpty())
		{
			FString Root = Override;
			Root.RemoveFromEnd(TEXT("/"));
			Root.RemoveFromEnd(TEXT("/v1"));
			Root.RemoveFromEnd(TEXT("/"));
			return Root;
		}
	}
	return TEXT("https://neostack.dev");
}

FString FNeoStackDeviceAuth::ComputeKeyName()
{
	// "Plugin · <hostname> · <project> · YYYY-MM-DD" — easy for the user to
	// recognise on the keys page and to pick which one to revoke later.
	const FString Host = FPlatformProcess::ComputerName();
	const FString Project = FApp::GetProjectName();
	const FString Date = FDateTime::Now().ToString(TEXT("%Y-%m-%d"));
	if (!Project.IsEmpty())
	{
		return FString::Printf(TEXT("UE Plugin · %s · %s · %s"), *Host, *Project, *Date);
	}
	return FString::Printf(TEXT("UE Plugin · %s · %s"), *Host, *Date);
}

void FNeoStackDeviceAuth::Begin()
{
	if (Status != ENeoStackDeviceAuthStatus::Idle &&
	    Status != ENeoStackDeviceAuthStatus::Success &&
	    Status != ENeoStackDeviceAuthStatus::Error)
	{
		// A flow is already running. Bring the UI back to whatever the
		// current state is in case the user re-clicked.
		BroadcastStatus(TEXT("Already in progress."), VerificationUri);
		return;
	}

	++FlowId;
	DeviceCode.Reset();
	UserCode.Reset();
	VerificationUri.Reset();
	KeyName = ComputeKeyName();
	PollIntervalSec = 5;
	ExpiresAtSeconds = 0.0;

	Status = ENeoStackDeviceAuthStatus::RequestingCode;
	BroadcastStatus(TEXT("Requesting device code…"));
	RequestDeviceCode();
}

void FNeoStackDeviceAuth::Cancel()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
	++FlowId; // invalidate any in-flight HTTP callbacks
	if (Status != ENeoStackDeviceAuthStatus::Idle)
	{
		Status = ENeoStackDeviceAuthStatus::Idle;
		BroadcastStatus(TEXT("Cancelled."));
	}
}

void FNeoStackDeviceAuth::Shutdown()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
	++FlowId;
	Status = ENeoStackDeviceAuthStatus::Idle;
}

void FNeoStackDeviceAuth::BroadcastStatus(const FString& Message, const FString& VerifyUri)
{
	StatusDelegate.Broadcast(Status, Message, VerifyUri);
}

void FNeoStackDeviceAuth::RequestDeviceCode()
{
	const FString Url = FString::Printf(TEXT("%s/api/auth/device/code"), *ResolveBaseUrl());
	const uint32 LocalFlow = FlowId;

	const FString Payload = FString::Printf(
		TEXT("{\"client_id\":\"%s\",\"scope\":\"openid profile email\"}"),
		ClientId);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetTimeout(15.0f);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Payload);

	Request->OnProcessRequestComplete().BindLambda(
		[LocalFlow](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			FNeoStackDeviceAuth& Self = FNeoStackDeviceAuth::Get();
			if (LocalFlow != Self.FlowId) return; // cancelled or superseded

			const int32 Code = (bOk && Response.IsValid()) ? Response->GetResponseCode() : 0;
			if (!bOk || !Response.IsValid() || Code < 200 || Code >= 300)
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					FString::Printf(TEXT("Failed to request device code (HTTP %d)."), Code));
				return;
			}

			TSharedPtr<FJsonObject> Json = ParseJson(Response);
			if (!Json.IsValid())
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error, TEXT("Malformed device code response."));
				return;
			}

			Json->TryGetStringField(TEXT("device_code"), Self.DeviceCode);
			Json->TryGetStringField(TEXT("user_code"), Self.UserCode);
			FString VerifyUri;
			Json->TryGetStringField(TEXT("verification_uri_complete"), VerifyUri);
			if (VerifyUri.IsEmpty())
			{
				Json->TryGetStringField(TEXT("verification_uri"), VerifyUri);
			}
			int32 Interval = 5;
			Json->TryGetNumberField(TEXT("interval"), Interval);
			int32 ExpiresIn = 1800;
			Json->TryGetNumberField(TEXT("expires_in"), ExpiresIn);

			if (Self.DeviceCode.IsEmpty() || Self.UserCode.IsEmpty() || VerifyUri.IsEmpty())
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					TEXT("Device code response missing required fields."));
				return;
			}

			// Append the auto-generated key name so /device/approve can show
			// the user what'll be created. URL-encode — spaces, dots, etc.
			const FString FullUri = FString::Printf(TEXT("%s&name=%s"),
				*VerifyUri, *UrlEncode(Self.KeyName));

			Self.VerificationUri = FullUri;
			Self.PollIntervalSec = FMath::Max(Interval, 1);
			Self.ExpiresAtSeconds = FPlatformTime::Seconds() + ExpiresIn;

			FString LaunchError;
			FPlatformProcess::LaunchURL(*FullUri, TEXT(""), &LaunchError);
			if (!LaunchError.IsEmpty())
			{
				UE_LOG(LogNeoStackAI, Warning,
					TEXT("[NSAI] Couldn't launch browser for device auth: %s"), *LaunchError);
			}

			Self.Status = ENeoStackDeviceAuthStatus::WaitingForUser;
			Self.BroadcastStatus(TEXT("Approve in your browser to continue."), FullUri);
			Self.StartPolling();
		});

	if (!Request->ProcessRequest())
	{
		Finish(ENeoStackDeviceAuthStatus::Error, TEXT("HTTP module rejected request."));
	}
}

void FNeoStackDeviceAuth::StartPolling()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
	}
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FNeoStackDeviceAuth::PollTick),
		static_cast<float>(PollIntervalSec));
}

bool FNeoStackDeviceAuth::PollTick(float /*Dt*/)
{
	if (Status != ENeoStackDeviceAuthStatus::WaitingForUser &&
	    Status != ENeoStackDeviceAuthStatus::Polling)
	{
		return false; // stop ticking
	}

	if (FPlatformTime::Seconds() > ExpiresAtSeconds)
	{
		Finish(ENeoStackDeviceAuthStatus::Error,
			TEXT("Sign-in window expired. Try again."));
		return false;
	}

	Status = ENeoStackDeviceAuthStatus::Polling;
	BroadcastStatus(TEXT("Waiting for approval…"), VerificationUri);

	const uint32 LocalFlow = FlowId;
	const FString Url = FString::Printf(TEXT("%s/api/auth/device/token"), *ResolveBaseUrl());
	const FString Payload = FString::Printf(
		TEXT("{\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\","
		     "\"device_code\":\"%s\",\"client_id\":\"%s\"}"),
		*DeviceCode, ClientId);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetTimeout(10.0f);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Payload);

	Request->OnProcessRequestComplete().BindLambda(
		[LocalFlow](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			FNeoStackDeviceAuth& Self = FNeoStackDeviceAuth::Get();
			if (LocalFlow != Self.FlowId) return;

			const int32 Code = (bOk && Response.IsValid()) ? Response->GetResponseCode() : 0;
			TSharedPtr<FJsonObject> Json = ParseJson(Response);

			if (Code == 200 && Json.IsValid())
			{
				FString AccessToken;
				if (Json->TryGetStringField(TEXT("access_token"), AccessToken) &&
				    !AccessToken.IsEmpty())
				{
					// Approval landed. Stop polling and redeem the API key.
					if (Self.PollHandle.IsValid())
					{
						FTSTicker::GetCoreTicker().RemoveTicker(Self.PollHandle);
						Self.PollHandle.Reset();
					}
					Self.Redeem();
					return;
				}
			}

			// 400-class with structured OAuth error.
			FString OAuthError;
			if (Json.IsValid()) Json->TryGetStringField(TEXT("error"), OAuthError);
			if (OAuthError == TEXT("authorization_pending"))
			{
				// Keep ticking.
				return;
			}
			if (OAuthError == TEXT("slow_down"))
			{
				Self.PollIntervalSec += 5;
				if (Self.PollHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Self.PollHandle);
				}
				Self.PollHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateRaw(&Self, &FNeoStackDeviceAuth::PollTick),
					static_cast<float>(Self.PollIntervalSec));
				return;
			}
			if (OAuthError == TEXT("access_denied"))
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					TEXT("Access denied. You can try again any time."));
				return;
			}
			if (OAuthError == TEXT("expired_token"))
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					TEXT("Sign-in window expired. Try again."));
				return;
			}

			if (Code != 200)
			{
				// Transient network blip — keep polling and let the expiry
				// guard end it eventually.
				return;
			}
		});

	Request->ProcessRequest();
	return true; // keep ticking
}

void FNeoStackDeviceAuth::Redeem()
{
	Status = ENeoStackDeviceAuthStatus::Redeeming;
	BroadcastStatus(TEXT("Connecting…"));

	const uint32 LocalFlow = FlowId;
	const FString Url = FString::Printf(TEXT("%s/api/device/redeem-key"), *ResolveBaseUrl());
	const FString Payload = FString::Printf(
		TEXT("{\"userCode\":\"%s\",\"deviceCode\":\"%s\",\"clientId\":\"%s\"}"),
		*UserCode, *DeviceCode, ClientId);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetTimeout(15.0f);
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Payload);

	Request->OnProcessRequestComplete().BindLambda(
		[LocalFlow](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			FNeoStackDeviceAuth& Self = FNeoStackDeviceAuth::Get();
			if (LocalFlow != Self.FlowId) return;

			const int32 Code = (bOk && Response.IsValid()) ? Response->GetResponseCode() : 0;
			TSharedPtr<FJsonObject> Json = ParseJson(Response);
			if (Code != 200 || !Json.IsValid())
			{
				FString Detail;
				if (Json.IsValid()) Json->TryGetStringField(TEXT("error"), Detail);
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					FString::Printf(TEXT("Couldn't fetch key (HTTP %d %s)."), Code, *Detail));
				return;
			}

			FString Key, OrgId;
			Json->TryGetStringField(TEXT("key"), Key);
			Json->TryGetStringField(TEXT("organizationId"), OrgId);
			if (Key.IsEmpty())
			{
				Self.Finish(ENeoStackDeviceAuthStatus::Error,
					TEXT("Server returned an empty key."));
				return;
			}
			Self.OnKeyReceived(Key, OrgId);
		});

	if (!Request->ProcessRequest())
	{
		Finish(ENeoStackDeviceAuthStatus::Error, TEXT("HTTP module rejected redeem request."));
	}
}

void FNeoStackDeviceAuth::OnKeyReceived(const FString& Key, const FString& /*OrganizationId*/)
{
	if (UACPSettings* Settings = UACPSettings::Get())
	{
		// SetChatProviderApiKey already calls SaveConfig(). RelayClient and
		// everything else read via GetChatProviderApiKey("neostack") after
		// the consolidation pass, so this single write is the whole update.
		Settings->SetChatProviderApiKey(TEXT("neostack"), Key);
	}
	FEntitlementClient::Get().Refresh();
	Finish(ENeoStackDeviceAuthStatus::Success, TEXT("Connected to NeoStack."));
}

void FNeoStackDeviceAuth::Finish(ENeoStackDeviceAuthStatus FinalStatus, const FString& Message)
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
	Status = FinalStatus;
	BroadcastStatus(Message);
}
