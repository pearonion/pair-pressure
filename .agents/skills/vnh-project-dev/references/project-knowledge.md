# VNHSimulator Project Knowledge

Last updated: 2026-07-11.

## Project Shape

- Unreal Engine project root: `A:/Unreal Projects/VNHSimulator`.
- Game module: `Source/VNHSimulator`.
- Avoid changing `Plugins/NeoStackAI` unless the task explicitly concerns the Agent Integration Kit tooling.
- Main content folders observed: `/Game/AI`, `/Game/Blueprints`, `/Game/Characters`, `/Game/ClothingStore`, `/Game/Creative_Characters`, `/Game/Input`, `/Game/Maps`, `/Game/Sounds`, `/Game/TNG`, `/Game/TryingOnClothesAnim`, `/Game/UI`.
- Important maps known from code/context: `MainMenu`, `Lobby`, `MVP_ClothingStore`, and `/Game/Creative_Characters/Maps/Demonstration`.

## Source Responsibilities

- `VNHGameplayTypes.h`: shared enums/structs including `EVNHRoundPhase`, `EVNHPlayerRole`, `EVNHCustomizationSlot`, and `FVNHCharacterCustomization`.
- `VNHGameMode.*`: round phase progression and `FVNHPhaseTiming`. Pre-round customization timing defaults to 30 seconds.
- `VNHGameState.*`: replicated round phase/current round state.
- `VNHPlayerState.*`: role and pre-round ready state.
- `VNHPlayerController.*`: input bindings, debug HUD toggle, pre-round customizer flow, composure widget creation/update, server RPCs for customization/ready.
- `VNHGameInstance.*`: main menu/customizer widget lifetime, customization profile, asset discovery, preset selection, randomization, slot cycling.
- `VNHShopperCharacter.*`: player/NPC character body/cosmetic components, composure system, customization application, replicated `FVNHCharacterCustomization`.
- `VNHCharacterCustomizerWidget.*`: C++ parent for `/Game/UI/WBP_VNHCharacterCustomizer`.
- `VNHDebugHUD.*`: test deck/debug panels.

## Round Flow

`EVNHRoundPhase` values:
- `WaitingForPlayers`
- `AssigningRoles`
- `AlienSetup`
- `AlienHeadStart`
- `Investigation`
- `Accusation`
- `Reveal`
- `Resetting`

Current customizer rules:
- In-game customization should be allowed only during `WaitingForPlayers` and `AssigningRoles`.
- The customizer must close when leaving pre-round phases.
- Composure loss should be locked during `WaitingForPlayers` and `AssigningRoles`.
- The composure HUD customize button should be visible only while a local shopper pawn exists and the round is in a customization phase.

## Character Customization Data

`FVNHCharacterCustomization` fields:
- `PresetIndex`
- `BodyMesh`
- `HairMesh`
- `FaceMesh`
- `HatMesh`
- `MustacheMesh`
- `OutfitMesh`
- `OutwearMesh`
- `PantsMesh`
- `ShoesMesh`
- `AccessoryMesh`
- `BodyColor`
- `HairColor`
- `OutfitColor`
- `bNoFace`
- `Nickname`

`FVNHCustomizationItem` is the MVP DataTable row struct for curated customizer content:
- `Category`
- `DisplayName`
- `Mesh`
- `Icon`
- `SortOrder`
- `bEnabled`

Expected DataTable path:
- `/Game/UI/Customizer/DT_VNHCustomizationItems`

DataTable state on 2026-07-01:
- `/Game/UI/Customizer/DT_VNHCustomizationItems` exists and uses row struct `VNHCustomizationItem`.
- It was seeded from `/Game/Creative_Characters/Skeleton_Meshes` with 391 enabled rows:
  - Body 16
  - Hair 26
  - Face 79
  - Hat 63
  - Mustache/Beard 18
  - Outfit/Costume 42
  - Outwear 60
  - Pants/Shorts 20
  - Shoes 21
  - Accessory 46
- The table was initially seeded without usable thumbnail assets. The `Icon` column now contains deterministic texture paths for every row.

