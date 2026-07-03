# Meshy for Unreal — plugin (internal dev repo)

Editor plugin for the Meshy DCC bridge: receives models from the Meshy web app
(`Send to Unreal`) over a local HTTP server (port `5327`), imports the FBX, and
rebuilds channel-correct PBR materials from the per-channel maps shipped in the
FBX zip (MES-12780).

> This is an **internal development repository** (`meshy-dev/meshy-unreal-plugin`).
> This README documents the repo conventions, build, and release process for
> maintainers. End users only consume the published GitHub Release assets.

---

## Repository layout

```
meshy.uplugin                 Plugin descriptor (VersionName, EngineVersion set per release)
Source/meshy/                 Plugin module (canonical, targets the latest engine)
  Private/MeshyBridge.cpp       HTTP server, import, PBR material rebuild
  meshy.Build.cs                module rules (zlib/minizip, editor deps)
Content/Materials/M_MeshyPBR   master PBR material — REQUIRED at runtime
Resources/                    plugin icons
Config/FilterPlugin.ini       packaging filter
Tools/generate_meshy_master_material.py   regenerates M_MeshyPBR in-editor
Build/ue5.4-overlay/          UE 5.4-only build overlay (see its README)
Scripts/build-windows-release.ps1   local multi-version Windows build + packaging
.github/workflows/release.yml workflow_dispatch: creates the Release + source zip
```

`Binaries/` and `Intermediate/` are **not committed** (see `.gitignore`). Binaries
ship only as Release assets, built locally per engine version.

---

## Supported engine versions & toolchain matrix

The canonical source targets the **latest** engine. Windows prebuilts are provided
for **UE 5.4–5.7**. Compiling locally requires a matching MSVC toolchain — the
constraints differ per engine version:

| Engine | MSVC requirement | Notes |
|--------|------------------|-------|
| 5.4 | **≤ 14.39** (use 14.38.33130) | 14.40+ trips `C4067` on the engine's `__has_feature` line under the C++20 conformant preprocessor. **Deprecated** — kept as a prebuilt convenience only. |
| 5.5 | 14.38+ | — |
| 5.6 | 14.38+ | — |
| 5.7 | 14.38 **or** ≥ 14.44 (14.40–14.43 are **banned** by UBT) | — |

**MSVC 14.38.33130 satisfies all four**, so the build script pins it for every
version (via a temporary user-level `BuildConfiguration.xml`, restored afterwards).
The engine source is never modified. Install the toolchain via the VS Installer
component `MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.38-17.8)`.

UE 5.4 also needs `Build/ue5.4-overlay/` (bundled minizip headers + a Build.cs
fallback + a `SkinnedAssetCommon.h` include) — applied automatically by the build
script to a temp copy only. The canonical source stays clean. See that folder's README.

> ⚠️ **Build under an ASCII path.** MSVC/UBT fails (`Cannot open source file`) if the
> build path contains non-ASCII characters (e.g. a Chinese Windows username in
> `%TEMP%`). The build script builds under the repo drive root for this reason.

---

## Branching & PR conventions

- Feature branches: `<author>/<ticket>`, e.g. `arliexubindu/mes-12780`. PRs target `main`.
- The `meshy-dev` org enforces **SAML SSO**: HTTPS push returns 403. Push over **SSH**:
  `git push git@github.com:meshy-dev/meshy-unreal-plugin.git <branch>`.
- Don't commit build output (`Binaries/`, `Intermediate/`).

---

## Release process

Unreal cannot be compiled on GitHub-hosted runners, so this is a **hybrid** flow:
Windows binaries are built **locally**; GitHub Actions creates the Release and the
cross-platform **source** package.

1. **Bump version** in `meshy.uplugin` (`VersionName`) and merge the feature PR into `main`.
2. **Build Windows binaries locally** (on a machine with UE 5.4–5.7 + MSVC 14.38):
   ```powershell
   .\Scripts\build-windows-release.ps1 -Version 0.2.0
   ```
   Produces, in `Build/release-out/`:
   - `meshy-for-unreal-ue5.4-0.2.0.zip`
   - `meshy-for-unreal-ue5.5-0.2.0.zip`
   - `meshy-for-unreal-ue5.6-0.2.0.zip`
   - `meshy-for-unreal-ue5.7-0.2.0.zip`
3. **Run the `Release` workflow** (Actions → Release → Run workflow, from `main`,
   `version=0.2.0`). It creates tag `v0.2.0`, the GitHub Release, and
   `meshy-for-unreal-source-0.2.0.zip`.
4. **Attach the Windows binaries** to that release:
   ```powershell
   gh release upload v0.2.0 Build\release-out\meshy-for-unreal-ue5.*.zip
   ```
   (or pass `-PublishTag v0.2.0` to the build script in step 2, after the release exists).

### Release asset naming (required)

| Asset | Contents |
|-------|----------|
| `meshy-for-unreal-source-<ver>.zip` | Source (latest engine). For macOS and any unlisted engine — users build it themselves. |
| `meshy-for-unreal-ue5.4-<ver>.zip` … `meshy-for-unreal-ue5.7-<ver>.zip` | Official **Windows** prebuilt per engine version. |

Each zip has a top-level `meshy/` folder (drop straight into a project's `Plugins/`).
Windows zips include `meshy.uplugin` (with the matching `EngineVersion`),
`Binaries/Win64/{UnrealEditor-meshy.dll, UnrealEditor.modules}`,
`Content/Materials/M_MeshyPBR.uasset`, `Resources/`, and `Source/` (no `.pdb`,
no `Intermediate/`). The DLL's BuildId is engine-version-specific — a zip only
works in its matching engine version.

> The `M_MeshyPBR` master material is **required**: without it the plugin logs
> "master material not found" and falls back to the imported materials (no PBR fix).

---

## Platform support

- **Windows**: official prebuilt binaries per engine version (above).
- **macOS**: ship the source package; users build it themselves (UE 5.5+; clang
  avoids the 5.4 `__has_feature` issue). 5.4 is not supported on macOS.
