# VNHSimulator Project Knowledge

Last updated: 2026-07-01.

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
- The `Icon` column is intentionally empty for now because no per-item thumbnails existed under `/Game/UI/CharacterCustomizer/Images` or `/Game/UI/CharacterCustomizer/Thumbnails` at the time of seeding. Existing widget code falls back to text labels.

GameInstance should prefer the DataTable when it exists and contains enabled rows for a category. If no rows exist for a category, it falls back to Creative_Characters asset discovery.
When DataTable rows exist, the runtime prepends a synthetic `None` option for every optional slot except `Body`. Selecting it passes an empty mesh path through the existing customization application path and removes that worn item.

Default body mesh currently points at:
- `/Game/Creative_Characters/Skeleton_Meshes/SK_Body_009.SK_Body_009`

Creative animation class currently referenced:
- `/Game/Creative_Characters/Animations/ABP_CreativeCharacter.ABP_CreativeCharacter_C`

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

Composure HUD:
- Asset: `/Game/UI/WBP_VNHComposure`
- Important button: `CustomizeButton`
- Should not appear on main menu and should not enable customization during active round phases.

## Debug/Test Deck

- Test deck/debug panel is owned by `VNHDebugHUD`.
- Toggle path includes an action mapping named `VNH_ToggleDebugHud` and direct key binding in `VNHPlayerController::SetupInputComponent`.
- `Config/DefaultInput.ini` has mapped `VNH_ToggleDebugHud` to `F1`; if the desired key changes, update both config and direct code binding.
- Do not use backtick/tilde for debug deck because Unreal console uses it.

## Known Pitfalls

- Do not run `Build.bat` while the editor is open unless specifically instructed.
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
- Runtime preset actions include `SAVE PRESET`, `LOAD PRESET`, and `BLANK CANVAS`; blank canvas clears all cosmetic meshes and face/hair while keeping the selected body mesh/body color.

Disciplined next step for item-grid wiring:
1. Add per-slot click handlers or one handler that maps button name to visible item index.
2. Store visible item asset paths for the active category/page inside `UVNHCharacterCustomizerWidget`.
3. Add a `SelectCustomizationSlotItem(EVNHCustomizationSlot, int32 ItemIndex)` style API to `UVNHGameInstance` if one does not already exist.
4. Reuse the existing `FVNHCharacterCustomization` application path. Do not add a parallel customization system.
5. Compile C++, then compile `/Game/UI/WBP_VNHCharacterCustomizer`, then manually test in pre-round.
