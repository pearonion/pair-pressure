// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Chat/ChatTypes.h"
#include "Chat/IChatEventSink.h"
#include "Chat/IChatStreamHandle.h"

/**
 * The chat provider interface.
 *
 * A provider represents one way to talk to a model backend — OpenRouter,
 * Anthropic native, a local Ollama server, a user-defined endpoint, etc.
 * Each provider owns its own model list, wire format, auth, and discovery.
 *
 * The session (FChatSession) depends only on this interface. It does not
 * know anything about HTTP, SSE, OpenAI JSON, or Anthropic events.
 *
 * Provider implementations come in two flavors:
 *
 *  1. OpenAI-compatible providers inherit from FOpenAICompatProviderBase,
 *     which handles SSE parsing, message translation, and stream events.
 *     Subclasses only override identity, base URL, default model, static
 *     model list, and optionally header/reasoning format hooks.
 *
 *  2. Native-protocol providers (e.g. FAnthropicNativeProvider) implement
 *     this interface directly and translate their own wire format to
 *     FChatEvent inside StreamCompletion.
 *
 * Thread contract:
 *   - Lifecycle methods (identity, capabilities, config validation, model
 *     listing) are called on the game thread.
 *   - StreamCompletion is invoked on the game thread and returns immediately
 *     with a handle. Actual request execution happens asynchronously.
 *   - Events delivered to the sink may arrive on any thread. Sinks that
 *     touch game state must marshal themselves.
 *   - DiscoverModelsAsync's callback may be invoked on any thread. Consumers
 *     must marshal themselves.
 */
class NEOSTACKAI_API IChatProvider
{
public:
	virtual ~IChatProvider() = default;

	// ── Identity ────────────────────────────────────────────────────────

	/** Stable, unique identifier. "openrouter", "anthropic", "userprovider_<guid>". */
	virtual FString GetId() const = 0;

	/** User-facing name shown in the picker and settings UI. */
	virtual FString GetDisplayName() const = 0;

	/** One-line description shown as help text in the settings panel. */
	virtual FString GetDescription() const = 0;

	// ── Capabilities ────────────────────────────────────────────────────

	virtual FChatProviderCapabilities GetCapabilities() const = 0;

	/** Whether this provider needs an API key. Local providers like Ollama return false. */
	virtual bool RequiresApiKey() const { return true; }

	// ── Configuration ───────────────────────────────────────────────────

	/**
	 * Checks that the provider has everything it needs to run a completion
	 * (API key present if required, base URL resolves, etc.). Called before
	 * a session connects and whenever settings change.
	 *
	 * Returns true if the provider is ready. On false, OutError contains a
	 * human-readable reason the settings UI can show as a badge.
	 */
	virtual bool ValidateConfig(FString& OutError) const = 0;

	// ── Models ──────────────────────────────────────────────────────────

	/**
	 * The provider's baked-in model list. Always returns something; this is
	 * the fallback used when discovery is unavailable or has failed.
	 */
	virtual TArray<FChatModelInfo> GetStaticModels() const = 0;

	/** Whether DiscoverModelsAsync does anything useful. */
	virtual bool SupportsModelDiscovery() const { return false; }

	/**
	 * Fetches the live model list from the provider's API.
	 *
	 * Callback receives (Models, ErrorString). An empty Models array with a
	 * non-empty ErrorString indicates failure — the registry falls back to
	 * GetStaticModels() in that case. An empty Models array with an empty
	 * ErrorString is a successful empty response (e.g. Ollama with no local
	 * models pulled).
	 *
	 * Default implementation reports "not supported". Providers that return
	 * true from SupportsModelDiscovery must override this.
	 */
	virtual void DiscoverModelsAsync(
		TFunction<void(TArray<FChatModelInfo>, FString /*Error*/)> Callback)
	{
		Callback({}, TEXT("Model discovery not supported by this provider"));
	}

	// ── Completion ──────────────────────────────────────────────────────

	/**
	 * Start a streaming completion.
	 *
	 * Called on the game thread. Returns immediately with a handle; the
	 * actual HTTP request is fired asynchronously. Events are delivered to
	 * the sink on whatever thread the transport uses (HTTP worker thread
	 * for HTTP-based providers).
	 *
	 * The sink must live at least until the stream emits Done or Error.
	 * The session holds a TSharedRef to both the handle and the sink to
	 * guarantee this; callers must do the same.
	 *
	 * The provider is responsible for:
	 *   - Translating FChatRequest::Messages/Tools into its wire format.
	 *   - Hoisting system messages out of Messages if its format requires
	 *     a separate system field (Anthropic).
	 *   - Emitting well-formed event sequences. Specifically, every stream
	 *     that was not cancelled must terminate with exactly one Done or
	 *     Error event, and ToolUseStart / ToolUseEnd must be paired.
	 *   - Parsing accumulated ToolUseArgsDelta buffers into ToolArgs before
	 *     emitting ToolUseEnd.
	 *   - Honoring Cancel() on the returned handle promptly.
	 */
	virtual TSharedRef<IChatStreamHandle> StreamCompletion(
		const FChatRequest& Request,
		TSharedRef<IChatEventSink> Sink) = 0;
};
