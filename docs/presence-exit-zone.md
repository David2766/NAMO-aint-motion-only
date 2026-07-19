# Presence Exit Evidence

This document describes the production exit evidence used when an LD2450 track
disappears. Exit evidence helps distinguish a likely departure from a temporary
loss of a still or low-motion person.

## 1. Product Rule

```text
target disappears after recent exit evidence
=> use the short exit coasting path

target disappears without exit evidence
=> use the longer non-exit coasting path
```

Exit areas are evidence, not filters. Entering an exit area does not remove a
target and does not turn presence off while the target is still observed.

## 2. User Configuration

The dashboard and stored zone model support these zone types:

```ts
type WebZoneType = "detection" | "filter" | "reduced" | "disabled" | "exit";
```

Users may place optional exit areas over doors, passages, or other likely
departure paths. A device continues to work without any configured exit area.

## 3. Tracker Evidence

Filtered target observations carry an exit-zone bit mask into
`PresenceTracker`. Each retained track records its latest mask and timestamp.
The default recent-evidence window is 8 seconds.

When floorplan room context is available, the tracker also watches signed
distance from the current room polygon. A confident inside-to-outside crossing
creates room-exit evidence. The confidence margin comes from Kalman position
covariance plus measurement noise so a point hovering near the boundary is not
treated as an exit by itself.

## 4. Drop Behavior

When a confirmed track loses observations it enters `coasting`.

- Recent configured exit-zone evidence uses the short exit limit and produces
  `lost_after_exit` when the track expires.
- Confirmed room-boundary crossing produces `lost_after_room_exit`.
- No exit evidence produces `lost_without_exit` and uses the longer non-exit
  limit.
- A target observed near an exit but not crossing or disappearing remains a
  normal active track.

The default missed-frame limits are policy values, currently 4 for recent exit
evidence, 12 for the general/room-exit path, and 24 when no exit zone has been
seen. They are implementation defaults rather than API guarantees.

## 5. Fusion Effect

`PresenceFusion` consumes only current tracker drop events. A new
`lost_after_exit` or `lost_after_room_exit` event vetoes LD2410C assistance for
that loss and clears any retained display coordinate. Stale drop reasons are
not reused after PIR or confirmed LD2450 reacquisition.

LD2410C does not decide whether a departure occurred. It can only maintain an
already armed session when no confirmed exit/filter veto is active.

## 6. Diagnostics and Replay

`/api/state`, diagnostic logs, and replay data expose exit masks, recent exit
age, room state, tracker drop reason, and fusion exit veto. Native tests cover:

- disappearance after a configured exit area;
- inside-to-outside room-boundary crossing;
- disappearance without crossing;
- stale drop events after reacquisition;
- LD2410C reentry blocked by exit or filter evidence.

## 7. Boundaries

- Exit areas must not behave like filter zones.
- Exit setup must remain optional during onboarding.
- Multi-sensor, Home Assistant, and BLE evidence are not required for an exit
  decision.
- Real installation replay data should be reviewed before changing coasting
  defaults.
