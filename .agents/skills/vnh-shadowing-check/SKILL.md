---
name: vnh-shadowing-check
description: Mandatory VNHSimulator C++ handoff guard for detecting variable, parameter, and helper-name shadowing before final responses. Use whenever Codex edits or reviews VNHSimulator C++ source/header files, fixes build/package errors, or changes Unreal native classes, especially when adding parameters, locals, anonymous-namespace helpers, UPROPERTY-backed widget fields, or unity-build-sensitive helpers.
---

# VNH Shadowing Check

Before finishing any VNHSimulator C++ edit, perform a shadowing pass.

## Required Checks

1. Inspect new or renamed parameters/local variables against class members in the same class.
   - Watch for names matching `UPROPERTY` fields such as `StatusText`, `LobbyMenuWidget`, `PlayerState`, widget pointers, and replicated state.
   - Prefer descriptive parameter names like `FallbackStatusText`, `NewStatusText`, `InServerName`, or `RequestedRole`.

2. Check anonymous-namespace helpers in touched `.cpp` files for unity-build collisions.
   - Search the game module for helper names before finalizing:
     ```powershell
     rg -n "HelperName|FunctionName" Source\VNHSimulator
     ```
   - If a helper may appear in another `.cpp`, give it a file-specific name, e.g. `EncodeBrowserTravelOption`.

3. Run a focused shadowing text scan on touched C++ files.
   - At minimum search for suspicious signatures and declarations:
     ```powershell
     rg -n "const FText& StatusText|FText& StatusText|const FString& [A-Z][A-Za-z0-9_]*|\\b[A-Za-z0-9_]+\\s+[A-Za-z0-9_]+\\s*=\\s*" Source\VNHSimulator\Private Source\VNHSimulator\Public
     ```
   - Review hits only in files you touched or directly affected.

4. Treat user build output with `C4458`, `C4456`, or duplicate anonymous-namespace helper errors as actionable.
   - Fix by renaming the local/parameter/helper; do not disable warnings.

5. In the final response, state whether the shadowing scan was run. If it was skipped, say why.

## Notes

- Unreal packaging may use unity builds, so anonymous namespaces do not protect duplicate helper names across concatenated `.cpp` files.
- `git diff --check` is not a shadowing check; run it as a separate hygiene check when useful.
