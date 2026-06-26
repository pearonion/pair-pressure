// Copyright 2026 Betide Studio. All Rights Reserved.

#include "NodeRuntime.h"
#include "NeoStackAIModule.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"

namespace
{
	// Pinned LTS version we test against. Keep in sync with the version Zed uses
	// (see references/zed/crates/node_runtime/src/node_runtime.rs ManagedNodeRuntime::VERSION),
	// since we run the same agents (claude-code-acp, codex-acp) and inherit their tested matrix.
	constexpr const TCHAR* kPinnedNodeVersion = TEXT("v24.11.0");
	constexpr int32 kMinSystemMajorVersion = 18;

	FString GetUserHomeDir()
	{
#if PLATFORM_WINDOWS
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
#else
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
#endif
		return Home;
	}
}

// ── Singleton ────────────────────────────────────────────────────────────────

FNodeRuntime& FNodeRuntime::Get()
{
	static FNodeRuntime Instance;
	return Instance;
}

const TCHAR* FNodeRuntime::GetTargetVersion()
{
	return kPinnedNodeVersion;
}

int32 FNodeRuntime::GetMinimumMajorVersion()
{
	return kMinSystemMajorVersion;
}

// ── Platform mapping ─────────────────────────────────────────────────────────

FString FNodeRuntime::GetPlatformKey()
{
	// Maps to the file-naming convention nodejs.org/dist uses.
#if PLATFORM_WINDOWS
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("win-arm64");
	#else
		return TEXT("win-x64");
	#endif
#elif PLATFORM_MAC
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("darwin-arm64");
	#else
		return TEXT("darwin-x64");
	#endif
#elif PLATFORM_LINUX
	#if PLATFORM_CPU_ARM_FAMILY
		return TEXT("linux-arm64");
	#else
		return TEXT("linux-x64");
	#endif
#else
	return FString();
#endif
}

FString FNodeRuntime::GetExpectedFolderName()
{
	// nodejs.org tarballs all extract to a folder named after the archive (minus extension).
	return FString::Printf(TEXT("node-%s-%s"), kPinnedNodeVersion, *GetPlatformKey());
}

FString FNodeRuntime::GetArchiveFileName()
{
#if PLATFORM_WINDOWS
	const TCHAR* Ext = TEXT(".zip");
#else
	const TCHAR* Ext = TEXT(".tar.gz");
#endif
	return FString::Printf(TEXT("%s%s"), *GetExpectedFolderName(), Ext);
}

FString FNodeRuntime::GetArchiveUrl()
{
	return FString::Printf(TEXT("https://nodejs.org/dist/%s/%s"),
		kPinnedNodeVersion, *GetArchiveFileName());
}

// ── Install paths ────────────────────────────────────────────────────────────

FString FNodeRuntime::GetManagedRootDir()
{
	const FString Home = GetUserHomeDir();
	if (Home.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(Home, TEXT(".agentintegrationkit"), TEXT("node"));
}

FString FNodeRuntime::GetManagedVersionDir()
{
	const FString Root = GetManagedRootDir();
	if (Root.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(Root, GetExpectedFolderName());
}

FString FNodeRuntime::GetManagedNodePath()
{
	const FString VersionDir = GetManagedVersionDir();
	if (VersionDir.IsEmpty())
	{
		return FString();
	}
#if PLATFORM_WINDOWS
	return FPaths::Combine(VersionDir, TEXT("node.exe"));
#else
	return FPaths::Combine(VersionDir, TEXT("bin"), TEXT("node"));
#endif
}

FString FNodeRuntime::GetManagedNpxPath()
{
	const FString VersionDir = GetManagedVersionDir();
	if (VersionDir.IsEmpty())
	{
		return FString();
	}
#if PLATFORM_WINDOWS
	// Windows distribution ships npx.cmd at the archive root.
	return FPaths::Combine(VersionDir, TEXT("npx.cmd"));
#else
	return FPaths::Combine(VersionDir, TEXT("bin"), TEXT("npx"));
#endif
}

// ── Detection ────────────────────────────────────────────────────────────────

bool FNodeRuntime::ProbeNodeVersion(const FString& NodePath, FString& OutVersion)
{
	if (NodePath.IsEmpty() || !IFileManager::Get().FileExists(*NodePath))
	{
		return false;
	}

	FString StdOut, StdErr;
	int32 ReturnCode = -1;
	FPlatformProcess::ExecProcess(*NodePath, TEXT("--version"), &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode != 0)
	{
		return false;
	}

	OutVersion = StdOut.TrimStartAndEnd();
	return !OutVersion.IsEmpty();
}

int32 FNodeRuntime::ParseMajorVersion(const FString& VersionString)
{
	// Accept "v24.11.0", "24.11.0", or "24" — strip the leading 'v' and split on '.'.
	FString Trimmed = VersionString.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("v")))
	{
		Trimmed.RightChopInline(1);
	}

	int32 DotIdx;
	const FString Major = Trimmed.FindChar(TEXT('.'), DotIdx) ? Trimmed.Left(DotIdx) : Trimmed;
	if (Major.IsEmpty() || !Major.IsNumeric())
	{
		return -1;
	}
	return FCString::Atoi(*Major);
}

