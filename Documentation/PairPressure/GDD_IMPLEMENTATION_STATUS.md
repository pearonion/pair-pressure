# Pair Pressure GDD Implementation Status

Last updated: 2026-07-13

This document separates editor-authored assets from native source that still requires a clean editor restart and normal build. It is intended to keep feature ownership modular and reduce merge conflicts.

## Working Prototype

Prototype map: `/Game/PairPressure/Maps/PP_FriendshipPhysics`

Implemented and previously compiled/saved in the editor:

- Four-player/two-team alternating team assignment prototype.
- `Lobby -> Build -> Run -> Results` round-flow contract, replicated run timer, and five-round match aggregation.
- Three sequential build sockets with Designer-authored choice UI.
- Hammer, fan, seesaw, sofa, drum, and piston blockouts. Hammer, fan, seesaw, sofa, and drum now have active prototype behavior; the piston remains a static map blockout because of the documented timeline-curve save defect.
- Same-team, two-player Bring Home finish validation for both teams. Finished teams are remembered and ignored on repeat overlap while transient occupants reset for the next team.
- One tether link per team with slack, pull, break, and reconnect behavior.
- Shared usable-item interface and base throw/drop behavior.
- Football, cone, bowling ball, plunger, extinguisher, umbrella, and shopping-cart placeholders, now with distinct primary-behavior proofs through the existing interfaces.
- Designer-owned HUD root, build overlay, and results overlay. Results are populated through `BPI_PP_ResultsView`, keeping controller/HUD/results-widget ownership separated without concrete widget casts. Award rows use `BPI_PP_AwardRowView` so final score/audit data remains separate from the row widget class. Live local-player presentation travels through `BPI_PP_UIDataProvider`, `BPI_PP_TetherPresentationProvider`, and `BPI_PP_PlayerHUDView` into the existing modular Designer panels for teammate state/link, tether tension/state, held-item state, home guidance, and physical/Daze state.

The last editor audit reported no Pair Pressure Blueprint compile errors. Asset-only PIE passes explicitly compiled the changed gameplay/UI Blueprints before play, loaded the course without scoped runtime errors, and confirmed a live `WBP_PP_HUD_Root` instance with `bPairPressureHUDAdded=True`. The round-flow probe then verified first finisher `0` leaves the phase in Run, stores `FirstWinningTeamId=0`, and resets the countdown to approximately 20 seconds; distinct team `1` transitions to Results while preserving winner `0`. The controller-to-HUD-to-results interface relay presented winner `0`, updated the Designer text, and made the Results overlay visible. A later focused HUD pass verified the nested runtime widgets received `HANDS FREE`, `GET BOTH PLAYERS HOME`, `BRING YOUR PARTNER INTO THE SAFE ZONE`, `Grounded`, and `PARTNER LINK ACTIVE`, with no scoped runtime errors. The tether-HUD pass then verified the active main HUD contains the Designer tether panel and receives the local team's provider baseline as `WAITING FOR PARTNER`, `KEEP TOGETHER. SHARE THE LOAD.`, and zero tension, again without scoped runtime errors. The teammate-presentation pass replaced the old direction fallback with an interface-provided relative bearing and added an interface-owned partner-home query; its single-player baseline correctly delivered `PARTNER SEARCHING` while preserving the home guidance with no runtime errors. The numeric-Daze pass added a clean Designer progress bar and formatted `DAZE {Value} / 100` readout; PIE verified the nested widget received `Grounded`, normalized zero, and formatted value `0` without runtime errors. A five-round PIE command probe produced Team 1 wins in rounds 1/3/5 and Team 2 wins in rounds 2/4, ending at round 5 with a 3-2 score and `bMatchComplete=True`; the runtime Results widget received the formatted score and `MATCH COMPLETE`. The final-match presentation also switched to `POST-MATCH AWARDS`, enabled the photo-moment callout, and populated interface-driven Designer rows with Team 1's 3 points, Team 2's 2 points, and the round 5/5 audit. A separate real-time check let the 12-second Results clock expire and observed round 2 enter Build with the 1-0 score preserved. The playtest screenshot path captures the rendered scene without Slate/UMG, so HUD details are verified through runtime object state; clean Widget Blueprint preview captures are used for Designer layout checks. A four-player PIE pass is still pending because automatic extra-player creation collided with existing platform-user slots; consequently, live two-player bearing/home and two-endpoint tether tension/break/reconnect presentation remain multiplayer validations rather than claimed runtime passes.

