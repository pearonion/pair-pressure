---
name: vnh-project-dev
description: Disciplined project-specific Unreal Engine development guidance for VNHSimulator. Use when Codex works on VNHSimulator gameplay, UMG widgets, Creative_Characters customization, round/pre-round flow, composure, debug HUD/test deck, map assets, repository pulls/pushes, or any potentially destructive asset/code changes.
---

# VNH Project Dev

## Core Rule

Before changing VNHSimulator, read `references/project-knowledge.md`. Treat it as the current project briefing and update it when you discover durable facts.

Work like a senior UE developer:
- Read existing code/assets before editing.
- Use NeoStack/Unreal discovery (`help`, `open_asset`, `widget_info`, `info`, `find_assets`, `find_nodes`) instead of guessing.
- Prefer small targeted changes over parallel replacement systems.
- Do not delete, overwrite, move, rename, or revert assets without explicit user approval.
- If a destructive action seems useful, explain the risk and ask the live developer first.
- Do not run `Build.bat` while the editor is open unless the user explicitly asks.
- Do not start PIE/playtests unless the user explicitly asks; this project has previously hit PIE cleanup crashes.

## Required Workflow

1. Inspect git status and identify unrelated local work.
2. Read the relevant source and UE assets before modifying them.
3. Trace behavior from input/event to outcome; fix the actual constraint, not a symptom.
4. Explain the finding before making risky or broad edits.
5. Make scoped edits only.
6. Compile/save changed Widget Blueprints through the editor.
7. For C++ edits, ask the user to Live Compile and use their compile output for fixes.
8. Verify with source scans, compile logs, widget screenshots, or asset re-reads.
9. Report exactly what changed and what still needs manual Live Compile/testing.

## Mandatory Commit and Pull Safety

- Before committing or pushing, compare `git diff --cached --name-status` with every implementation/status claim. Do not commit documentation that claims assets, Blueprint wiring, tests, or runtime integration absent from the staged paths.
- Resolve every new hard or soft `/Game/...` dependency to a staged or already tracked asset. If a required asset is missing, preserve the existing production path and keep the replacement opt-in or dormant.
- When replacing a HUD/widget/controller path, trace all consumers of the old widget pointer, named children, interfaces, and update functions. Do not remove or stop assigning an established pointer until every consumer has a verified replacement.
- Treat untracked `.uasset`, `.umap`, `Config`, and project documentation as user work. Before any reset/clean/overwrite pull, list them explicitly and create a recoverable backup or stash including untracked files unless the user explicitly rejects preservation after seeing the list.
- Never use a successful native build as proof that referenced Content assets or Blueprint bridges exist. Verify staged Content paths, Blueprint compile/save state, and runtime initialization independently.
- Exclude generated conflict copies, manifests, solution GUID churn, and other tooling noise unless the user explicitly identifies them as deliverables.

## VNH-Specific Guardrails

- Main project code lives in `Source/VNHSimulator`; avoid editing `Plugins/NeoStackAI` unless the task is explicitly about the plugin.
- UMG customizer lives at `/Game/UI/WBP_VNHCharacterCustomizer` and is backed by `UVNHCharacterCustomizerWidget`.
- The composure HUD lives at `/Game/UI/WBP_VNHComposure`; only show its customize button during pre-round phases.
- Character customization data is `FVNHCharacterCustomization` in `VNHGameplayTypes.h`.
- Creative character meshes live primarily under `/Game/Creative_Characters/Skeleton_Meshes`.
- Pre-round customization is allowed only in `WaitingForPlayers` and `AssigningRoles`; it must close once the active round starts.
- Keep the debug/test deck separate from customizer UX. If key bindings do not work, inspect both `Config/DefaultInput.ini` action mappings and direct `BindKey` code paths.
- Do not replace `WBP_PP_HUD_Root` or stop assigning `PairPressureHUDWidget` until the replacement includes its tracked config/UMG assets and preserves adaptive controller prompt consumers.

## Reference

Read `references/project-knowledge.md` for:
- asset paths and map names,
- customization categories and mesh naming buckets,
- round/composure/debug HUD behavior,
- known pitfalls,
- current implementation notes.
