// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Extensions/NeoStackExtensionInstaller.h"
#include "Extensions/NeoStackExtensionInstallerInternal.h"
#include "Extensions/NeoStackExtensionCatalog.h"
#include "BuildVariant.h"

#include "ACPSettings.h"
#include "NeoStackAIModule.h"
#include "NSAIAnalytics.h"
#include "Extensions/NeoStackExtensionProjectService.h"
#include "Skills/NeoStackSkillRegistry.h"
#include "Skills/NeoStackSkillInstaller.h"

#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformMisc.h"

// ────────────────────────────────────────────────────────────────
// Module-static state
// ────────────────────────────────────────────────────────────────
//
// Installer state lives here in one place. Step functions in Http.cpp / Fs.cpp
// mutate the op they're passed via TSharedPtr; when they finish, they call
// AdvanceOp which may promote the orchestrator to the next queued op.
//
// All state access happens on the game thread: bridge calls arrive there,
// HTTP callbacks are dispatched there by UE's HTTP manager, and filesystem
// work is synchronous. No mutex required.

namespace
{
	struct FInstallerState
	{
		// Ops in submission order. Completed ops stay in the list so the UI
		// can display a post-apply summary until the user clears.
		TArray<TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>> Ops;

		bool bRunning = false;
		int32 ActiveIndex = INDEX_NONE;
		FDateTime StartedAt;
		FDateTime CompletedAt;

		// When true, StartNextOp's batch-complete branch suppresses the
		// auto-launch of LaunchPendingRestartUpdater and instead fires
		// FNeoStackExtensionInstaller::OnAllOpsStaged() so the unified-updater
		// caller can take over. Cleared after the delegate fires so the next
		// Apply() call returns to default behavior.
		bool bDeferRestartScriptLaunch = false;
	};

	FInstallerState GState;

	FNeoStackExtensionInstaller::FOnAllOpsStagedDelegate GOnAllOpsStaged;

	// ── Phase helpers ─────────────────────────────────────────────
	const TCHAR* KindToString(ENeoStackOpKind Kind)
	{
		switch (Kind)
		{
			case ENeoStackOpKind::Install:   return TEXT("install");
			case ENeoStackOpKind::Update:    return TEXT("update");
			case ENeoStackOpKind::Uninstall: return TEXT("uninstall");
		}
		return TEXT("unknown");
	}

	const TCHAR* PhaseToString(ENeoStackOpPhase Phase)
	{
		switch (Phase)
		{
			case ENeoStackOpPhase::Queued:            return TEXT("queued");
			case ENeoStackOpPhase::ResolvingDownload: return TEXT("resolving_download");
			case ENeoStackOpPhase::Downloading:       return TEXT("downloading");
			case ENeoStackOpPhase::Verifying:         return TEXT("verifying");
			case ENeoStackOpPhase::Extracting:        return TEXT("extracting");
			case ENeoStackOpPhase::Installing:        return TEXT("installing");
			case ENeoStackOpPhase::UpdatingProject:   return TEXT("updating_project");
			case ENeoStackOpPhase::Uninstalling:      return TEXT("uninstalling");
			case ENeoStackOpPhase::PendingRestart:    return TEXT("pending_restart");
			case ENeoStackOpPhase::Success:           return TEXT("success");
			case ENeoStackOpPhase::Failed:            return TEXT("failed");
		}
		return TEXT("unknown");
	}

	bool IsTerminalPhase(ENeoStackOpPhase Phase)
	{
		return Phase == ENeoStackOpPhase::Success
			|| Phase == ENeoStackOpPhase::PendingRestart
			|| Phase == ENeoStackOpPhase::Failed;
	}

	void RecordExtensionOperation(const FNeoStackExtensionOp& Op, const TCHAR* Status)
	{
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetStringField(TEXT("slug"), Op.Slug);
		Props->SetStringField(TEXT("operation"), KindToString(Op.Kind));
		Props->SetStringField(TEXT("status"), Status);
		Props->SetStringField(TEXT("phase"), PhaseToString(Op.Phase));
		Props->SetStringField(TEXT("channel"), Op.RequestedChannel);
		Props->SetStringField(TEXT("variant"), Op.RequestedVariant);
		Props->SetStringField(TEXT("engine_version"), Op.EngineVersion);
		Props->SetStringField(TEXT("platform"), Op.Platform);
		if (!Op.ResolvedVersion.IsEmpty())
		{
			Props->SetStringField(TEXT("resolved_version"), Op.ResolvedVersion);
		}
		if (GState.StartedAt.GetTicks() > 0)
		{
			Props->SetNumberField(TEXT("duration_ms"), (FDateTime::UtcNow() - GState.StartedAt).GetTotalMilliseconds());
		}
		if (!Op.ErrorMessage.IsEmpty())
		{
			Props->SetStringField(TEXT("error"), FNSAIAnalytics::SanitizeErrorForAnalytics(Op.ErrorMessage));
		}
		FNSAIAnalytics::Get().RecordEvent(TEXT("extension_operation"), Props);
	}

	// ── Next-phase table ──────────────────────────────────────────
	// Given the op's current phase + kind, return the next phase it should
	// enter. Called by AdvanceOp after a step completes successfully.

	ENeoStackOpPhase NextPhaseFor(ENeoStackOpKind Kind, ENeoStackOpPhase Current)
	{
		// Uninstall path:
		if (Kind == ENeoStackOpKind::Uninstall)
		{
			switch (Current)
			{
				case ENeoStackOpPhase::Queued:          return ENeoStackOpPhase::UpdatingProject;
				case ENeoStackOpPhase::UpdatingProject: return ENeoStackOpPhase::PendingRestart;
				case ENeoStackOpPhase::PendingRestart:  return ENeoStackOpPhase::Success;
				default: return ENeoStackOpPhase::Failed;
			}
		}

		// Install / Update path:
		switch (Current)
		{
			case ENeoStackOpPhase::Queued:            return ENeoStackOpPhase::ResolvingDownload;
			case ENeoStackOpPhase::ResolvingDownload: return ENeoStackOpPhase::Downloading;
			case ENeoStackOpPhase::Downloading:       return ENeoStackOpPhase::Verifying;
			case ENeoStackOpPhase::Verifying:         return ENeoStackOpPhase::Extracting;
			case ENeoStackOpPhase::Extracting:        return ENeoStackOpPhase::Installing;
			case ENeoStackOpPhase::Installing:        return ENeoStackOpPhase::UpdatingProject;
			case ENeoStackOpPhase::UpdatingProject:   return ENeoStackOpPhase::Success;
			default: return ENeoStackOpPhase::Failed;
		}
	}

