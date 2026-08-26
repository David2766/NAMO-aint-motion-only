# Documentation Index

Start here when deciding which project document to read or update.

## Current Contracts

- [api-contract.md](api-contract.md): Stable HTTP API response shape, status code rules, error code registry, and frontend/firmware compatibility rules.
- [firmware-signing.md](firmware-signing.md): Signed OTA package, key custody, rotation, recovery, migration, and release gates.
- [api-contract.ko.md](api-contract.ko.md): Korean version of the API contract.
- [home-assistant-integration.md](home-assistant-integration.md): Current Home Assistant Site/Space model, entity behavior, diagnostics, migrations, and remaining release gates.
- [home-assistant-integration.ko.md](home-assistant-integration.ko.md): Korean version of the Home Assistant integration guide.

## Spatial Platform Design

- [platform/architecture.md](platform/architecture.md): Distributed Site, per-Floorplan Configuration Owner, bounded Fusion Group, metadata-only Site Leader, high-rate Group Leader, bounded quorum/lease authority contract, calibration, platform integration, and explicit current implementation boundaries.
- [platform/architecture.ko.md](platform/architecture.ko.md): Korean version of the spatial platform architecture.
- [platform/satellite-protocol.md](platform/satellite-protocol.md): Draft observation, trust, attestation, and conformance contract for NAMO Nodes and observer-only third-party Satellites.
- [platform/satellite-protocol.ko.md](platform/satellite-protocol.ko.md): Korean version of the Satellite protocol.
- [platform/implementation-roadmap.md](platform/implementation-roadmap.md): Phases, implemented distributed-control P0/P1 corrections, remaining physical verification gates, release slices, and the optional pre-v1.0 formal-model checkpoint.
- [platform/implementation-roadmap.ko.md](platform/implementation-roadmap.ko.md): Korean version of the implementation roadmap.
- [adr/0001-distributed-spatial-platform.md](adr/0001-distributed-spatial-platform.md): Accepted bounded-control architecture for distributed NAMO Node leadership, open observer-only Satellites, and manufacturer attestation without a closed Node gate.
- [adr/0001-distributed-spatial-platform.ko.md](adr/0001-distributed-spatial-platform.ko.md): Korean version of ADR-0001.
- [adr/0002-authenticated-observation-transport.md](adr/0002-authenticated-observation-transport.md): Accepted Phase 3B UDP-unicast, MessagePack, HMAC, and embedded-resource transport boundary.
- [adr/0002-authenticated-observation-transport.ko.md](adr/0002-authenticated-observation-transport.ko.md): Korean version of ADR-0002.
- [adr/0003-participant-credential-storage.md](adr/0003-participant-credential-storage.md): Implemented and native-tested Phase 3C transactional participant-credential storage and embedded-resource boundary.
- [adr/0003-participant-credential-storage.ko.md](adr/0003-participant-credential-storage.ko.md): Korean version of ADR-0003.
- [adr/0004-secure-community-pairing.md](adr/0004-secure-community-pairing.md): Accepted Phase 3C bounded P-256 community pairing and local confirmation workflow.
- [adr/0004-secure-community-pairing.ko.md](adr/0004-secure-community-pairing.ko.md): Korean version of ADR-0004.

## Superseded Design Notes

- [multi-sensor-contract.md](multi-sensor-contract.md): Pointer from the former fixed-primary proposal to the current platform design.
- [multi-sensor-contract.ko.md](multi-sensor-contract.ko.md): Korean version of the superseded note.

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
- `tools/platform-sim`: Host-side V1 spatial protocol fixtures, validation, bounded authenticated datagrams, golden vectors, stream simulation, and MessagePack benchmarking. It is not a network service or Home Assistant integration.
- `tools/namo-cluster`: Two-to-seven-process host integration for production coordinator, persisted configuration, UDP fault, restart, partition, and rejoin testing. It does not yet execute firmware HTTP pairing/calibration handlers or replace hardware testing.
- `scripts/namo-failure-test.ps1`: Physical multi-Node role display, timestamped endpoint capture, operator markers, and post-capture analysis. The implementation roadmap defines the commands and evidence boundary.

## Editing Rules

- Keep `api-contract.md` focused on rules that current code should obey.
- Put future designs in separate future/design documents.
- Mark planned platform behavior clearly and do not list draft endpoints as implemented APIs.
- Keep English and Korean versions in separate files.
- When code changes affect an API response, update the API contract first or in the same change.
