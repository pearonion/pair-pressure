# Character Customizer, Locomotion, and Camera Progress

Date: 2026-07-04

## Goals

- Replace idle-only sliding with real character locomotion animation.
- Prevent other shoppers from forcing the third-person spring arm forward.
- Use GmRapidThumbnailCreator to produce real customizer item icons.
- Preserve a detailed record suitable for Discord development updates.

## Investigation

### Locomotion

- `AVNHShopperCharacter::ConfigureCreativeCharacterVisuals()` already attempted to load:
  `/Game/Creative_Characters/Animations/ABP_CreativeCharacter`.
- That Anim Blueprint did not exist.
- The existing failure path played only `ANIM_TNG_Idle_Breathing` as a single-node animation.
- Character movement continued normally, so moving characters visibly slid in the idle pose.
- The Creative character body and locomotion sequences use the same 44-bone Creative skeleton.

### Third-Person Camera

- `FollowCameraBoom` retains normal spring-arm collision testing on the `Camera` trace channel.
- Other shopper capsules blocked that channel and shortened the boom when they passed behind the player.
- Disabling spring-arm collision globally would also remove useful wall collision.
- The targeted correction is for shopper capsules to ignore `ECC_Camera`.

### Customizer Thumbnails

- `DT_VNHCustomizationItems` contains 391 icon paths.
- No texture assets existed under `/Game/UI/CharacterCustomizer/Thumbnails`.
- The paths were therefore dangling references rather than usable icons.
- GmRapidThumbnailCreator supports skeletal mesh capture and texture creation.
- Its stock save path displayed a confirmation modal after every item and assumed an editor widget was present.
- The plugin referenced `/GmRapidThumbnailCreator/Materials/RT/RT_ThumbnailCreatorPNG`, but that render-target asset was absent from the installed plugin.
- The missing target left `RenderTarget2D_PNG` null and caused the first scripted `SaveStaticTexture` call to fail.

## Implemented

### Locomotion Assets

- Created `/Game/Creative_Characters/Animations/ABP_CreativeCharacter`.
- Created `/Game/Creative_Characters/Animations/BS_CreativeLocomotion`.
- Added a `Speed` variable updated from the owning pawn's planar velocity.
- Added blend-space samples:
  - Idle at `0`
  - Walk at `135`
  - Run at `300`
- Compiled and saved the Anim Blueprint.
- Verification: `UpToDate`, zero Blueprint errors, zero Blueprint warnings.

### Camera Collision

- Updated `VNHShopperCharacter.cpp`.
- Shopper capsules now ignore `ECC_Camera`.
- Spring-arm world collision remains enabled.
- Live Compile initially caught a warnings-as-errors shadowing issue.
- Renamed the local capsule pointer to `ShopperCapsule`.
- Verification: subsequent Live Compile succeeded.

### Thumbnail Plugin Automation

- Updated GmRapidThumbnailCreator for optional headless scripted capture.
- Added `bSuppressSaveConfirmation`; interactive behavior remains the default.
- Added null-safe behavior when no editor utility widget is attached.
- PNG captures now use `TC_EditorIcon`, no mipmaps, and sRGB.
- Restored the plugin's missing PNG render target at its expected path using the supplied EXR target as the non-destructive source.
- Configured the PNG target as `RTF_RGBA8_SRGB` with a dark clear color.
- Added an explicit scripted fallback assignment because the plugin module had cached the previously missing asset for the current editor session.
- Updated `Scripts/generate_customizer_thumbnails.py` to prefer the plugin backend.
- Retained the existing VNH SceneCapture backend as a fallback.
- Python syntax validation passed.
- Verification: plugin and VNH modules Live Compiled successfully.

### Thumbnail Capture Validation

- Generated `/Game/UI/CharacterCustomizer/Thumbnails/Body/T_SK_Body_001`.
- Generated a five-item body batch covering `SK_Body_001` through `SK_Body_005`.
- Verified all five textures are:
  - 512x512
  - `TC_EditorIcon`
  - `TEXTUREGROUP_UI`
  - No mipmaps
  - Clamp addressing
  - sRGB
- Reused one capture actor for the batch rather than spawning one actor per item.
- Removed all temporary capture actors after generation.
- Verification: zero `VNH_TempRapidThumbnailCreator` actors remain in the level.
- Visual QA proved those first five textures were blank despite having valid texture metadata.
- Traced the headless capture failures to three separate integration issues:
  - Reflected boolean names were incorrect in Unreal Python.
  - The plugin editor utility normally enables its lights, but the scripted path did not.
  - The plugin Blueprint mixes world-space bounds into relative offsets and therefore must capture at world origin.
- Corrected headless property assignment with reflected-name fallbacks.
- Enabled three point lights with controlled intensities.
- Moved the isolated show-only capture stage to world origin.
- Added a 20 percent framing margin.
- Added final vertical normalization from the rendered component's world bounds.
- Visually checked one item from every category:
  - Accessory
  - Body
  - Face
  - Hair
  - Hat
  - Mustache/Beard
  - Outfit
  - Outwear
  - Pants
  - Shoes
- Beard/mustache clipping was found during the first cross-category pass and passed after bounds-center normalization.

### Full Catalog Generation

- Generated 386 missing catalog textures with GmRapidThumbnailCreator.
- Skipped the five existing first-pass body textures rather than overwriting them.
- Generation report:
  - Rows seen: 391
  - Generated: 386
  - Existing: 5
  - Missing mesh: 0
  - Failed: 0
- Verified all 391 DataTable rows now resolve to 391 unique texture assets.
- Verified all 391 textures use:
  - `TC_EditorIcon`
  - `TEXTUREGROUP_UI`
  - No mipmaps
  - sRGB
- The 60-second MCP response timeout left four generator actors in the open map after the batch completed.
- Removed only those four actors by their exact generated names.
- Verification: zero temporary generator actors remain.

## Pending Verification

- Replace the five known-blank body textures generated before the capture corrections.
- Remove the 15 validation-only texture assets after explicit approval.
- Manually verify locomotion and camera behavior in a developer-run play session.

## Discord Draft

### Character System Progress

We tracked down why the new shoppers were sliding around in an idle pose: the runtime was already looking for a locomotion Anim Blueprint, but the asset did not exist, so every character fell back to one looping idle animation.

The new locomotion setup now includes:

- A dedicated Creative character Anim Blueprint
- Idle, walk, and run blending driven by real movement speed
- Existing character movement and customization paths left intact

We also fixed the third-person camera reacting to shoppers walking behind the player. Shopper capsules now ignore the camera trace channel while walls still protect the camera normally.

For the DRIP CHECK customizer, the thumbnail pipeline is now connected to GmRapidThumbnailCreator. We repaired the plugin's missing PNG render target, added non-modal scripted capture, enabled deterministic lighting, corrected its off-origin bounds bug, and added consistent framing and centering.

We visually tested every customization category and caught a beard crop before the full run. After fixing it, the catalog pass generated 386 new icons with zero failures. All 391 DataTable icon paths now resolve to unique 512x512 UI textures with editor-icon compression, no mipmaps, and sRGB.

The first five body icons came from the earlier blank capture pass and still need an approved replacement. Locomotion and the shopper-safe spring-arm collision change are also ready for an in-game developer verification pass.
