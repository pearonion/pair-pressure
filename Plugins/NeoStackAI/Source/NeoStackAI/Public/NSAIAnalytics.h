// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/**
 * Lightweight, privacy-first analytics for NeoStackAI.
 *
 * - Anonymous random UUID per install (no PII, no machine fingerprint)
 * - Batches events in memory, flushes periodically + on shutdown
 * - Single HTTP POST of JSON to the Betide analytics endpoint
 * - Opt-out via plugin settings (bEnableAnalytics)
 * - Never sends: user names, file paths, code content, API keys
 */
class NEOSTACKAI_API FNSAIAnalytics
{
public:
	static FNSAIAnalytics& Get();

	/** Call once from StartupModule */
	void Initialize();

	/** Call from ShutdownModule — flushes remaining events synchronously */
	void Shutdown();

	/** Record an analytics event. Properties should be simple key-value pairs. */
	void RecordEvent(const FString& EventName, TSharedPtr<FJsonObject> Properties = nullptr);

	/** Convenience: record a tool execution (error message is sanitized — paths stripped) */
	void RecordToolExecution(const FString& ToolName, bool bSuccess, double DurationMs, const FString& ErrorMessage = FString());

	/** Convenience: record which agent was selected */
	void RecordAgentSelected(const FString& AgentName);

	/** Convenience: record a Lua error */
	void RecordLuaError(const FString& BindingName, const FString& ErrorMessage);

	/** Strip /Game/ and /Engine/ asset paths from error messages to avoid leaking project structure */
	static FString SanitizeErrorForAnalytics(const FString& ErrorMessage);

	/** Get or create the anonymous install ID (shared with crash reporter) */
	FString GetInstallId();

private:
	FNSAIAnalytics() = default;

	/** Flush buffered events to the server */
	void Flush(bool bSynchronous = false);

	/** Ticker callback for periodic flush */
	bool OnTick(float DeltaTime);

	/** Whether analytics collection is enabled (reads project settings — not safe during module shutdown). */
	bool IsEnabled() const;

	/** Buffer an event without calling IsEnabled() (used when UObject settings may be torn down). */
	void RecordEventWithoutOptInCheck(const FString& EventName, TSharedPtr<FJsonObject> Properties = nullptr);

	struct FAnalyticsEvent
	{
		FString EventName;
		TSharedPtr<FJsonObject> Properties;
		double ClientTimestamp; // Unix time in ms
	};

	FCriticalSection BufferLock;
	TArray<FAnalyticsEvent> EventBuffer;

	FString CachedInstallId;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bInitialized = false;
	/** True if analytics was enabled when Initialize() ran — avoids UACPSettings::Get() during Shutdown(). */
	bool bHadActiveSession = false;

	static constexpr float FlushIntervalSeconds = 60.0f;
	/** Server accepts up to 500 events per request. Flush before exceeding that contract. */
	static constexpr int32 MaxBufferSize = 500;
};
