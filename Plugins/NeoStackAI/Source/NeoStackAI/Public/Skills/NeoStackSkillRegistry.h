// Copyright 2026 Betide Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Skills/NeoStackSkill.h"

/**
 * In-memory catalog of every Agent Skill shipped by the NeoStack core plugin and its
 * enabled extensions. Populated by scanning `<PluginRoot>/Resources/Skills/<name>/SKILL.md`
 * for each NeoStack plugin the engine has enabled.
 *
 * This is the *source* side of the skill system. The `FNeoStackSkillInstaller` (landing
 * in a later step) takes this snapshot and materialises the files into
 * `<ProjectDir>/.claude/skills/` and `<ProjectDir>/.agents/skills/` so running ACP
 * agents pick them up through their normal filesystem discovery.
 */
class NEOSTACKAI_API FNeoStackSkillRegistry
{
public:
	static FNeoStackSkillRegistry& Get();

	/** Wipe any existing entries and re-scan every enabled NeoStack plugin.
	 *  Safe to call repeatedly (after extension install/uninstall, for example). */
	void Refresh();

	/** Snapshot of all currently-known skills, ordered by `SourceId` then `Name`. */
	TArray<FNeoStackSkill> GetAll() const;

	/** Skills contributed by one source (`"neostack.core"`, `"neostack.chaosfracture"`…). */
	TArray<FNeoStackSkill> GetBySource(const FString& SourceId) const;

	/** Lookup a single skill by its frontmatter `name`. Returns false if not found. */
	bool FindByName(const FString& SkillName, FNeoStackSkill& OutSkill) const;

private:
	FNeoStackSkillRegistry() = default;

	mutable FCriticalSection Lock;
	TArray<FNeoStackSkill> Skills;
};