bool FNodeRuntime::TryFindSystemNode(FString& OutNodePath, FString& OutNpxPath, FString& OutVersion) const
{
	// Reuse AgentInstaller's PATH + login-shell resolution so we honor the same conventions
	// (homebrew, nvm, .npm-global, etc.) used elsewhere in the plugin.
	FAgentInstaller& Installer = FAgentInstaller::Get();

	FString NodePath;
	if (!Installer.ResolveExecutable(TEXT("node"), NodePath)
		&& !Installer.ResolveExecutableViaLoginShell(TEXT("node"), NodePath))
	{
		return false;
	}

	FString NpxPath;
	if (!Installer.ResolveExecutable(TEXT("npx"), NpxPath)
		&& !Installer.ResolveExecutableViaLoginShell(TEXT("npx"), NpxPath))
	{
#if PLATFORM_WINDOWS
		if (!Installer.ResolveExecutable(TEXT("npx.cmd"), NpxPath))
		{
			return false;
		}
#else
		return false;
#endif
	}

	if (!ProbeNodeVersion(NodePath, OutVersion))
	{
		return false;
	}

	OutNodePath = NodePath;
	OutNpxPath = NpxPath;
	return true;
}

bool FNodeRuntime::HasUsableSystemNode() const
{
	FString NodePath, NpxPath, Version;
	if (!TryFindSystemNode(NodePath, NpxPath, Version))
	{
		return false;
	}
	const int32 Major = ParseMajorVersion(Version);
	return Major >= kMinSystemMajorVersion;
}

bool FNodeRuntime::IsManagedNodeInstalled() const
{
	const FString NodePath = GetManagedNodePath();
	if (NodePath.IsEmpty() || !IFileManager::Get().FileExists(*NodePath))
	{
		return false;
	}

	// Validate it actually runs and reports the version we expect. Catches partial extracts,
	// AV quarantine, and corrupt archives (Zed's pattern in node_runtime.rs).
	FString Version;
	if (!ProbeNodeVersion(NodePath, Version))
	{
		return false;
	}
	return Version.Contains(kPinnedNodeVersion);
}

bool FNodeRuntime::TryGetAnyNpxPath(FString& OutNpxPath) const
{
	if (HasUsableSystemNode())
	{
		FString NodePath, Version;
		FString SystemNpx;
		if (TryFindSystemNode(NodePath, SystemNpx, Version))
		{
			OutNpxPath = SystemNpx;
			return true;
		}
	}

	if (IsManagedNodeInstalled())
	{
		OutNpxPath = GetManagedNpxPath();
		return !OutNpxPath.IsEmpty();
	}

	return false;
}

