// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AgentInstaller.h"

/**
 * Manages a bundled Node.js runtime so users don't have to install Node themselves.
 *
 * Mirrors Zed's ManagedNodeRuntime: pin a Node version, download from nodejs.org on
 * first use, cache forever under ~/.agentintegrationkit/node/.
 *
 * Lookup precedence: system Node (>=18 on PATH) → bundled Node → trigger download.
 * The download is async; everything else is sync. Single-threaded — coordinate
 * concurrent EnsureManagedNodeAsync calls via the static install gate.
 */
class NEOSTACKAI_API FNodeRuntime
{
public:
	static FNodeRuntime& Get();

	/** Pinned Node version that we know works with claude-agent-acp / codex-acp / gemini-cli. */
	static const TCHAR* GetTargetVersion();   // e.g. TEXT("v24.11.0")

	/** Minimum acceptable system Node version. Below this we ignore PATH and use bundled. */
	static int32 GetMinimumMajorVersion();    // currently 18

	// ── System lookup ────────────────────────────────────────────────────────

	/** Search PATH (and login shell) for node + npx. Returns false if either is missing. */
	bool TryFindSystemNode(FString& OutNodePath, FString& OutNpxPath, FString& OutVersion) const;

	/** True if a usable system Node (>=GetMinimumMajorVersion) is on PATH. */
	bool HasUsableSystemNode() const;

	// ── Managed (bundled) install ────────────────────────────────────────────

	/** Root directory where the bundled Node lives. May not exist yet. */
	static FString GetManagedRootDir();

	/** Subdir for the pinned version (e.g. ~/.agentintegrationkit/node/node-v24.11.0-win-x64). */
	static FString GetManagedVersionDir();

	/** Returns the bundled node executable path even if not yet installed. Caller must check exists. */
	static FString GetManagedNodePath();

	/** Returns the bundled npx executable path even if not yet installed. */
	static FString GetManagedNpxPath();

	/** True if the bundled Node is downloaded, extracted, and reports the expected version. */
	bool IsManagedNodeInstalled() const;

	/**
	 * Download + extract the pinned Node version if not already installed.
	 * Runs the work on a background thread; both delegates fire on the game thread.
	 *
	 * Idempotent: if a download is already in flight, the new caller's delegates are
	 * queued and fire when the in-flight one finishes.
	 */
	void EnsureManagedNodeAsync(FOnInstallProgress OnProgress, FOnInstallComplete OnComplete);

	// ── Combined "give me a working npx" entry point ─────────────────────────

	/**
	 * Returns the best npx path available right now, without triggering a download.
	 * Order: system npx → bundled npx → empty.
	 */
	bool TryGetAnyNpxPath(FString& OutNpxPath) const;

	/**
	 * Async: ensure *some* npx is available. Tries system first, then triggers a managed
	 * Node download if necessary. On success the OutNpxPath inside the completion can be
	 * read via TryGetAnyNpxPath.
	 */
	void EnsureNpxAvailableAsync(FOnInstallProgress OnProgress, FOnInstallComplete OnComplete);

	/**
	 * Build a node-aware PATH augmentation: prepend the bundled node's directory so
	 * child processes find it. Pass through unchanged if no managed node is installed.
	 */
	FString BuildAugmentedPath(const FString& BasePath) const;

private:
	FNodeRuntime() = default;

	/** Synchronous worker (background thread). Returns true on success. */
	bool DownloadAndExtractManagedNode(FOnInstallProgress OnProgress, FACPInstallError& OutError) const;

	static FString GetPlatformKey();          // "win-x64" / "darwin-arm64" / "linux-x64" / ...
	static FString GetArchiveFileName();      // "node-v24.11.0-win-x64.zip"
	static FString GetArchiveUrl();           // "https://nodejs.org/dist/v24.11.0/<file>"
	static FString GetExpectedFolderName();   // "node-v24.11.0-win-x64"

	/** Run "<node> --version" and parse the result. Returns true on success. */
	static bool ProbeNodeVersion(const FString& NodePath, FString& OutVersion);

	/** Pull the major version from "v24.11.0" → 24. Returns -1 on parse failure. */
	static int32 ParseMajorVersion(const FString& VersionString);

	mutable FCriticalSection InstallLock;
	mutable bool bInstallInFlight = false;

	/** Pending callbacks queued while an install is in flight. Drained on completion. */
	mutable TArray<TPair<FOnInstallProgress, FOnInstallComplete>> PendingCallbacks;
};
