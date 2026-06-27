// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Named-constructor helpers for ACP JSON-RPC request params.
 *
 * Hand-rolled JSON has historically been the #1 source of -32602 "Invalid params" in our
 * client (e.g. sending `capabilities` when the spec field is `clientCapabilities`).
 * Centralising the wire shapes here means a typo can only be made in one place,
 * not at every call site.
 *
 * Reference: references/agent-client-protocol/schema/schema.json (canonical) and
 * references/agent-client-protocol/src/agent.rs (Rust SDK — the reference impl that
 * other adapters are checked against).
 *
 * One function per request shape. Each returns a populated FJsonObject ready to hand to
 * FACPClient::SendRequest / SendNotification. They never serialize themselves — that's
 * the transport layer's job.
 */
namespace ACPRequests
{
	// ── MCP server config (spec: McpServerHttp / McpServerSse / McpServerStdio) ─────

	/**
	 * HTTP MCP entry. Per spec, callers MUST gate on agentCapabilities.mcpCapabilities.http
	 * before including HTTP servers in `mcpServers` — strict adapters reject otherwise.
	 */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeHttpMcpServer(
		const FString& Name,
		const FString& Url,
		const TArray<TPair<FString, FString>>& Headers = {});

	/** Stdio MCP entry. All ACP agents support this transport (spec: McpServerStdio). */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeStdioMcpServer(
		const FString& Name,
		const FString& Command,
		const TArray<FString>& Args = {},
		const TMap<FString, FString>& Env = {});

	// ── Content blocks (for prompt arrays, tool results, etc.) ──────────────────────

	NEOSTACKAI_API TSharedRef<FJsonObject> MakeTextContentBlock(const FString& Text);
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeImageContentBlock(const FString& MimeType, const FString& Base64Data);

	// ── Top-level request param builders (one per ACP method) ───────────────────────

	/**
	 * initialize — the request that sets up the session and negotiates capabilities.
	 *
	 * IMPORTANT: the field is `clientCapabilities`, not `capabilities`. This was the
	 * single largest source of -32602s before this helper existed.
	 */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeInitializeParams(
		int32 ProtocolVersion,
		const FACPClientCapabilities& ClientCaps,
		const FString& ClientName,
		const FString& ClientVersion);

	/**
	 * session/new — `cwd` MUST be an absolute path with no trailing slash; spec says
	 * `mcpServers` is required (use [] if you have none).
	 */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeNewSessionParams(
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers,
		TSharedPtr<FJsonObject> Meta = nullptr);

	/** session/load — same shape as session/new plus a sessionId. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeLoadSessionParams(
		const FString& SessionId,
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers);

	/** session/resume (UNSTABLE: unstable_session_resume). Caller must pre-check capability. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeResumeSessionParams(
		const FString& SessionId,
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers);

	/** session/prompt — sessionId + array of content blocks. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakePromptParams(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& PromptBlocks);

	/** session/cancel — notification (no response). */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeCancelParams(const FString& SessionId);

	/** session/close (UNSTABLE: unstable_session_close). */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeCloseSessionParams(const FString& SessionId);

	/** session/delete — non-standard extension; only Codex/Gemini-style adapters that opt in. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeDeleteSessionParams(const FString& SessionId);

	/** session/set_mode — `modeId` is the discriminator. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeSetModeParams(const FString& SessionId, const FString& ModeId);

	/** session/set_model (UNSTABLE: unstable_session_model). */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeSetModelParams(const FString& SessionId, const FString& ModelId);

	/**
	 * session/set_config_option — unified key/value config (Codex's pattern).
	 * Value is a string per the current schema; structured values go via _meta.
	 */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeSetConfigOptionParams(
		const FString& SessionId,
		const FString& ConfigId,
		const FString& Value);

	/** session/list — with optional cursor for pagination. Empty cursor = first page. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeListSessionsParams(const FString& Cursor = FString());

	/** authenticate — `methodId` is one of the IDs from initialize response's authMethods. */
	NEOSTACKAI_API TSharedRef<FJsonObject> MakeAuthenticateParams(const FString& MethodId);
}
