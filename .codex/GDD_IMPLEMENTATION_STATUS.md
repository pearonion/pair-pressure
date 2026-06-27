# Very Normal Human Simulator - GDD Implementation Status

Last updated: 2026-06-26

## Current Target

Implement the MVP foundation from the GDD first-night checklist, with focus on:

- two-player replicated role assignment
- shopper possession by the Alien
- basic shopper routine movement
- first pass of skill-based Alien locomotion

## Implemented In Local Commits

- `ba90ba9` Add replicated round state foundation
- `92cb0b7` Add shopper routine foundation
- `0f81495` Fix replicated player role field name
- `01b47a7` Add hunter test and accusation requests
- `627baa8` Add alien shopper possession setup
- `3cb3f02` Fix role parameter shadowing
- `32b2b31` Add basic shopper routine movement

## Current Uncommitted Work

- Added `UVNHAlienLocomotionComponent`.
- Attached Alien locomotion component to `AVNHShopperCharacter`.
- Added camera-relative move input, desired speed, acceleration, coast braking, manual brake detection, fast walk, body turn rate, and correction-step state.
- Exposed locomotion state and input functions to Blueprint for Enhanced Input wiring.
- Added optional `AVNHPlayerController` bindings for Alien move, fast-walk, and Act Natural Enhanced Input assets.
- Alien input mapping context is applied only when the local controller owns the possessed shopper.
- Added fallback `DefaultInput.ini` controls and `AVNHPlayerController` legacy input binds for WASD/left stick, Shift/right trigger, and Q/left shoulder.
- Added `UVNHMovementTuningData` and optional tuning-data reads in `UVNHAlienLocomotionComponent`.
- Added `bHasMoveInput` and `BodyYawDeltaDegrees` to `FVNHAlienLocomotionState` for animation blueprint consumption.
- Live Coding compile succeeded after user manually triggered it.
- PIE started successfully in the current `Untitled_1` world with `VNHGameMode`; PIE was stopped afterward.
- Added debug console commands: `vnh.StartRound`, `vnh.SkipPhase`, `vnh.ForceRole`, `vnh.TriggerTest`, and `vnh.PossessHuman`.
- Added `LogVNH` debug logging for the `vnh.*` commands and GameMode debug helpers.
- Attempted command validation once; commands landed after PIE teardown, so runtime possession/test behavior is not validated yet.
- Runtime possession is now confirmed: user log shows `vnh.PossessHuman 0` possessed `VNHShopperCharacter_0`.
- Added `vnh.LogAlienLocomotion` to print current alien locomotion state on demand.
- Added direct movement debug commands to bypass keyboard binding/focus while validating the locomotion component:
  - `vnh.SetAlienMove X Y`
  - `vnh.ClearAlienMove`
  - `vnh.SetAlienFastWalk 0|1`
- Latest user log shows possession works but key presses produced `DebugManager.CycleToPreviousColumn`, so the next test is direct command movement before chasing input binding.
- Added `vnh.LogAlienInput` and `AVNHPlayerController` counters to inspect whether legacy axes, Enhanced Input movement, and fast-walk presses reach the active controller.
- Added `AlienInput:` setup/possession logs for confirming the controller class and input component at runtime.
- Direct command movement is confirmed working: after `vnh.SetAlienMove 0 1`, the next locomotion log showed `DesiredSpeed=330.0 CurrentSpeed=330.0 HasInput=true`.
- Added controller-level polled keyboard fallback in `AVNHPlayerController::PlayerTick()`:
  - WASD drives Alien movement.
  - Left/Right Shift drives fast walk.
  - Q triggers Act Natural on press edge.
  - Polling only writes when a polled key is active or changes, so idle polling does not overwrite direct console movement probes.
- Pivoted from console-only movement probes to a visible test-pawn workflow:
  - `AVNHShopperCharacter` now has a follow camera boom and follow camera.
  - Shopper characters try to use Manny from `/MoverTests/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny`.
  - If Manny is not mounted, a visible cylinder debug body is used.
  - Added `vnh.SpawnTestHuman [X Y Z]` to spawn and possess a visible shopper test pawn directly.
- Verified the Unreal Editor viewport via Computer Use and fixed the black scene directly in the level:
  - Added a large engine-cube floor.
  - Added a point light component to the floor.
  - Reframed the editor camera and confirmed the viewport now shows a lit test area.
