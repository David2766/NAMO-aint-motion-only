# Documentation Index

Start here when deciding which project document to read or update.

## Current Contracts

- [api-contract.md](api-contract.md): Stable HTTP API response shape, status code rules, error code registry, and frontend/firmware compatibility rules.
- [api-contract.ko.md](api-contract.ko.md): Korean version of the API contract.

## Future API Design

- [multi-sensor-contract.md](multi-sensor-contract.md): Future multi-sensor and coordinator API notes.
- [multi-sensor-contract.ko.md](multi-sensor-contract.ko.md): Korean version of the multi-sensor notes.

## Presence Tracking

- [presence-exit-zone.md](presence-exit-zone.md): Current exit-zone and room-boundary evidence used by the production tracker.
- [presence-exit-zone.ko.md](presence-exit-zone.ko.md): Korean version of the current exit-evidence behavior.
- [presence-tracker.md](presence-tracker.md): Current tracker responsibilities, boundaries, and implementation notes.
- [presence-tracker.ko.md](presence-tracker.ko.md): Korean version of the current tracker notes.
- [presence-tracker-future.md](presence-tracker-future.md): Future validation and tuning work that preserves the current acquisition boundary.
- [presence-tracker-future.ko.md](presence-tracker-future.ko.md): Korean version of future tracker notes.

## Replay and Simulation

- [presence-replay.md](presence-replay.md): Replay log, ground-truth labeling, scoring, and tool shape.
- [presence-replay.ko.md](presence-replay.ko.md): Korean version of replay notes.
- [presence-simulation.md](presence-simulation.md): Natural motion and Monte Carlo simulation notes.
- [presence-simulation.ko.md](presence-simulation.ko.md): Korean version of simulation notes.

## Editing Rules

- Keep `api-contract.md` focused on rules that current code should obey.
- Put future designs in separate future/design documents.
- Keep English and Korean versions in separate files.
- When code changes affect an API response, update the API contract first or in the same change.
