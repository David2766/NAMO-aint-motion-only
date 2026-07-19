# Presence Tracker Future Work

Future work must preserve the current acquisition boundary: PIR and LD2450 may
start presence, while optional LD2410C assistance may only maintain an already
established session.

## Next Validation Steps

1. Collect replay logs from sleeping, desk work, empty rooms, fans, HVAC, and
   confirmed exits across several installations.
2. Tune Kalman noise, association gates, and coasting limits from those logs
   instead of individual anecdotes.
3. Measure whether a sustained invalid LD2410C distance needs an expiry while
   preserving the current protection against brief unknown values.
4. Validate the 750 mm held-target distance tolerance against room size,
   orientation, and close multi-radar installations.
5. Compare false-off time, false-on time, fragmentation, and reacquisition
   latency before changing production defaults.

## Separate Features

- BLE nearby evidence must remain optional and must not silently become a
  required acquisition source.
- Sleep schedules should be a product mode with explicit user behavior, not an
  implicit tracker heuristic.
- Multi-device coordination belongs to
  [multi-sensor-contract.md](multi-sensor-contract.md), not this tracker.
