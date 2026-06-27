// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One Agent Skill, parsed from a `SKILL.md` file shipped inside a plugin's
 * `Resources/Skills/<name>/` directory. Mirrors the open agentskills.io format
 * (YAML frontmatter + markdown body) used by Claude Code and Codex CLI.
 */
struct NEOSTACKAI_API FNeoStackSkill
{
	/** Directory name on disk and the identifier the agent sees. */
	FString Name;

	/** Short description from frontmatter — front-loads the trigger phrase for the agent. */
	FString Description;

	/** Optional tags from frontmatter; free-form, used for grouping in the UI. */
	TArray<FString> Tags;

	/** Origin identifier — `"neostack.core"` for the core plugin, or the extension's
	 *  `ExtensionId` (e.g., `"neostack.chaosfracture"`) for skills shipped by extensions. */
	FString SourceId;

	/** Human-readable source label for the UI (`"NeoStack AI Core"`, `"Chaos Fracture"`…). */
	FString SourceDisplayName;

	/** Absolute path to the SKILL.md file on disk (inside the plugin's Resources/Skills/). */
	FString SkillMdAbsPath;

	/** Lowercase hex sha1 digest of the full SKILL.md file contents as shipped. Used by
	 *  the installer to detect drift between shipped and project-installed copies.
	 *  sha1 (not sha256) because UE 5.7's `FPlatformMisc::GetSHA256Signature` has no
	 *  Windows/Mac backend — see NeoStackExtensionInstallerFs.cpp for the workaround.
	 *  Collision resistance is not a concern here: we only need "did these bytes change". */
	FString ShippedDigest;

	/** Version string inherited from the shipping plugin/extension (`VersionName` field of
	 *  the `.uplugin`). Recorded in the install manifest so we know which release a given
	 *  installed skill came from. */
	FString SourceVersion;

	bool IsValid() const { return !Name.IsEmpty() && !SkillMdAbsPath.IsEmpty(); }
};

/**
 * Parse a single `SKILL.md` file into an `FNeoStackSkill`. Reads the file, strips the
 * `---`-delimited YAML frontmatter, extracts `name`/`description`/`tags`, and fills in
 * `ShippedDigest` from the raw bytes.
 *
 * `SourceId`, `SourceDisplayName`, and `SourceVersion` are NOT filled here — those are
 * set by the registry based on which plugin owns the skill file.
 *
 * Returns false if the file doesn't exist, the frontmatter is malformed, or `name` is
 * missing. On false, `OutSkill` is left in an unspecified state.
 */
NEOSTACKAI_API bool ParseSkillMdFile(const FString& AbsPath, FNeoStackSkill& OutSkill);