	// ── Platform detection ────────────────────────────────────────
	FString GetCurrentPlatformName()
	{
#if PLATFORM_WINDOWS
		return TEXT("Win64");
#elif PLATFORM_MAC
		return TEXT("Mac");
#else
		return TEXT("Win64"); // Fallback — nothing else ships today.
#endif
	}

	FString GetCurrentEngineVersion()
	{
		return FString::Printf(TEXT("%d.%d"), ENGINE_MAJOR_VERSION, ENGINE_MINOR_VERSION);
	}

	// ── Serial queue drive ────────────────────────────────────────
	/** Start the first op in Queued state, or mark the batch as complete. */
	void StartNextOp()
	{
		for (int32 Idx = 0; Idx < GState.Ops.Num(); ++Idx)
		{
			TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op = GState.Ops[Idx];
			if (Op.IsValid() && Op->Phase == ENeoStackOpPhase::Queued)
			{
				GState.ActiveIndex = Idx;
				// Kick the state machine. RunCurrentStep examines Phase and
				// dispatches — for Queued, that's the first real step.
				Op->Phase = NextPhaseFor(Op->Kind, ENeoStackOpPhase::Queued);
				NeoStackExtensionInstallerInternal::RunCurrentStep(Op);
				return;
			}
		}

		// No Queued op found — batch done.
		GState.bRunning = false;
		GState.ActiveIndex = INDEX_NONE;
		GState.CompletedAt = FDateTime::UtcNow();
		UE_LOG(LogNeoStackAI, Log, TEXT("Extension install batch complete."));

		// An install/update/uninstall can change which extensions ship skills, so
		// re-scan Resources/Skills and reconcile <Project>/.claude/skills and
		// <Project>/.agents/skills with the new source set. Newly-installed plugins
		// contribute skills instantly; uninstalled plugins have their clean-install
		// skill files cleaned up while user-edited ones stay behind.
		FNeoStackSkillRegistry::Get().Refresh();
		FNeoStackSkillInstaller::SyncProject();

		// Deferred mode: the unified-updater caller wants to write a single
		// combined script after every op has staged. Fire the delegate once,
		// then reset the flag so the next Apply() returns to default behavior.
		// The OnAllOpsStaged subscriber is responsible for either calling
		// WriteAndLaunchUnifiedUpdater (with or without a core payload) or
		// LaunchPendingRestartUpdater (to fall back to the legacy script).
		if (GState.bDeferRestartScriptLaunch)
		{
			GState.bDeferRestartScriptLaunch = false;
			GOnAllOpsStaged.Broadcast();
		}
	}
}

// ────────────────────────────────────────────────────────────────
// Internal API (consumed by Http.cpp / Fs.cpp)
// ────────────────────────────────────────────────────────────────

namespace NeoStackExtensionInstallerInternal
{
	void AdvanceOp(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		if (Op->Phase == ENeoStackOpPhase::Failed)
		{
			RecordExtensionOperation(*Op, TEXT("failed"));
			UE_LOG(LogNeoStackAI, Warning, TEXT("[installer] %s %s failed: %s"),
				KindToString(Op->Kind), *Op->Slug, *Op->ErrorMessage);
			StartNextOp();
			return;
		}

		if (Op->Phase == ENeoStackOpPhase::PendingRestart)
		{
			RecordExtensionOperation(*Op, TEXT("pending_restart"));
			UE_LOG(LogNeoStackAI, Log, TEXT("[installer] %s %s staged for restart-time apply"),
				KindToString(Op->Kind), *Op->Slug);
			StartNextOp();
			return;
		}

		const ENeoStackOpPhase Next = NextPhaseFor(Op->Kind, Op->Phase);
		Op->Phase = Next;

		if (IsTerminalPhase(Next))
		{
			RecordExtensionOperation(*Op, PhaseToString(Next));
			UE_LOG(LogNeoStackAI, Log, TEXT("[installer] %s %s: %s"),
				KindToString(Op->Kind), *Op->Slug, PhaseToString(Next));
			StartNextOp();
			return;
		}

		RunCurrentStep(Op);
	}

	void FailOp(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op, const FString& ErrorMessage)
	{
		if (!Op.IsValid()) return;
		Op->Phase = ENeoStackOpPhase::Failed;
		Op->ErrorMessage = ErrorMessage;
		AdvanceOp(Op);
	}

	void RunCurrentStep(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;
		switch (Op->Phase)
		{
			case ENeoStackOpPhase::ResolvingDownload: Step_ResolveDownload(Op);   return;
			case ENeoStackOpPhase::Downloading:       Step_DownloadFile(Op);      return;
			case ENeoStackOpPhase::Verifying:         Step_Verify(Op);            return;
			case ENeoStackOpPhase::Extracting:        Step_Extract(Op);           return;
			case ENeoStackOpPhase::Installing:        Step_AtomicInstall(Op);     return;
			case ENeoStackOpPhase::UpdatingProject:
			{
				if (Op->Kind == ENeoStackOpKind::Uninstall) { Step_DisableInProject(Op); return; }
				Step_EnableInProject(Op);
				return;
			}
			case ENeoStackOpPhase::Uninstalling:      Step_DeletePluginDir(Op);   return;
			case ENeoStackOpPhase::PendingRestart:    Step_StageForRestart(Op);   return;
			default:
				// Queued / Success / Failed — should not be dispatched.
				return;
		}
	}

