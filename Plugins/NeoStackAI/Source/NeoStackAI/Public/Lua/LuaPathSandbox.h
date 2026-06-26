// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Shared filesystem-path sandbox for Lua bindings. Every binding that writes
// caller-supplied paths to disk (read_file/write_file, export_asset, screenshot,
// hlod_export, ndisplay_save_config, mrq_save_queue, replay_*, stringtable_export_csv,
// loc_export_po, etc.) should gate writes through IsWritePathAllowed() and reads
// through IsPathAllowed() so the sandbox is consistent across the plugin surface.

#pragma once

#include "CoreMinimal.h"

namespace NeoLuaPath
{
	// Convert a possibly-relative path to absolute form, normalize separators,
	// and collapse .. segments. Returns the normalized absolute path.
	NEOSTACKAI_API FString NormalizePath(const FString& InPath);

	// Read-side policy: allows project dir, engine dir, and the user temp dir.
	// Returns false if, after normalization, `..` segments remain (symlink trick).
	NEOSTACKAI_API bool IsPathAllowed(const FString& AbsPath);

	// Write-side policy: allows project dir and user temp dir only.
	// Engine dir is intentionally *not* writable.
	NEOSTACKAI_API bool IsWritePathAllowed(const FString& AbsPath);
}
