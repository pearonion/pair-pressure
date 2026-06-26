// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ACPRegistryClient.h"

/**
 * Classified install failure modes — mirrors Zed's LoadError so the WebUI can render
 * a different action button per case ("Install Node now", "Open releases page", etc.)
 * instead of a generic toast.
 */
enum class EACPInstallErrorKind : uint8
{
	Success,                  // Not an error — install succeeded.
	PlatformUnsupported,      // Registry has no distribution at all for this OS/arch.
	NoBinaryForPlatform,      // Registry has npx/uvx but the user requested a binary install.
	NodeNotFound,             // npx requested but Node is not on PATH and we haven't bundled one yet.
	UvxNotFound,              // uvx requested but uv is not installed.
	DownloadFailed,           // HTTP fetch of the archive failed (network, 404, etc).
	ChecksumMismatch,         // SHA256 didn't match the expected hash.
	ExtractionFailed,         // Unzip / tar extraction failed.
	ManifestWriteFailed,      // Couldn't write the install manifest to disk.
	Cancelled,                // User cancelled mid-install.
	Unknown,                  // Fallback for anything else.
};

struct FACPInstallError
{
	EACPInstallErrorKind Kind = EACPInstallErrorKind::Success;

	/** Human-readable message. Always populated, including for Success (in which case it's empty). */
	FString Message;

	/** Optional URL for the UI to surface as a "fix this" link (download page, docs, etc.). */
	FString ActionUrl;

	bool IsSuccess() const { return Kind == EACPInstallErrorKind::Success; }

	static FACPInstallError Ok() { return FACPInstallError{}; }
	static FACPInstallError Make(EACPInstallErrorKind InKind, const FString& InMessage, const FString& InActionUrl = FString())
	{
		FACPInstallError E;
		E.Kind = InKind;
		E.Message = InMessage;
		E.ActionUrl = InActionUrl;
		return E;
	}

	/** Stable string identifier for the WebUI / telemetry. Matches the enum names. */
	const TCHAR* KindString() const
	{
		switch (Kind)
		{
		case EACPInstallErrorKind::Success:             return TEXT("success");
		case EACPInstallErrorKind::PlatformUnsupported: return TEXT("platform_unsupported");
		case EACPInstallErrorKind::NoBinaryForPlatform: return TEXT("no_binary_for_platform");
		case EACPInstallErrorKind::NodeNotFound:        return TEXT("node_not_found");
		case EACPInstallErrorKind::UvxNotFound:         return TEXT("uvx_not_found");
		case EACPInstallErrorKind::DownloadFailed:      return TEXT("download_failed");
		case EACPInstallErrorKind::ChecksumMismatch:    return TEXT("checksum_mismatch");
		case EACPInstallErrorKind::ExtractionFailed:    return TEXT("extraction_failed");
		case EACPInstallErrorKind::ManifestWriteFailed: return TEXT("manifest_write_failed");
		case EACPInstallErrorKind::Cancelled:           return TEXT("cancelled");
		default:                                        return TEXT("unknown");
		}
	}
};

DECLARE_DELEGATE_OneParam(FOnInstallProgress, const FString& /* StatusMessage */);
DECLARE_DELEGATE_TwoParams(FOnInstallComplete, bool /* bSuccess */, const FACPInstallError& /* Error */);

class NEOSTACKAI_API FAgentInstaller
{
public:
	static FAgentInstaller& Get();

	// Binary permissions (used by registry binary installs)
	static bool EnsureNativeAdapterExecutable(const FString& BinaryPath);

	// Executable resolution (for base CLIs like claude, codex, cursor)
	TArray<FString> GetExtendedPaths() const;
	bool ResolveExecutable(const FString& ExecutableName, FString& OutResolvedPath) const;
	bool ResolveExecutableViaLoginShell(const FString& ExecutableName, FString& OutResolvedPath) const;

	// ── Registry-based Installation ─────────────────────────────────

	/** Install method preference for registry agents */
	enum class ERegistryInstallMethod : uint8
	{
		Binary,  // Download platform-specific archive
		Npx,     // Use npx (requires Node.js)
		Uvx,     // Use uvx (requires uv/Python)
		Auto,    // Best available: Binary > Npx > Uvx
	};

	/** Status of a registry agent's installation */
	struct FRegistryInstallStatus
	{
		FString AgentId;
		FString InstalledVersion;
		FString InstalledPath;   // Path to binary or resolved command
		ERegistryInstallMethod Method = ERegistryInstallMethod::Auto;
		bool bIsInstalled = false;
		bool bUpdateAvailable = false; // Registry version > installed version
	};

	/** Install directory for registry agents: ~/.agentintegrationkit/agents/ */
	static FString GetManagedAgentsDir();

	/** Get the install directory for a specific agent version */
	static FString GetAgentInstallDir(const FString& AgentId, const FString& Version);

	/** Get the install directory based on archive URL hash (Zed pattern) */
	static FString GetAgentVersionDir(const FString& AgentId, const FString& ArchiveUrl);

	/** Check if an agent binary has been downloaded and extracted */
	static bool IsAgentBinaryExtracted(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd);

	/** Get the resolved executable path for an extracted agent binary */
	static FString GetExtractedAgentExecutable(const FString& AgentId, const FString& ArchiveUrl, const FString& Cmd);

	/** Check install status for a registry agent */
	FRegistryInstallStatus GetRegistryInstallStatus(const FACPRegistryAgent& Agent) const;

	/** Install a registry agent (async — downloads archive, extracts, sets permissions) */
	void InstallRegistryAgentAsync(
		const FACPRegistryAgent& Agent,
		ERegistryInstallMethod PreferredMethod,
		FOnInstallProgress OnProgress,
		FOnInstallComplete OnComplete
	);

	/** Uninstall a registry agent (removes downloaded binaries) */
	bool UninstallRegistryAgent(const FString& AgentId);

	/**
	 * Extract a .zip / .tar.gz / .tar.bz2 archive into DestDir using the platform's
	 * built-in extractor (Expand-Archive on Windows, tar/unzip elsewhere).
	 * Exposed so FNodeRuntime can reuse the same dispatch instead of duplicating it.
	 */
	bool ExtractArchivePublic(const FString& ArchivePath, const FString& DestDir, FString& OutError)
	{
		return ExtractArchive(ArchivePath, DestDir, OutError);
	}

private:
	FAgentInstaller() = default;

	FString GetLoginShellPath() const;
	FString BuildShellCommand(const FString& Command) const;

	// Registry install helpers
	void RunRegistryInstallOnBackgroundThread(
		const FACPRegistryAgent& Agent,
		ERegistryInstallMethod Method,
		FOnInstallProgress OnProgress,
		FOnInstallComplete OnComplete
	);
	bool DownloadAndExtractBinary(const FACPRegistryBinaryTarget& Target, const FString& AgentId, const FString& Version, FOnInstallProgress OnProgress, FString& OutError);
	bool ExtractArchive(const FString& ArchivePath, const FString& DestDir, FString& OutError);
	bool WriteInstallManifest(const FString& AgentId, const FString& Version, ERegistryInstallMethod Method, const FString& Cmd);
public:
	bool ReadInstallManifest(const FString& AgentId, FString& OutVersion, FString& OutCmd) const;
private:

	mutable FCriticalSection CacheLock;
	mutable TMap<FString, FString> ResolvedPathCache;
	mutable FDateTime LastCacheRefresh;
	static constexpr double CacheTTLSeconds = 300.0;
};