	// ── Path helpers ─────────────────────────────────────────────
	FString GetCacheRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("NeoStackExtensions-Cache"));
	}

	static FString SlugifyForPath(const FString& In)
	{
		FString Out = In;
		Out.ReplaceInline(TEXT("/"), TEXT("_"));
		Out.ReplaceInline(TEXT("\\"), TEXT("_"));
		return Out;
	}

	FString BuildTempArchivePath(const FString& Slug)
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		return GetCacheRoot() / FString::Printf(TEXT("%s-%s.zip"), *SlugifyForPath(Slug), *Timestamp);
	}

	FString BuildStagingDir(const FString& Slug)
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		return GetCacheRoot() / FString::Printf(TEXT("%s-%s-staged"), *SlugifyForPath(Slug), *Timestamp);
	}

	FString BuildTargetPluginDir(const FString& PluginName)
	{
		return FPaths::ProjectPluginsDir() / TEXT("NeoExtensions") / PluginName;
	}

	FString GetApiBaseUrl()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			FString Override = Settings->GetChatProviderBaseUrlOverride(TEXT("neostack"));
			if (!Override.IsEmpty())
			{
				// Strip trailing /v1 if present — that's the AI chat base; we
				// want the site root for /api/* routes.
				Override.RemoveFromEnd(TEXT("/"));
				Override.RemoveFromEnd(TEXT("/v1"));
				Override.RemoveFromEnd(TEXT("/"));
				return Override;
			}
		}
		return TEXT("https://neostack.dev");
	}

	FString GetApiKey()
	{
		if (const UACPSettings* Settings = UACPSettings::Get())
		{
			return Settings->GetChatProviderApiKey(TEXT("neostack"));
		}
		return FString();
	}
}

// ────────────────────────────────────────────────────────────────
// Public API — FNeoStackExtensionInstaller
// ────────────────────────────────────────────────────────────────

void FNeoStackExtensionInstaller::Queue(
	const FString& Slug,
	const FString& PluginName,
	ENeoStackOpKind Kind,
	const FString& Channel)
{
	// Dedup: if the same (Slug, Kind) is already queued or in progress, ignore.
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Existing : GState.Ops)
	{
		if (Existing.IsValid()
			&& Existing->Slug == Slug
			&& Existing->Kind == Kind
			&& !IsTerminalPhase(Existing->Phase))
		{
			return;
		}
	}

	TSharedRef<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op = MakeShared<FNeoStackExtensionOp, ESPMode::ThreadSafe>();
	Op->Slug = Slug;
	Op->PluginName = PluginName;
	Op->Kind = Kind;
	Op->RequestedChannel = Channel.IsEmpty() ? TEXT("stable") : Channel;
	// Variant follows the user's tier: prefer the catalog's variantHint
	// (server-derived, accounts for lifetime+subscriber combo); fall back
	// to the compile-time variant macro baked into this DLL by the release
	// workflow's binary pass (source build → full, binary distribution →
	// binary). The server-side download endpoint enforces variant pinning
	// regardless, so this hint is purely a UX optimization.
	{
		const FNeoStackCatalogResult Cached = FNeoStackExtensionCatalog::GetCachedResult();
		if (!Cached.VariantHint.IsEmpty())
		{
			Op->RequestedVariant = Cached.VariantHint;
		}
		else
		{
#if NEOSTACK_BUILD_VARIANT_BINARY
			Op->RequestedVariant = TEXT("binary");
#else
			Op->RequestedVariant = TEXT("full");
#endif
		}
	}
	Op->EngineVersion = GetCurrentEngineVersion();
	Op->Platform = GetCurrentPlatformName();
	Op->Phase = ENeoStackOpPhase::Queued;
	Op->TargetPluginDir = NeoStackExtensionInstallerInternal::BuildTargetPluginDir(PluginName);

	GState.Ops.Add(Op);
	RecordExtensionOperation(Op.Get(), TEXT("queued"));
}

void FNeoStackExtensionInstaller::Dequeue(const FString& Slug, ENeoStackOpKind Kind)
{
	for (int32 Idx = GState.Ops.Num() - 1; Idx >= 0; --Idx)
	{
		const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op = GState.Ops[Idx];
		if (Op.IsValid()
			&& Op->Slug == Slug
			&& Op->Kind == Kind
			&& Op->Phase == ENeoStackOpPhase::Queued)
		{
			GState.Ops.RemoveAt(Idx);
		}
	}
}

void FNeoStackExtensionInstaller::ClearPending()
{
	GState.Ops.RemoveAll([](const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op)
	{
		return Op.IsValid() && Op->Phase == ENeoStackOpPhase::Queued;
	});
}

FNeoStackExtensionInstaller::FOnAllOpsStagedDelegate& FNeoStackExtensionInstaller::OnAllOpsStaged()
{
	return GOnAllOpsStaged;
}

void FNeoStackExtensionInstaller::ApplyDeferred()
{
	// Set the flag BEFORE calling Apply so a synchronous batch (e.g. queue
	// already empty) still gets the correct deferred semantic.
	GState.bDeferRestartScriptLaunch = true;
	Apply();
}

void FNeoStackExtensionInstaller::Apply()
{
	if (GState.bRunning) return;

	// Anything queued? If not, there's nothing to do.
	const bool bHasQueued = GState.Ops.ContainsByPredicate(
		[](const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op)
		{
			return Op.IsValid() && Op->Phase == ENeoStackOpPhase::Queued;
		});
	if (!bHasQueued)
	{
		// Nothing to do, but the deferred caller still expects a one-shot
		// fire so it can transition out of "waiting for staging" UI without
		// hanging. Mirror StartNextOp's deferred branch here.
		if (GState.bDeferRestartScriptLaunch)
		{
			GState.bDeferRestartScriptLaunch = false;
			GOnAllOpsStaged.Broadcast();
		}
		return;
	}

	GState.bRunning = true;
	GState.StartedAt = FDateTime::UtcNow();
	GState.CompletedAt = FDateTime();
	UE_LOG(LogNeoStackAI, Log, TEXT("Extension install batch starting (%d ops)."), GState.Ops.Num());

	// Ensure cache root exists up front so no step has to do it.
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*NeoStackExtensionInstallerInternal::GetCacheRoot());

	StartNextOp();
}