## Live Minimal HUD

- `/Game/PairPressure/Core/Interfaces/BPI_PP_PlayerHUDView` is a presentation-only Blueprint contract with narrow team, item, home, Daze, and tether functions. `/Game/PairPressure/Core/Interfaces/BPI_PP_TetherPresentationProvider` exposes read-only team, distance, normalized tension, broken, and connected state from tether actors.
- `/Game/PairPressure/Player/Character/BP_PP_PlayerCharacter` now fills its existing `BPI_PP_UIDataProvider` player/team outputs from replicated Pair Pressure variables instead of returning placeholder defaults. `BPI_PP_TeamMember.GetHomeState` supplies partner-home state without a concrete character cast, and `BPI_PP_UIDataProvider.GetPartnerDirectionPresentation` supplies a relative signed bearing or `PARTNER SEARCHING` when no partner is valid.
- `/Game/PairPressure/Player/Controller/BP_PP_PlayerController` reads only the locally controlled pawn through the data-provider interface and relays presentation through `BPI_PP_PlayerHUDView`. It also implements `BPI_PP_HUDModeProvider.UseAttachedHUD`; the base implementation returns false and selects the standard Designer root, while preserving the same downstream presentation contracts.
- `/Game/PairPressure/Player/Controller/BP_PP_PlayerController_Attached` overrides only `UseAttachedHUD` to return true. `/Game/PairPressure/Core/GameModes/BP_PP_AttachedGameMode` selects that child controller, so mode ownership is isolated without duplicating controller logic. Both assets and the shared controller selection graph were compiled and saved.
- `/Game/PairPressure/UI/HUD/WBP_PP_HUD_Root` forwards presentation to its modular child widgets through the same interface, without concrete widget casts. The child instances are Designer variables. `/Game/PairPressure/UI/HUD/WBP_PP_HUD_Attached` implements the same tether relay and is the class selected by the Attached controller path.
- Team, item, home, Daze, and tether panels keep their layout, typography, colors, and text targets in UMG Designer. Graph events only update transient presentation fields bound to the existing Designer controls. `WBP_PP_DazeSignal` now includes a Designer progress bar and numeric `DAZE {Value} / 100` label bound to the same normalized replicated value as the physical-state presentation.
- `/Game/PairPressure/UI/Modes/Attached/WBP_PP_TetherStatus` binds its Designer progress bar and text blocks to transient values. The tether link computes distance and normalized tension from its existing authoritative endpoints and thresholds; the local controller selects the matching team provider before presenting it.
- Relative teammate bearing, partner-home state, and numeric Daze treatment are authored through interfaces and Designer controls. Live two-player bearing/home and two-endpoint tether transitions remain follow-up validation work.

## Round Completion and Results

- `/Game/PairPressure/Core/Flow/BP_PP_RoundDirector` accepts results only during Run, rejects a duplicate team ID, stores the first finisher, starts a 20-second post-first-finish clock, and completes when all required teams finish or the authoritative run clock expires.
- `/Game/PairPressure/Course/BP_PP_HomeFinishZone` records completed teams independently, clears only the current occupant group after submission, and reopens for the other team.
- `/Game/PairPressure/Core/Interfaces/BPI_PP_ResultsView` carries the winning team ID through an interface relay. The local controller polls the round-flow interface, the HUD root forwards presentation to its child, and `/Game/PairPressure/UI/Modes/Results/WBP_PP_Results` changes existing Designer-authored `WinnerText` and `SummaryText` blocks for Team 1, Team 2, or timeout.
- The Results layout, fonts, spacing, and images remain Designer-owned; graphs only route state and update text/visibility.

## Multi-Round Match Flow

- `/Game/PairPressure/Core/Flow/BP_PP_RoundDirector` owns `CurrentRoundNumber`, `TotalRounds` (default 5), per-team scores, and `bMatchComplete`.
- Completing a round adds one point only when the winning team ID is 0 or 1; a timeout result does not award a point.
- Non-final Results phases use their existing 12-second clock, increment the round, enter Build, wait the authored Build duration, and then enter Run. Round five remains in Results and marks the match complete.
- `BPI_PP_RoundFlow.GetMatchState` exposes match data without a direct director dependency. The controller relays it through `BPI_PP_ResultsView.PresentMatchState` to the HUD root and Results child.
- `/Game/PairPressure/UI/Modes/Results/WBP_PP_Results` contains a Designer-authored `MatchScoreText` and Designer-bound continue label. Runtime text reports the current round/total and 0-1 team score, then changes from `NEXT ROUND AUTO` to `MATCH COMPLETE` on the final round.

