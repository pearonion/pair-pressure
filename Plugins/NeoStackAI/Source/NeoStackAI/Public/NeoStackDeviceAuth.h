// Copyright 2025 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Delegates/Delegate.h"

/**
 * OAuth 2.0 Device Authorization Grant flow against neostack.dev. Drives the
 * "Sign in with NeoStack" button: requests a device code, opens the browser
 * to /device/approve, polls /api/auth/device/token, then redeems the bearer
 * for a freshly minted neostack_* API key via /api/device/redeem-key. On
 * success, writes the key to the "neostack" chat-provider row in
 * UACPSettings — the canonical store everyone reads from.
 *
 * Single-flight: Begin() is a no-op while a flow is already running.
 * Cancel() drops in-flight HTTP + ticker so the user can back out.
 */
enum class ENeoStackDeviceAuthStatus : uint8
{
	Idle,
	RequestingCode,    // POST /api/auth/device/code
	WaitingForUser,    // browser opened, waiting for human approval
	Polling,           // poll loop on /api/auth/device/token
	Redeeming,         // POST /api/device/redeem-key
	Success,
	Error
};

class NEOSTACKAI_API FNeoStackDeviceAuth
{
public:
	static FNeoStackDeviceAuth& Get();

	DECLARE_MULTICAST_DELEGATE_ThreeParams(
		FOnStatusChanged,
		ENeoStackDeviceAuthStatus /*Status*/,
		const FString& /*Message — human-readable, may be empty*/,
		const FString& /*VerificationUri — populated only in WaitingForUser*/);

	/** Kick off the device flow. No-op if a flow is already in progress. */
	void Begin();

	/** Stop the flow. Drops the ticker; in-flight HTTP responses become no-ops. */
	void Cancel();

	/** Editor shutdown — releases the ticker without firing a status update. */
	void Shutdown();

	ENeoStackDeviceAuthStatus GetStatus() const { return Status; }
	FOnStatusChanged& OnStatusChanged() { return StatusDelegate; }

private:
	FNeoStackDeviceAuth() = default;

	void RequestDeviceCode();
	void StartPolling();
	bool PollTick(float Dt);
	void Redeem();
	void OnKeyReceived(const FString& Key, const FString& OrganizationId);
	void Finish(ENeoStackDeviceAuthStatus FinalStatus, const FString& Message);

	void BroadcastStatus(const FString& Message = FString(),
	                     const FString& VerificationUri = FString());

	static FString ComputeKeyName();
	static FString ResolveBaseUrl();

	ENeoStackDeviceAuthStatus Status = ENeoStackDeviceAuthStatus::Idle;
	FString DeviceCode;
	FString UserCode;
	FString VerificationUri;
	FString KeyName;
	int32   PollIntervalSec = 5;
	double  ExpiresAtSeconds = 0.0;
	uint32  FlowId = 0;                       // bumps each Begin(); stale HTTP callbacks check this
	FTSTicker::FDelegateHandle PollHandle;
	FOnStatusChanged StatusDelegate;

	static constexpr const TCHAR* ClientId = TEXT("neostack-ue-plugin");
};
