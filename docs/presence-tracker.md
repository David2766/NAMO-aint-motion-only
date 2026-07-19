# Presence Tracker and Fusion

This document describes the production presence path between filtered LD2450
observations and the final presence and motion outputs.

## 1. Runtime Path

- The tracker path is enabled by default. The legacy lambda path remains an
  explicit fallback through the advanced legacy-presence setting.
- The 500 ms ESPHome lambda passes filtered LD2450 observations, PIR state,
  room/exit evidence, and optional LD2410C evidence into C++ components.
- `PresenceTracker` owns LD2450 track identity, lifecycle, position, velocity,
  direction, room crossing, and exit evidence.
- `PresenceFusion` combines PIR and tracker evidence and may use an optional
  LD2410C only to maintain a session that PIR or a confirmed LD2450 track
  already established.
- Outputs are exposed through `/api/state`, Home Assistant entities,
  diagnostics, and replay logs.

## 2. LD2450 Tracking

The tracker handles at most three observations and retained tracks, matching
the LD2450 hardware limit. It evaluates the small set of possible assignments
directly instead of using a general Hungarian implementation.

Tracks move through these states:

- `tentative`: awaiting enough recent observations.
- `confirmed`: contributes directly to presence and moving/still counts.
- `coasting`: temporarily retained after observations disappear.
- `idle`: unused slot.

Position and velocity use a constant-velocity Kalman filter with bounded time
steps, process noise, measurement noise, and covariance. A large teleport-like
measurement resets the filter instead of dragging the previous estimate across
the room. Confirmed and coasting tracks use a wider reassociation gate than new
tentative tracks.

`trackScore` is a bounded diagnostic score derived from recent hits, misses,
and lifecycle state. It is not a probability or a user-facing confidence
percentage.

## 3. Exit and Room Evidence

Each track records recent exit-zone hits and room-boundary state.

- A recent configured exit-zone hit can shorten coasting and produce
  `lost_after_exit`.
- A confident inside-to-outside room-boundary crossing can produce
  `lost_after_room_exit`.
- A disappearance without either signal produces `lost_without_exit` and uses
  the longer non-exit coasting policy.
- `filter_blocked` remains a missed-observation frame and does not reset every
  track immediately.

Current drop events are consumed by `PresenceFusion`. Confirmed exit drops veto
a new LD2410C hold for that loss event.

## 4. Presence Fusion

PIR and LD2450 are complementary acquisition evidence. Either PIR motion or a
confirmed/coasting tracker result can keep the base presence path active;
motion is the OR of PIR motion and tracker motion unless filtering blocks it.

LD2410C assistance follows these rules:

- Missing hardware is ignored through `available: false`.
- Static radar alone cannot start presence, motion, target counts, room state,
  zone state, or heatmap samples.
- Static presence must overlap an existing PIR/tracker session for 2 seconds
  before assistance is armed.
- When LD2450 disappears without confirmed exit evidence, an armed LD2410C may
  maintain presence.
- After stable presence ends without exit/filter veto, one 30-second reentry
  window allows the already armed assistance to recover that same session.
- PIR or LD2450 reacquisition resumes normal combined evidence without a
  primary/secondary sensor switch.

For display only, assistance may retain the last coordinate of exactly one
unambiguous confirmed or coasting LD2450 track. This retained coordinate does
not become an active target and is excluded from counts, zones, rooms,
calibration, and heatmaps.

## 5. Static Distance Stabilization

`detectionDistanceMm` is the only LD2410C distance used for held-target display
confidence.

- At most one valid sample per second enters a three-sample median window.
- The first valid sample is available immediately.
- A single spike cannot replace the stable value.
- Brief invalid distance samples retain the last stable value while static
  presence remains active.
- Static presence clear or sensor unavailability resets the distance history.
- Moving/still distances and energies remain raw diagnostic telemetry and are
  not combined to infer position.

## 6. Validation and Limits

- Native tests cover lifecycle, assignment, Kalman smoothing, exit/room drops,
  fusion arming, exit veto, reentry, missing hardware, held-target confidence,
  and distance stabilization.
- Replay logs distinguish raw observations, production-filtered observations,
  tracker output, exit evidence, static-radar input, and fusion state.
- BLE nearby and sleep schedules are not acquisition inputs.
- Track policy constants and LD2410C distance matching still require validation
  against varied real installations.

Future experiments live in
[presence-tracker-future.md](presence-tracker-future.md).
