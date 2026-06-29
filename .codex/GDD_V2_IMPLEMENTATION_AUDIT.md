# VNH GDD v2 Implementation Audit

Source: `.codex/Very_Normal_Human_Simulator_3P_MVP_GDD_UE5_8_v2.docx`

## Sections 7 and 8 - Current Pass

### 7. Win Conditions

Status: implemented at MVP-readable level, still not final presentation.

- Hunter win: correct accusation resolves to Reveal and now presents a Hunter win line.
- Alien win: wrong accusation resolves to Reveal and now presents an Alien win line.
- Timeout/no resolved accusation now presents Alien win by social camouflage instead of a placeholder.
- False accusation now explicitly spotlights the wrongfully accused target and frames it as comedy without adding public-mode rewards that would encourage sabotage.
- Remaining gap: the reveal is still text/HUD driven. The GDD wants a more polished reveal screen and awards presentation later.

### 8. Role Feature Requirements

Status: core role contract implemented, with some UI/interaction depth still missing.

- Human: has a real role, gets private role/goal text, gets a light errand, can be a false positive, and now shares the same body locomotion component path as Alien.
- Alien: remains visually identical to Human in public play; private role text now says the player is pretending and must not get accused.
- Hunter: has limited tools: public commands, one direct question budget, target marking/fake pressure, and one accusation budget.
- Fixed issue: `EVNHPlayerRole` had a duplicate `Human` enumerator after merge resolution. Removed the duplicate.
- Fixed issue: body locomotion was gated by `IsPossessedByAlien()`, which meant Human players did not truly share Alien controls. It now gates out Hunter instead, so Human and Alien player bodies use the same public movement surface.
- Remaining gap: Direct Question is still a single automatic response, not the GDD's "one question with three quick choices" UI.

## Full GDD MVP Standing

### Solid / Present

- 3+ player role model: one Hunter, one Alien, remaining Humans.
- Private lobby/main menu groundwork and host start path.
- Store travel path from lobby to MVP clothing store.
- Round phases: role assignment, Alien setup/head start, investigation, accusation, reveal, reset.
- Hunter tool budgets: public commands, direct question, accusation.
- Role reveal/goal text and non-Hunter light errand text.
- Quick-chat presets and replicated latest quick-chat message.
- Shopper body locomotion with acceleration, turn arc, instability, and recovery-oriented Act Natural.
- NPC public-test auto reactions while player-controlled bodies must manually sell the reaction.
- Target lock, suspect marking, fake accusation pressure, and accusation path.

### Partial / Needs Polish

- Reveal and awards: functional and now better aligned with section 7, but still not a final AAA screen.
- Human role fun: errands and quick chat exist, but there is no robust per-human objective tracking or completion feedback.
- Direct Question: server budget exists; the actual player-facing three-choice interaction is not implemented.
- Suspicious civilian: concept exists in design/status, but the round does not guarantee one suspicious innocent NPC/player behavior.
- Lobby: functional path exists, but final ready states/session UX need validation.
- Store level: needs a disciplined pass against exact GDD counts, sightlines, routes, and prop interaction expectations.
- NPC system: routines and public-test reactions exist, but full StateTree richness and data-driven quirks remain incomplete.
- Scoring/awards: textual awards only; no persistent scoring, award selection logic, or ceremony.
- Networking: replicated basics exist, but real listen-server multi-client verification is still required.

### Missing / Not MVP-Complete

- VOIP/proximity voice plan.
- Public server guardrails beyond copy-level caution.
- Full rematch/role swap/reset UX.
- Final reveal with side-by-side characters and award presentation.
- Data-driven quirk/activity definitions.
- Three-choice direct-question UI with equalized Human/Alien response presentation.
- Suspicious innocent guarantee per round.
- Public/private session browser polish.

## Verification Notes

- `git diff --check` passed.
- Conflict-marker scan passed.
- UBT was not available on PATH from this shell, and no accessible `Build.bat` was found. Full Unreal compile still needs editor/UBT verification.