- Added `vnh.SetupTestArena` and updated `vnh.SpawnTestHuman` so test spawning also creates the lit arena.
- After Live Coding succeeded, populated `/Game/Maps/MVP_ClothingStore` with the MVP greybox store content:
  - floor, side/back walls, checkout counter, mirrors, six clothing racks
  - hunter entrance PlayerStart
  - key shop light and nav bounds
  - 12 shopper waypoints across four three-point loops
  - 8 `AVNHShopperCharacter` actors
- Assigned waypoint metadata for context, suggested next activity, held prop, and wait time.
- Assigned each MVP shopper's `RoutineComponent.RoutineWaypoints` array and verified it persists after reloading the level.
- Set `GameDefaultMap=/Game/Maps/MVP_ClothingStore`.
- PIE smoke test confirmed `vnh.StartRound` and `vnh.PossessHuman 1` execute on the saved MVP map.
- PIE smoke test exposed that legacy zero-axis samples were overwriting direct debug movement every frame.
- Patched `AVNHPlayerController::PushLegacyAlienMoveInput()` so legacy axes only push when the legacy vector changes, preserving direct debug movement and polled keyboard input while still clearing real legacy input on release.
- Wired `AVNHShopperCharacter` constructor to the actual sample-content Manny mesh and unarmed animation blueprint:
  - `/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple`
  - `/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed`
- Also assigned the same Manny skeletal mesh, AnimBP, mannequin offset/rotation, and hidden debug cylinder state directly to all 8 placed MVP shoppers in `/Game/Maps/MVP_ClothingStore`.
- Visual PIE check passed: the MVP map renders Manny mannequins in the shop.
- User confirmed WASD keyboard movement works.
- Added mouse/gamepad look axis bindings in `AVNHPlayerController`:
  - `Turn Right / Left Mouse`
  - `Turn Right / Left Gamepad`
  - `Look Up / Down Mouse`
  - `Look Up / Down Gamepad`
- Look input now calls `AddYawInput` / `AddPitchInput` while the controller owns a possessed alien shopper, so the shopper follow camera can rotate independently from body-facing movement.
- Added `AVNHDebugHUD`, a lightweight clickable in-game debug panel.
- `AVNHGameMode` now uses `AVNHDebugHUD` as `HUDClass`.
- Debug panel buttons execute the current testing commands:
  - start round
  - possess shopper 0 / 1
  - Freeze / Look Entrance / Clear Aisle tests
  - log input / locomotion
  - direct move on / clear move
  - fast walk on / off
  - setup arena / spawn test human
- Added `VNH_ToggleDebugHud` mapped to F1.
- `AVNHPlayerController` toggles the panel and switches mouse cursor/click input on while the panel is visible, then restores game-only input for mouse-look when hidden.
- Added first visible public-test behavior for Freeze:
  - `AVNHShopperCharacter` now tracks replicated `bFrozenByPublicTest`.
  - Non-possessed shoppers stop movement, stop AI movement, and pause their Manny animation pose when `EVNHPublicTestType::Freeze` is applied.
  - Freeze lasts `FreezeTestHoldSeconds` seconds, currently 4.0.
  - After the timer clears, shoppers restore walking movement and resume moving toward their current routine waypoint.
  - Possessed alien shopper is intentionally not frozen by this path, so the player must choose how to react.
- Fixed NPC routine startup path:
  - `AVNHShopperCharacter::BeginPlay()` now ensures authority-side placed shoppers have an AI controller and schedules `StartRoutineMovement()` for the next tick.
  - `AVNHShopperAIController::StartRoutineMove()` is now public and logs missing waypoint / failed MoveToActor cases.
  - `AVNHGameMode` explicitly calls `StartShopperRoutines()` during normal and debug round start.
  - Added `vnh.StartRoutines` and a `Kick NPC Routines` debug HUD button.
- Added always-visible round status text to `AVNHDebugHUD`:
  - round number
  - current phase
  - countdown seconds
  - remaining public tests
- Compact debug HUD buttons into two columns so they fit better in PIE.
- Enabled the `GameplayStateTree` plugin in `VNHSimulator.uproject`.
- Added a soft StateTree slot to `AVNHShopperCharacter` pointing at intended asset path `/Game/AI/ST_Shoppers`.
- Attempted to create `/Game/AI/ST_Shoppers`, but creation failed before plugin restart because `GameplayStateTree` was disabled in the active editor session.