## Final Awards and Photo Moment

- `/Game/PairPressure/Core/Interfaces/BPI_PP_AwardRowView` keeps Results-to-row presentation independent from the concrete award-row widget class.
- `/Game/PairPressure/UI/Components/WBP_PP_AwardRow` owns its title/value text in Designer and exposes only transient bound presentation values.
- `/Game/PairPressure/UI/Modes/Results/WBP_PP_Results` changes `POST-ROUND AWARDS` to `POST-MATCH AWARDS` when `Match Complete` is true, fills the three existing Designer award rows with both team scores and the current/total round audit, and changes the separate Designer photo callout from `UNLOCKS AFTER ROUND 5` to `HOLD STILL, IDIOTS`.
- This is a presentation-ready photo moment, not yet an image-capture/storage system. Individual-player award metrics and actual camera/photo capture remain follow-up work.

## Per-Round Build Reset

- `BPI_PP_BuildFlow.ResetBuildState` and `BPI_PP_BuildSocketContract.ResetBuildSocket` divide reset ownership between the build director and sockets.
- `/Game/PairPressure/Core/Build/BP_PP_BuildDirector` tracks only obstacles spawned by accepted build placements. On the next Build phase, its authority-gated reset destroys those runtime instances, clears the tracking array, resets placement/selection state, and sends reset messages to the sockets.
- `/Game/PairPressure/Course/BP_PP_BuildSocket` clears its occupying actor reference and occupancy flag when reset. Editor-placed assets and blockout actors are not deleted.
- `/Game/PairPressure/Core/Flow/BP_PP_RoundDirector.StartBuildPhase` discovers build-flow providers through the interface and requests reset before publishing the Build phase.
- PIE runtime inspection verified `PlacedCount=0`, `SelectedChoiceIndex=-1`, an empty spawned-actor tracking array, and cleared socket occupancy with no scoped errors. Automated focus/click input could not reliably target the Build overlay because this playtest path omits Slate geometry; an end-to-end `place three -> reset -> place again` UI pass remains a focused manual/multiplayer validation.

## Finish-State Reset and Rematch

- `BPI_PP_FinishParticipant` now owns `SetHomeState` and `ResetFinishState`. The player character updates its home flag on zone enter/exit, marks notified winning players as team-finished, and clears both flags at the next Run start.
- `BPI_PP_FinishZoneReset.ResetFinishZoneState` gives the round director a narrow reset contract for remembered team IDs, transient occupants, active team, and completion state. The home zone notifies only its current winning-player array through `OnTeamFinished` instead of broadcasting winning state to every finish participant.
- `BP_PP_RoundDirector.StartRunPhase` clears its own finish tracking, requests all finish-zone resets, requests all finish-participant resets, then publishes Run. This closes the prior gap where the zone's remembered team IDs could survive into round two.
- `BPI_PP_RoundFlow.RequestMatchRestart` is authority-gated in the director. It resets the match to round 1, 0-0, not complete, no winner, enters Build, and schedules the normal Build-to-Run transition.
- The existing Designer `ContinueButton` displays `REMATCH` only after match completion. Its bound click event discovers the round-flow provider through the interface and sends the restart request; non-final clicks do not request a rematch.
- A round-five 3-2 PIE probe invoked the same rematch interface and verified round 1, 0-0 scores, `bMatchComplete=False`, Build enum state, cleared character finish flags, and no scoped runtime errors. The actual mouse-click path remains a Slate-geometry validation.
- After a clean editor restart, the home-finish-zone structural edits were recovered and compile-validated. `PlayersHome` is a valid `Actor` array; the compiler failure came from three stale output wires on an obsolete, unexecuted `For Each Loop`, not from the variable. Those stale wires were disconnected without deleting the old node. A second graph read confirmed that only the `PlayersHome` loop drives `OnTeamFinished` and continuation, then `/Game/PairPressure/Course/BP_PP_HomeFinishZone` compiled and saved with 0 errors and 0 warnings. Historical blocked-compile report: `arep_ae26fbab80bb489484a0f941b15619ee`.

## Native Rescue Loop (Staged, Not Yet Loaded)

Native feature components are under `Source/VNHSimulator/{Public,Private}/PairPressure`:

- `UPPTeamMemberComponent`: replicated team/partner/home state.
- `UPPPhysicalStateComponent`: replicated Daze and physical-state machine, hybrid ragdoll, natural recovery, and teammate revive progress.
- `UPPImpactSensorComponent`: server-only capsule/mesh collision normalization with head/heavy-obstacle weighting and hit cooldown.
- `UPPCarryComponent`: server-authoritative teammate targeting, drag force, and revive contribution.
- `UPPPlayerActionRouterComponent`: thin input-to-feature routing for dive and assist.

These components are attached to `AVNHShopperCharacter` but activate only in maps whose name contains `PP_`, preventing inherited TNG/VNH maps from running Pair Pressure polling or feature ticks.

The shared shopper/controller source also suppresses legacy Composure, automatic fart events, role HUD, suspect HUD, and Composure HUD in `PP_` maps. This was added after the two-player runtime log showed inherited Composure draining and firing repeatedly during the Pair Pressure course. Native Pair Pressure HUD creation additionally requires an attached local player, avoiding widget creation on debug controllers without a `ULocalPlayer`. The asset-only `/Game/PairPressure/Player/Controller/BP_PP_PlayerController` keeps its existing `Is Local Controller` guard; the legacy controller-ID node cannot be used as a `ULocalPlayer` proxy because Enhanced Input/platform-user PIE returns `-1` for a valid local player.

## Launch Item Prototype Behavior

- `/Game/PairPressure/Items/Football/BP_PP_Football`: `ExecutePrimary` now detaches, restores collision/physics, reads aim through `BPI_PP_ItemUser`, launches at 1650 uu/s velocity-change impulse, and clears the holder through the same interface. Authoritative hit handling maps real normal impulse from 10,000-90,000 into 10-100 severity and sends location/direction/instigator through `BPI_PP_ImpactReceiver`.
- `/Game/PairPressure/Items/Plunger/BP_PP_Plunger`: authority-only primary use traces 2,500 units along interface-provided aim and applies a strong reverse pull to a hit `BPI_PP_TetherEndpoint`, providing the rescue-line proof without a concrete character cast.
- `/Game/PairPressure/Items/Cone/BP_PP_TrafficCone`: primary use detaches and deploys the cone as a non-simulating collision blocker, then clears the holder through `BPI_PP_ItemUser`.
- `/Game/PairPressure/Items/BowlingBall/BP_PP_BowlingBall`: primary use launches a slower 850 velocity-change roll/throw; authority-only hits map normal impulse from 5,000-60,000 into stronger 25-100 severity and deliver it through `BPI_PP_ImpactReceiver`.
- `/Game/PairPressure/Items/Extinguisher/BP_PP_Extinguisher`: `ExecutePrimary` applies a strong reverse propulsion burst to the holder through `BPI_PP_TetherEndpoint`, avoiding a concrete character cast.
- `/Game/PairPressure/Items/Umbrella/BP_PP_Umbrella`: `ExecutePrimary` applies an upward lift/glide burst through the same force-delivery interface, allowing it to compose with fan and tether forces.
- `/Game/PairPressure/Items/Wildcard/BP_PP_ShoppingCart`: primary use detaches the cart and applies a high-momentum 1,800 velocity-change shove along interface-provided aim as the transport/wildcard proof.

These are asset-only interaction proofs. A short PIE smoke pass loaded all four newly changed item classes from the prototype map with no scoped runtime errors. Direct interaction/replication tuning and client-to-server use requests still need to move into the staged native action/item authority path after a clean build.

## Active Obstacle Prototype Behavior

- Rotating hammer: active rotation plus impact-interface delivery.
- Giant fan: active blades plus a 1,200 x 600 x 600 authored wind volume; authority-only entrants receive a forward force through `BPI_PP_TetherEndpoint`.
- Seesaw launcher: left/right authored weight sensors tilt the platform toward the occupied side and reset on exit.
- Swinging sofa: active shared rotating platform blockout.
- Rolling drum: active rotating traversal blockout.
- Piston wall: authored timeline asset retained, but the prototype map uses a static blockout until the private timeline-curve save defect is fixed.

Authority rules:

- Clients may request a bounded 10-Daze dive.
- Clients cannot submit arbitrary impact packets.
- Environmental collision severity, Daze changes, state changes, drag force, and revive progress are server-owned.
- Normal unconscious recovery defaults to 10 seconds; teammate revive defaults to 3 seconds and returns the player at 70 percent Daze.

