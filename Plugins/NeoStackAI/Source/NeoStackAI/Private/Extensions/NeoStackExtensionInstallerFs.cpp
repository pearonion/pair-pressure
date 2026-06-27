// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Filesystem-backed steps for FNeoStackExtensionInstaller:
//   • Step_Verify             — SHA-256 of the downloaded archive
//   • Step_Extract            — Unzip into a staging dir under the cache root
//   • Step_AtomicInstall      — Swap staging dir into Plugins/NeoExtensions/
//   • Step_EnableInProject    — Mark enabled in .uproject
//   • Step_DisableInProject   — Mark disabled in .uproject (uninstall)
//   • Step_DeletePluginDir    — Remove the plugin folder (uninstall)
//
// All work here runs synchronously on the game thread. These operations are
// small — zip extraction via the platform's native tool is process-launch-
// bound but completes in under a second for typical extensions.

#include "Extensions/NeoStackExtensionInstallerInternal.h"

#include "NeoStackAIModule.h"
#include "Extensions/NeoStackExtensionProjectService.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"

namespace NeoStackExtensionInstallerInternal
{
	// ── ComputeFileSha256 ───────────────────────────────────────────
	// UE 5.7's FPlatformMisc::GetSHA256Signature has no Windows/Mac backend
	// ("No SHA256 Platform implementation" — asserts). Shell out to the same
	// platform-native tools we already depend on for unzip. The output is
	// trimmed + lowercased before returning so callers can string-compare
	// against the manifest checksum.

	static FString ExtractFirstHexToken(const FString& StdOut)
	{
		// Accepts both "<hash>\n" (Windows `Get-FileHash`) and
		// "<hash>  /path/to/file\n" (Mac `shasum -a 256`) outputs.
		for (const FString& Line : { StdOut })
		{
			FString Trimmed = Line;
			Trimmed.TrimStartAndEndInline();
			int32 SpaceIdx = INDEX_NONE;
			if (Trimmed.FindChar(' ', SpaceIdx))
			{
				Trimmed = Trimmed.Left(SpaceIdx);
			}
			if (!Trimmed.IsEmpty())
			{
				return Trimmed.ToLower();
			}
		}
		return FString();
	}

	FString EscapePowerShellSingleQuotedString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("'"), TEXT("''"), ESearchCase::CaseSensitive);
		return Escaped;
	}

	FString ComputeFileSha256(const FString& FilePath, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.FileExists(*FilePath))
		{
			OutError = FString::Printf(TEXT("File does not exist: %s"), *FilePath);
			return FString();
		}

#if PLATFORM_WINDOWS
		const FString EscapedPath = EscapePowerShellSingleQuotedString(FilePath);
		const FString Cmd = FString::Printf(
			TEXT("-NoProfile -ExecutionPolicy Bypass -Command \"$p = '%s'; (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash\""),
			*EscapedPath);
		TUniquePtr<FMonitoredProcess> Process = MakeUnique<FMonitoredProcess>(TEXT("powershell.exe"), Cmd, true, true);
		if (!Process->Launch())
		{
			OutError = TEXT("Failed to launch powershell.exe for SHA256 computation.");
			return FString();
		}
		while (Process->Update()) { FPlatformProcess::Sleep(0.0f); }
		const int32 ReturnCode = Process->GetReturnCode();
		const FString StdOut = Process->GetFullOutputWithoutDelegate();
		if (ReturnCode != 0)
		{
			OutError = FString::Printf(
				TEXT("powershell Get-FileHash failed with exit code %d: %s"),
				ReturnCode,
				*StdOut);
			return FString();
		}
		const FString Hash = ExtractFirstHexToken(StdOut);
		if (Hash.IsEmpty())
		{
			OutError = TEXT("powershell Get-FileHash returned no hash output.");
		}
		return Hash;