bool FNeoStackExtensionInstaller::HasPendingRestartOps()
{
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : GState.Ops)
	{
		if (Op.IsValid() && Op->Phase == ENeoStackOpPhase::PendingRestart)
		{
			return true;
		}
	}
	return false;
}

bool FNeoStackExtensionInstaller::LaunchPendingRestartUpdater()
{
	TArray<TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>> Pending;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : GState.Ops)
	{
		if (Op.IsValid() && Op->Phase == ENeoStackOpPhase::PendingRestart)
		{
			Pending.Add(Op);
		}
	}

	if (Pending.Num() == 0)
	{
		return false;
	}

	const FString ScriptDir = NeoStackExtensionInstallerInternal::GetCacheRoot();
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*ScriptDir);

	FString ScriptContent;
	FString ScriptPath;
	FString LaunchExe;
	FString LaunchArgs;

#if PLATFORM_WINDOWS
	ScriptPath = FPaths::ConvertRelativePathToFull(ScriptDir / TEXT("extension-updater.bat"));
	FString WinScriptPath = ScriptPath;
	FPaths::MakePlatformFilename(WinScriptPath);

	ScriptContent =
		TEXT("@echo off\r\n")
		TEXT("setlocal\r\n")
		TEXT("echo.\r\n")
		TEXT("echo ============================================\r\n")
		TEXT("echo   NeoStack AI Extension Updater\r\n")
		TEXT("echo ============================================\r\n")
		TEXT("echo.\r\n")
		TEXT("echo Waiting for Unreal Editor to close...\r\n")
		TEXT(":WAIT_EDITOR\r\n")
		TEXT("tasklist /FI \"IMAGENAME eq UnrealEditor.exe\" 2>NUL | find /I \"UnrealEditor.exe\" >NUL\r\n")
		TEXT("if %ERRORLEVEL% == 0 (\r\n")
		TEXT("    timeout /t 2 /nobreak >NUL\r\n")
		TEXT("    goto WAIT_EDITOR\r\n")
		TEXT(")\r\n")
		TEXT("timeout /t 3 /nobreak >NUL\r\n");

	int32 Index = 0;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : Pending)
	{
		++Index;
		FString Target = Op->TargetPluginDir;
		FString Backup = Target + TEXT(".backup-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		FString Staged = Op->StagedPluginRoot;
		FString StagingDir = Op->StagingDir;
		FString Archive = Op->TempArchivePath;
		FPaths::MakePlatformFilename(Target);
		FPaths::MakePlatformFilename(Backup);
		FPaths::MakePlatformFilename(Staged);
		FPaths::MakePlatformFilename(StagingDir);
		FPaths::MakePlatformFilename(Archive);

		if (Op->Kind == ENeoStackOpKind::Update)
		{
			ScriptContent += FString::Printf(TEXT(
				"\r\n"
				"echo Updating %s...\r\n"
				"if not exist \"%s\" (\r\n"
				"    echo ERROR: staged plugin is missing: %s\r\n"
				"    goto FAIL_%d\r\n"
				")\r\n"
				"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
				":MOVE_OLD_%d\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto MOVE_NEW_%d\r\n"
				"echo Could not move existing plugin. Close tools that may be locking files, then press any key to retry.\r\n"
				"pause >NUL\r\n"
				"goto MOVE_OLD_%d\r\n"
				":MOVE_NEW_%d\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto CLEAN_%d\r\n"
				"echo ERROR: Could not move staged plugin into place. Restoring backup...\r\n"
				"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"goto FAIL_%d\r\n"
				":CLEAN_%d\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"del /F /Q \"%s\" >NUL 2>&1\r\n"
				"goto OK_%d\r\n"
				":FAIL_%d\r\n"
				"set UPDATE_FAILED=1\r\n"
				":OK_%d\r\n"),
				*Op->PluginName,
				*Staged, *Staged, Index,
				*Backup, *Backup,
				Index,
				*Target, *Backup, Index,
				Index,
				Index,
				*Staged, *Target, Index,
				*Target, *Target,
				*Backup, *Target,
				Index,
				Index,
				*Backup,
				*StagingDir,
				*Archive,
				Index,
				Index,
				Index);
		}
		else if (Op->Kind == ENeoStackOpKind::Uninstall)
		{
			ScriptContent += FString::Printf(TEXT(
				"\r\n"
				"echo Removing %s...\r\n"
				":DELETE_%d\r\n"
				"if not exist \"%s\" goto OK_%d\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto OK_%d\r\n"
				"echo Could not delete plugin folder. Close tools that may be locking files, then press any key to retry.\r\n"
				"pause >NUL\r\n"
				"goto DELETE_%d\r\n"
				":OK_%d\r\n"),
				*Op->PluginName,
				Index,
				*Target, Index,
				*Target, Index,
				Index,
				Index);
		}
	}

	ScriptContent +=
		TEXT("\r\n")
		TEXT("echo.\r\n")
		TEXT("if \"%UPDATE_FAILED%\" == \"1\" (\r\n")
		TEXT("    echo One or more extension operations failed. The existing plugins were left in place where possible.\r\n")
		TEXT(") else (\r\n")
		TEXT("    echo Extension updates completed successfully.\r\n")
		TEXT(")\r\n")
		TEXT("echo Restart Unreal Editor to continue.\r\n")
		TEXT("pause\r\n");

	LaunchExe = TEXT("cmd.exe");
	LaunchArgs = FString::Printf(TEXT("/c start \"NeoStack Extension Updater\" /wait cmd.exe /c \"\"%s\"\""), *WinScriptPath);
#else
	ScriptPath = FPaths::ConvertRelativePathToFull(ScriptDir / TEXT("extension-updater.sh"));
	ScriptContent =
		TEXT("#!/bin/bash\n")
		TEXT("echo\n")
		TEXT("echo '============================================'\n")
		TEXT("echo '  NeoStack AI Extension Updater'\n")
		TEXT("echo '============================================'\n")
		TEXT("echo\n")
		TEXT("echo 'Waiting for Unreal Editor to close...'\n")
		TEXT("while pgrep -x 'UnrealEditor' > /dev/null 2>&1; do\n")
		TEXT("    sleep 2\n")
		TEXT("done\n")
		TEXT("sleep 3\n");

	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : Pending)
	{
		const FString Backup = Op->TargetPluginDir + TEXT(".backup-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		if (Op->Kind == ENeoStackOpKind::Update)
		{
			ScriptContent += FString::Printf(TEXT(
				"\n"
				"echo 'Updating %s...'\n"
				"rm -rf '%s'\n"
				"mv '%s' '%s' || exit 1\n"
				"mv '%s' '%s' || { rm -rf '%s'; mv '%s' '%s'; exit 1; }\n"
				"rm -rf '%s'\n"
				"rm -rf '%s'\n"
				"rm -f '%s'\n"),
				*Op->PluginName,
				*Backup,
				*Op->TargetPluginDir, *Backup,
				*Op->StagedPluginRoot, *Op->TargetPluginDir,
				*Op->TargetPluginDir, *Backup, *Op->TargetPluginDir,
				*Backup,
				*Op->StagingDir,
				*Op->TempArchivePath);
		}
		else if (Op->Kind == ENeoStackOpKind::Uninstall)
		{
			ScriptContent += FString::Printf(TEXT(
				"\n"
				"echo 'Removing %s...'\n"
				"rm -rf '%s'\n"),
				*Op->PluginName,
				*Op->TargetPluginDir);
		}
	}
	ScriptContent += TEXT("\necho 'Extension updates completed. Restart Unreal Editor to continue.'\nread -p 'Press Enter to exit...'\n");
	LaunchExe = TEXT("/usr/bin/open");
	LaunchArgs = FString::Printf(TEXT("-a Terminal \"%s\""), *ScriptPath);
#endif

	if (!FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath))
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("[installer] failed to write extension updater script: %s"), *ScriptPath);
		return false;
	}

