# Two to Tangle Match HUD MVP

## Integration decision

The project module remains `VNHSimulator`; the requested `TTT` classes live under
`Source/VNHSimulator/{Public,Private}/TwoToTangle`. Creating a second Unreal module
would duplicate build/runtime ownership and is not required for feature isolation.

The MVP reuses the existing Pair Pressure gameplay foundation:

- `UPPPhysicalStateComponent` remains the authoritative Daze/physical-state owner.
- `UPPCarryComponent` remains the authoritative teammate-assist owner.
- `UPPGrabberComponent` remains the authoritative held-target/throw owner.
- `UPPTeamMemberComponent` remains the replicated team/partner owner.
- Existing Pair Pressure finish zones and round flow remain the gameplay entry points.

The TTT layer adds presentation-safe contracts, synchronized race timing/ranking,
event routing, and UMG-native parent classes. It must not add a second Daze, carry,
or held-item state machine.

## Runtime ownership

| Owner | Component | Responsibility |
| --- | --- | --- |
| `AVNHGameState` | `UTTTRaceStateComponent` | Replicated phase and server countdown timestamp |
| `AVNHGameState` | `UTTTRaceClockComponent` | Replicated start/finish timestamps and official elapsed time |
| `AVNHGameState` | `UTTTRaceRankingComponent` | Deterministic team ordering from finish/progress data |
| `AVNHGameState` | `UTTTFinishTrackerComponent` | Idempotent two-player team completion and elimination |
| `AVNHShopperCharacter` | `UTTTPlayerHUDSourceComponent` | Presentation-safe aggregation and native delegate source |
| `AVNHPlayerController` | `UTTTMatchHUDPresenterComponent` | Local widget lifetime, subscriptions, and results transition |

Core classes only construct/reference these components. Race, Daze, carry, item,
and result rules remain inside components.

## Existing Blueprint integration

`/Game/PairPressure/Player/Controller/BP_PP_PlayerController` currently creates the
legacy root from this execution chain:

`BeginPlay -> Delay -> Is Local Controller -> UseAttachedHUD -> Create Widget -> AddToPlayerScreen`

After the native presenter is loaded and its Designer classes are configured, only
the execution connection into the legacy creation chain should be disconnected.
The Blueprint Tick chain must remain intact until its unrelated Build-phase input
mode behavior is moved or explicitly replaced.

## MVP delivery phases

1. Native contracts and synchronized state
   - TTT enums/structs/interfaces
   - race phase, clock, ranking, finish tracker
   - carried Daze recovery threshold and action-permission contract
2. Event-driven presentation
   - player HUD source
   - local presenter
   - debug console commands
3. Designer assets
   - native-parent child widgets
   - native-parent match root
   - config/style data assets
   - no Widget Blueprint polling or gameplay casts
4. Existing-flow bridge
   - retire only the legacy HUD creation branch
   - route authoritative round start/finish calls into race components
5. Validation
   - C++ Development Editor build in Visual Studio
   - Widget Blueprint compile/save/re-read
   - preview screenshots at representative aspect ratios
   - user-authorized listen/dedicated multiplayer testing

## Native build policy

Structural reflection changes in this project must not use Live Coding. The user
owns Visual Studio builds. Use `Development Editor | Win64` for a normal editor
launch, or launch the matching `DebugGame Editor` target directly from Visual
Studio. Verify `/Script/VNHSimulator.TTTMatchHUDWidget` in reflection before
creating or reparenting production widgets.

## Current implementation status

- Native TTT contracts, race components, HUD source/presenter classes, widget
  parent classes, and debug commands are staged in source.
- The native presenter is disabled by default. The established
  `WBP_PP_HUD_Root` creation path and `PairPressureHUDWidget` assignment remain
  active so adaptive controller prompts continue to work.
- Production TTT UMG children, match-root Widget Blueprint, config/style data
  assets, football interface wiring, authoritative countdown bridges, and finish
  tracker Blueprint calls are not committed yet.
- Fully-Dazed players no longer auto-recover. Carried recovery is authoritative,
  stops in the green zone, and supports a server-validated Jump dismount.

The native source previously completed a Development Editor build, but the
corrective source changes still require a new normal Visual Studio Development
Editor build. No production TTT Widget Blueprint or race-integration validation
is claimed. Do not use Live Coding for these structural reflection changes.