#elif PLATFORM_MAC
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/shasum"),
			*FString::Printf(TEXT("-a 256 \"%s\""), *FilePath),
			&ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0)
		{
			OutError = FString::Printf(
				TEXT("shasum failed with exit code %d: %s%s%s"),
				ReturnCode,
				*StdErr.TrimStartAndEnd(),
				StdOut.IsEmpty() ? TEXT("") : TEXT(" "),
				*StdOut.TrimStartAndEnd());
			return FString();
		}
		const FString Hash = ExtractFirstHexToken(StdOut);
		if (Hash.IsEmpty())
		{
			OutError = TEXT("shasum returned no hash output.");
		}
		return Hash;
#else
		OutError = TEXT("SHA256 verification is not implemented for this platform.");
		return FString();
#endif
	}

	// ── DeleteDirectoryTree ─────────────────────────────────────────
	// Best-effort recursive delete. UE's FPlatformFileManager has a
	// DeleteDirectoryRecursively which is what we want.

	bool DeleteDirectoryTree(const FString& Path, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (!PlatformFile.DirectoryExists(*Path))
		{
			return true; // nothing to delete = success
		}

		if (!PlatformFile.DeleteDirectoryRecursively(*Path))
		{
			OutError = FString::Printf(TEXT("Could not delete %s"), *Path);
			return false;
		}
		return true;
	}

	// ── UnzipArchive ────────────────────────────────────────────────
	// Shells out to the platform's native zip tool. UE doesn't ship a
	// general-purpose zip reader we can call from C++, so this matches the
	// approach the existing AgentInstaller + plugin-update flow already uses.
	//
	// Windows: powershell Expand-Archive
	// Mac:     /usr/bin/unzip
	// Fails loudly on unexpected exit codes.

	bool UnzipArchive(const FString& ArchivePath, const FString& DestDir, FString& OutError)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*DestDir);

#if PLATFORM_WINDOWS
		const FString Cmd = FString::Printf(
			TEXT("-NoProfile -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\""),
			*ArchivePath, *DestDir);
		TUniquePtr<FMonitoredProcess> Process = MakeUnique<FMonitoredProcess>(TEXT("powershell.exe"), Cmd, true, true);
		if (!Process->Launch())
		{
			OutError = TEXT("Failed to launch powershell.exe for zip extraction.");
			return false;
		}
		while (Process->Update()) { FPlatformProcess::Sleep(0.0f); }
		const int32 ReturnCode = Process->GetReturnCode();
		const FString StdOut = Process->GetFullOutputWithoutDelegate();
		if (ReturnCode != 0)
		{
			OutError = StdOut.IsEmpty() ? TEXT("PowerShell Expand-Archive failed") : StdOut;
			return false;
		}
		return true;
#elif PLATFORM_MAC
		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		FPlatformProcess::ExecProcess(
			TEXT("/usr/bin/unzip"),
			*FString::Printf(TEXT("-o \"%s\" -d \"%s\""), *ArchivePath, *DestDir),
			&ReturnCode, &StdOut, &StdErr);
		if (ReturnCode != 0)
		{
			OutError = StdErr.IsEmpty() ? TEXT("/usr/bin/unzip failed") : StdErr.TrimStartAndEnd();
			return false;
		}
		return true;
#else
		OutError = TEXT("Zip extraction not implemented for this platform.");
		return false;