Thumbnail state on 2026-07-04:
- All 391 DataTable rows contain unique icon paths under `/Game/UI/CharacterCustomizer/Thumbnails`.
- Texture assets now exist at all 391 configured paths.
- Verified catalog counts: Accessory 46, Body 16, Face 79, Hair 26, Hat 63, Mustache 18, Outfit 42, Outwear 60, Pants 20, Shoes 21.
- All 391 textures load with `TC_EditorIcon`, `TEXTUREGROUP_UI`, no mipmaps, and sRGB.
- The first five body textures were generated before the headless capture framing fix and are visually blank. They must be explicitly replaced before the catalog is considered finished.
- `Scripts/generate_customizer_thumbnails.py` prefers the `GmRapidThumbnailCreator` plugin as its capture backend and retains the VNH SceneCapture implementation as a fallback.
- The plugin's scripted path includes null-safe operation without an editor widget, optional save-confirmation suppression, explicit point-light setup, origin-based capture to match the plugin's Blueprint bounds calculations, 20 percent framing margin, bounds-center correction, and UI-icon compression.
- The plugin package was missing `/GmRapidThumbnailCreator/Materials/RT/RT_ThumbnailCreatorPNG`; the project copy now contains a 512-capable `RTF_RGBA8_SRGB` target at that expected path.
- A cross-category visual sample passed for body, accessory, face, hair, hat, outfit, outerwear, pants, and shoes after centering. Beard/mustache clipping was corrected by bounds-center normalization.
- Temporary generator actors use the exact label `VNH_TempRapidThumbnailCreator` and must be removed after interrupted or timed-out batches. Current verified count is zero.

GameInstance should prefer the DataTable when it exists and contains enabled rows for a category. If no rows exist for a category, it falls back to Creative_Characters asset discovery.
When DataTable rows exist, the runtime prepends a synthetic `None` option for every optional slot except `Body`. Selecting it passes an empty mesh path through the existing customization application path and removes that worn item.

Default body mesh currently points at:
- `/Game/Creative_Characters/Skeleton_Meshes/SK_Body_009.SK_Body_009`

Creative animation class:
- `/Game/Creative_Characters/Animations/ABP_CreativeCharacter.ABP_CreativeCharacter_C` exists and is assigned explicitly by `AVNHShopperCharacter::ConfigureCreativeCharacterVisuals` because the game mode spawns the native character class directly. If it cannot load, runtime falls back to the compatible single-node idle animation.

Runtime locomotion assets:
- `/Game/Creative_Characters/Animations/ABP_CreativeCharacter`
- `/Game/Creative_Characters/Animations/BS_CreativeLocomotion`
- The AnimGraph routes `BS_CreativeLocomotion` through `Slot 'DefaultSlot'` before `Output Pose`, allowing the role-action montages to override locomotion during playback.
- Jump animation is handled by the `LocomotionAndJump` state machine in `ABP_CreativeCharacter`: Locomotion -> `ANIM_Jump_Start` -> looping `ANIM_Jump_Loop` -> `ANIM_Jump_End` -> Locomotion. `IsInAir` is updated from CharacterMovement `IsFalling`, and the state-machine output remains routed through `DefaultSlot`.
- The three Creative Character jump sequences force-lock their root because CharacterMovement controls capsule translation.
- The Anim Blueprint updates `Speed` from the owning pawn's planar velocity and feeds a 1D blend space with idle at 0, walk at 135, and run at 300.
- The locomotion sequences enable root motion and force-lock the root to the reference pose so CharacterMovement controls translation without visual mesh drift.
- Before these assets existed, `VNHShopperCharacter` fell back to a single idle sequence, so moving characters appeared to slide.
- Shopper capsules ignore the `Camera` collision channel. The spring arm still probes world geometry, but other shoppers should no longer retract the camera.
- Hunter, Human, and Alien players all use the third-person follow camera and `ABP_CreativeCharacter`; Hunter role-action montages remain blocked by native role validation, while locomotion and jump animation remain available.