#if !PLATFORM_WINDOWS
	FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *ScriptPath), nullptr, nullptr, nullptr);
#endif

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*LaunchExe, *LaunchArgs,
		true,
		false,
		false,
		nullptr, 0, nullptr, nullptr);

	if (!ProcHandle.IsValid())
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("[installer] failed to launch extension updater: %s %s"), *LaunchExe, *LaunchArgs);
		return false;
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("[installer] extension updater launched: %s"), *ScriptPath);
	return true;
}

bool FNeoStackExtensionInstaller::WriteAndLaunchUnifiedUpdater(const TOptional<FCoreUpdatePayload>& Core)
{
	// Collect every op currently waiting for restart-time apply. Mirror
	// LaunchPendingRestartUpdater so the unified flow inherits the same
	// "stage everything first, then swap" guarantee.
	TArray<TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>> Pending;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : GState.Ops)
	{
		if (Op.IsValid() && Op->Phase == ENeoStackOpPhase::PendingRestart)
		{
			Pending.Add(Op);
		}
	}

	if (Pending.Num() == 0 && !Core.IsSet())
	{
		// Caller had nothing to do. Treat as no-op success — caller decides
		// whether to re-show the notification or just clear it.
		return false;
	}

	const FString ScriptDir = NeoStackExtensionInstallerInternal::GetCacheRoot();
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*ScriptDir);

	FString ScriptContent;
	FString ScriptPath;
	FString LaunchExe;
	FString LaunchArgs;