#endif
	}

	// ── Step_Verify ─────────────────────────────────────────────────
	// Compare SHA256 of the downloaded file against the manifest checksum.
	// Pure filesystem work, runs synchronously.

	void Step_Verify(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		if (Op->ResolvedSha256.IsEmpty())
		{
			// No manifest checksum — skip verification with a warning. We'd
			// rather continue than block on missing server-side data.
			UE_LOG(LogNeoStackAI, Warning,
				TEXT("[installer] %s: no server-side checksum, skipping verify"), *Op->Slug);
			AdvanceOp(Op);
			return;
		}

		FString HashError;
		const FString Actual = ComputeFileSha256(Op->TempArchivePath, HashError);
		if (Actual.IsEmpty())
		{
			FailOp(Op, FString::Printf(
				TEXT("Could not compute SHA256 for %s: %s"),
				*Op->TempArchivePath,
				HashError.IsEmpty() ? TEXT("unknown error") : *HashError));
			return;
		}

		if (!Actual.Equals(Op->ResolvedSha256, ESearchCase::IgnoreCase))
		{
			FailOp(Op, FString::Printf(
				TEXT("Checksum mismatch — expected %s, got %s. Aborting install."),
				*Op->ResolvedSha256, *Actual));
			return;
		}

		AdvanceOp(Op);
	}

	// ── Step_Extract ────────────────────────────────────────────────
	// Unzip into a fresh staging dir. If the archive contains a single
	// top-level folder matching the plugin name, the later AtomicInstall
	// step will promote that folder to the target; otherwise we treat the
	// staging dir contents as the plugin root.

	void Step_Extract(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		Op->StagingDir = BuildStagingDir(Op->Slug);

		// If a prior failed run left residue with the same timestamp, wipe it.
		FString DelErr;
		DeleteDirectoryTree(Op->StagingDir, DelErr); // ignore err — best effort

		FString UnzipErr;
		if (!UnzipArchive(Op->TempArchivePath, Op->StagingDir, UnzipErr))
		{
			FailOp(Op, FString::Printf(TEXT("Unzip failed: %s"), *UnzipErr));
			return;
		}

		AdvanceOp(Op);
	}

	// ── ResolveStagingRoot ──────────────────────────────────────────
	// Archives produced by our release workflow are structured like
	//   <PluginName>/<PluginName>.uplugin
	//   <PluginName>/Source/...
	//   <PluginName>/Binaries/...
	// So after unzipping to StagingDir, the real plugin root is usually
	// StagingDir/<PluginName>. Fallback to StagingDir if the nested folder
	// isn't there (e.g. a hand-built zip).

	FString ResolveStagingPluginRoot(const FString& StagingDir, const FString& PluginName)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Nested = StagingDir / PluginName;
		if (PlatformFile.DirectoryExists(*Nested))
		{
			return Nested;
		}
		return StagingDir;
	}

	// ── Step_AtomicInstall ──────────────────────────────────────────
	// Move the staged plugin into Plugins/NeoExtensions/<PluginName>/ in a
	// way that's atomic-at-the-directory level. If the target already exists
	// (update case), back it up first, then rename staging into place. Only
	// delete the backup after the rename succeeds — failures roll back.

	void Step_AtomicInstall(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		const FString StagedRoot = ResolveStagingPluginRoot(Op->StagingDir, Op->PluginName);
		const FString Target = Op->TargetPluginDir;

		if (!PlatformFile.DirectoryExists(*StagedRoot))
		{
			FailOp(Op, FString::Printf(TEXT("Staging root missing after extract: %s"), *StagedRoot));
			return;
		}

		// Ensure parent dir (<project>/Plugins/NeoExtensions) exists.
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Target));

		FString Backup;
		// Mirror the web UI's installed-vs-available decision using GetExtensionSettings()
		const bool bIsPluginInstalled = FNeoStackExtensionProjectService::IsPluginManaged(Op->PluginName);
		if (bIsPluginInstalled)
		{
			if (Op->Kind != ENeoStackOpKind::Update)
			{
				FailOp(Op, FString::Printf(TEXT("Plugin directory already exists: %s"), *Target));
				return;
			}

			Op->StagedPluginRoot = StagedRoot;
			Op->Phase = ENeoStackOpPhase::PendingRestart;
			AdvanceOp(Op);
			return;
		}

		// We are Installing (plugin not managed), but a stale dir may remain on disk.
		// MoveFile fails if the destination exists, so we clean the target destination first.
		{
			FString DelErr;
			DeleteDirectoryTree(Target, DelErr);
		}

		if (!PlatformFile.MoveFile(*Target, *StagedRoot))
		{
			// Restore the backup if we have one — don't leave the user with a missing plugin.
			if (!Backup.IsEmpty() && PlatformFile.DirectoryExists(*Backup))
			{
				PlatformFile.MoveFile(*Target, *Backup);
			}
			FailOp(Op, FString::Printf(TEXT("Could not move staged plugin into %s."), *Target));
			return;
		}

		// Install succeeded — delete backup if we made one.
		if (!Backup.IsEmpty())
		{
			FString DelErr;
			DeleteDirectoryTree(Backup, DelErr); // best-effort
		}

		// Clean up leftovers in StagingDir (the rename only promoted the nested
		// folder; top-level staging dir may still be around).
		{
			FString DelErr;
			DeleteDirectoryTree(Op->StagingDir, DelErr);
		}

		// Remove the temp archive too — we verified it, extracted it, we're done.
		PlatformFile.DeleteFile(*Op->TempArchivePath);

		AdvanceOp(Op);
	}

	void Step_StageForRestart(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		if (Op->Kind == ENeoStackOpKind::Update)
		{
			Op->StagedPluginRoot = ResolveStagingPluginRoot(Op->StagingDir, Op->PluginName);
			if (!FPlatformFileManager::Get().GetPlatformFile().DirectoryExists(*Op->StagedPluginRoot))
			{
				FailOp(Op, FString::Printf(TEXT("Staging root missing after extract: %s"), *Op->StagedPluginRoot));
				return;
			}
		}

		Op->Phase = ENeoStackOpPhase::PendingRestart;
		AdvanceOp(Op);
	}

	// ── Step_EnableInProject ────────────────────────────────────────
	// Turn on the plugin in the .uproject so UE loads it on next launch. The
	// existing FNeoStackExtensionProjectService already owns this logic,
	// including triggering a registry rescan so the restart banner shows.

	void Step_EnableInProject(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		// The plugin folder only just landed on disk, so IPluginManager's
		// cached plugin list doesn't know about it yet. Force a rescan so
		// FNeoStackExtensionProjectService::SetProjectExtensionEnabled can
		// find the new plugin descriptor via FindPlugin.
		IPluginManager::Get().RefreshPluginsList();

		FString Error;
		const bool bOk = FNeoStackExtensionProjectService::SetProjectExtensionEnabled(Op->PluginName, true, Error);
		if (!bOk)
		{
			FailOp(Op, FString::Printf(TEXT("Could not enable %s in .uproject: %s"),
				*Op->PluginName, *Error));
			return;
		}

		AdvanceOp(Op);
	}

	// ── Step_DisableInProject ───────────────────────────────────────
	// Uninstall path. Toggle the plugin off first so UE won't try to load it.

	void Step_DisableInProject(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		FString Error;
		// SetProjectExtensionEnabled tolerates "already disabled" — it won't
		// fail if the plugin isn't currently enabled.
		const bool bOk = FNeoStackExtensionProjectService::SetProjectExtensionEnabled(Op->PluginName, false, Error);
		if (!bOk)
		{
			FailOp(Op, FString::Printf(TEXT("Could not disable %s in .uproject: %s"),
				*Op->PluginName, *Error));
			return;
		}

		AdvanceOp(Op);
	}

	// ── Step_DeletePluginDir ────────────────────────────────────────
	// Remove Plugins/NeoExtensions/<PluginName>/. Terminal step for uninstall.

	void Step_DeletePluginDir(TSharedPtr<FNeoStackExtensionOp, ESPMode::ThreadSafe> Op)
	{
		if (!Op.IsValid()) return;

		FString Err;
		if (!DeleteDirectoryTree(Op->TargetPluginDir, Err))
		{
			FailOp(Op, FString::Printf(TEXT("Uninstall could not delete %s: %s"),
				*Op->TargetPluginDir, *Err));
			return;
		}

		AdvanceOp(Op);
	}
}