Role-action animation assets:
- `/Game/Data/DT_HumanActionAnimations` and `/Game/Data/DT_AlienActionAnimations` use `UAnimMontage` arrays for high/low/no composure, not raw `UAnimSequence` references.
- Compatible role-action montages live under `/Game/Animations/RoleActions` and target `/Game/Creative_Characters/Animations/SKEL_Skeleton`, the skeleton used by `/Game/Creative_Characters/Animations/SK_Animations`.
- Human/Alien Inspect HUD clicks and action-slot 1 always request `EVNHUniversalAction::Inspect`, matching Point, Wave, and Laugh; normal world interaction remains on the separate interact input.
- Hunter action slots are: 1 Mark, 2 Accuse, 3 Pressure, 4 Human Drill, 5 Everyone Point, 6 Fake Drill. Keyboard/controller slot dispatch and Hunter-specific hotkey widget suffixes use this same order. Hovered shoppers take priority over an older locked target; Accuse and Pressure use server RPCs, with Pressure applying an authoritative composure penalty.
- MVP interaction props are spawned in `MVP_ClothingStore` from `/Game/Interactions/BP_VNHProp_Box`, `Bag`, `Cup`, `Tool`, and `Suspicious`, then tagged `VNH.Interactable`. Only Human and Alien roles may carry them. `C` picks up the focused prop and attaches it to `hand_r`; held props are a replicated stack, and `B` drops the most recently picked-up prop with physics restored.
- Throwing is contextual on `Q`: with an empty hand it preserves Act Natural; while carrying a prop, press/release throws and holding up to 1.5 seconds charges velocity from 850 to 2400 uu/s. Human/Alien HUDs expose `ActionButton_Throw`, and both role-action DataTables contain a `Throw` row with high/low/no-composure arrays. Thrown prop hits select Front/Back/Left/Right knockdown rows from the target role table; Hunters use the Human table's high-composure array. `/Game/Interactions/BP_VNHProp_Football` is the current oval placeholder spawned in `MVP_ClothingStore`.

## Creative_Characters Mesh Taxonomy

Observed source folder:
- `/Game/Creative_Characters/Skeleton_Meshes`

Registry counts on 2026-07-01:
- Body: 16
- Hair: 26
- Face/emotions: 79
- Hat: 63
- Mustache/beard: 18
- Outfit/costume: 42
- Outwear: 60
- Pants/shorts: 20
- Shoes: 21
- Accessory: 46
- Other: 22

Recommended name bucket rules:
- `Body`: contains `Body`
- `Hair`: contains `Hair` or `Hairstyle`
- `Face`: contains `Emotion` or `Face`
- `Hat`: contains `Hat`, `Helmet`, `Cap`, or `Crown`
- `Mustache`: contains `Mustache` or `Beard`
- `Outfit`: contains `Costume` or `Outfit`
- `Outwear`: contains `Outwear`, `Jacket`, `Coat`, or `Hoodie`
- `Pants`: contains `Pants`, `Short`, or `Trouser`
- `Shoes`: contains `Shoe`, `Boot`, or `Sneaker`
- `Accessory`: contains `Glasses`, `Earring`, `Glove`, `Bandage`, or `Nose`

Example discovered assets:
- Body: `/Game/Creative_Characters/Skeleton_Meshes/SK_Body_001.SK_Body_001` through body variants including `SK_Body_009`.
- Hair: `/Game/Creative_Characters/Skeleton_Meshes/SK_Hairstyle_Female_001.SK_Hairstyle_Female_001`, `SK_Hairstyle_Male_001`, and single variants.
- Face: `SK_Female_emotion_*` and likely male emotion variants.
- Hat: `SK_Hat_001` onward.
- Mustache/beard: `SK_Mustache_001` onward and `SK_Beard_001` onward.
- Outfit: `SK_Costume_*`.
- Outwear: `SK_Outwear_*`.
- Pants: `SK_Pants_*`, `SK_Shorts_*`.
- Shoes: `SK_Shoe_Slippers_*`, `SK_Shoe_Sneakers_*`.
- Accessory: `SK_Glasses_*`, `SK_Gloves_*`, `SK_Bandage_001`, `SK_Clown_nose_001`, `SK_Earrings_001`.

## UMG Assets

