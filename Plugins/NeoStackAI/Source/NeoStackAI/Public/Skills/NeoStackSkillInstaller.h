// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * One installed copy of a skill on disk, along with how it currently stands relative
 * to what we shipped. Populated by `FNeoStackSkillInstaller::GetStatus()` for the UI.
 */
struct NEOSTACKAI_API FNeoStackSkillStatus
{
	FString Name;
	FString SourceId;
	FString SourceDisplayName;
	FString SourceVersion;
	FString Description;
	TArray<FString> Tags;

	/** Absolute paths to the installed copies (one per target dir). Only entries that
	 *  actually exist on disk are included. */
	TArray<FString> InstalledPaths;

	/** Absolute path to the `SKILL.new.md` conflict sibling, if we dropped one. Empty
	 *  when no conflict is pending. */
	FString ConflictNewPath;

	/** True when the user has locally edited at least one installed copy (digest drifted
	 *  from what we last wrote). */
	bool bUserEdited = false;

	/** True when we detected an upstream update we could not apply because the user had
	 *  edited the file — the new version is parked at `ConflictNewPath`. */
	bool bConflictPending = false;
};

/** One user-authored skill found on disk in the project skill dirs but not tracked in
 *  the install manifest. Populated by `FNeoStackSkillInstaller::GetProjectSkills()`. */
struct NEOSTACKAI_API FNeoStackProjectSkillStatus
{
	FString Name;
	/** Directory name on disk — used to locate SKILL.md for Open-in-editor. */
	FString FolderName;
	FString Description;
	TArray<FString> Tags;

	/** Absolute paths to each installed SKILL.md (.claude and/or .agents). */
	TArray<FString> Paths;

	/** Non-empty when SKILL.md exists but frontmatter parsing failed. */
	FString ParseError;
};

struct NEOSTACKAI_API FNeoStackSkillSyncReport
{
	int32 Installed = 0;       // first-time writes for a given (skill, target)
	int32 Updated = 0;         // upstream changes applied over a clean install
	int32 NoOp = 0;            // clean install, no upstream change
	int32 UserEditsKept = 0;   // user-edited, no upstream change, left alone
	int32 Conflicts = 0;       // user-edited AND upstream changed → SKILL.new.md dropped
	int32 OrphansRemoved = 0;  // source skill gone, clean install deleted
	int32 OrphansKept = 0;     // source skill gone, user-edited, file preserved
	TArray<FString> Errors;    // non-fatal per-skill write/delete failures
};

/**
 * Copies the skill files discovered by `FNeoStackSkillRegistry` from each plugin's
 * shipping `Resources/Skills/<name>/SKILL.md` into the per-project dirs where Claude
 * Code and Codex CLI expect to find them:
 *
 *   <ProjectDir>/.claude/skills/<name>/SKILL.md
 *   <ProjectDir>/.agents/skills/<name>/SKILL.md
 *
 * A manifest at `<ProjectDir>/.neostack/skills-manifest.json` records per-skill
 * digests so we can distinguish between:
 *   - a clean install that needs updating (we overwrite),
 *   - a user-edited file (we preserve it), and
 *   - a user-edited file whose upstream has *also* changed (we drop the new version
 *     as `SKILL.new.md` beside the user's copy for manual review).
 *
 * User-authored skills that have no manifest entry are never touched.
 */
class NEOSTACKAI_API FNeoStackSkillInstaller
{
public:
	/** Enumerate registry state and reconcile the on-disk target dirs with it.
	 *  Safe to call repeatedly and cheap — reads + writes a handful of small files. */
	static FNeoStackSkillSyncReport SyncProject();

	/** One row per skill the registry knows about (including orphan-manifest entries
	 *  whose source plugin was uninstalled). Used by the WebUI panel. */
	static TArray<FNeoStackSkillStatus> GetStatus();

	/** Scan `.claude/skills/` and `.agents/skills/` for user-authored skills that have
	 *  no manifest entry. Read-only — never writes or modifies files. */
	static TArray<FNeoStackProjectSkillStatus> GetProjectSkills();

	/** Load the markdown body of a skill for preview in the UI.
	 *  Reads the installed `.claude/skills/<name>/SKILL.md` — falls back to the shipped
	 *  source file if nothing is installed yet. Returns empty on miss. */
	static FString ReadSkillBody(const FString& SkillName);

	/** Load the upstream `SKILL.new.md` body (the version waiting on conflict resolution).
	 *  Empty string if no conflict is pending for this skill. */
	static FString ReadConflictBody(const FString& SkillName);

	/** Resolve a pending conflict for one skill. Mode:
	 *    "keep-user"  — discard the `SKILL.new.md`, keep the user's edited SKILL.md
	 *                   and pin the manifest to the new upstream digest (so further
	 *                   upstream changes will re-trigger conflicts).
	 *    "take-new"   — overwrite the user's SKILL.md with the `SKILL.new.md` contents,
	 *                   delete the `.new.md`, update manifest.
	 *  Returns true on success; false if the skill has no pending conflict. */
	static bool ResolveConflict(const FString& SkillName, const FString& Mode);

	/** Absolute path of the project root manifest file, for diagnostics/logging. */
	static FString GetManifestPath();
};