#if PLATFORM_WINDOWS
	ScriptPath = FPaths::ConvertRelativePathToFull(ScriptDir / TEXT("unified-updater.bat"));
	FString WinScriptPath = ScriptPath;
	FPaths::MakePlatformFilename(WinScriptPath);

	// Single editor-close wait at the top — no need to repeat per op.
	ScriptContent =
		TEXT("@echo off\r\n")
		TEXT("setlocal\r\n")
		TEXT("echo.\r\n")
		TEXT("echo ============================================\r\n")
		TEXT("echo   NeoStack AI Updater\r\n")
		TEXT("echo ============================================\r\n")
		TEXT("echo.\r\n")
		TEXT("echo Waiting for Unreal Editor to close...\r\n")
		TEXT(":WAIT_EDITOR\r\n")
		TEXT("tasklist /FI \"IMAGENAME eq UnrealEditor.exe\" 2>NUL | find /I \"UnrealEditor.exe\" >NUL\r\n")
		TEXT("if %ERRORLEVEL% == 0 (\r\n")
		TEXT("    timeout /t 2 /nobreak >NUL\r\n")
		TEXT("    goto WAIT_EDITOR\r\n")
		TEXT(")\r\n")
		TEXT("timeout /t 5 /nobreak >NUL\r\n");

	// ── Core block (optional) ──────────────────────────────────────
	// Same retry-loop pattern as the legacy core updater (NeoStackAIModule
	// InstallUpdate): move target → backup with a press-any-key retry on
	// lock, Expand-Archive into parent dir, restore backup on extract fail.
	if (Core.IsSet())
	{
		FString WinTarget = Core->TargetPluginDir;
		FString WinBackup = Core->TargetPluginDir + TEXT("_backup");
		FString WinZip = Core->ZipPath;
		FString WinParent = Core->PluginParentDir;
		FPaths::MakePlatformFilename(WinTarget);
		FPaths::MakePlatformFilename(WinBackup);
		FPaths::MakePlatformFilename(WinZip);
		FPaths::MakePlatformFilename(WinParent);

		ScriptContent += FString::Printf(TEXT(
			"\r\n"
			"echo.\r\n"
			"echo Updating NeoStack AI core to v%s...\r\n"
			"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
			":CORE_RETRY_MOVE\r\n"
			"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
			"if %%ERRORLEVEL%% == 0 goto CORE_EXTRACT\r\n"
			"echo Core plugin files are still locked. Close any IDE/build tools and press any key to retry.\r\n"
			"pause >NUL\r\n"
			"goto CORE_RETRY_MOVE\r\n"
			":CORE_EXTRACT\r\n"
			"echo Extracting core update...\r\n"
			"powershell -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"\r\n"
			"if %%ERRORLEVEL%% == 0 (\r\n"
			"    rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
			"    del /F /Q \"%s\" >NUL 2>&1\r\n"
			"    echo Core updated successfully.\r\n"
			") else (\r\n"
			"    echo ERROR: Core extraction failed. Restoring backup...\r\n"
			"    if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
			"    move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
			"    set UPDATE_FAILED=1\r\n"
			")\r\n"),
			*Core->LatestVersion,
			*WinBackup, *WinBackup,
			*WinTarget, *WinBackup,
			*WinZip, *WinParent,
			*WinBackup,
			*WinZip,
			*WinTarget, *WinTarget,
			*WinBackup, *WinTarget);
	}

	// ── Extension blocks ─────────────────────────────────────────────
	// Identical to LaunchPendingRestartUpdater's per-op blocks (Update +
	// Uninstall variants). Each op has its own labels and rollback path.
	int32 Index = 0;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : Pending)
	{
		++Index;
		FString Target = Op->TargetPluginDir;
		FString Backup = Target + TEXT(".backup-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		FString Staged = Op->StagedPluginRoot;
		FString StagingDir = Op->StagingDir;
		FString Archive = Op->TempArchivePath;
		FPaths::MakePlatformFilename(Target);
		FPaths::MakePlatformFilename(Backup);
		FPaths::MakePlatformFilename(Staged);
		FPaths::MakePlatformFilename(StagingDir);
		FPaths::MakePlatformFilename(Archive);

		if (Op->Kind == ENeoStackOpKind::Update)
		{
			ScriptContent += FString::Printf(TEXT(
				"\r\n"
				"echo Updating %s...\r\n"
				"if not exist \"%s\" (\r\n"
				"    echo ERROR: staged plugin is missing: %s\r\n"
				"    goto FAIL_%d\r\n"
				")\r\n"
				"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
				":MOVE_OLD_%d\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto MOVE_NEW_%d\r\n"
				"echo Could not move existing plugin. Close tools that may be locking files, then press any key to retry.\r\n"
				"pause >NUL\r\n"
				"goto MOVE_OLD_%d\r\n"
				":MOVE_NEW_%d\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto CLEAN_%d\r\n"
				"echo ERROR: Could not move staged plugin into place. Restoring backup...\r\n"
				"if exist \"%s\" rmdir /S /Q \"%s\"\r\n"
				"move /Y \"%s\" \"%s\" >NUL 2>&1\r\n"
				"goto FAIL_%d\r\n"
				":CLEAN_%d\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"del /F /Q \"%s\" >NUL 2>&1\r\n"
				"goto OK_%d\r\n"
				":FAIL_%d\r\n"
				"set UPDATE_FAILED=1\r\n"
				":OK_%d\r\n"),
				*Op->PluginName,
				*Staged, *Staged, Index,
				*Backup, *Backup,
				Index,
				*Target, *Backup, Index,
				Index,
				Index,
				*Staged, *Target, Index,
				*Target, *Target,
				*Backup, *Target,
				Index,
				Index,
				*Backup,
				*StagingDir,
				*Archive,
				Index,
				Index,
				Index);
		}
		else if (Op->Kind == ENeoStackOpKind::Uninstall)
		{
			ScriptContent += FString::Printf(TEXT(
				"\r\n"
				"echo Removing %s...\r\n"
				":DELETE_%d\r\n"
				"if not exist \"%s\" goto OK_%d\r\n"
				"rmdir /S /Q \"%s\" >NUL 2>&1\r\n"
				"if %%ERRORLEVEL%% == 0 goto OK_%d\r\n"
				"echo Could not delete plugin folder. Close tools that may be locking files, then press any key to retry.\r\n"
				"pause >NUL\r\n"
				"goto DELETE_%d\r\n"
				":OK_%d\r\n"),
				*Op->PluginName,
				Index,
				*Target, Index,
				*Target, Index,
				Index,
				Index);
		}
	}

	ScriptContent +=
		TEXT("\r\n")
		TEXT("echo.\r\n")
		TEXT("if \"%UPDATE_FAILED%\" == \"1\" (\r\n")
		TEXT("    echo One or more updates failed. The previous version was restored where possible.\r\n")
		TEXT(") else (\r\n")
		TEXT("    echo All updates installed successfully.\r\n")
		TEXT(")\r\n")
		TEXT("echo Restart Unreal Editor to continue.\r\n")
		TEXT("pause\r\n");

	LaunchExe = TEXT("cmd.exe");
	LaunchArgs = FString::Printf(TEXT("/c start \"NeoStack AI Updater\" /wait cmd.exe /c \"\"%s\"\""), *WinScriptPath);
#else
	ScriptPath = FPaths::ConvertRelativePathToFull(ScriptDir / TEXT("unified-updater.sh"));
	ScriptContent =
		TEXT("#!/bin/bash\n")
		TEXT("echo\n")
		TEXT("echo '============================================'\n")
		TEXT("echo '  NeoStack AI Updater'\n")
		TEXT("echo '============================================'\n")
		TEXT("echo\n")
		TEXT("echo 'Waiting for Unreal Editor to close...'\n")
		TEXT("while pgrep -x 'UnrealEditor' > /dev/null 2>&1; do\n")
		TEXT("    sleep 2\n")
		TEXT("done\n")
		TEXT("sleep 5\n")
		TEXT("UPDATE_FAILED=0\n");

	if (Core.IsSet())
	{
		const FString CoreBackup = Core->TargetPluginDir + TEXT("_backup");
		ScriptContent += FString::Printf(TEXT(
			"\n"
			"echo 'Updating NeoStack AI core to v%s...'\n"
			"rm -rf '%s'\n"
			"if mv '%s' '%s'; then\n"
			"  if unzip -o '%s' -d '%s'; then\n"
			"    rm -rf '%s'\n"
			"    rm -f '%s'\n"
			"    echo 'Core updated successfully.'\n"
			"  else\n"
			"    echo 'ERROR: Core extraction failed. Restoring backup...'\n"
			"    rm -rf '%s'\n"
			"    mv '%s' '%s'\n"
			"    UPDATE_FAILED=1\n"
			"  fi\n"
			"else\n"
			"  echo 'ERROR: Failed to back up core plugin.'\n"
			"  UPDATE_FAILED=1\n"
			"fi\n"),
			*Core->LatestVersion,
			*CoreBackup,
			*Core->TargetPluginDir, *CoreBackup,
			*Core->ZipPath, *Core->PluginParentDir,
			*CoreBackup,
			*Core->ZipPath,
			*Core->TargetPluginDir,
			*CoreBackup, *Core->TargetPluginDir);
	}

	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : Pending)
	{
		const FString Backup = Op->TargetPluginDir + TEXT(".backup-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d%H%M%S"));
		if (Op->Kind == ENeoStackOpKind::Update)
		{
			// Inline-rollback per-op so a failure on one extension doesn't
			// abort the rest. Mirrors the Windows variant's UPDATE_FAILED
			// flag pattern.
			ScriptContent += FString::Printf(TEXT(
				"\n"
				"echo 'Updating %s...'\n"
				"rm -rf '%s'\n"
				"if mv '%s' '%s'; then\n"
				"  if mv '%s' '%s'; then\n"
				"    rm -rf '%s'\n"
				"    rm -rf '%s'\n"
				"    rm -f '%s'\n"
				"  else\n"
				"    echo 'ERROR: Failed to install %s — restoring backup.'\n"
				"    rm -rf '%s'\n"
				"    mv '%s' '%s'\n"
				"    UPDATE_FAILED=1\n"
				"  fi\n"
				"else\n"
				"  echo 'ERROR: Could not back up %s.'\n"
				"  UPDATE_FAILED=1\n"
				"fi\n"),
				*Op->PluginName,
				*Backup,
				*Op->TargetPluginDir, *Backup,
				*Op->StagedPluginRoot, *Op->TargetPluginDir,
				*Backup,
				*Op->StagingDir,
				*Op->TempArchivePath,
				*Op->PluginName,
				*Op->TargetPluginDir,
				*Backup, *Op->TargetPluginDir,
				*Op->PluginName);
		}
		else if (Op->Kind == ENeoStackOpKind::Uninstall)
		{
			ScriptContent += FString::Printf(TEXT(
				"\n"
				"echo 'Removing %s...'\n"
				"rm -rf '%s'\n"),
				*Op->PluginName,
				*Op->TargetPluginDir);
		}
	}

	ScriptContent +=
		TEXT("\necho\n")
		TEXT("if [ \"$UPDATE_FAILED\" -eq 1 ]; then\n")
		TEXT("  echo 'One or more updates failed. The previous version was restored where possible.'\n")
		TEXT("else\n")
		TEXT("  echo 'All updates installed successfully.'\n")
		TEXT("fi\n")
		TEXT("echo 'Restart Unreal Editor to continue.'\n")
		TEXT("read -p 'Press Enter to exit...'\n");

	LaunchExe = TEXT("/usr/bin/open");
	LaunchArgs = FString::Printf(TEXT("-a Terminal \"%s\""), *ScriptPath);