Customizer:
- Asset: `/Game/UI/WBP_VNHCharacterCustomizer`
- Parent class: `VNHCharacterCustomizerWidget`
- Important widget names:
  - Category buttons: `BodyButton`, `HatButton`, `HairButton`, `FaceButton`, `OutfitButton`, `PantsButton`, `ShoesButton`, `RandomButton`, `AccessoryButton`, `MustacheButton`, `OutwearButton`
  - Item grid: `ItemGrid`
  - Item slots: `LookItemSlot_1` through `LookItemSlot_20`
  - Text: `TitleText`, `PreviewText`, `StatusText`
  - Controls: `PreviousButton`, `NextButton`, `BackReadyButton`, `CloseButton`
  - Header image: `DripCheckHeaderImage`, using `/Game/UI/Textures/Customizer/DripCheckHeader`
- The customizer currently has visual slots but needs category-specific item-slot selection behavior tied to real asset arrays.
- `LookItemSlot_1..20` can now be populated with runtime label/icon content. Icon widget naming convention is `LookItemSlot_N_Icon`; label naming convention is `LookItemSlot_N_Label`. If the designer widget does not provide these children, the C++ parent creates a runtime vertical stack inside the button.
- Customizer item slots clip to their button bounds. Runtime thumbnail images use a fixed 118x82 desired size, centered padding, and `SetBrushFromTexture(..., false)` so source texture dimensions cannot expand beyond the slot. `BackReadyButton` uses the same `T_ReadyBtn` brush for normal/hovered/pressed, and `CloseButton` uses `/Game/UI/Images/x_button_ui__1_`, matching the How To Play close control.
- Customizer item tiles use a clean flat-blue rounded rectangle with a cyan hover outline; the former metallic `/Game/UI/Textures/Customizer/clothing-box` button background is not used. Runtime option labels stay collapsed for image-backed choices and are shown only when the option label is exactly `None`.
- Customizer thumbnails fill the blue cards at 158x110 with zero internal padding. The preview capture is isolated to the character components, clears to transparent alpha, uses controlled shadowless key/fill/rim lighting, and displays `CLICK + DRAG TO ROTATE` directly below the preview image.
- Customizer data consolidates the former `Outwear` category into `Outfit`/Shirt, and the `OutwearButton` is collapsed during widget binding while its runtime contract name remains intact. Split `SK_Costume_*` pieces are categorized by their actual mesh region: head-height pieces use Hat, small mid-body pieces use Accessory, and the lower-body costume piece uses Pants. The blank `NewRow` placeholder remains present but disabled.
- Character presets persist in SaveGame slot `VNHCharacterProfile` (user index 0). Item changes write directly into the active preset and call `SaveGameToSlot`; preset selection persists `ActivePresetIndex`, so the customizer intentionally has no unsaved draft layer.

Composure HUD:
- Asset: `/Game/UI/WBP_VNHComposure`
- Important button: `CustomizeButton`
- Should not appear on main menu and should not enable customization during active round phases.

Lobby HUD:
- Runtime lobby overlay is `/Game/UI/WBP_LobbyMenu`, parented to `UVNHLobbyMenuWidget`, spawned by `AVNHPlayerController::ShowLobbyMenu` on `/Game/Maps/Lobby`.
- `UVNHLobbyMenuWidget` owns behavior and live data updates; the Widget Blueprint owns editable layout, images, text widgets, buttons, and panel styling. Keep the native widget construction path as a fallback only.
- The lobby overlay owns the host/player HUD labels, player list, ping bars, customize action, start-console hover prompt, and Steam friends invite dialog.

Settings dialog:
- Asset: `/Game/UI/WBP_SettingsDialog`
- Parent class: `VNHSettingsDialogWidget`
- Existing Blueprint save/load path uses `/Game/UI/BP_SettingsSaveGame` and slot name `PlayerSettings`.
- `VNHSettingsDialogWidget` mirrors settings into the same `PlayerSettings` SaveGame and applies master/music/SFX audio, brightness gamma, and mute-when-unfocused at runtime. Master volume is applied as a multiplier into the effective Music/SFX SoundClass overrides; the transient primary audio device volume is reserved for focus mute.
- Accessibility tab, Hold to Act Natural, and Act Natural controls are intentionally hidden. Controls list includes Jump `SPACEBAR` and Crouch `CTRL`.

## Debug/Test Deck