FString FNodeRuntime::BuildAugmentedPath(const FString& BasePath) const
{
	if (!IsManagedNodeInstalled())
	{
		return BasePath;
	}

	const FString NodeBinDir = FPaths::GetPath(GetManagedNodePath());
	if (NodeBinDir.IsEmpty())
	{
		return BasePath;
	}

#if PLATFORM_WINDOWS
	const FString Sep = TEXT(";");
#else
	const FString Sep = TEXT(":");
#endif

	if (BasePath.Contains(NodeBinDir))
	{
		return BasePath;
	}

	return BasePath.IsEmpty() ? NodeBinDir : (NodeBinDir + Sep + BasePath);
}

// ── Download + extract ───────────────────────────────────────────────────────

bool FNodeRuntime::DownloadAndExtractManagedNode(FOnInstallProgress OnProgress, FACPInstallError& OutError) const
{
	const FString PlatformKey = GetPlatformKey();
	if (PlatformKey.IsEmpty())
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::PlatformUnsupported,
			TEXT("This platform does not have a Node.js distribution we can bundle."));
		return false;
	}

	const FString ArchiveUrl = GetArchiveUrl();
	const FString ManagedRoot = GetManagedRootDir();
	const FString VersionDir = GetManagedVersionDir();
	if (ManagedRoot.IsEmpty() || VersionDir.IsEmpty())
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::Unknown,
			TEXT("Could not determine HOME directory for Node install."));
		return false;
	}

	// Already installed? Bail early.
	if (IsManagedNodeInstalled())
	{
		return true;
	}

	IFileManager::Get().MakeDirectory(*ManagedRoot, true);

	// Wipe any half-extracted leftover from a previous failed install before we re-extract.
	if (IFileManager::Get().DirectoryExists(*VersionDir))
	{
		UE_LOG(LogNeoStackAI, Log, TEXT("FNodeRuntime: Removing stale install at %s"), *VersionDir);
		IFileManager::Get().DeleteDirectory(*VersionDir, /*RequireExists=*/false, /*Tree=*/true);
	}

	AsyncTask(ENamedThreads::GameThread, [OnProgress, ArchiveUrl]()
	{
		OnProgress.ExecuteIfBound(FString::Printf(TEXT("Downloading Node.js (%s)..."), *ArchiveUrl));
	});

	// Synchronous HTTP — same pattern as FAgentInstaller::DownloadAndExtractBinary.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ArchiveUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("NeoStackAI/1.0"));

	FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
	bool bDownloadOk = false;
	int32 ResponseCode = 0;
	TArray<uint8> Body;

	Request->OnProcessRequestComplete().BindLambda(
		[&bDownloadOk, &ResponseCode, &Body, DoneEvent](FHttpRequestPtr, FHttpResponsePtr Resp, bool bSucceeded)
		{
			if (bSucceeded && Resp.IsValid())
			{
				ResponseCode = Resp->GetResponseCode();
				if (ResponseCode == 200)
				{
					Body = Resp->GetContent();
					bDownloadOk = true;
				}
			}
			DoneEvent->Trigger();
		});

	Request->ProcessRequest();
	DoneEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

	if (!bDownloadOk || Body.Num() == 0)
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::DownloadFailed,
			FString::Printf(TEXT("Node.js download failed (HTTP %d) from %s"), ResponseCode, *ArchiveUrl),
			TEXT("https://nodejs.org/en/download"));
		return false;
	}

	UE_LOG(LogNeoStackAI, Log, TEXT("FNodeRuntime: Downloaded %d bytes from %s"), Body.Num(), *ArchiveUrl);

	// Persist to a temp file, then hand off to AgentInstaller's existing extractor so we don't
	// duplicate the per-platform tar/zip dispatch logic.
	const FString TempDir = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("aik-node-install"));
	IFileManager::Get().MakeDirectory(*TempDir, true);
	const FString TempArchive = FPaths::Combine(TempDir, GetArchiveFileName());

	if (!FFileHelper::SaveArrayToFile(Body, *TempArchive))
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::DownloadFailed,
			TEXT("Failed to write Node.js archive to temp directory."));
		return false;
	}

	AsyncTask(ENamedThreads::GameThread, [OnProgress]()
	{
		OnProgress.ExecuteIfBound(TEXT("Extracting Node.js..."));
	});

	FString ExtractError;
	const bool bExtracted = FAgentInstaller::Get().ExtractArchivePublic(TempArchive, ManagedRoot, ExtractError);
	IFileManager::Get().Delete(*TempArchive);

	if (!bExtracted)
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::ExtractionFailed,
			FString::Printf(TEXT("Failed to extract Node.js archive: %s"), *ExtractError));
		return false;
	}

	// nodejs.org tarballs extract to a folder named after the archive — verify it's there.
	if (!IFileManager::Get().DirectoryExists(*VersionDir))
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::ExtractionFailed,
			FString::Printf(TEXT("Node archive extracted but expected directory '%s' is missing. "
				"This is usually antivirus quarantine — exempt the folder and retry."), *VersionDir));
		return false;
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	// Set executable bit on bin/node and bin/npx (tar usually preserves this, but be defensive).
	FAgentInstaller::EnsureNativeAdapterExecutable(GetManagedNodePath());
	FAgentInstaller::EnsureNativeAdapterExecutable(GetManagedNpxPath());