#endif

	if (!FFileHelper::SaveStringToFile(ScriptContent, *ScriptPath))
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("[installer] failed to write unified updater script: %s"), *ScriptPath);
		return false;
	}

#if !PLATFORM_WINDOWS
	FPlatformProcess::ExecProcess(TEXT("/bin/chmod"), *FString::Printf(TEXT("+x \"%s\""), *ScriptPath), nullptr, nullptr, nullptr);
#endif

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*LaunchExe, *LaunchArgs,
		true,
		false,
		false,
		nullptr, 0, nullptr, nullptr);

	if (!ProcHandle.IsValid())
	{
		UE_LOG(LogNeoStackAI, Error, TEXT("[installer] failed to launch unified updater: %s %s"), *LaunchExe, *LaunchArgs);
		return false;
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("[installer] unified updater launched: %s (core=%s, extensions=%d)"),
		*ScriptPath,
		Core.IsSet() ? TEXT("yes") : TEXT("no"),
		Pending.Num());
	return true;
}

TArray<FNeoStackExtensionUpdateCandidate> FNeoStackExtensionInstaller::ComputePendingUpdates(
	const FString& Channel,
	const FString& EngineVersion)
{
	TArray<FNeoStackExtensionUpdateCandidate> Candidates;

	// Catalog must be in Ready state — we need its `latestVersion` per slug
	// to do the comparison. If RefreshAsync is still in flight (or failed),
	// return empty so callers fall back to core-only flow rather than
	// blocking on the network here.
	const FNeoStackCatalogResult Catalog = FNeoStackExtensionCatalog::GetCachedResult();
	if (Catalog.Status != ENeoStackCatalogStatus::Ready)
	{
		UE_LOG(LogNeoStackAI, Verbose,
			TEXT("[installer] ComputePendingUpdates: catalog not ready (status=%d) — skipping extension updates"),
			static_cast<int32>(Catalog.Status));
		return Candidates;
	}

	// Build a quick PluginName → catalog entry lookup so we can iterate
	// the on-disk extensions in O(N+M) instead of O(N×M).
	TMap<FString, const FNeoStackCatalogEntry*> ByPluginName;
	ByPluginName.Reserve(Catalog.Entries.Num());
	for (const FNeoStackCatalogEntry& E : Catalog.Entries)
	{
		if (!E.PluginName.IsEmpty())
		{
			ByPluginName.Add(E.PluginName, &E);
		}
	}

	const TArray<FNeoStackManagedExtension> Managed = FNeoStackExtensionProjectService::GetManagedExtensions();
	for (const FNeoStackManagedExtension& M : Managed)
	{
		// Only consider extensions the user has actually opted into. A
		// disabled-but-present extension shouldn't get auto-updated under
		// them; if they re-enable later the panel will surface it.
		if (!M.bEnabledInProject) continue;

		const FNeoStackCatalogEntry* const* Found = ByPluginName.Find(M.PluginName);
		if (!Found || !*Found) continue; // not in catalog (deprecated / private build)

		const FNeoStackCatalogEntry& Entry = **Found;
		if (Entry.LatestVersion.IsEmpty()) continue;
		if (Entry.LatestVersion == M.Version) continue;

		// Optional engine filter — skip extensions that don't have an
		// artifact for the running engine. Catalog already filters this
		// when EngineVersion is passed to RefreshAsync, but defend in
		// case the cached result was fetched with a different engine.
		if (!EngineVersion.IsEmpty() && Entry.SupportedEngineVersions.Num() > 0
			&& !Entry.SupportedEngineVersions.Contains(EngineVersion))
		{
			continue;
		}

		FNeoStackExtensionUpdateCandidate C;
		C.Slug = Entry.Slug;
		C.PluginName = M.PluginName;
		C.DisplayName = !Entry.DisplayName.IsEmpty() ? Entry.DisplayName : M.PluginName;
		C.InstalledVersion = M.Version;
		C.LatestVersion = Entry.LatestVersion;
		Candidates.Add(MoveTemp(C));
	}

	UE_LOG(LogNeoStackAI, Log,
		TEXT("[installer] ComputePendingUpdates: %d extension(s) need updates on channel '%s'"),
		Candidates.Num(), *Channel);
	return Candidates;
}