- Test deck/debug panel is owned by `VNHDebugHUD`.
- `/Game/Blueprints/BP_VNHDebugDeckSpawner` creates `/Game/UI/WBP_VNHDebugDeck` in `MVP_ClothingStore` and other gameplay maps. The deck starts collapsed and the player controller toggles the actual UMG widget visibility.
- `VNH_ToggleDebugHud` is mapped only to `F1` in `Config/DefaultInput.ini`, with no direct Tab/Backslash bindings, so it works in PIE/editor play and packaged builds without consuming those gameplay keys.
- Do not use backtick/tilde for debug deck because Unreal console uses it.
- Debug shopper possession sorts shoppers by actor name before applying an index. Any role selected through the deck pauses the NPC routine, restores Pawn capsule collision and walking mode, grounds the shopper with a downward trace, then possesses it and updates the view target. This prevents Hunter test possession from inheriting unsafe NPC movement/collision state or falling through the store floor.
- `/Game/UI/WBP_VNHDebugDeck` exposes `Alien 0`, `Hunter 1`, and `Human 2` possession buttons. Each executes exactly one parser-free dedicated command (`vnh.PossessAlien 0`, `vnh.PossessHunter 1`, or `vnh.PossessHumanRole 2`); do not chain possession with a second ForceRole command. The legacy Canvas fallback uses the same commands. The possession path sorts shoppers, pauses routines, restores collision/movement, ground-traces and teleports to capsule height, then logs role, Z, and `Falling` state for automated verification.

## Known Pitfalls