## Next Steps

- Compile the latest Freeze behavior normally if Live Coding hits the NeoStack patch linker error again.
- Restart Unreal after the `GameplayStateTree` plugin change, then create `/Game/AI/ST_Shoppers` as a `StateTree` asset with `StateTreeComponentSchema` or `StateTreeAIComponentSchema`.
- Assign `/Game/AI/ST_Shoppers` to placed shoppers' `ShopperStateTree` soft reference after the asset exists.
- In PIE on `/Game/Maps/MVP_ClothingStore`, run `vnh.StartRound`, `vnh.PossessHuman 1`, `vnh.SetAlienMove 0 1`, and `vnh.LogAlienLocomotion`.
- Expected after the input-stomp fix is compiled: the second locomotion log should show `DesiredSpeed=330.0` and `HasInput=true`.
- Also verify mouse movement rotates the camera while possessed.
- Verify the debug HUD appears by default in PIE, its buttons execute commands, and F1 hides/shows the panel.
- Click the debug HUD `Freeze Test` button and verify non-possessed shoppers hold pose for about four seconds, then resume their routes.
- Check that NPCs begin walking after `Start Round`; if they do not, click `Kick NPC Routines` and inspect the `ShopperRoutine:` log warnings.
- If the scene is dark, run `vnh.SetupTestArena`.
- `vnh.SpawnTestHuman` remains available for isolated visible possessed-pawn testing.
- Check `Saved/Logs/VNHSimulator.log` for `LogVNH` lines after running commands.
- Test WASD and Shift in the viewport on the visible pawn; direct command movement already proves the locomotion component works.
- If input counters stay zero after pressing keys, replace the temporary fallback path with concrete Enhanced Input assets or resolve viewport/debug input capture.
- If input counters increase but locomotion stays idle, debug the possessed-shopper role/locomotion lookup gate.
- Create and assign a `UVNHMovementTuningData` asset after compile.
- Optional later: assign actual Enhanced Input assets to the new `AVNHPlayerController` defaults or Blueprint subclass.
- Tune movement values in PIE with two clients.
- Consume locomotion state in the shared human AnimBP for speed, body yaw delta, and correction-step feedback.

## 2026-06-26 Continued

- Created `/Game/AI/ST_Shoppers` after the `GameplayStateTree` plugin was available.
- Assigned the StateTree asset to all placed MVP shoppers through their `ShopperStateTree` soft reference.
- Verified the current nav failure is not missing waypoints:
  - shoppers have waypoints
  - `MoveToActor` fails in PIE
  - editor nav projection fails for shopper and waypoint sample positions
  - `MVP_NavBounds_Shop` reports zero brush extent through `navmesh_list_bounds()`
- Added a direct-movement fallback to `AVNHShopperAIController`:
  - attempts normal nav `MoveToActor` first
  - falls back to walking the character toward the current routine waypoint when nav fails
  - advances through existing `UVNHRoutineComponent` waypoints and wait times
  - respects possessed-alien pause and Freeze public-test pause
- Updated Freeze resume to re-enter the same routine movement path.
- `git diff --check` passed.

## Immediate Test

- Compile the fallback patch.
- Start PIE on `/Game/Maps/MVP_ClothingStore`.
- Click `Start Round`; if needed click `Kick NPC Routines`.
- Expected: NPCs visibly walk between routine waypoints even before the nav bounds brush is repaired.
- Click `Freeze Test`; expected: non-possessed shoppers stop/hold animation briefly, then resume moving.

## 2026-06-26 Animation / Visual Test Pass

- Debug HUD now uses `/Game/ThirdPerson/Demo/BurbankBigCondensed-Black_Font`.
- Confirmed the selected Manny sample AnimBP already includes a `Control Rig` node, and `/Game/Characters/Mannequins/Rigs/CR_Mannequin_FootIK` exists.
- Tuned normal shopper movement to indoor human pace:
  - character default max walk speed `135`
  - AI fallback routine speed `125`
  - lower acceleration/braking values for less abrupt movement
