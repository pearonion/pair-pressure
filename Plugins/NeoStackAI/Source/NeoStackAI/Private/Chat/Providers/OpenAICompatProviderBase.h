// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/IChatProvider.h"
#include "Interfaces/IHttpRequest.h"

class FHttpStreamHandle;

/**
 * Abstract base class for OpenAI-compatible chat providers.
 *
 * Handles all the shared plumbing so concrete providers only override a
 * handful of identity and wire-format-nuance methods:
 *
 *   - GetId / GetDisplayName / GetDescription     — always required
 *   - GetDefaultBaseUrl / GetDefaultModel         — always required
 *   - GetStaticModels                             — always required
 *   - GetSettingsApiKey / GetSettingsBaseUrl      — how to read the per-provider settings rows
 *   - ConfigureHeaders                            — optional: custom auth / tracking headers
 *   - FormatReasoningParams                       — optional: reasoning format per provider
 *   - ModelSupportsReasoning                      — optional: per-model capability check
 *   - ParseDiscoveryResponse                      — optional: non-standard /models response
 *   - SupportsModelDiscovery / DiscoverModelsAsync — optional: live model list fetching
 *
 * The base class implements:
 *
 *   - ValidateConfig (checks API key presence if required)
 *   - GetCapabilities (default: tools + streaming, no images/reasoning)
 *   - RequiresApiKey (default: true)
 *   - StreamCompletion: builds request, fires HTTP, parses SSE, emits events
 *   - Message and tool translation to OpenAI JSON
 *   - SSE chunk parsing with tool-call delta accumulation
 *   - Usage / cost parsing
 *   - Error formatting and event emission
 *
 * Thread contract: as specified in IChatProvider. StreamCompletion returns
 * on the caller's thread; HTTP events fire on the HTTP worker thread.
 */
class FOpenAICompatProviderBase : public IChatProvider
{
public:
	// ── IChatProvider — subclass must override these ────────────────────

	virtual FString GetId() const override = 0;
	virtual FString GetDisplayName() const override = 0;
	virtual FString GetDescription() const override = 0;
	virtual TArray<FChatModelInfo> GetStaticModels() const override = 0;

	// ── IChatProvider — defaults provided; subclass may override ────────

	virtual FChatProviderCapabilities GetCapabilities() const override;
	virtual bool RequiresApiKey() const override { return true; }
	virtual bool ValidateConfig(FString& OutError) const override;

	virtual TSharedRef<IChatStreamHandle> StreamCompletion(
		const FChatRequest& Request,
		TSharedRef<IChatEventSink> Sink) override;

protected:
	// ── Hooks for concrete subclasses ───────────────────────────────────

	/** Default base URL for this provider if none is configured in settings. */
	virtual FString GetDefaultBaseUrl() const = 0;

	/** Default model id used if no selection has been made. */
	virtual FString GetDefaultModel() const = 0;

	/** API path for streaming chat completions. */
	virtual FString GetChatCompletionsPath() const { return TEXT("/chat/completions"); }

	/** API path for model discovery. */
	virtual FString GetModelsPath() const { return TEXT("/models"); }

	/**
	 * Read the user-configured API key for this provider from settings.
	 * Default implementation reads from FChatProviderSettings::ApiKey by
	 * matching GetId(); subclasses only need to override if their key is
	 * stored under a different field.
	 */
	virtual FString GetSettingsApiKey() const;

	/**
	 * Read the user-configured base URL override from settings. Returns
	 * an empty string if no override is set (in which case GetDefaultBaseUrl
	 * is used).
	 */
	virtual FString GetSettingsBaseUrlOverride() const;

	/**
	 * Configure provider-specific HTTP headers. Default implementation
	 * sets Authorization: Bearer <ApiKey>. Override for custom auth
	 * schemes or tracking headers (e.g. OpenRouter's HTTP-Referer / X-Title).
	 */
	virtual void ConfigureHeaders(
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest,
		const FString& ApiKey) const;

	/**
	 * Populate the reasoning-effort field in the request body for providers
	 * that support it. Default implementation does nothing (no reasoning).
	 *
	 * OpenRouter: override to set {reasoning: {effort: "..."}}.
	 * OpenAI direct: override to set top-level {reasoning_effort: "..."}.
	 */
	virtual void FormatReasoningParams(
		TSharedRef<FJsonObject> RequestBody,
		EReasoningEffort Effort) const { }

	/** Whether the given model supports reasoning. Default: false. */
	virtual bool ModelSupportsReasoning(const FString& ModelId) const { return false; }

	/**
	 * Parse the /models endpoint response into FChatModelInfo entries.
	 * Default implementation parses the OpenAI shape:
	 *   {"data": [{"id": "...", "name"?: "..."}...]}
	 * Override for non-standard formats (e.g. Ollama's /api/tags).
	 */
	virtual TArray<FChatModelInfo> ParseDiscoveryResponse(const FString& ResponseBody) const;

	/** Format an error message for display. Default: extracts "error.message" from JSON. */
	virtual FString FormatErrorMessage(int32 ResponseCode, const FString& ResponseBody) const;

private:
	// Per-stream state. Held via TSharedRef captured into lambdas so the
	// state outlives the provider's stack frame. One context per in-flight stream.
	struct FStreamContext;

	/** Build the OpenAI JSON request body from a canonical FChatRequest. */
	FString BuildRequestBody(const FChatRequest& Request) const;

	/** Translate canonical messages to OpenAI messages[] JSON. */
	TArray<TSharedPtr<FJsonValue>> BuildMessagesArray(const TArray<FChatMessage>& Messages) const;

	/** Translate canonical tools to OpenAI tools[] JSON. */
	TArray<TSharedPtr<FJsonValue>> BuildToolsArray(const TArray<FChatTool>& Tools) const;

	/** Process buffered SSE data: extract lines, parse events, emit to sink. */
	static void ProcessSseChunk(TSharedRef<FStreamContext> Context, const FString& NewBytes);

	/** Parse a single "data: {...}" line and emit events. */
	static void ProcessSseDataLine(TSharedRef<FStreamContext> Context, const FString& Json);

	/** Parse the "usage" object out of a stream chunk and emit UsageUpdate. */
	static void ParseAndEmitUsage(TSharedRef<FStreamContext> Context, const TSharedPtr<FJsonObject>& Chunk);

	/** Close any open tool calls and emit ToolUseEnd for each. */
	static void FinalizeOpenToolCalls(TSharedRef<FStreamContext> Context);

	/** Helper: emit an event to the sink. */
	static void Emit(TSharedRef<FStreamContext> Context, const FChatEvent& Event);

	/**
	 * Emit ReasoningDelta events for any reasoning text not yet seen on this
	 * stream. Handles both cumulative streams (each chunk contains
	 * everything-so-far, e.g. OpenRouter) and delta streams (each chunk is
	 * just the new text) by comparing FullReasoning against the running
	 * Context->EmittedReasoning buffer.
	 */
	static void EmitReasoningIncremental(
		TSharedRef<FStreamContext> Context, const FString& FullReasoning);
};
