// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h" // FACPToolResultImage

// ============================================================================
// Canonical chat types
//
// Everything above the provider layer speaks these types. Concrete providers
// translate to/from their wire format (OpenAI JSON, Anthropic native, etc.)
// at the edge. No wire-format knowledge leaks into FChatSession or the UI.
//
// These are plain C++ structs, not USTRUCTs — they're runtime-only (never
// saved to config, never exposed to Blueprint, never sent over Unreal's
// serialization). Keeping them out of the reflection system lets them hold
// TSharedPtr<FJsonObject> directly and avoids .generated.h churn.
//
// The settings-layer counterparts (FChatProviderSettings, FUserChatProvider,
// FChatModelEntry) are separate USTRUCTs declared in ACPSettings.h; these
// canonical types are the in-memory working set built from and handed to
// providers at runtime.
// ============================================================================

enum class EChatRole : uint8
{
	System,
	User,
	Assistant,
	Tool
};

enum class EChatContentKind : uint8
{
	Text,        // Plain text (any role)
	Image,       // User-attached image
	ToolUse,     // Assistant requesting a tool call
	ToolResult   // Tool role message body
};

/**
 * A single content block inside a message. Messages can contain multiple blocks
 * (an assistant turn often looks like [Text, ToolUse, ToolUse]). This mirrors
 * Anthropic's native block structure; OpenAI-compat providers flatten it.
 */
struct FChatContentBlock
{
	EChatContentKind Kind = EChatContentKind::Text;

	// Text blocks: plain text body.
	// ToolResult blocks: stringified output from the tool execution.
	FString Text;

	// Image blocks
	FString ImageBase64;
	FString ImageMime;

	// ToolUse + ToolResult share a tool-call id so the provider can correlate them.
	FString ToolUseId;

	// ToolUse: the tool being invoked.
	FString ToolName;

	// ToolUse: parsed JSON arguments object (populated by the provider when
	// streaming completes; null while arguments are still being accumulated).
	TSharedPtr<FJsonObject> ToolArgs;

	// ToolUse: raw JSON-string buffer accumulated from streaming arg deltas.
	// The provider parses this into ToolArgs when the tool-use block closes.
	FString RawArgsBuffer;

	// ToolResult: true if the tool reported a failure.
	bool bToolError = false;

	// ToolResult: images produced by the tool (e.g. screenshot). Only providers
	// that advertise image support forward these to the model.
	TArray<FACPToolResultImage> ResultImages;
};

/**
 * A single message in a conversation. Role determines which blocks are valid:
 *   System    -> [Text]
 *   User      -> [Text, Image*]  or  [ToolResult+] (Anthropic uses user role for results)
 *   Assistant -> [Text?, ToolUse*]
 *   Tool      -> [ToolResult] (OpenAI wire format)
 */
struct FChatMessage
{
	EChatRole Role = EChatRole::User;
	TArray<FChatContentBlock> Content;

	// Assistant-only. Accumulated reasoning/thinking text from streaming
	// `reasoning_content` / `reasoning_details[].text` deltas. Kept off
	// FChatContentBlock so UI code that only cares about displayable content
	// doesn't have to filter it out. Re-emitted on subsequent turns so
	// providers in thinking mode (DeepSeek V4, etc.) can satisfy their
	// "reasoning_content must be passed back" requirement on follow-up calls.
	FString Reasoning;
};

/**
 * A tool definition advertised to the provider in FChatRequest::Tools.
 * Sourced from NeoStackToolRegistry + MCPServer registered tools.
 */
struct FChatTool
{
	FString Name;
	FString Description;

	// JSON Schema "object" describing the tool's input. Required; an empty
	// object schema is used if the tool takes no arguments.
	TSharedPtr<FJsonObject> InputSchema;
};

/**
 * Reasoning/thinking effort level. Canonical enum; providers map to their
 * wire format (OpenAI's reasoning_effort, OpenRouter's reasoning.effort,
 * Anthropic's thinking.budget_tokens, etc.) inside their own class.
 */
enum class EReasoningEffort : uint8
{
	None,
	Low,
	Medium,
	High
};

/**
 * Token/cost accounting for a single turn or a full session.
 */
struct FChatUsage
{
	int32 InputTokens = 0;
	int32 OutputTokens = 0;
	int32 TotalTokens = 0;
	int32 CachedTokens = 0;
	int32 ReasoningTokens = 0;
	double CostAmount = 0.0;
	FString CostCurrency;
};