- Forced Manny mesh animation settings to keep `ABP_Unarmed` and foot IK updating while NPCs move.
- Saved animation-tick settings onto placed MVP shoppers in `/Game/Maps/MVP_ClothingStore`.
- Added visible public-test reactions for the existing debug buttons:
  - `Look Entrance`: turn toward entrance briefly, then resume routine.
  - `Clear Aisle`: short side-step reaction, then resume routine.
- `git diff --check` passed.
- Direct UBT compile reached the link step, but link failed because the editor was running and holding `UnrealEditor-VNHSimulator.dll` open. No C++ compile errors were reported before the link lock.

## Immediate Test After Compile

- Use Live Coding or restart the editor for a full link.
- In PIE, click `Start Round` and verify shoppers walk with leg animation at a slower indoor pace.
- Click `Look Entrance`, `Clear Aisle`, and `Freeze Test` to verify all three visible reactions.

## 2026-06-26 Nav Repair Pass

- Repaired the MVP map navigation enough for editor projection:
  - `MVP_NavBounds_Shop` now reports a real bounds extent instead of `0,0,0`.
  - `MVP_Floor_Shop` affects navigation.
  - Recast builds 12 tiles after async build completion.
  - Sample shoppers and waypoints project onto navmesh at `Z=10` once `navmesh_info().is_building` is false.
- Saved `/Game/Maps/MVP_ClothingStore` after the nav build completed.
- PIE still showed `MoveTo` failures with the currently loaded old code because routines start during BeginPlay before PIE nav is ready.
- Patched C++ routine startup:
  - placed shoppers delay automatic routine start by 2 seconds
  - GameMode routine kickoff skips shoppers with no current waypoint
- UBT compile reached link without compile errors; final link is blocked by the open editor DLL lock.

## Immediate Test After Next Compile

- Recompile through Live Coding or full editor restart.
- Start PIE and wait a couple seconds after map load before clicking `Start Round`.
- Expected: shoppers begin routine movement without early nav failures; fallback movement handles any remaining edge case.

## 2026-06-26 Interaction / Animation Fix Pass

- Fixed the actual Manny sliding cause in `/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed`:
  - `ShouldMove` was gated by both speed and nonzero acceleration.
  - AI movement could have velocity but zero acceleration, keeping the AnimBP in idle.
  - Rewired `ShouldMove` to use the existing `GroundSpeed > threshold` boolean directly.
  - Saved the AnimBP; compile log reported no errors/warnings.
- PIE visual check after the AnimBP asset edit showed a moving shopper in a walking pose.
- Added first-pass NPC interaction/question support in C++:
  - focus trace from the player camera
  - `E` and gamepad face button bottom mapped to `VNH_Interact`
  - `vnh.Interact` console command
  - `Question Target` debug HUD button
  - HUD prompt/response panel
  - shopper response uses routine context, held prop, and suggested next activity
- UBT compile status:
  - enum-name compile errors were fixed
  - changed C++ now compiles to link
  - final link still fails while the editor is open because `UnrealEditor-VNHSimulator.dll` is locked

## Immediate Test After Next Compile

- Use Live Coding or restart editor to load the C++ changes.
- Verify the HUD font has changed to `/Game/ThirdPerson/Demo/BurbankBigCondensed-Black_Font`.
- Aim at a shopper and press `E`, or click `Question Target`; expected: prompt plus a short context response.
- Start the round and verify shoppers walk with the Manny locomotion pose rather than idle-sliding.

## 2026-06-26 Follow-up Verification

- PIE verification after reload showed:
  - Burbank-style condensed HUD text visible
  - shoppers in walking poses while moving
  - interaction panel rendering on HUD
- Improved interaction reliability:
  - direct pawn trace remains
  - fallback camera-cone shopper search added
  - no-target interaction now logs and displays an explicit message
- Removed no-waypoint routine spam from actors with no routine waypoint.
- Latest UBT pass compiles C++ and fails only at link because the editor holds the game DLL.

## Immediate Test After Next Compile

- Live Coding/restart once more for the latest targeting patch.
- Aim near any shopper and press `E` or click `Question Target`; expected response uses shopper routine context.

## 2026-06-26 HUD Scale / Hunter Loop Pass

- Enlarged the C++ debug HUD after the Burbank font proved readable but too small:
  - larger title, button, status, prompt, and response text
  - taller/wider buttons
  - panel moved up/left to fit compact PIE captures
