# ADR-0001: Distributed Spatial Platform, NAMO Nodes, and Open Satellites

Status: **Accepted architecture; bounded multi-Node verification remains active**

Date: 2026-07-21

Korean version: [0001-distributed-spatial-platform.ko.md](0001-distributed-spatial-platform.ko.md)

## Context

NAMO currently operates as an independent ESPHome device with local tracking,
presence logic, an embedded dashboard, Home Assistant entities, and a
SmartThings Edge Driver. Future spatial fusion must support sensors that observe
the same room from similar or different angles. It must also leave room for
multiple Floorplans, multiple Spaces, and multiple independently bounded
Fusion Groups without concentrating their high-rate work on one ESP32-S3.

A fixed primary device would make platform identity and availability depend on
one physical sensor. A mandatory server would simplify fusion but remove NAMO's
standalone and local-first property. Embedding every third-party driver into the
firmware would couple the platform to product-specific code and memory use.

NAMO also needs to distinguish officially manufactured hardware from compatible
community builds without turning an AGPL-licensed platform into a manufacturer-
locked protocol. Device provenance, protocol role, and observation capability
therefore need separate definitions.

## Decision

NAMO will be designed as a distributed local Spatial Platform with:

- stable Site, Floorplan, Space, Fusion Group, NAMO Node, Satellite, and shared
  participant identities;
- NAMO Node reserved for a device running the NAMO Core Runtime and Node contract;
- Satellite reserved for an approved external sensor or bridge that contributes
  observations only;
- at least one NAMO Node in every active Fusion Group;
- one dynamic Group Leader per Group, elected only from approved NAMO Nodes,
  receiving and fusing that Group's high-rate observations;
- one independent, low-rate Site Leader for the bounded Group directory and
  Site health, also restricted to approved NAMO Nodes;
- exactly one Configuration Owner per active Floorplan, with independent
  ownership, generation, and handover, and at most one active Floorplan owned by
  one physical NAMO Node;
- direct authenticated chunk pulls from each Floorplan's Configuration Owner to
  its assigned NAMO Nodes, with one active owner-side transfer and bounded
  deterministic waiting slots;
- standalone operation as a one-NAMO-Node Group;
- Group-local Observation exchange rather than Site-wide raw-data broadcast;
- an open, capability-based Satellite contract for third-party sensors;
- stable logical Space identities for Home Assistant and SmartThings;
- explicit pairing, trust, versioning, replay protection, and calibration;
- a design target of at most eight Fusion Groups per Site and one Group Leader
  plus at most six other sensing participants per Group, validated before being
  claimed as supported;
- direct Group-state reads by platform integrations after they resolve the
  current Group Leader through the Site Leader directory.

Official production hardware will receive a unique private key and device
certificate signed by a NAMO manufacturer root. Local challenge-response during
pairing establishes `official_verified` provenance. The private key never leaves
the device, and attestation does not grant Site membership, capabilities, Group
placement, or calibration by itself.

Community builds running the NAMO Core Runtime may enroll as
`community_unverified` after explicit local administrator approval. They may
receive Leader eligibility only through a separate opt-in grant. External
Satellites remain observer-only regardless of vendor certificate or protocol
conformance.

Site and Group authority use a bounded control-plane contract rather than a
general replicated-log consensus protocol. A role may become authoritative only
with a strict majority of the canonical configured membership, a matching
election-view digest, current term and lease fences, and complete credentials.
A minority partition keeps local sensing but clears distributed authority.
The current Site and Group election digest identifies the complete configured
canonical roster, its roster revision, and the selected winner. Liveness,
Directory assignment, and credential readiness remain separate admission
fences instead of mutable election identity. Configuration Owner handover or a
returning member therefore cannot disturb a surviving Leader merely because
replicas and heartbeats converge at different times. Heartbeat v12 carries this
contract and retains v11 winner-bound compatibility during rolling updates.
Role placement is deterministic and prefers anti-affinity: Configuration Owner
may share one Leader role when capacity requires it, Site Leader and Group
Leader are separated when another eligible Node exists, and one Node holds all
three roles only as an emergency fallback when its Group has no other eligible
Node. That placement yields as soon as an eligible peer appears. A Site Leader replacement ranks a
current canonical Group Leader after otherwise eligible role-free and
Owner-only candidates so an unrelated Site failure does not cascade into a
Group authority change.

