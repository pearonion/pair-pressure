// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaPathSandbox.h"

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace NeoLuaPath
{
	FString NormalizePath(const FString& InPath)
	{
		FString Normalized = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Normalized);
		FPaths::CollapseRelativeDirectories(Normalized);
		return Normalized;
	}

	bool IsPathAllowed(const FString& AbsPath)
	{
		FString Normalized = NormalizePath(AbsPath);

		if (Normalized.Contains(TEXT("..")))
			return false;

		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString EngineDir  = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

		if (Normalized.StartsWith(ProjectDir)) return true;
		if (Normalized.StartsWith(EngineDir))  return true;

		const FString TempDir = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
		if (Normalized.StartsWith(TempDir)) return true;

		return false;
	}

	bool IsWritePathAllowed(const FString& AbsPath)
	{
		FString Normalized = NormalizePath(AbsPath);

		if (Normalized.Contains(TEXT("..")))
			return false;

		const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (Normalized.StartsWith(ProjectDir)) return true;

		const FString TempDir = FPaths::ConvertRelativePathToFull(FPlatformProcess::UserTempDir());
		if (Normalized.StartsWith(TempDir)) return true;

		return false;
	}
}