- Reduced the visible debug panel to gameplay-facing test controls:
  - Start Round
  - Kick NPC Routines
  - Possess 0 / Possess 1
  - Freeze Test
  - Look Entrance
  - Clear Aisle
  - Question Target
  - Mark Target
  - Fake Accuse
  - Accuse Target
- Kept removed plumbing commands available through console, but removed them from the clickable HUD.
- Added first-pass Hunter social-stealth controls:
  - `R` / left shoulder marks the focused shopper, up to 3 suspects.
  - `F` / left trigger fake-accuses the focused shopper.
  - left mouse / right trigger debug-accuses the focused shopper and enters Reveal.
- HUD now displays the marked suspect list.
- Added `AVNHGameMode::DebugResolveAccusation()` for single-player debug accusation testing without requiring a full two-player role gate.
- Added console commands:
  - `vnh.MarkTarget`
  - `vnh.FakeAccuse`
  - `vnh.AccuseTarget`
- `git diff --check` passed.
- UBT compiled C++ cleanly; final link failed only because the editor has `UnrealEditor-VNHSimulator.dll` open.

## Immediate Test After Next Compile

- Use Live Coding or restart editor to load the HUD scale and Hunter controls.
- Expected test flow:
  - click `Start Round`
  - aim near a shopper
  - use `Question Target` / `E`
  - use `Mark Target` / `R`
  - use `Fake Accuse` / `F`
  - use `Accuse Target` / left mouse
  - verify the round enters Reveal with correct/incorrect accusation state

## 2026-06-26 UEFN/Fortnite-Style HUD Polish Pass

- Polished the existing C++ Canvas debug HUD toward a UEFN/Fortnite-adjacent test deck:
  - chunkier dark-blue surfaces
  - cyan/yellow accent rails
  - stronger shadows
  - inset button faces
  - more polished status, prompt, marked-suspect, and response panels
- Added button motion:
  - hover lift
  - hover width expansion
  - press punch
  - timed click pulse cleanup
- Added UI audio hooks:
  - hover uses `/Engine/VREditor/Sounds/VR_click1_Cue`
  - click uses `/Engine/VREditor/Sounds/VR_confirm_Cue`
  - missing sounds are safe; the HUD still works without them
- `git diff --check` passed.
- UBT compiled C++ cleanly; final link failed only because the editor has `UnrealEditor-VNHSimulator.dll` open.

## Immediate Test After Next Compile

- Use Live Coding or restart editor to load the polished HUD.
- In PIE, move the mouse over the debug buttons and click them.
- Expected:
  - visible hover lift/expansion
  - click punch
  - hover/click UI sounds
  - no loss of existing button commands

## Editor Safety

Do not close, kill, restart, or force-stop the running Unreal Editor unless explicitly approved.

## 2026-06-26 Natural Locomotion Visibility Pass

### GDD 11.3 Status

- Natural alien locomotion is implemented in C++ and remains the current test target:
  - acceleration ramp
  - coast braking
  - manual brake on reversal
  - capped body turn rate
  - camera-relative movement direction
  - fast-walk state
  - correction-step flag
- Added visible runtime instrumentation so this can be tested without console commands:
  - role label: Alien/Human state
  - locomotion label: current speed, desired speed, fast-walk, brake, and turn delta

### Changed

- `AVNHPlayerController` now exposes:
  - `GetRoleStatusText()`
  - `GetLocomotionStatusText()`
- `AVNHPlayerController::PlayerTick()` now updates the debug deck's named UMG text blocks when present:
  - `RoleStatusText`
  - `LocomotionStatusText`
- `/Game/UI/WBP_VNHDebugDeck` now has a polished `CONTROL STATUS` panel using the Burbank font.

### Verification

- `/Game/UI/WBP_VNHDebugDeck` compile log: 0 errors, 0 warnings.
- Widget preview screenshot verified the new panel is visible and text is inside bounds.
- External UBT did not reach C++ compile due source-engine target-rule conflict:
  - `VNHSimulatorEditor modifies StructMemberAlignment: 8 != null`
  - likely follow-up: update target build settings for this engine or continue using Live Coding for the immediate test loop.

### Immediate Test

