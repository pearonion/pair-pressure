// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/** Kinds of operations the installer can batch. */
enum class ENeoStackOpKind : uint8
{
	Install,    // Download + extract + enable in .uproject
	Update,     // Same as Install but overwrites existing install directory
	Uninstall,  // Disable in .uproject + delete install directory
};

/**
 * Finite-state-machine phases an op walks through. Purely informational for
 * the UI — the orchestrator drives transitions.
 *
 *  Install / Update path:
 *    Queued → ResolvingDownload → Downloading → Verifying → Extracting →
 *    Installing → UpdatingProject → Success   (or → PendingRestart for
 *    loaded updates that must be swapped after editor shutdown)
 *
 *  Uninstall path:
 *    Queued → UpdatingProject → PendingRestart   (or → Failed)
 */
enum class ENeoStackOpPhase : uint8
{
	Queued,
	ResolvingDownload,  // Hitting /api/external/download for the signed R2 URL
	Downloading,        // Streaming the .zip from R2 to disk
	Verifying,          // SHA256 of downloaded bytes matches the manifest
	Extracting,         // Unzip into a staging directory
	Installing,         // Atomic rename staging → Plugins/NeoExtensions/<Name>/
	UpdatingProject,    // Edit .uproject to enable (install/update) or disable (uninstall)
	Uninstalling,       // Delete Plugins/NeoExtensions/<Name>/ on disk
	PendingRestart,     // Staged for an external updater after Unreal releases DLL handles
	Success,
	Failed,
};

/**
 * One queued or in-flight operation. The orchestrator owns a vector of these
 * as TSharedPtr so step callbacks can keep them alive past a queue reshuffle.
 */
struct NEOSTACKAI_API FNeoStackExtensionOp
{
	// ── Identity ────────────────────────────────────────────────────
	FString Slug;              // Catalog slug (e.g. "neo-stack-ai-chaos-fracture")
	FString PluginName;        // UE plugin name (e.g. "NeoStackAI_ChaosFracture") — matches local registry
	ENeoStackOpKind Kind = ENeoStackOpKind::Install;

	// ── Target selection (install/update only) ──────────────────────
	FString RequestedChannel;  // "stable" | "beta" | "dev" | "alpha". Empty = whatever the caller passes on apply.
	FString RequestedVariant;  // "binary" (default) | "full" | "fab". Most users want binary.
	FString EngineVersion;     // "5.7" style; auto-detected at enqueue time.
	FString Platform;          // "Win64" | "Mac"

	// ── Runtime state ───────────────────────────────────────────────
	ENeoStackOpPhase Phase = ENeoStackOpPhase::Queued;
	FString ErrorMessage;      // populated when Phase == Failed

	// Download progress (set during Downloading phase).
	int64 BytesTotal = 0;
	int64 BytesDone = 0;

	// ── Post-resolve data ───────────────────────────────────────────
	// Populated by Step_ResolveDownload from the /api/external/download reply
	// so later steps can verify + extract without re-hitting the server.
	FString ResolvedVersion;       // e.g. "1.0.75"
	FString ResolvedFileName;      // e.g. "NeoStackAI_ChaosFracture-UE5.7-v1.0.75_Binary.zip"
	FString ResolvedSha256;
	int64   ResolvedFileSize = 0;
	FString ResolvedUrl;           // short-lived presigned URL (don't log)

	// ── Local paths (populated as we go) ────────────────────────────
	FString TempArchivePath;       // <ProjectSaved>/NeoStackExtensions-Cache/<slug>-<timestamp>.zip
	FString StagingDir;            // <ProjectSaved>/NeoStackExtensions-Cache/<slug>-<timestamp>-staged/
	FString TargetPluginDir;       // <project>/Plugins/NeoExtensions/<PluginName>/
	FString StagedPluginRoot;      // Resolved root under StagingDir used by restart updater
};

/**
 * One installed extension whose on-disk version disagrees with the catalog's
 * latest. Returned by FNeoStackExtensionInstaller::ComputePendingUpdates so
 * the unified-updater notification can list what's about to be touched.
 */
struct NEOSTACKAI_API FNeoStackExtensionUpdateCandidate
{
	FString Slug;             // catalog slug, e.g. "neo-stack-ai-chaos-fracture"
	FString PluginName;       // matches FNeoStackManagedExtension::PluginName
	FString DisplayName;      // catalog DisplayName (fallback: PluginName)
	FString InstalledVersion; // VersionName from the on-disk .uplugin
	FString LatestVersion;    // catalog's latestVersion for this slug
};

