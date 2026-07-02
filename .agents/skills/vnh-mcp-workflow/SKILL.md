---
name: vnh-mcp-workflow
description: Disciplined Unreal MCP workflow for Very Normal Human Simulator. Use when Codex works in this project through the Unreal Editor MCP or NeoStack Lua tools, especially for asset edits, Widget Blueprints, Blueprints, runtime C++ integration, role HUDs, mark/accusation UI, or any request that says no guesswork, stay disciplined, stay efficient, only modify what is needed, or use best practices.
---

# VNH MCP Workflow

Use this skill for all Unreal MCP work in this project.

## Operating Rules

1. Discover before acting. Call `help()` or the relevant `help("Domain")`, then inspect existing assets with `open_asset`, `info`, `widget_info`, `list_widgets`, `read_graph`, `find_assets`, `get_referencers`, or source search.
2. Read the existing runtime path before modifying assets. For UI, find who creates the widget, which widget names C++/Blueprint code looks up, and what visibility/update logic already exists.
3. Reuse existing assets and logic. Change the current asset or runtime connection when that is the direct fix; do not create parallel systems unless the request explicitly requires separation.
4. Preserve runtime contract names. Do not rename widgets, variables, functions, or assets that C++/Blueprint code looks up by string unless the lookup is updated in the same change.
5. Keep edits scoped. Avoid broad refactors, unrelated styling churn, or metadata-only changes outside the requested area.
6. Never delete assets, graph nodes, or components without explicit confirmation. Removing/rebuilding disposable widget-tree children is acceptable when the requested UI redesign requires it, but preserve named runtime hooks.
7. After Blueprint or Widget Blueprint edits, compile, save, then re-open/re-read the asset to verify the tree or graph.
8. After C++ edits, run the narrowest practical build or compile check. If the toolchain is unavailable, report that clearly.
9. If an MCP tool cannot perform a required operation or contradicts its docs, call `report_issue` before telling the user it is a tool limitation.

## UI Workflow

1. Inspect the widget asset and any referenced widgets.
2. Search C++ and Blueprint logic for `CreateWidget`, `LoadClass`, `GetWidgetFromName`, `AddToViewport`, and the target widget path/name.
3. Identify which fields are static layout versus runtime-driven labels or panels.
4. Modify only the target widget tree and the smallest runtime integration needed.
5. Use stable UMG structure: `CanvasPanel` root, fixed-size widgets through `SizeBox`, layered card backgrounds through `Overlay`, and text in `TextBlock` children.
6. Keep role-specific HUDs separate: Hunter, Human, and Alien widgets should be distinct assets even if Human/Alien start from a shared baseline.
7. For Hunter HUD work, keep the marked humans panel gated by the existing mark logic; do not add a second mark-list system.

## Verification Checklist

- Asset exists at the intended `/Game/...` path.
- Widget tree contains the runtime hook names expected by code.
- Widget compiles and saves successfully.
- Source changes compile or a clear build blocker is reported.
- Final response names changed assets/files and states what was verified.
