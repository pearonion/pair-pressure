// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Internal-only helpers shared between the orchestrator (.cpp) and the
// HTTP / filesystem step implementations. Not exported.

#pragma once

#include "Extensions/NeoStackExtensionInstaller.h"

namespace NeoStackExtensionInstallerInternal
{
	// ── Advance ────────────────────────────────────────────────────
	// Called by each step when it finishes (success or fail). Transitions the
	// op to the next phase and either starts the next step for this op or
	// advances the orchestrator to the next queued op.
	void AdvanceOp(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);

	// Shorthand — moves op to Failed with an error message and advances.
	void FailOp(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op, const FString& ErrorMessage);

	// ── Step entry points ──────────────────────────────────────────
	// Each step reads Op->Phase, does its work (often async), updates Phase
	// on completion, and calls AdvanceOp. The orchestrator never calls these
	// directly — it calls RunNextStep() below, which dispatches by Phase.

	void Step_ResolveDownload(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);  // Http.cpp
	void Step_DownloadFile(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);     // Http.cpp
	void Step_Verify(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);           // Fs.cpp
	void Step_Extract(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);          // Fs.cpp
	void Step_AtomicInstall(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);    // Fs.cpp
	void Step_StageForRestart(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);  // Fs.cpp
	void Step_EnableInProject(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);  // Fs.cpp
	void Step_DisableInProject(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op); // Fs.cpp
	void Step_DeletePluginDir(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);  // Fs.cpp

	/** Dispatch the op's current Phase to the correct Step_* function. */
	void RunCurrentStep(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op);

	// ── Path helpers ───────────────────────────────────────────────
	// Cache root: <project>/Saved/NeoStackExtensions-Cache/ — temp zips + staging
	// go here so failed ops leave no residue in the real Plugins/ tree.
	FString GetCacheRoot();

	/** <CacheRoot>/<slug>-<yyyymmddhhmmss>.zip */
	FString BuildTempArchivePath(const FString& Slug);

	/** <CacheRoot>/<slug>-<yyyymmddhhmmss>-staged/ */
	FString BuildStagingDir(const FString& Slug);

	/** <project>/Plugins/NeoExtensions/<PluginName>/ */
	FString BuildTargetPluginDir(const FString& PluginName);

	/** The nested plugin root inside an extracted archive, or StagingDir for flat zips. */
	FString ResolveStagingPluginRoot(const FString& StagingDir, const FString& PluginName);

	// ── Auth / routing ─────────────────────────────────────────────
	// Both mirror FNeoStackExtensionCatalog's behavior: read from
	// UACPSettings for the "neostack" chat provider row.
	FString GetApiBaseUrl();
	FString GetApiKey();

	// ── Misc ────────────────────────────────────────────────────────
	/** SHA-256 of a file on disk, as lowercase hex. Empty string on error. */
	FString ComputeFileSha256(const FString& FilePath, FString& OutError);

	/** Recursive directory delete with a forgiving best-effort semantic. */
	bool DeleteDirectoryTree(const FString& Path, FString& OutError);

	/** Unzip ArchivePath into DestDir using the platform's native tool
	 *  (PowerShell's Expand-Archive on Windows, /usr/bin/unzip on Mac).
	 *  Returns false and populates OutError on failure. DestDir is assumed
	 *  to exist; extraction proceeds into it. */
	bool UnzipArchive(const FString& ArchivePath, const FString& DestDir, FString& OutError);
}