Input staging in `Config/DefaultInput.ini`:

- Dive: Left Ctrl / gamepad face-right.
- Assist: E / gamepad face-left.
- Grab/assist prototype: Left Mouse / right trigger.

No Live Coding was run for this source. A clean restart/build is required before these classes can be loaded or tested in PIE.

## Collaboration Contracts

Blueprint interfaces live under `/Game/PairPressure/Core/Interfaces` and cover team membership, finish participation, round flow, results presentation, build flow/socket state, tether endpoints, and usable items.

Native contracts live in `PPGameplayInterfaces.h` and cover physical state, impact receiving, team membership, carrying, UI data, and interaction prompts.

Preferred ownership boundaries:

- Character/Controller: component creation and thin input routing only.
- Gameplay components: state and authority rules.
- Course actors/items: implement interfaces; do not cast to a concrete player Blueprint.
- Widget Blueprints: all layout, images, fonts, and responsive Designer work.
- Native/Blueprint widget logic: orchestration and data updates only; avoid per-frame property bindings for the production HUD.

## GDD Coverage Matrix

| GDD area | Status | Next validation/implementation |
|---|---|---|
| Four players / two teams | Prototype authored | Focused 4-client PIE validation |
| Alternating build choices / 3 sockets | Prototype plus authority-gated per-round director/socket reset authored | End-to-end round-2 UI placement, network ownership, and rejection feedback |
| Bring Home team finish | Both-team completion, duplicate rejection, character home/team flags, interface reset ownership, and clean-session finish-zone compile authored | Round-2 finish and dragged unconscious teammate test |
| Daze / unconscious / natural recovery | Native staged | Clean build, ragdoll replication PIE test |
| Teammate drag / revive | Native staged | Clean build, distance/latency tuning |
| Context grab | Partial | Split item grab from teammate assist and add server trace validation |
| Attached tether | Prototype authored with read-only team/tension/break presentation contract, Designer HUD relay, interface-selected Attached controller, and mode-specific GameMode | Author/save the mode-specific map override; PIE-prove the Attached HUD; four-client tension/break/reconnect test; pivot integration |
| Obstacles | Five active prototypes plus static piston fallback | Replication/tuning, continuous fan wind, coordinated seesaw weighting, piston tool fix |
| Items | Seven distinct primary-behavior proofs authored | Direct interaction/replication tuning, server-routed use requests, secondary behaviors, cooldown/data assets |
| Minimal HUD | Live teammate state/link/bearing contract, partner-home query, tether baseline/tension contract, held-item, home guidance, and Designer numeric/physical Daze presentation use interface relays | Validate live two-player bearing/home and two-endpoint tether transitions |
| Results/awards/photo moment | Winner/timeout, live match score, interface-driven team score/audit rows, and final photo-moment callout authored in Designer | Add individual-player metrics and actual photo capture/storage |
| Multi-round scoring | Five-round aggregation, timed Results→Build→Run, build/participant resets, authority-gated rematch, and Designer REMATCH request authored | Clean-restart finish-zone compile; validate rematch click and round-2 placement/finish in multiplayer |
| Lobby/cosmetics | Legacy systems reusable | Pair Pressure-specific lobby rules and team presentation |

## Safe Next Editor Session

1. Save all unrelated open assets, close the editor normally, and perform a standard clean build (not Live Coding).
2. Reopen the project and compile `/Game/PairPressure/Player/Character/BP_PP_PlayerCharacter` plus the HUD assets.
3. Run a short two-player rescue test: dive, repeated impacts, unconscious threshold, natural recovery, teammate drag, teammate revive.
4. Run a separate four-player test for alternating teams, both tether links, build ownership, item pickup/drop, and team finish.
5. Capture logs/screenshots and tune only after authority and replication behavior are confirmed.

Attached mode map handoff: duplicating `PP_FriendshipPhysics` for a mode-specific map succeeded only in memory; the subsequent editor `load_level` call access-violated and rolled the duplicate back, and later editor-control requests were cancelled before execution. There is therefore no persisted `PP_AttachedPrototype` map yet. Do not claim the mode selection is runtime-validated until a fresh editor session creates/saves the duplicate, sets `WorldSettings.DefaultGameMode` to `BP_PP_AttachedGameMode`, and confirms a live `WBP_PP_HUD_Attached` instance in PIE.
