# New 3+ Player GDD Pivot Assessment

Source GDD:

- `.codex/Very_Normal_Human_Simulator_Brand_New_3P_MVP_GDD_UE5_8.docx`
- `.codex/Very_Normal_Human_Simulator_Brand_New_3P_MVP_GDD_UE5_8.txt`

## Verdict

The new 3+ player GDD is a better product direction than the original 1v1 MVP if the goal is a funny, replayable social-deception game.

The main improvement is that Human players become active false positives. This solves a major weakness of the old 1v1 version: with only Hunter vs Alien, the hunter is mostly comparing player weirdness against NPC weirdness. With 3+ players, the round can create real social chaos even when NPC AI is simple.

## What Changes

Old target:

- 1 Hunter vs 1 Alien
- NPC crowd carries most false-positive work
- Alien possession is the main special flow
- Debug deck is acceptable during early iteration

New target:

- Minimum 3 players: 1 Hunter, 1 Alien, 1 Human
- Recommended 4-6 players
- Human and Alien share third-person civilian controls
- Hunter is first-person
- Lobby, host play button, private role reveal, store round, reveal, rematch
- Public/private listen-server flow matters earlier
- Debug deck must stop being the product UI

## Current Project Reuse

Keep and evolve:

- `AVNHGameMode`, `AVNHGameState`, `AVNHPlayerState`, `AVNHPlayerController`
- replicated role foundation
- round phase/timer foundation
- public test charge foundation
- accusation/reveal result foundation
- `AVNHShopperCharacter` as the first civilian/NPC base
- Manny visual/animation setup
- `UVNHAlienLocomotionComponent`, renamed/refactored later into shared civilian movement
- NPC routines/waypoints
- Freeze and Look/Clear prototype reactions
- target-lock interaction work
- MVP clothing store greybox

Demote or replace:

- UMG debug deck as primary UI
- `vnh.*` command-driven flow as the main test path
- single-player Alien-only assumptions
- 1v1 round state names like `AlienSetup` / `AlienHeadStart` as the final public flow

## Current Distance From New GDD

Approximate completion against new required MVP:

- M0 Project setup: partial
- M1 Lobby flow: missing
- M2 Role assignment: partial, currently 1v1/2-role biased
- M3 Store spawn: partial, but role-specific player spawns are not complete
- M4 Civilian movement: partial for Alien only; must become shared Human/Alien movement
- M5 NPC routine: partial
- M6 Public commands: partial, Freeze exists; Look Here needs GDD naming/behavior
- M7 Direct question: very early debug implementation only
- M8 Accusation: partial debug implementation
- M9 Fun pass: mostly missing

Overall: about 35-45% of a technical prototype foundation, but only about 20-30% of the new player-facing MVP.

## Recommended Pivot Order

1. Stabilize current uncommitted code and push a checkpoint.
2. Rename the target internally from 1v1 MVP to 3+ player MVP.
3. Add `Human` as a real role in the role enum and assignment logic.
4. Change role assignment to exactly one Hunter, one Alien, all remaining players Human.
5. Add minimum player validation for 3 players in normal start flow.
6. Create a lobby map and host-only `BP_LobbyPlayButton`.
7. Add a temporary role reveal UMG screen.
8. Split spawn behavior:
   - Hunter gets first-person pawn/control.
   - Alien and Humans get identical third-person civilian pawn/control.
   - NPCs use the same visual/animation baseline.
9. Refactor `UVNHAlienLocomotionComponent` into shared civilian movement or make it explicitly support Human and Alien.
10. Replace the debug deck with role HUDs:
   - `WBP_HunterHUD`
   - `WBP_CivilianHUD`
   - `WBP_RoleReveal`
   - `WBP_RevealScreen`
11. Implement required public commands:
   - Freeze
   - Look Here
12. Implement Direct Question as one server-validated target interaction.
13. Implement round reveal and immediate rematch/reroll.

## Near-Term Sprint

The next sprint should not add more debug buttons. It should build the new MVP spine:

- compile and push current checkpoint
- add Human role
- update role assignment
- add lobby state / minimum 3-player validation
- convert movement from Alien-only to shared Civilian
- create first pass role reveal widget
- create first pass Hunter/Civilian HUD split

## Risk

The biggest risk is trying to support the old 1v1 debug deck, the new 3+ player flow, and full polish at the same time. The project should commit the current work as a checkpoint, then deliberately pivot. Otherwise the code will keep accumulating one-off debug paths that fight the actual GDD.