This intentionally does not implement Raft or Paxos. Site configuration has a
single writer, while Site and Group leadership protect derived runtime state
that can be recomputed from current observations. The tradeoff is an explicit
verification burden: same-revision conflicts, stale rejoin, candidate loss,
role placement, persistence failure, and transient transport failure must be
covered by deterministic model and fault-sequence tests. A small TLA+ model may
be added after the executable contract stabilizes; it does not replace native,
multi-process, or physical-device tests.

Presence-only evidence cannot start or re-arm occupancy, create a spatial
target, increase target count, or mark a spatial Zone occupied. It may only
confirm or maintain a session already armed by accepted point-track or motion
evidence. This applies to LD2410C-style assistance and equivalent external
Satellite evidence.

The current local tracker remains the owner of local sensor acquisition and
track stabilization. Fusion consumes bounded local observations through an
adapter. Platform integrations consume final Group state and do not reproduce
fusion.

## Consequences

Positive:

- a single device and a multi-device installation share one model;
- no permanent coordinator is required for local operation;
- Leader replacement does not recreate automation entities;
- third-party sensors can integrate without modifying NAMO tracker internals;
- high-rate traffic and computation are scoped to a Fusion Group;
- Site coordination does not require the Site Leader to store or relay
  Floorplan payloads;
- each NAMO Node persistently stores only its assigned Floorplan;
- official devices receive verifiable provenance and streamlined local pairing;
- community NAMO builds remain possible under the same open source contract;
- observer-only Satellites cannot silently take control of Site configuration or
  final automation state.

Costs:

- identity, election, expiry, configuration conflict, and security rules become
  first-class implementation work;
- manufacturing needs per-device key provisioning and certificate records;
- root rotation, revocation, replacement, recovery, and secure key storage need
  lifecycle procedures;
- Home Assistant requires a NAMO Integration for stable logical Site and Space
  entities;
- SmartThings Edge must separate physical participants from logical Site/Space
  devices;
- calibration and time quality must be explicit before coordinates can be fused;
- multi-Floorplan configuration requires a versioned bounded owner directory and
  independent owner-handover state per Floorplan;
- the seven-input Group and eight-Group Site targets require separate simulator
  and hardware load validation.
- the bounded authority protocol requires explicit safety and liveness
  invariants plus repeatable failure/rejoin testing; deterministic election
  alone is not sufficient evidence.

## Rejected Alternatives

### Fixed Primary and Secondary Devices

Rejected as the long-term model because a physical Primary becomes an automation
identity, failure point, and scaling bottleneck. A temporary Group Leader is
still used, but it is authority for one term rather than permanent identity.

### Mandatory Central Server or Host Coordinator

Rejected as the default because a single NAMO must remain fully functional
without Home Assistant, SmartThings, a Docker host, or cloud service. A generic
host does not become a NAMO Node merely because it can run fusion code. Adding a
host authority later requires a separate ADR and must not silently weaken the
NAMO Node trust boundary.

### Full Peer Mesh

Rejected because Site-wide high-rate all-to-all traffic grows poorly and wastes
ESP32 resources. Discovery and configuration may be Site-wide; observations are
Group-local and directed to the current Group Leader.

### Firmware Plugins for Every Sensor

Rejected because product-specific dependencies would expand firmware size,
couple releases, and bypass a stable compatibility boundary. External adapters
must emit the Satellite contract instead.

### Fusion in HA or SmartThings

Rejected because the embedded dashboard, HA, and SmartThings would produce
different answers and availability semantics. The local Group state remains the
source of truth; platform adapters only expose it.

### Manufacturer Certificate as the Only Node License

Rejected because cryptographic provenance and protocol authority solve different
problems. Requiring a NAMO-issued certificate for every locally built Node would
weaken the practical meaning of the AGPL source release and the stated open
platform goal. Official attestation instead provides a verifiable status,
streamlined enrollment, and default Leader eligibility. Community Nodes remain
manual and visibly unverified.

### Attestation as Automatic Trust

Rejected because an authentic official device can still be assigned to the
wrong Site, Space, or Group, have stale firmware, or advertise unavailable
sensor capabilities. Explicit local pairing, assignment, capability validation,
and calibration remain mandatory.

## Revisit Conditions

Revisit this decision if measurements show that an ESP32-class Group Leader
cannot meet the approved safety margin for realistic Groups, or if reliable
Leader election cannot be achieved on supported LANs. First consider smaller
Groups, stricter bounded workloads, or a revised hardware target. Introducing a
host authority, changing who may be a NAMO Node, or making manufacturer
attestation mandatory for community operation requires a new ADR rather than an
implicit implementation shortcut.