- Do not run `Build.bat` while the editor is open unless specifically instructed.
- Steam hosting uses the native `UVNHCreateServerWidget` path. It creates and starts a Steam lobby session named `GameSession` before traveling to `Lobby`; retries must clear any existing named session first. The create path should explicitly prefer the Steam subsystem when the default subsystem lookup is unavailable or resolves to `NULL`, log subsystem/session state, resolve the host id from PlayerState, LocalPlayer, or the identity interface, and fall back to local user index 0 instead of aborting only because the id was not pre-resolved. Packaged builds stage `steam_appid.txt` in all configs for local AppID 480 Steam tests launched outside Steam. Runtime maps must be explicitly listed in `ProjectPackagingSettings.MapsToCook` (`/Game/Maps/MainMenu`, `/Game/Maps/Lobby`, `/Game/Maps/MVP_ClothingStore`) so packaged create-game travel does not fail back to the default main menu.
- General project metadata uses `That's Not Greg` for the packaged project/display/debug title. Steam AppID 480 is Valve's Spacewar test application, so Steam's own library/overlay product name remains Spacewar until the project uses its assigned Steamworks AppID.
- Server-browser Steam searches must resolve the local Steam unique net id, use UE's real `SEARCH_LOBBIES` key from `Online/OnlineSessionNames.h`, then filter locally by the advertised `VNH_GAME_ID=VNHSimulator` tag. Do not send `VNH_GAME_ID` as a backend query under AppID 480; it can hide valid lobbies.
- If no online subsystem is available, `UVNHCreateServerWidget` may fall back to opening `/Game/Maps/Lobby` as a local listen lobby. If Steam is active and Steam session creation fails, do not local-fallback; show the Steam failure so packaged-game hosting problems are visible.
- As of 2026-07-05, create-server travel uses `/Game/Maps/Lobby` and URL-encoded `ServerName`/`Password` options. Private passwords are not advertised in Steam search metadata; clients enter the password in `UVNHServerBrowserWidget`, which appends it to client travel, and `AVNHGameMode::PreLogin` rejects incorrect private-game passwords.
- Created-game round length is carried by the `RoundSeconds` travel/session setting from `UVNHCreateServerWidget` and applied to `FVNHPhaseTiming::InvestigationSeconds` in `AVNHGameMode::InitGame`. Supported UI values are 2, 3, and 5 minutes, defaulting to 2 minutes. The lobby-to-store `ServerTravel` URL must forward the lobby's parsed `PhaseTiming.InvestigationSeconds` as `RoundSeconds`; otherwise the gameplay map silently falls back to 2 minutes.
- `/Game/UI/WBP_VNHMainMenu` is currently a plain `UserWidget` owned through the Blueprint main-menu bootstrap/player-controller/HUD path, not `UVNHMainMenuWidget`. Host Private and Host Public both open `/Game/UI/WBP_CreateServerDialog`; Quit/Clock Out is wired in that widget graph to `QuitGame`.
- Lobby round start is handled by the runtime `AVNHLobbyPlayButton` spawned from `AVNHGameMode::EnsureLobbyRuntimeActors`. Only the host can start; the host must stand near the static-mesh start console, hover it, and hold E. Listen-server host detection should treat the local authoritative controller as host immediately instead of depending only on `GameState->PlayerArray[0]`, because the lobby HUD can appear before player-array ordering settles. Runtime lobby HUD visuals are designer-owned in `/Game/UI/WBP_LobbyMenu`, parented to `UVNHLobbyMenuWidget`; C++ should bind/update named widgets such as `LobbyNameText`, `PlayerRowsBox`, `InviteButton`, `CustomizeButton`, `LobbyStartPromptPanel`, `FriendsScrollBox`, and `InviteStatusText` instead of rebuilding all visuals in code. The native `UVNHLobbyMenuWidget` tree is only a fallback if the Blueprint is unavailable or missing required names. `ShowLobbyMenu()` should add the widget to the viewport, set full viewport anchors, zero alignment, zero position, and set desired size to `GetViewportSize() / UWidgetLayoutLibrary::GetViewportScale(Widget)`. Do not use raw `GetViewportSize()` for desired size; raw pixels fight Slate/DPI at 2560x1440 and 4k. Do not omit desired size either; then the root canvas can collapse to child desired size and all anchor-based panels pile into the left side. Do not multiply positions/sizes by a 1920 reference scale; Unreal's DPI scaling already handles 1080p/1440p/4k, and extra manual scaling makes 2560x1440 oversized. The start-console hover prompt should use `UWidgetLayoutLibrary::ProjectWorldLocationToWidgetPosition` and top-left CanvasPanel anchors while following the projected point; mixing `ProjectWorldLocationToScreen` pixels with bottom-center anchors misplaces it. Create-server/server-browser/main-menu widgets can sit above the lobby HUD in PIE if they survive travel, so remove top-level non-lobby widgets before showing the lobby overlay and keep the lobby overlay above those menu Z orders. Solo/two-player starts are intentionally allowed for testing through `StartRoundFromLobby`/`DebugStartRound`.
- Prefer user Live Compile for C++ changes.
- Avoid automated PIE unless explicitly requested; previous sessions saw PIE cleanup crashes referencing stale `VNHGameInstance` through `TransBuffer`.
- Widget button names that match C++ `UPROPERTY` fields should generally not be set as designer variables unless required; duplicate UPROPERTY/widget variable names can cause compile problems.
- Do not delete legacy widgets/assets just because they look unused. First inspect graph bindings, C++ `GetWidgetFromName`, and asset references.
- Marketplace folders such as `/Game/Creative_Characters`, `/Game/TNG`, and `/Game/HE_SmoothOutlines` may be candidates for later migration/cleanup, but do not move/delete them without a live dev decision.

## Customizer Implementation Direction