- Live Code/restart editor.
- PIE:
  - `Possess 0` or `Possess 1`
  - move with WASD
  - hold Shift for fast walk
  - reverse direction to trigger manual brake/correction
  - watch the `CONTROL STATUS` panel update live

## 2026-06-26 Manny Animation Enforcement Pass

### GDD 13 / 22 Status

- The project is now using the imported Manny unarmed animation stack as the shared placeholder human animation path:
  - `/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed`
  - `/Game/Characters/Mannequins/Anims/Unarmed/BS_Idle_Walk_Run`
- `ABP_Unarmed` already contains velocity/speed/direction update logic and a Control Rig IK node.

### Changed

- `AVNHShopperCharacter` now enforces Manny visual setup during runtime:
  - Manny skeletal mesh assigned if missing
  - `ABP_Unarmed` assigned if missing
  - animation blueprint mode enabled
  - animation unpaused
  - normal animation rate
  - pose and bones always tick for visible testing
  - update-rate optimization disabled for debug readability
- Added `DescribeAnimationDebugState()`.
- Added console command `vnh.LogShopperAnim`.

### Verification / Blockers

- `git diff --check` passed.
- Full external UBT is currently not a clean verifier because forcing a unique editor build environment made the source engine build large editor/plugin chunks and hit a NeoStack third-party Lua warning-as-error:
  - `C4191` in `Plugins/NeoStackAI/Source/ThirdParty/Lua/src/loadlib.c`
- Continue to use Live Coding/restart for the immediate test loop unless/until the NeoStack plugin build settings are adjusted.

### Immediate Test

- Live Code/restart editor.
- PIE:
  - click `Kick NPC Routines`
  - run `vnh.LogShopperAnim`
  - expected for shoppers: Manny mesh, `ABP_Unarmed_C`, animation not paused, rate `1.00`, nonzero `SpeedXY` while moving
  - visually confirm shoppers walk instead of sliding

## 2026-06-27 Updated GDD Core Loop Alignment Pass

### GDD 4 / 8 / 9 / 24 Status

- Added first-class `Human` role support.
- Updated role assignment to match the new 3+ player MVP rule:
  - exactly one Hunter
  - exactly one Alien
  - all remaining connected players are Humans
- Raised normal `RequiredPlayers` to 3.
- Added replicated Hunter tool budgets:
  - two public commands
  - one direct question
  - one accusation
- Routed direct question through the server so it is Hunter-only, investigation-phase-only, and consumes the one-question budget.
- Real accusation now consumes the replicated one-accusation budget before entering Reveal.
- Added Blueprint-facing role reveal text, role goal text, Hunter tool text, and reveal summary text.
- Added replicated lightweight errand text for non-Hunter players. Alien receives the same kind of normal shopping cover errand so objective/UI structure does not reveal the role.
- Added quick-chat MVP foundation:
  - preset bark enum
  - replicated latest quick-chat message on GameState
  - `AVNHPlayerController::RequestQuickChat`
  - `vnh.QuickChat Browse|Shirt|Friend|NoThanks|WrongSize`
  - temporary default input: `T` / D-pad up sends "Just browsing."
- Added host-authoritative lobby start skeleton:
  - normal auto-start on BeginPlay/PostLogin is now gated behind `bAutoStartRoundOnPlayerJoin` and defaults off
  - `AVNHGameMode::StartRoundFromLobby(APlayerController*)` accepts only the host and requires the normal MVP player count
  - `AVNHPlayerController::RequestStartRoundFromLobby()` server RPC is ready for `BP_Lobby_PlayButton`
  - `vnh.StartFromLobby` exercises the same path
- Changed NPC public-test reactions to auto-run only on NPC shoppers. Player-controlled shopper bodies must react manually, matching the GDD command-read goal.
- Changed Act Natural availability to player-controlled shopper bodies, not Alien-only, so Human and Alien share the same recovery button path.
- Updated `vnh.ForceRole` to accept `Human`.
- Updated the Canvas debug HUD status line to show command/question/accusation budgets.

### Verification

- `git diff --check` passed with only existing Windows CRLF warnings.
- UHT processed successfully in the external build attempt.
- Full external compile is still blocked while Live Coding is active in the running editor:
  - `Unable to build while Live Coding is active. Exit the editor and game, or press Ctrl+Alt+F11...`

### Immediate Test

