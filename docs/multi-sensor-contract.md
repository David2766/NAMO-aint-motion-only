# Multi-Sensor API Contract

Status: **Superseded design note. Not an implemented API contract.**

Korean version: [multi-sensor-contract.ko.md](multi-sensor-contract.ko.md)

This document previously proposed a fixed `primary` and `secondary` sensor
model. That model has been superseded by the distributed NAMO Spatial Platform
design.

Use these documents for future work:

- [Spatial Platform Architecture](platform/architecture.md)
- [Satellite Protocol](platform/satellite-protocol.md)
- [Implementation Roadmap](platform/implementation-roadmap.md)
- [ADR-0001: Distributed Spatial Platform](adr/0001-distributed-spatial-platform.md)
- [ADR-0002: Authenticated Observation Transport](adr/0002-authenticated-observation-transport.md)

The replacement design keeps the useful constraints from the original note:

- exchange structured observations instead of user-facing strings;
- expire stale observations with explicit TTL and ordering;
- preserve standalone local operation;
- require explicit sensor pose, Floorplan transform, and calibration before
  spatial fusion;
- keep confidence merging and conflict resolution out of HA and SmartThings.

It replaces the following assumptions:

- no physical NAMO Node is permanently configured as the automation `primary`;
- each Fusion Group contains at least one NAMO Node and elects a temporary
  Group Leader from approved NAMO Nodes with `term` and `lease`;
- Site, Space, Group, and platform entity identities do not depend on that
  Leader or its IP address;
- a capability-based open Satellite contract allows observer-only third-party
  sensors without granting them Leader or configuration authority;
- official NAMO hardware uses per-device manufacturer attestation while
  explicitly approved community NAMO Nodes remain supported;
- presence-only evidence may maintain an armed occupancy session but cannot
  start one or create a spatial target;
- the architecture separates a low-rate Site Leader from high-rate Group
  Leaders, targets at most eight Groups per Site and seven sensing participants
  per Group, and subjects both limits to the roadmap's resource gates;
- every active Floorplan has one independent Configuration Owner, and assigned
  Nodes pull authenticated chunks directly from it one transfer session at a
  time before committing the replica locally;
- the Site Leader stores only bounded Floorplan/Group owner, revision, route,
  and health metadata and never relays Floorplan payloads by role;
- platform integrations use the Site Leader as a Group directory and consume
  final logical Space state directly from each Group Leader.

The [current API contract](api-contract.md) remains authoritative for implemented
firmware endpoints. Planned cluster and Satellite endpoints must not be treated
as available until they are added there with implementation and tests.