Target behavior:
- Category rail selects an `EVNHCustomizationSlot`.
- Center grid displays the first page of items for that category.
- `LookItemSlot_1..20` should represent actual assets in the current category, not generic previous/next buttons.
- Clicking an item applies that exact mesh to the active customization and previews on the local pawn.
- Previous/next controls page through category items.
- Presets remain separate from item-grid options.
- Random randomizes the active customization from available Creative_Characters assets.
- READY marks pre-round ready and hides/locks the customizer when round starts.
- The character preview is rendered by `AVNHCustomizationPreviewActor` into a portrait render target and bound to `PreviewRenderImage`. Keep the render target aspect aligned with the right-side preview panel; a square render target stretches the character vertically in UMG.
- While the customizer is open, `VNHPlayerController` should suppress world interaction polling/focus traces and force the normal cursor so NPCs/interactables do not steal hover state through the UI.
- The designer widget currently stores preset buttons in `ItemGrid`, but `UVNHCharacterCustomizerWidget` moves them into a runtime bottom `PresetControlsBar_Runtime` and then normalizes `LookItemSlot_1..20` to a 5-column grid starting at row 0, column 0.
- Pagination arrows are hidden/disabled when a category has only one page.
- Preview rotation accepts right-click drag over `PreviewRenderImage` while preserving left-click drag as a fallback.
- Runtime preset edits are immediate: selecting a preset makes it active, and item changes auto-save to that active preset. Preset buttons should visibly show the active selection. `SAVE PRESET` and `LOAD PRESET` are intentionally hidden/removed from the current UX. `BLANK CANVAS` remains and clears all cosmetic meshes and face/hair while keeping the selected body mesh/body color.
- Preview rotation must rotate the character mesh rig only, not the camera/lights. `AVNHCustomizationPreviewActor` uses a fixed capture/light root plus a nested `CharacterRoot` for yaw.
- Preview animations should use the manually retargeted assets in `/Game/TNG/Characters/Animations/TryingOnClothesAnims/Male` and `/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female`. The customizer exposes a Female Preview Anims checkbox under the preview panel to choose which set is preferred; if the selected set has no compatible clip, fall back to the other set/procedural try-on flourish.
- As of inspection, all 47 AnimSequences in `/Game/TNG/Characters/Animations/TryingOnClothesAnims` still report Skeleton `/Game/Creative_Characters/Animations/SKEL_Skeleton`, while `/Game/TNG/Characters/BaseBodies/SK_TNG_Body_*` report `/Game/TNG/Characters/Animations/TryingOnClothesAnims/Female/SKEL_TNG_CreativeCharacter`. If the active preview body is a TNG body, these animation clips will be rejected by the compatibility check unless they are truly retargeted to the TNG skeleton.
- Player-controlled Human, Alien, and Hunter shoppers share `UVNHAlienLocomotionComponent` despite its legacy name. Mouse/cursor body and camera rotation is intentionally disconnected in both `SetupInputComponent` and `DefaultInput.ini`; RMB remains Hunter target-lock input only. The gameplay cursor moves independently and drives interaction deprojection plus a world-target head-look pose. Cursor rays are traced into the scene, converted to actor-local direction from the `Head` bone, clamped to ±60 yaw/±30 vertical, and rejected when local forward X is non-positive so targets behind the character return smoothly to neutral. `ABP_CreativeCharacter` applies component-space additive yaw plus roll (the Creative mesh's correct vertical head axis) after the montage slot.
- Shared gameplay targeting uses mouse deprojection from an independently movable cursor. Hunter, Human, and Alien HUDs each contain non-hit-testable `CenterReticleShadow` and `CenterReticleDot` widgets whose Canvas positions follow the mouse while the hardware cursor remains hidden. Lobby, customizer, and debug interfaces retain their explicit normal cursor modes.
- The third-person follow boom defaults to 460 units behind the pawn with a 68-unit raised target offset. Spring-arm camera collision is enabled on `ECC_Camera`; compression dynamically raises the socket by up to 125 units and pitches from -18 to -55 degrees for an obstruction-aware overhead view.
- Motion blur is disabled project-wide with `r.DefaultFeature.MotionBlur=False` and `r.MotionBlurQuality=0`; both shopper camera components override Amount, Max, and PerObjectSize to zero, and the customizer SceneCapture disables its motion-blur show flag. Follow-camera lag is disabled and rendering uses FXAA (`r.AntiAliasingMethod=1`, temporal upsampling off) to prevent TSR/TAA ghost trails that can resemble motion blur.
- `/Game/UI/Customizer/DT_VNHCustomizationItems` contains 392 rows: 391 enabled mesh-backed entries and one disabled blank `NewRow`. A full mesh-name and bounds audit found no missing meshes or category-region anomalies after costume-piece reclassification and Outerwear consolidation.

Disciplined next step for item-grid wiring:
1. Add per-slot click handlers or one handler that maps button name to visible item index.
2. Store visible item asset paths for the active category/page inside `UVNHCharacterCustomizerWidget`.
3. Add a `SelectCustomizationSlotItem(EVNHCustomizationSlot, int32 ItemIndex)` style API to `UVNHGameInstance` if one does not already exist.
4. Reuse the existing `FVNHCharacterCustomization` application path. Do not add a parallel customization system.
5. Compile C++, then compile `/Game/UI/WBP_VNHCharacterCustomizer`, then manually test in pre-round.