#endif

	// Final validation: it has to actually run.
	FString InstalledVersion;
	if (!ProbeNodeVersion(GetManagedNodePath(), InstalledVersion))
	{
		OutError = FACPInstallError::Make(EACPInstallErrorKind::ExtractionFailed,
			TEXT("Node.js was extracted but failed to execute. The archive may be corrupt or blocked by AV."));
		return false;
	}

	AsyncTask(ENamedThreads::GameThread, [OnProgress, InstalledVersion]()
	{
		OnProgress.ExecuteIfBound(FString::Printf(TEXT("Node.js %s ready."), *InstalledVersion));
	});

	UE_LOG(LogNeoStackAI, Log, TEXT("FNodeRuntime: Managed Node %s installed at %s"),
		*InstalledVersion, *GetManagedNodePath());

	return true;
}

void FNodeRuntime::EnsureManagedNodeAsync(FOnInstallProgress OnProgress, FOnInstallComplete OnComplete)
{
	// Fast path: already installed, no work needed.
	if (IsManagedNodeInstalled())
	{
		OnProgress.ExecuteIfBound(TEXT("Node.js (bundled) already installed."));
		OnComplete.ExecuteIfBound(true, FACPInstallError::Ok());
		return;
	}

	// Concurrency guard: if a download is already in flight, queue the callback and return.
	{
		FScopeLock Lock(&InstallLock);
		if (bInstallInFlight)
		{
			PendingCallbacks.Emplace(OnProgress, OnComplete);
			return;
		}
		bInstallInFlight = true;
	}

	Async(EAsyncExecution::Thread, [this, OnProgress, OnComplete]()
	{
		FACPInstallError Err;
		const bool bOk = DownloadAndExtractManagedNode(OnProgress, Err);

		// Drain queued callbacks under the lock, then deliver everything on the game thread.
		TArray<TPair<FOnInstallProgress, FOnInstallComplete>> Drained;
		{
			FScopeLock Lock(&InstallLock);
			Drained = MoveTemp(PendingCallbacks);
			PendingCallbacks.Reset();
			bInstallInFlight = false;
		}

		AsyncTask(ENamedThreads::GameThread, [OnComplete, bOk, Err, Drained]()
		{
			OnComplete.ExecuteIfBound(bOk, Err);
			for (const auto& Pair : Drained)
			{
				Pair.Value.ExecuteIfBound(bOk, Err);
			}
		});
	});
}

void FNodeRuntime::EnsureNpxAvailableAsync(FOnInstallProgress OnProgress, FOnInstallComplete OnComplete)
{
	FString DummyNpx;
	if (TryGetAnyNpxPath(DummyNpx))
	{
		OnComplete.ExecuteIfBound(true, FACPInstallError::Ok());
		return;
	}
	EnsureManagedNodeAsync(OnProgress, OnComplete);
}