/**
 * Snapshot of the installer's state. Used for the bridge `getExtensionOpProgress`
 * poll. The orchestrator rebuilds this on each query from its internal ops array.
 */
struct NEOSTACKAI_API FNeoStackInstallerState
{
	bool bRunning = false;
	bool bRestartRecommended = false;  // true after any successful op
	int32 SucceededCount = 0;
	int32 FailedCount = 0;
	FDateTime StartedAt;
	FDateTime CompletedAt;

	TArray<TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>> Ops;
};

/**
 * Orchestrator for batched extension installs / updates / uninstalls.
 *
 * Use from the WebUI thread (game thread). Internal HTTP callbacks all marshal
 * back to game thread via UE's HTTP manager, so no cross-thread locking needed.
 *
 * Typical flow from the Svelte panel:
 *   1. User selects extensions → bridge calls Queue() for each.
 *   2. User clicks Apply → bridge calls Apply().
 *   3. Svelte polls GetState() every ~250 ms until bRunning flips to false.
 *   4. On success, the existing restart-required banner kicks in.
 */
class NEOSTACKAI_API FNeoStackExtensionInstaller
{
public:
	// ── Queue management ────────────────────────────────────────────

	/** Enqueue an install/update for a catalog slug. Deduplicates by (Slug, Kind). */
	static void Queue(const FString& Slug, const FString& PluginName, ENeoStackOpKind Kind, const FString& Channel);

	/** Remove a pending op. No-op if the op isn't in the queue or has already started. */
	static void Dequeue(const FString& Slug, ENeoStackOpKind Kind);

	/** Clear everything (only pending ops — active + completed ops stay for display). */
	static void ClearPending();

	// ── Execution ───────────────────────────────────────────────────

	/** Begin processing the queue. Idempotent; returns immediately if a batch is already running. */
	static void Apply();

	/** Like Apply, but suppresses the auto-launch of the post-editor-close updater
	 *  script. The unified-updater path uses this so it can write a single combined
	 *  script that swaps core + extensions after the editor exits. The orchestrator
	 *  fires `OnAllOpsStaged` once every staged op has reached PendingRestart (or
	 *  Success / Failed). Defaults to false → existing single-flow behavior. */
	static void ApplyDeferred();

	/** Delegate fired exactly once per ApplyDeferred batch when every op has
	 *  reached a terminal phase (Success / PendingRestart / Failed). The unified
	 *  caller subscribes here, then calls WriteAndLaunchUnifiedUpdater. */
	DECLARE_MULTICAST_DELEGATE(FOnAllOpsStagedDelegate);
	static FOnAllOpsStagedDelegate& OnAllOpsStaged();

	/** Launches the external updater for staged update/uninstall ops, then returns true. */
	static bool LaunchPendingRestartUpdater();

	/** Optional core-update payload passed to WriteAndLaunchUnifiedUpdater. When
	 *  set, the combined script first swaps the core plugin folder (extracting
	 *  the downloaded ZIP into PluginParentDir) before processing extension ops. */
	struct FCoreUpdatePayload
	{
		FString TargetPluginDir;   // current core plugin dir (Plugin->GetBaseDir())
		FString ZipPath;           // downloaded core ZIP, not yet extracted
		FString PluginParentDir;   // where Expand-Archive lands (parent of TargetPluginDir)
		FString LatestVersion;     // for log/banner messages
	};

	/** Writes a single combined updater script (core swap + per-extension swap)
	 *  and launches it. Only one of `Core` and the staged-ops list need be set;
	 *  if both are present, core is swapped first. Returns false if neither is
	 *  given or the script can't be written/launched. */
	static bool WriteAndLaunchUnifiedUpdater(const TOptional<FCoreUpdatePayload>& Core);

	/** True when one or more completed ops are waiting for an external restart-time swap. */
	static bool HasPendingRestartOps();

	/** Compute the set of installed-and-enabled NeoStack extensions whose
	 *  on-disk VersionName disagrees with the catalog's `latestVersion` for
	 *  their slug. Returns empty if the catalog isn't in Ready state — the
	 *  unified-updater notification then falls back to core-only copy. */
	static TArray<FNeoStackExtensionUpdateCandidate> ComputePendingUpdates(
		const FString& Channel,
		const FString& EngineVersion);

	// ── Introspection ───────────────────────────────────────────────

	static FNeoStackInstallerState GetState();
	static FString QueueToJson();
	static FString StateToJson();
};
