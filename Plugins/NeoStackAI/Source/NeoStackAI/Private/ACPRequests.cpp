// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPRequests.h"

namespace ACPRequests
{
	// ── MCP server config ───────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> MakeHttpMcpServer(
		const FString& Name,
		const FString& Url,
		const TArray<TPair<FString, FString>>& Headers)
	{
		// spec: McpServerHttp — required fields are { name, url, headers }; type discriminator
		// is "http". headers must be an array (not a map) of { name, value } objects.
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("type"), TEXT("http"));
		Obj->SetStringField(TEXT("url"), Url);

		TArray<TSharedPtr<FJsonValue>> HeaderArray;
		HeaderArray.Reserve(Headers.Num());
		for (const TPair<FString, FString>& Pair : Headers)
		{
			TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
			H->SetStringField(TEXT("name"), Pair.Key);
			H->SetStringField(TEXT("value"), Pair.Value);
			HeaderArray.Add(MakeShared<FJsonValueObject>(H));
		}
		Obj->SetArrayField(TEXT("headers"), HeaderArray);
		return Obj;
	}

	TSharedRef<FJsonObject> MakeStdioMcpServer(
		const FString& Name,
		const FString& Command,
		const TArray<FString>& Args,
		const TMap<FString, FString>& Env)
	{
		// spec: McpServerStdio — required { name, command, args, env }. env is an array of
		// { name, value }, not a map. args must always be present (use [] if none).
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Name);
		Obj->SetStringField(TEXT("command"), Command);

		TArray<TSharedPtr<FJsonValue>> ArgArray;
		ArgArray.Reserve(Args.Num());
		for (const FString& A : Args)
		{
			ArgArray.Add(MakeShared<FJsonValueString>(A));
		}
		Obj->SetArrayField(TEXT("args"), ArgArray);

		TArray<TSharedPtr<FJsonValue>> EnvArray;
		EnvArray.Reserve(Env.Num());
		for (const TPair<FString, FString>& Pair : Env)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), Pair.Key);
			E->SetStringField(TEXT("value"), Pair.Value);
			EnvArray.Add(MakeShared<FJsonValueObject>(E));
		}
		Obj->SetArrayField(TEXT("env"), EnvArray);
		return Obj;
	}

	// ── Content blocks ──────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> MakeTextContentBlock(const FString& Text)
	{
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		return Block;
	}

	TSharedRef<FJsonObject> MakeImageContentBlock(const FString& MimeType, const FString& Base64Data)
	{
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("image"));
		Block->SetStringField(TEXT("mimeType"), MimeType);
		Block->SetStringField(TEXT("data"), Base64Data);
		return Block;
	}

	// ── initialize ──────────────────────────────────────────────────────────────────

	TSharedRef<FJsonObject> MakeInitializeParams(
		int32 ProtocolVersion,
		const FACPClientCapabilities& ClientCaps,
		const FString& ClientName,
		const FString& ClientVersion)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("protocolVersion"), ProtocolVersion);

		// clientInfo — optional today, becomes required in a future protocol version per spec.
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		Info->SetStringField(TEXT("name"), ClientName);
		Info->SetStringField(TEXT("version"), ClientVersion);
		Params->SetObjectField(TEXT("clientInfo"), Info);

		// clientCapabilities — NOT "capabilities". This is the field name that strict
		// serde-based adapters require. Sending the wrong key produces -32602.
		TSharedRef<FJsonObject> Caps = MakeShared<FJsonObject>();

		TSharedRef<FJsonObject> Fs = MakeShared<FJsonObject>();
		Fs->SetBoolField(TEXT("readTextFile"), ClientCaps.bSupportsFileSystem);
		Fs->SetBoolField(TEXT("writeTextFile"), ClientCaps.bSupportsFileSystem);
		Caps->SetObjectField(TEXT("fs"), Fs);

		// terminal is a top-level boolean per spec, not an object.
		Caps->SetBoolField(TEXT("terminal"), ClientCaps.bSupportsTerminal);

		// auth.terminal is not in the stable schema — Zed sends it anyway via meta extensions.
		// Lenient adapters ignore unknown keys; we mirror Zed's behavior.
		if (ClientCaps.bSupportsAuthTerminal)
		{
			TSharedRef<FJsonObject> Auth = MakeShared<FJsonObject>();
			Auth->SetBoolField(TEXT("terminal"), true);
			Caps->SetObjectField(TEXT("auth"), Auth);
		}

		Params->SetObjectField(TEXT("clientCapabilities"), Caps);
		return Params;
	}

	// ── session lifecycle ───────────────────────────────────────────────────────────

	static void AttachMcpServers(const TSharedRef<FJsonObject>& Params, const TArray<TSharedPtr<FJsonValue>>& McpServers)
	{
		// Spec: mcpServers is required even when empty. Always set the field.
		Params->SetArrayField(TEXT("mcpServers"), McpServers);
	}

	TSharedRef<FJsonObject> MakeNewSessionParams(
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers,
		TSharedPtr<FJsonObject> Meta)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("cwd"), AbsoluteCwd);
		AttachMcpServers(Params, McpServers);
		if (Meta.IsValid() && Meta->Values.Num() > 0)
		{
			Params->SetObjectField(TEXT("_meta"), Meta);
		}
		return Params;
	}

	TSharedRef<FJsonObject> MakeLoadSessionParams(
		const FString& SessionId,
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetStringField(TEXT("cwd"), AbsoluteCwd);
		AttachMcpServers(Params, McpServers);
		return Params;
	}

	TSharedRef<FJsonObject> MakeResumeSessionParams(
		const FString& SessionId,
		const FString& AbsoluteCwd,
		const TArray<TSharedPtr<FJsonValue>>& McpServers)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetStringField(TEXT("cwd"), AbsoluteCwd);
		AttachMcpServers(Params, McpServers);
		return Params;
	}

	TSharedRef<FJsonObject> MakePromptParams(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& PromptBlocks)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetArrayField(TEXT("prompt"), PromptBlocks);
		return Params;
	}

	TSharedRef<FJsonObject> MakeCancelParams(const FString& SessionId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		return Params;
	}

	TSharedRef<FJsonObject> MakeCloseSessionParams(const FString& SessionId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		return Params;
	}

	TSharedRef<FJsonObject> MakeDeleteSessionParams(const FString& SessionId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		return Params;
	}

	TSharedRef<FJsonObject> MakeSetModeParams(const FString& SessionId, const FString& ModeId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetStringField(TEXT("modeId"), ModeId);
		return Params;
	}

	TSharedRef<FJsonObject> MakeSetModelParams(const FString& SessionId, const FString& ModelId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetStringField(TEXT("modelId"), ModelId);
		return Params;
	}

	TSharedRef<FJsonObject> MakeSetConfigOptionParams(
		const FString& SessionId,
		const FString& ConfigId,
		const FString& Value)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), SessionId);
		Params->SetStringField(TEXT("configId"), ConfigId);
		Params->SetStringField(TEXT("value"), Value);
		return Params;
	}

	TSharedRef<FJsonObject> MakeListSessionsParams(const FString& Cursor)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		// spec: cursor is optional; only include it on second-and-later pages.
		if (!Cursor.IsEmpty())
		{
			Params->SetStringField(TEXT("cursor"), Cursor);
		}
		return Params;
	}

	TSharedRef<FJsonObject> MakeAuthenticateParams(const FString& MethodId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("methodId"), MethodId);
		return Params;
	}
}