- Trigger Live Coding or restart editor when ready.
- Run a 3-client PIE test:
  - from the host, run `vnh.StartFromLobby`; expected round starts only when 3 players are connected
  - start round
  - verify exactly one Hunter, one Alien, one Human
  - verify Hunter HUD budget starts at `Cmd 2 Q 1 Acc 1`
  - aim at a shopper and press `E`; expected question budget decrements to `0`
  - accuse a shopper; expected accusation budget decrements to `0` and Reveal summary reports correct/wrong result
  - trigger Freeze / Look Here; expected NPC shoppers auto-react while player-controlled bodies must perform manually
  - press `T` or run `vnh.QuickChat Shirt`; expected latest quick-chat text replicates and appears in interaction/debug feedback

## 2026-06-27 Main Menu / Lobby Pass

### GDD 4 / 28 Status

- Added `UVNHGameInstance` and set it in `DefaultEngine.ini`.
- Set `GameDefaultMap=/Game/Maps/MainMenu`.
- Created `/Game/Maps/MainMenu`, `/Game/Maps/Lobby`, and `/Game/UI/WBP_VNHMainMenu`.
- `/Game/UI/WBP_VNHMainMenu` is intentionally still parented to `UserWidget`; the runtime binds named buttons directly:
  - `HostPrivateButton`
  - `HostPublicButton`
  - `JoinAddressTextBox`
  - `JoinButton`
  - `QuitButton`
  - `MenuStatusText`
- Main menu host actions open `/Game/Maps/Lobby` as a listen server.
- Join action travels to the entered address, defaulting to `127.0.0.1`.
- Added `AVNHLobbyPlayButton`.
- `AVNHGameMode` now spawns a runtime lobby play button in the Lobby map if one is not already present.
- `AVNHPlayerController` interaction targeting now supports the lobby play button as well as shoppers.
- Lobby host start now travels connected players to `/Game/Maps/MVP_ClothingStore?listen?StartRound=1` instead of starting the round inside the lobby.
- After store travel, `StartRound` waits for the required player count through `PostLogin`, then assigns roles and starts the role reveal flow.
- Populated `/Game/Maps/Lobby` with saved MVP lobby markers:
  - start pad
  - backdrop
  - waiting lane markers
  - five additional player starts
  - play-button light
- Added a narrow always-on HUD phase overlay:
  - in `/Game/Maps/Lobby`, players see the connected-player count and lobby readiness state
  - during `AssigningRoles`, each local player sees their private role reveal, role goal, cover errand, and Hunter tool budget line
  - during `Reveal`, players see the replicated accusation result summary
- Expanded quick-chat input beyond the one default bark:
  - `T`: Just browsing
  - `Y`: Looking for a shirt
  - `U`: Waiting for someone
  - `I`: No thanks
  - `O`: Wrong size
  - D-pad up/right/down/left maps to the first four lines

### Verification

- `/Game/UI/WBP_VNHMainMenu` compile log: 0 errors, 0 warnings.
- Reloaded `/Game/Maps/Lobby` and verified all labeled lobby actors are present.
- Lobby viewport screenshot confirmed the pad, lanes, player starts, and backdrop are visible.
- `git diff --check` passed with only Windows CRLF warnings.
- UE 5.8 UBT through bundled .NET 10:
  - UHT processed successfully.
  - C++ compile reached and compiled edited game files after fixing a UE 5.8 checked-format issue in `AVNHShopperCharacter::DescribeAnimationDebugState()`.
  - Follow-up C++ verifier compiled the role/reveal overlay and expanded quick-chat bindings.
  - Follow-up C++ verifier compiled the Lobby waiting overlay.
  - Final link is blocked by the running editor holding `UnrealEditor-VNHSimulator.dll` and NeoStackAI PDB files open.

### Immediate Test

- Restart the editor, or trigger Live Coding if it can relink the game module cleanly.
- Launch the project to `/Game/Maps/MainMenu`.
- Click `Host Private`; expected: listen-server Lobby opens.
- With 3 clients connected, aim at the lobby start pad/button and press `E`; expected: host travels everyone to `/Game/Maps/MVP_ClothingStore`.
- Expected after store travel: once all 3 players reconnect, one Hunter, one Alien, and remaining Human roles are assigned and the round enters role reveal.
