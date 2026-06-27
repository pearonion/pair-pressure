// Copyright 2026 Betide Studio. All Rights Reserved.

#include "ACPAgentQuirks.h"

// ============================================================================
// Default quirks (no special behavior — pure ACP)
// ============================================================================

static const FACPAgentQuirks GDefaultQuirks;

// ============================================================================
// Quirks Map
// ============================================================================

TMap<FString, FACPAgentQuirks>& FACPAgentQuirksMap::GetMap()
{
	static TMap<FString, FACPAgentQuirks> Map;
	static bool bInitialized = false;
	if (!bInitialized)
	{
		InitializeMap(Map);
		bInitialized = true;
	}
	return Map;
}

void FACPAgentQuirksMap::InitializeMap(TMap<FString, FACPAgentQuirks>& Map)
{
	// ────────────────────────────────────────────────────────────────
	// GitHub Copilot CLI
	// Needs MCP server injected via --additional-mcp-config @<path>
	// Resumes sessions at process launch via --resume flag
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.MCPInjection = EMCPInjectionStrategy::CliConfigFile;
		Q.MCPCliFlag = TEXT("--additional-mcp-config");
		Q.bMCPCliPrefixAt = true;
		Q.MCPConfigTemplate = TEXT(R"({"mcpServers":{"unreal-editor":{"type":"http","url":"{url}","tools":["*"]}}})");
		Q.ResumeStrategy = EResumeStrategy::LaunchArg;
		Q.ResumeLaunchFlag = TEXT("--resume");
		Map.Add(TEXT("github-copilot"), Q);
		Map.Add(TEXT("github-copilot-cli"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// Gemini CLI
	// Needs MCP server injected via GEMINI_CLI_SYSTEM_SETTINGS_PATH env var
	// Resumes sessions at process launch via --resume flag
	// Model changes are local-only (model selected at launch time)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.MCPInjection = EMCPInjectionStrategy::EnvVarConfigFile;
		Q.MCPEnvVarName = TEXT("GEMINI_CLI_SYSTEM_SETTINGS_PATH");
		Q.MCPConfigTemplate = TEXT(R"({"mcpServers":{"unreal-editor":{"httpUrl":"{url}"}}})");
		Q.ResumeStrategy = EResumeStrategy::LaunchArg;
		Q.ResumeLaunchFlag = TEXT("--resume");
		Q.bModelChangesLocalOnly = true;
		Map.Add(TEXT("gemini"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// OpenCode
	// System prompt must be delivered via first user message
	// (it ignores _meta.systemPrompt in session/new)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.SystemPromptDeliveryOverride = TEXT("FirstUserMessage");
		Map.Add(TEXT("opencode"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// OpenRouter (built-in Chat Gateway, not ACP subprocess)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.Transport = EAgentTransport::ChatGateway;
		Q.MCPInjection = EMCPInjectionStrategy::None;
		Map.Add(TEXT("openrouter"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// NeoStack Cloud (built-in Chat Gateway, scoped to the "neostack" provider)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.Transport = EAgentTransport::ChatGateway;
		Q.MCPInjection = EMCPInjectionStrategy::None;
		Map.Add(TEXT("neostack"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// codex-acp
	// Prefer the npm package (@zed-industries/codex-acp) over the Rust binary release.
	// Reasoning: the npm wrapper version-locks against the Rust crate version exactly,
	// while binary GitHub releases lag for some platforms. The npm path is also smaller
	// (downloads ~5MB of JS shim that fetches the platform binary on first run via
	// optional dependencies) versus the 30MB+ binary archive.
	// Registry id verified against:
	//   - references/zed/crates/agent_servers/src/custom.rs:19 (CODEX_ID = "codex-acp")
	//   - references/codex-acp/Cargo.toml + npm/package.json (@zed-industries/codex-acp)
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.PreferredDistribution = EPreferredDistribution::Npx;
		// Codex ignores _meta.systemPrompt on session/new — the Rust adapter's prompt
		// path (thread.rs:build_prompt_items) only consumes UserInput / Compact / Review /
		// OverrideTurnContext ops. System instructions come from AGENTS.md or must be
		// prepended to the first user message. Without this override, NeoStack's custom
		// system prompt silently drops for Codex users.
		Q.SystemPromptDeliveryOverride = TEXT("FirstUserMessage");
		Map.Add(TEXT("codex-acp"), Q);
	}

	// ────────────────────────────────────────────────────────────────
	// Cursor (cursor-agent)
	// Cursor CLI runs ACP natively (`cursor-agent acp`) and does not advertise
	// `_meta.terminal-auth` in its initialize response. Without an override the
	// auth-terminal fallback would launch `cursor-agent.cmd acp`, which waits
	// on stdin JSON-RPC and appears frozen to the user (reported by AIK users
	// on Windows). The correct interactive command per Cursor's CLI docs is
	// `cursor-agent login` — it opens the browser flow and stores credentials
	// locally. See https://cursor.com/docs/cli/reference/authentication.
	// ────────────────────────────────────────────────────────────────
	{
		FACPAgentQuirks Q;
		Q.bHasTerminalAuthArgsOverride = true;
		Q.TerminalAuthArgsOverride = { TEXT("login") };
		Map.Add(TEXT("cursor"), Q);
	}

	// All other agents (claude-acp, cline, goose, kimi, mistral-vibe,
	// qwen-code, etc.) use pure ACP defaults — no quirks needed.
}

const FACPAgentQuirks& FACPAgentQuirksMap::GetQuirks(const FString& RegistryId)
{
	const TMap<FString, FACPAgentQuirks>& Map = GetMap();
	if (const FACPAgentQuirks* Found = Map.Find(RegistryId))
	{
		return *Found;
	}
	return GDefaultQuirks;
}

bool FACPAgentQuirksMap::HasQuirks(const FString& RegistryId)
{
	return GetMap().Contains(RegistryId);
}