FNeoStackInstallerState FNeoStackExtensionInstaller::GetState()
{
	FNeoStackInstallerState S;
	S.bRunning = GState.bRunning;
	S.StartedAt = GState.StartedAt;
	S.CompletedAt = GState.CompletedAt;
	S.Ops = GState.Ops; // shallow copy of TSharedPtrs

	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : GState.Ops)
	{
		if (!Op.IsValid()) continue;
		if (Op->Phase == ENeoStackOpPhase::Success || Op->Phase == ENeoStackOpPhase::PendingRestart) S.SucceededCount++;
		if (Op->Phase == ENeoStackOpPhase::Failed)  S.FailedCount++;
	}

	// Any successful install / update / uninstall changes .uproject; user
	// should restart the editor to load the new module layout cleanly.
	S.bRestartRecommended = (S.SucceededCount > 0) || HasPendingRestartOps();

	return S;
}

// ────────────────────────────────────────────────────────────────
// JSON serialization (for the bridge)
// ────────────────────────────────────────────────────────────────

namespace
{
	TSharedRef<FJsonObject> OpToJson(const FNeoStackExtensionOp& Op)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("slug"), Op.Slug);
		O->SetStringField(TEXT("pluginName"), Op.PluginName);
		O->SetStringField(TEXT("kind"), KindToString(Op.Kind));
		O->SetStringField(TEXT("phase"), PhaseToString(Op.Phase));
		O->SetStringField(TEXT("channel"), Op.RequestedChannel);
		O->SetStringField(TEXT("engine"), Op.EngineVersion);
		O->SetStringField(TEXT("platform"), Op.Platform);
		if (!Op.ErrorMessage.IsEmpty())
		{
			O->SetStringField(TEXT("error"), Op.ErrorMessage);
		}
		if (!Op.StagedPluginRoot.IsEmpty())
		{
			O->SetStringField(TEXT("stagedPluginRoot"), Op.StagedPluginRoot);
		}
		if (Op.BytesTotal > 0 || Op.BytesDone > 0)
		{
			O->SetNumberField(TEXT("bytesTotal"), static_cast<double>(Op.BytesTotal));
			O->SetNumberField(TEXT("bytesDone"), static_cast<double>(Op.BytesDone));
		}
		if (!Op.ResolvedVersion.IsEmpty()) O->SetStringField(TEXT("resolvedVersion"), Op.ResolvedVersion);
		if (!Op.ResolvedFileName.IsEmpty()) O->SetStringField(TEXT("resolvedFileName"), Op.ResolvedFileName);
		return O;
	}

	FString SerializeRoot(const TSharedRef<FJsonObject>& Root)
	{
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}
}

FString FNeoStackExtensionInstaller::QueueToJson()
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : GState.Ops)
	{
		if (!Op.IsValid() || Op->Phase != ENeoStackOpPhase::Queued) continue;
		Arr.Add(MakeShared<FJsonValueObject>(OpToJson(*Op)));
	}
	Root->SetArrayField(TEXT("queue"), Arr);
	Root->SetNumberField(TEXT("count"), Arr.Num());
	return SerializeRoot(Root);
}

FString FNeoStackExtensionInstaller::StateToJson()
{
	const FNeoStackInstallerState State = GetState();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("running"), State.bRunning);
	Root->SetBoolField(TEXT("restartRecommended"), State.bRestartRecommended);
	Root->SetNumberField(TEXT("succeeded"), State.SucceededCount);
	Root->SetNumberField(TEXT("failed"), State.FailedCount);
	if (State.StartedAt.GetTicks() > 0)
	{
		Root->SetStringField(TEXT("startedAt"), State.StartedAt.ToIso8601());
	}
	if (State.CompletedAt.GetTicks() > 0)
	{
		Root->SetStringField(TEXT("completedAt"), State.CompletedAt.ToIso8601());
	}

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe>& Op : State.Ops)
	{
		if (!Op.IsValid()) continue;
		Arr.Add(MakeShared<FJsonValueObject>(OpToJson(*Op)));
	}
	Root->SetArrayField(TEXT("ops"), Arr);

	return SerializeRoot(Root);
}
