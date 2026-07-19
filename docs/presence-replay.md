# Presence Replay

This document defines the test workflow for presence reliability work.
The goal is to stop relying on long manual sitting tests and move toward
repeatable replay, scoring, and simulation.

Replay diagnostics record production inputs and outputs but do not change
production behavior by themselves.

## 1. Goal

The device should be tested with repeatable input streams instead of requiring a
person to sit in front of the sensor for every tracker change.

The replay workflow should answer questions such as:

- Did presence turn off while the room was occupied?
- Did presence stay on while the room was empty?
- Did target identity or tracker state fragment during small motion?
- Did motion/still/direction states flap too often?
- Did a proposed tracker change improve one case while breaking another?

## 2. Test Layers

### 2.1 Firmware Smoke Test

This remains necessary, but it should be small.

Use it to confirm:

- The firmware boots.
- The API remains responsive.
- Diagnostics download works.
- Presence still works in obvious near-field cases.
- No crash or rollback loop appears.

This should not be the main way to tune tracker behavior.

### 2.2 Real Log Replay

The device captures a compact raw replay log that can be downloaded and
replayed on a PC.

The replay log must include the input signals needed to reconstruct tracker
behavior, not only event summaries.

Endpoint:

```text
GET /api/diagnostics/replay.ndjson
```

The endpoint returns newline-delimited JSON. If the endpoint fails, it
must follow `docs/api-contract.md`.

Each row should represent one sampling tick. The preferred interval is the same
as the production presence tick.

Example compact row:

```json
{"q":12,"t":123456,"p":0,"lx":50.2,"r":[[1,1240,1810,2,2195,0,0,1],[0,0,0,0,0,0,0,0],[0,0,0,0,0,0,0,0]],"tg":[[1,1240,1810,2,2195],[0,0,0,0,0],[0,0,0,0,0]],"sr":[1,1,0,1,0,1850,0,67,1850],"sf":[0,0,1,1,0,0,0],"f":[0,0,0,0,0],"ex":[0,0,0,-1],"l":[1,0,1,1,0,1,82,0,9,0,3,2,4],"tr":[0,0,0,0,0,0,0,0,0,0,0,0]}
```

Minimum fields:

- `q`: sample sequence.
- `t`: device uptime in milliseconds.
- `p`: PIR motion state.
- `lx`: illuminance if available.
- `r`: up to three LD2450 target slots before software/reduced/range filtering.
  Target tuple: `[valid, x, y, speed, distance, filterMode, filtered, rangeValid]`.
- `tg`: up to three target slots after the current production filter/range-gate path.
  Target tuple: `[valid, x, y, speed, distance]`.
- `sr`: optional LD2410C static-radar input tuple:
  `[available, presence, moving, still, movingDistanceMm, stillDistanceMm, movingEnergy, stillEnergy, detectionDistanceMm]`.
  The final field is the canonical stabilized distance used by presence fusion
  and dashboard positioning. Legacy eight-item tuples remain valid and decode
  `detectionDistanceMm` as `0`.
- `sf`: optional presence-fusion tuple:
  `[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending, exitVeto, armElapsedMs]`.
  Missing tuples default to zeros so older replay files remain valid.
- `f`: filter/range tuple:
  `[filterBlocked, rangeReasonCode, suspectCount, outOfRangeCount, remoteCandidateCount]`.
- `ex`: exit-zone evidence tuple:
  `[exitActive, exitZoneMask, exitTargetCount, exitLastSeenAgeMs]`.
- `l`: production presence-output tuple:
  `[presence, motion, still, targetCount, movingCount, stillCount, stillConfidence, emptySamples,
  presenceReasonCode, presenceOffReasonCode, motionReasonCode, stillStateCode, stillReasonCode]`.
- `tr`: tracker tuple:
  `[presence, motion, stateCode, reasonCode, score, inputCount, activeCount, tentativeCount,
  confirmedCount, coastingCount, movingCount, stillCount]`.

The replay log is RAM-only. It is diagnostic data, not user configuration.

### 2.3 Ground Truth Labels

For real logs, ground truth should be written manually as short segments.

Example:

```json
{
  "segments": [
    {"from": "21:18:00", "to": "21:25:30", "occupied": true, "note": "sitting at desk"},
    {"from": "21:25:30", "to": "21:40:00", "occupied": false, "note": "room empty"}
  ]
}
```

The first version only needs occupancy truth. Exact target coordinates are not
required.

Optional future labels:

- `zone`: desk, bed, bathroom, entrance, etc.
- `activity`: sitting, eating, walking, sleeping, cleaning robot, empty.
- `sensor`: room, bathroom, or primary coordinator in multi-sensor tests.

## 3. Scoring

The replay tool should compare replay output against ground truth and produce
numbers.

Initial metrics:

- `false_off_count`: number of occupied segments where presence turned off.
- `false_off_seconds`: total time presence was off while occupied.
- `false_on_count`: number of empty segments where presence turned on.
- `false_on_seconds`: total time presence was on while empty.
- `presence_fragment_count`: number of on/off fragments during occupied time.
- `reacquire_latency_ms`: time from target reappearance or PIR hint to presence.
- `motion_flip_count`: number of motion on/off transitions.
- `still_flip_count`: number of still confirmed/lost transitions.
- `tracker_state_flip_count`: number of tracker lifecycle transitions.
- `target_drop_count`: number of ticks where all radar targets disappeared while
  truth was still occupied.

Later metrics can borrow ideas from multi-object tracking evaluation:

- Misses and false positives from CLEAR MOT.
- ID switches and fragmentation.
- Localization error if coordinate ground truth becomes available.

## 4. Replay Tool Shape

The PC replay and native tracker tools live under:

```text
tools/presence-replay/
```

Available commands:

```text
python tools/presence-replay/replay.py logs/replay.ndjson --truth logs/truth.json
python tools/presence-replay/test_replay.py
python tools/presence-replay/native/test.py
python tools/presence-replay/native/build.py
```

`replay.py` scores existing firmware output. The native commands compile the
tracker runner and unit tests with `cl`, `g++`, or `clang++`.

The native runner executes the current Kalman-based tracker. Future experiments
may compare policy or noise-model variants, but replay files must remain
backward compatible.

## 5. Firmware Boundary

The firmware should only collect compact replay samples and expose downloads.

It should not run Monte Carlo simulation.

The 500 ms lambda should not grow. It only passes the current sample to the
dedicated diagnostics component.

## 6. Current Status

The firmware ring buffer, diagnostics endpoint, dashboard download, PC scorer,
native tracker runner, and native unit tests are implemented. Scenario and
Monte Carlo tools remain optional follow-up work. Production tracker changes
should continue to be checked against captured replay data.

## 7. LD2410C Fusion Fields

The `sr` tuple records LD2410C sensor input and the stabilized distance used by
the current fusion path:

```text
[available, presence, moving, still, movingDistanceMm, stillDistanceMm, movingEnergy, stillEnergy, detectionDistanceMm]
```

Legacy eight-item `sr` tuples default `detectionDistanceMm` to zero. The `sf`
tuple records fusion state:

```text
[pirEvidence, trackerEvidence, assistArmed, assistActive, armPending, exitVeto, armElapsedMs]
```

Replay files without `sf` default every fusion field to zero. LD2410C
assistance may maintain an already established presence session. Its stabilized
distance may only evaluate display confidence for the last unambiguous LD2450
position; it cannot acquire presence, motion, or a new spatial position by
itself.