/**
 * A single completion request handed to IChatProvider::StreamCompletion.
 * The provider translates this into its wire format and streams events back.
 */
struct FChatRequest
{
	// Bare model id, no provider prefix. E.g. "anthropic/claude-sonnet-4.5"
	// for OpenRouter, "claude-sonnet-4-5-20250929" for Anthropic native.
	FString ModelId;

	// Full conversation history including system message at index 0.
	// Provider is responsible for hoisting system out if its wire format
	// uses a separate system field (e.g. Anthropic).
	TArray<FChatMessage> Messages;

	// Tools advertised to the model. May be empty.
	TArray<FChatTool> Tools;

	float Temperature = 0.7f;
	int32 MaxTokens = 16384;
	EReasoningEffort Reasoning = EReasoningEffort::None;
};

// ============================================================================
// Streaming events
// ============================================================================

enum class EChatEventKind : uint8
{
	TextDelta,         // Assistant text fragment (streamed)
	ReasoningDelta,    // Assistant "thinking" / reasoning fragment (streamed)
	ToolUseStart,      // New tool call beginning: ToolUseId + ToolName
	ToolUseArgsDelta,  // Raw JSON fragment for the in-progress tool call's arguments
	ToolUseEnd,        // Tool call args complete; provider has parsed ToolArgs
	UsageUpdate,       // Token/cost accounting for the current turn
	Error,             // Terminal failure; no Done will follow
	Done               // Clean end of stream
};

/**
 * A single event emitted by a provider during StreamCompletion.
 * Only fields relevant to Kind are meaningful; others should be ignored.
 */
struct FChatEvent
{
	EChatEventKind Kind = EChatEventKind::TextDelta;

	// TextDelta, ReasoningDelta, ToolUseArgsDelta
	FString TextChunk;

	// ToolUseStart, ToolUseArgsDelta, ToolUseEnd
	FString ToolUseId;

	// ToolUseStart
	FString ToolName;

	// ToolUseEnd: parsed arguments (provider already converted RawArgsBuffer).
	TSharedPtr<FJsonObject> ToolArgs;

	// UsageUpdate
	FChatUsage Usage;

	// Error
	FString ErrorMessage;
	int32 ErrorCode = 0;
};

// ============================================================================
// Model descriptor
// ============================================================================

/**
 * A single model as exposed to the UI and session. Owned by exactly one
 * provider (no cross-provider routing). The picker stores / selects models
 * using the prefixed form "<providerId>:<modelId>".
 */
struct FChatModelInfo
{
	// Bare model id as sent to the provider's API.
	FString ModelId;

	// Human-readable name for the UI.
	FString DisplayName;

	// Short description shown under the model name in the picker.
	FString Description;

	// The provider that owns this model (single owner, always set).
	FString ProviderId;

	// Cached from the provider for UI badge rendering.
	FString ProviderDisplayName;

	// Capability hints shown as icons in the picker.
	bool bSupportsReasoning = false;
	bool bSupportsImages = false;
	bool bSupportsTools = true;

	// Returns "<providerId>:<modelId>" — the form used in the picker and settings.
	FString GetPrefixedId() const
	{
		return FString::Printf(TEXT("%s:%s"), *ProviderId, *ModelId);
	}
};

// ============================================================================
// Provider capability descriptor
// ============================================================================

struct FChatProviderCapabilities
{
	bool bSupportsTools = true;
	bool bSupportsStreaming = true;
	bool bSupportsReasoning = false;
	bool bSupportsImages = false;
};

// ============================================================================
// Helpers
// ============================================================================

namespace ChatTypes
{
	/**
	 * Split "providerId:modelId" into its parts on the first ':' only,
	 * preserving any ':' or '/' in the model id. Returns false if the
	 * input has no ':' or if either side is empty.
	 */
	NEOSTACKAI_API bool SplitPrefixedModelId(
		const FString& PrefixedId,
		FString& OutProviderId,
		FString& OutModelId);

	/** Convert an EChatRole to the OpenAI wire-format role string. */
	NEOSTACKAI_API FString RoleToOpenAIString(EChatRole Role);

	/** Convert an EReasoningEffort to a lowercase string ("none"/"low"/"medium"/"high"). */
	NEOSTACKAI_API FString ReasoningEffortToString(EReasoningEffort Effort);

	/** Parse a lowercase string back to EReasoningEffort; unknown values map to None. */
	NEOSTACKAI_API EReasoningEffort ReasoningEffortFromString(const FString& Str);
}
