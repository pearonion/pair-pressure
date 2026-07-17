# Two to Tangle Grab System

## Runtime ownership

- `IA_Grab` is the single hold action. `Started` begins a predicted local reach and a server request; `Completed` or `Canceled` releases immediately.
- `UPPGrabberComponent` owns authoritative selection, grab state, constraints, directional escape, reciprocal-grab cancellation, item throws, and release.
- `UPPGrabbableComponent` owns a target's editable `FPPGrabProfile` and current grabber.
- Standing-player grabs use movement forces. Ragdolled or unconscious players remain on `UPPCarryComponent` and are rejected by the standing-grab path.

## Target authoring

Preferred setup: add `PPGrabbableComponent` and edit its `GrabProfile`.

- `GameplayItem`: simulated primitive, optional scene component named `GripPoint` or tagged `GripPoint`.
- `Player`: installed natively on `AVNHShopperCharacter`; upper-spine/pelvis sockets are preferred automatically.
- `LargePushable`: simulated primitive with an appropriate `MaximumMass`, low `CarryDistance`, movement multiplier, and break force.
- `LedgeOrHandle`: static or movable primitive with an authored `GripPoint`; it is never lifted as an item.

Blueprint-only actors that already implement `BPI_PP_Grabbable` or `BPI_PP_GrabbableContract` remain supported. They default to item behavior and may expose an `FPPGrabProfile` variable named `GrabProfile`. Actor tags `PP.Grab.Pushable`, `PP.Grab.Ledge`, or `PP.Grab.Handle` select the non-item defaults.

## Presentation hooks

Bind to `OnGrabStateChanged` and read `GetPresentationGrabPoint` / `GetActiveGrabProfile` for hand IK and state-specific animation. Bind to `OnGrabFailed` for failed-reach feedback. The replicated states distinguish item hold, player grab, push, and hanging. Reciprocal player grabs cancel both sides instead of entering `MutualGrab`.

## Debug and validation

Enable `bDrawGrabDebug` on the grabber component to show the sphere sweep, rejected/valid candidates, score, selected point, constraint line, force, break threshold, hold duration, and immunity timing.

After native compilation, validate incrementally:

1. Item pickup/release, held movement, lag, collision, Q throw, mass and break limits.
2. One-way standing player grab, complete victim movement lock, item drop, airborne momentum, directional jump escape, dive rejection, LOS/force/distance breaks.
3. Reciprocal-grab cancellation, opponent escape, recoil, and same-opponent immunity.
4. Mass-scaled push/pull without lifting.
5. Ledge/handle grip, swing, lateral influence, state/force release.
6. Listen server plus remote clients under latency and packet loss; confirm no through-wall or over-range acceptance and tune profiles/animation/IK.
