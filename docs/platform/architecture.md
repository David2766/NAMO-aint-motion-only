# NAMO Spatial Platform Architecture

Status: **Draft under incremental verification.**

- **Implemented:** stable identity, bounded registries, secure community Node
  pairing, authenticated heartbeat and observation transport, shared Floorplan
  replication, calibration, deterministic role handover, fixed-memory
  seven-input Group fusion, and the Home Assistant custom integration.
- **Verified on hardware:** a five-Node, single-Group Site has completed the
  new-Node pairing, Floorplan replication, calibration, activation, single-role
  outage, paired-role outage, and healthy rejoin flows. Automation remained
  available on the surviving strict majority. Sequential expansion to seven
  Nodes also completed pairing, replication, calibration, activation, final
  7/7 canonical convergence, active fusion/automation, and resource checks.
- **Remaining release gates:** physical failure/rejoin matrices at other Group
  sizes, multi-Group operation, and rolling upgrades.
- **Not complete:** manufacturer attestation, the external Satellite SDK, and
  SmartThings integration. Host tests cover same-revision recovery isolation,
  emergency triple-role fallback and relinquishment, pairing-poll resilience,
  one bounded pairing lifetime, bounded handover retry, and incumbent-preserving
  five-to-six-to-seven Group roster expansion.

Korean version: [architecture.ko.md](architecture.ko.md)

This document is for developers who need to understand how NAMO devices share
configuration, elect runtime roles, exchange observations, and preserve
automation state during failures. It defines the architectural boundaries for
multi-sensor work. Exact production HTTP behavior remains defined by the
firmware and the [API contract](../api-contract.md).

## Reading Guide

- For a third-party sensor or bridge, start with Sections 3, 5, 6, and 12, then
  read the [Satellite Protocol](satellite-protocol.md).
- For pairing, replication, elections, or failure recovery, read Sections 4,
  6, 7, and 11.
- For coordinate conversion, calibration, or target fusion, read Sections 8,
  9, and 10.
- For endpoint fields, status codes, and errors, use the
  [API Contract](../api-contract.md).

## 1. Goals

- Preserve the current standalone behavior when only one sensor is installed.
- Organize each Site as a low-rate directory of at most eight Fusion Groups.
- Bound each Fusion Group to one Group Leader plus at most six other sensing
  participants, seven inputs total.
- Fuse observations from sensors looking in similar or different directions.
- Keep the system fully local and operational without a mandatory cloud or
  always-on external coordinator.
- Allow third-party and community sensors to participate through a documented,
  capability-based Satellite protocol.
- Keep control authority on NAMO Nodes while allowing both verified official
  devices and explicitly approved community NAMO builds.
- Keep Home Assistant and SmartThings automations attached to stable logical
  spaces, not to a physical sensor or the current Group Leader.
- Bound CPU, RAM, storage, packet rate, and queue sizes for ESP32-class devices.

## 2. Non-Goals for the First Release

- Thread, Zigbee, or general mesh packet forwarding.
- Personal identity or cross-floor person identification.
- Sharing raw radar frames between devices.
- Automatically correcting large installation or floorplan errors.
- Allowing a Satellite or bridge to become a Group or Site Leader.
- Requiring manufacturer attestation for community builds to operate locally.
- Guaranteeing the hierarchical capacity target before its Group-fusion and
  Site-coordination loads are measured and verified.

## 3. Domain Model

| Concept | Meaning | Stable identity |
| --- | --- | --- |
| Site | One installation administered as a unit. | `siteId` |
| Floorplan | One map and its physical coordinate system. | `floorplanId` |
| Space | A logical room or area used by users and automations. | `spaceId` |
| Fusion Group | Participants whose observations may be fused. | `groupId` |
| NAMO Node | A device running the NAMO Core Runtime and Node contract. | `nodeId` |
| Verified NAMO Device | An official NAMO Node with valid manufacturer attestation. | `nodeId + deviceCertificate` |
| Community NAMO Node | A user-built NAMO Node without manufacturer attestation. | `nodeId` |
| Satellite | An approved external sensor or bridge that only contributes observations. | `satelliteId` |
| Participant | A NAMO Node or Satellite counted toward Site and Group limits. | `participantId` |
| Configuration Owner | The NAMO Node that owns the authoritative geometry and payload for exactly one Floorplan. | `floorplanId + generation` |
| Group Leader | A NAMO Node temporarily authorized to receive and fuse the high-rate observations of one Group. | `groupId + term` |
| Site Leader | A NAMO Node temporarily authorized to coordinate the low-rate Group directory and Site health. | `siteId + term` |

A Satellite is not a NAMO Node. Product origin does not decide observation
capabilities, but only a NAMO Node may own authoritative Floorplan
configuration, be elected Leader, approve trust, or publish final automation
state. Every Fusion Group must contain at least one NAMO Node.

Manufacturer attestation and Node capability are separate. A Verified NAMO
Device can be verified automatically during an explicit local pairing flow. A
Community NAMO Node can receive the same local Node role after explicit
administrator approval and an opt-in grant of Leader eligibility. Attestation
proves official manufacture and firmware provenance; it is not a permanent
lock on locally built AGPL-compatible Nodes.

## 4. Topology

```text
Site
|-- Floorplan A
|   |-- Space: Living room
|   |   `-- Fusion Group: living-room
|   |       |-- Verified NAMO Device A (Leader eligible)
|   |       |-- Community NAMO Node B (manually trusted, optionally eligible)
|   |       `-- Third-party Satellite C (observer only)
|   `-- Space: Bedroom
|       `-- Fusion Group: bedroom
`-- Floorplan B
    `-- Space: Upstairs hall
        `-- Fusion Group: upstairs-hall
```

Site-wide traffic is limited to discovery, heartbeats, topology, and
configuration versions. High-rate observations stay inside their Fusion Group.
The Site Leader receives bounded Group summaries rather than every observation.
It maintains the current Group Leader route, authority term, health, member
count, and configuration revision for each Group. It does not proxy Group
observation streams or fused target snapshots.

Each active Floorplan has exactly one persisted Configuration Owner. Ownership
and generation advance independently per Floorplan. The Site Leader keeps only
the bounded `floorplanId`, owner route, generation/digest, Group assignment, and
health metadata required for coordination. It does not store a Floorplan image
or geometry merely because it is the Site Leader. One physical NAMO Node owns
at most one active Floorplan.

The implemented heartbeat path uses stable `participantId` identity and a
pairwise credential. mDNS supplies a candidate route, while an authenticated UDP
source updates the peer's current IP address in RAM. Network addresses are not
part of persistent Site identity and may change after DHCP renewal or reboot.

## 5. Roles, Capabilities, and Evidence Authority

The protocol recognizes observation capabilities instead of product names.

| Capability | Responsibility |
| --- | --- |
| `point_tracks` | Supplies local target position and velocity. |
| `range` | Supplies a radial distance constraint. |
| `presence` | Supplies non-spatial occupancy evidence. |
| `motion` | Supplies non-spatial motion evidence. |
| `zone_presence` | Supplies occupancy evidence scoped to a configured Zone. |
| `bridge` | Translates another protocol into Satellite observations. |

Leadership and configuration replication are NAMO Node permissions, not
Satellite capabilities. A Satellite descriptor cannot request or advertise
Leader eligibility.

Evidence authority is explicit:

| Evidence | Start or maintain occupancy | Create spatial targets |
| --- | --- | --- |
| Confirmed point track | Yes | Yes |
| Trusted motion evidence such as PIR | Policy-controlled | No |
| Presence-only evidence such as LD2410C | Maintain an already armed session only | No |
| Range-only evidence | Supporting constraint only | No |

A `presence`-only Satellite cannot start an occupancy session, create a spatial
target, increase target count, or mark a spatial Zone occupied. It may only
confirm or maintain a session that was first armed by accepted point-track or
motion evidence. This preserves NAMO's current LD2410C design rule while still
allowing non-spatial sensors to reinforce a valid session. Old drop reasons or
expired evidence must not re-arm a session.

## 6. Control Plane and Data Plane

The control plane handles low-rate, reliable operations:

- discovery and pairing;
- Site, Floorplan, Space, Group, NAMO Node, and Satellite configuration;
- capability negotiation and trust classification;
- version and hash comparison;
- calibration commands and snapshots;
- health and Leader discovery.

Shared Floorplan payloads are a special bounded control-plane transfer. A NAMO
Node pulls only its assigned Floorplan directly from that Floorplan's persisted
Configuration Owner over authenticated chunks, then commits it to local
inactive flash slots. Each Configuration Owner serves at most one active
replication session; other eligible Nodes wait in deterministic bounded slots
and retry with bounded backoff. The Site Leader does not queue, store, or relay
those bytes merely because it currently holds a lease. Only small owner,
generation, hash, assignment, and health metadata reaches the Site Leader.

The canonical Site directory follows a separate low-rate path on the same
authenticated, bounded chunk transport. Every active non-Site-Leader NAMO Node
normally pulls it directly from the elected Site Leader and commits it through
the existing local A/B directory store. Initial enrollment has a distinct
bootstrap phase: the quarantined joiner normally reads the committed canonical
copy through its already authenticated Configuration Owner relationship before
role authority is admitted. A stale same-Site canonical copy does not block
activation; election and replication fencing keep the Node non-authoritative
until it converges. In a Site with more than two active members, an Owner
that is not the Site Leader may serve this read-only copy only while a live
quorum exists and the elected Site Leader advertises the exact same Directory
revision and digest. The joiner never promotes its compatibility mirror into
canonical authority during this phase. Heartbeat v7
advertises the committed Directory revision and semantic digest so election
prefers a fresher persisted copy before role and Node-ID tie breaking.
Cross-device freshness uses that revision and digest; each device's flash
persistence generation remains local bookkeeping. Removed canonical Groups
and unregistered Floorplans are pruned. This convergence is automatic and does
not require a user topology workflow.

The data plane carries short-lived observations:

- bounded local tracks rather than raw radar frames;
- monotonic sequence numbers and boot identifiers;
- timestamps, TTL, coordinate-frame version, and calibration version;
- source confidence and available uncertainty information.

The logical data contract is transport-independent. HTTP/JSON is preferred for
the control plane. V1 uses standard MessagePack for observations because the
firmware already includes a compatible ArduinoJson implementation and the
bounded host benchmark is smaller and faster than JSON without maintaining a
private binary codec. Target-hardware load validation remains mandatory before
observation transport is enabled. A JSON debug representation remains available
for third-party development.

## 7. Leadership and Failure Semantics

Each Fusion Group elects one Group Leader from eligible NAMO Nodes. The Site
elects one low-rate Site Leader from eligible NAMO Nodes independently. A
Satellite is never a Leader or configuration replica. The Site Leader must not
process every target observation.

Healthy operation uses a deterministic role anti-affinity order so Site
control and high-rate Group fusion do not normally share one physical device.
The Site Leader ranks candidates as: neither a Configuration Owner nor a
current canonical Group Leader, Configuration Owner only, current canonical
Group Leader only, then both roles. This protects a surviving Group Leader from
an unrelated Site Leader replacement and avoids an unnecessary Group-term
change.
Group Leader candidates are restricted to active, ready NAMO Nodes in that
Group and ranked as: neither Site Leader nor that Group's Floorplan
Configuration Owner, the Floorplan Configuration Owner, then Site Leader as the
final degraded fallback. Node ID breaks ties. One physical ESP32-S3 may lead at
most one Fusion Group. Configuration Owner plus Site Leader and Configuration
Owner plus Group Leader are valid placements. Site Leader plus Group Leader is
avoided and used only when no ordinary candidate exists. A multi-Node Site
may temporarily assign Configuration Owner, Site Leader, and Group Leader to
the same physical Node only when that Group has no other eligible candidate.
The triple-role incumbent is not protected and yields when a candidate appears.
Standalone compatibility may collapse these responsibilities locally but does
not establish a multi-Node placement precedent. When multiple Groups are
visible but the local Group assignment is not canonical, the local Node fails
closed instead of claiming an arbitrary Group.

Published role diagnostics are immutable generation snapshots. An HTTP reader
must observe the Site coordinator and every local Group coordinator from one
completed control-loop publication, never a mixture of mutable states from two
election cycles.

When a healthy Group Leader is replaced, the last complete automation snapshot
may remain visible as non-authoritative, read-only continuity for at most 30
seconds. The fixed window is not extended by repeated failures. Motion is
forced false, no new target or Zone evidence is created, and configuration,
Floorplan, pose, calibration, membership, or authority rollback mismatches
invalidate continuity immediately.

Verified NAMO Devices are Leader-eligible by default after Site approval.
Community NAMO Nodes require explicit administrator approval for Leader
eligibility. A Group cannot be activated without at least one eligible NAMO
Node.

Authority is represented by `term` and a bounded `lease`. A consumer accepts a
state only when its authority, configuration version, and sequence are current.
An old Leader's delayed packets are ignored.

During a Leader handover:

- the last valid occupancy state is retained for its defined TTL;
- health changes to `degraded` while authority is unresolved;
- transport failure does not produce an unoccupied event;
- expired state becomes `unavailable`, not `unoccupied`;
- a new Leader publishes a complete snapshot before incremental events.

The first implementation may use a deterministic lease election rather than a
general consensus system. Configuration conflict rules must still be explicit
and testable.

The bounded control-plane contract is:

- role elections use a strict majority of the canonical configured membership,
  not a majority of whichever Nodes one participant happens to see online;
- eligible voters must endorse the same bounded election-view digest before a
  Site or Group Leader becomes authoritative;
- current Site and Group election-view identity covers the complete configured
  canonical roster, its roster revision, and the selected winner. Liveness,
  Directory assignment, and transient credential readiness are excluded from
  that stable identity, but remain independent quorum, semantic-consistency,
  candidate-eligibility, and authority-admission gates;
- terms, leases, canonical roster/directory revisions, and semantic digests
  fence stale or divergent candidates;
- within one authority epoch, an active current-wire Node exactly one
  generation and roster revision behind or ahead may retain the already-running
  heartbeat and fusion data plane only when credentials, Floorplan, and radar
  pose still match. Shared writes, owner advertisements, and Group summaries
  remain fenced until the exact canonical revision converges. Epoch changes,
  non-adjacent revisions, legacy wire versions, and spatial mismatches do not
  qualify;
- a minority clears distributed role authority and keeps local sensing rather
  than inventing a smaller canonical membership;
- a stable eligible majority must eventually converge, while a failed
  candidate or transient transport/storage failure must be reevaluated through
  bounded retry rather than leave authority permanently stuck;
- conflict suppression identifies the rejected semantic candidate, not merely
  its numeric revision, so stronger quorum evidence for a different candidate
  at the same revision can be evaluated without allowing replay oscillation.

Configuration Owner handover is independent from Site and Group runtime
leadership. If the existing Group Leader and its canonical assignment survive
an Owner outage, handover, or former-Owner rejoin, transient credential-bundle
convergence must not change the Group Leader or term. A short election-fence
gap still clears public authority and fails closed; when the exact same
non-rebooted Leader regains the matching view, it resumes the suspended term.
A different Leader always establishes a newer term.

With the current configured-membership contract, a one-Node Site operates
standalone, a two-Node Site cannot transfer distributed authority after losing
one Node, and larger Sites require `floor(N/2)+1` matching active members. A
future survivor-reconfiguration policy is a separate quorum-contract change;
it must not be introduced as an implicit recovery shortcut.

## 8. Shared Coordinates and Calibration

Each Floorplan's Configuration Owner owns its authoritative geometry. A NAMO
Node stores only the Floorplan and Spaces assigned to it. Every spatial
participant has a versioned transform:

```text
local sensor coordinates
-> rotation
-> translation
-> bounded calibration correction
-> Floorplan world coordinates
```

Initial placement remains user-controlled. A calibration snapshot may compare
simultaneous observations of one person in an overlap area. Software may suggest
a small translation or rotation correction, but it must not silently apply
scale, shear, or a large correction that hides a physical installation error.

Uncalibrated or version-mismatched observations may be shown for diagnostics but
must not enter spatial fusion.

Once calibration is approved, the Site configuration stores one canonical
spatial profile per NAMO Node. The profile contains the approved pose revision,
calibration revision, and effective transform. Because this record follows the
authenticated Site replication path, a new Group or configuration Leader uses
the same coordinate frame after handover instead of rebuilding it from local
placement. Missing or stale profiles fail closed.

## 9. Fusion Boundary

The current local tracker remains responsible for converting one LD2450 stream
into stable local tracks. The Fusion Engine receives those tracks through an
adapter and does not parse raw sensor frames.

Fusion considers:

- world-coordinate distance and uncertainty;
- velocity and direction compatibility;
- observation age and TTL;
- continuity with existing fused tracks;
- source health and confidence;
- correlated error from sensors installed at similar angles.

Different-angle observations can improve geometry. Similar-angle observations
must not simply double confidence. Ambiguous observations remain separate
rather than being forcibly merged.

Non-spatial PIR or LD2410C-style evidence follows the evidence-authority table
above. It may reinforce an armed occupancy session but cannot invent target
coordinates or increase target count.

## 10. Platform Output

The Group Leader publishes a bounded logical Space state:

```text
occupancy
motion
fused target count
zone occupancy and counts
entry and exit transitions
fusion health
contributing and expected participant counts
authority term, sequence, and observation time
```

The embedded dashboard consumes the local cached state. Home Assistant and
SmartThings first use the Site Leader as a low-rate Group directory, then read
each logical Space state directly from its current Group Leader. The Site
Leader does not aggregate or relay high-rate target state. Platform adapters do
not perform confidence merging, target association, TTL resolution, or conflict
resolution.

Home Assistant identities are based on `siteId + spaceId + entity type`.
SmartThings logical children are based on `spaceId` and `spaceId + zoneId`.
Leader and IP changes therefore do not recreate automation targets.

## 11. Hierarchical Capacity Target

Capacity is bounded independently at the Site control plane and Group data
plane.

- A Site contains at most eight active Fusion Groups.
- Every active Fusion Group belongs to exactly one Floorplan, so a Site with
  eight one-Group Floorplans has eight active Floorplans.
- Every active Floorplan has one independent Configuration Owner; the Site
  Leader stores only its bounded directory record. One physical NAMO Node owns
  at most one active Floorplan.
- Each Fusion Group contains one Group Leader and at most six other NAMO Nodes
  or Satellites, seven sensing participants total.
- Every Fusion Group contains at least one NAMO Node, because a Satellite cannot
  become Group Leader.
- One physical ESP32-S3 leads at most one Fusion Group.
- The Site Leader stores only eight bounded Group directory/health records. It
  does not store every Group observation or perform Group fusion.
- Maximum local tracks per participant are declared and bounded.
- Maximum fused tracks are separately bounded by the implementation contract.
- All queues and registries use bounded storage.
- Stale observations expire without retransmission.
- Spatial gating reduces association candidates before expensive matching.
- A Leader receives local track summaries, not radar frames or dashboard data.

The current firmware keeps a flat twenty-participant relationship registry for
local administration. Group membership and fusion are separately bounded at
seven sensing participants, and the Site directory is bounded at eight Groups.
The relationship registry is therefore not the Group capacity contract.
Verification must separately cover a seven-input Group, an eight-Group Site
directory, and the degraded case where one Node temporarily holds both Site and
Group leadership. Tests measure packet rate, dropped observations, peak heap,
largest free block, CPU time per fusion cycle, Leader handover time, and false
transitions.

## 12. Security, Trust, and Device Attestation

Trust classes are separate from observation capabilities:

| Trust class | Enrollment | Leader eligibility | User-facing status |
| --- | --- | --- | --- |
| Verified NAMO Device | Manufacturer attestation verified during local pairing | Enabled by default after Site approval | `official_verified` |
| Community NAMO Node | Explicit administrator approval | Explicit opt-in only | `community_unverified` |
| Satellite | Explicit approval and capability validation | Never | `satellite` |
| Unknown device | Quarantined | Never | `unknown` |

Production NAMO hardware receives a unique private key and device certificate
during manufacturing. The certificate is signed by a NAMO manufacturer root.
Pairing verifies ownership with a fresh challenge and signature; the private key
never leaves the device. The Site stores the verified certificate identity and
trust result, not the private key.

The attestation design must define secure key storage, certificate chain and
version, root rotation, device revocation, replacement and recovery, and
diagnostic redaction. A certificate proves official device provenance but does
not assign Site membership, Group placement, calibration, or observation
capabilities by itself. Explicit local pairing remains required.

Community NAMO Nodes remain possible under the AGPL source model. They cannot
claim official verification, are never silently trusted, and become Leader
eligible only after a local administrator explicitly grants that permission.
This keeps official hardware distinguishable without turning manufacturer
attestation into a closed protocol gate.

For all participants:

- pairing is allowed only during an explicit bounded window;
- the Site assigns identity, Group membership, and trust state;
- messages carry `siteId`, participant identity, `bootId`, `sequence`, and TTL;
- authentication and replay protection are required before observations can
  influence automation state;
- newly joined or uncalibrated participants start as `quarantined`;
- capability and protocol versions are validated before activation;
- unknown optional fields are ignored, while unsupported required features
  reject pairing or observation.

## 13. Code Ownership

| Current path | Responsibility |
| --- | --- |
| `components/radar_api_server/presence_tracker.*` | Local LD2450 tracking. |
| Static-presence modules under `components/radar_api_server` | PIR and LD2410C evidence. |
| `site_*`, `group_*`, `participant_*`, `observation_*`, and `spatial_*` modules under `components/radar_api_server` | Registry, trust, authenticated transport, coordination, fusion, calibration, and replicated spatial state. |
| Handler and dashboard modules under `components/radar_api_server` | Public HTTP API and embedded dashboard transport. |
| `custom_components/namo` | Home Assistant platform adapter. |
| `smartthings-edge/radar-zone` | SmartThings platform adapter. |

The spatial platform is split into focused C++ modules under
`components/radar_api_server`; it is not implemented as more branches inside
`presence_tracker.cpp`. A future directory extraction may change ownership
without changing these boundaries. Third-party developers implement the
Satellite contract and do not depend on NAMO's local tracker internals.

## 14. Compatibility

- Fusion disabled means current local tracking and presence behavior.
- A one-NAMO-Node Site uses the same Site, Space, Group, and authority schema.
- Adding another participant does not change logical Space identifiers.
- Existing ESPHome physical entities remain available.
- New HA and SmartThings logical entities are added without silently changing
  the meaning of existing automations.
- Planned cluster endpoints do not become part of the current API contract until
  they are implemented and covered by compatibility tests.

## 15. Related Documents

- [Satellite Protocol](satellite-protocol.md)
- [Implementation Roadmap](implementation-roadmap.md)
- [ADR-0001: Distributed Spatial Platform](../adr/0001-distributed-spatial-platform.md)
- [ADR-0002: Authenticated Observation Transport](../adr/0002-authenticated-observation-transport.md)
- [ADR-0003: Participant Credential Storage](../adr/0003-participant-credential-storage.md)
- [ADR-0004: Secure Community Pairing](../adr/0004-secure-community-pairing.md)
- [Current API Contract](../api-contract.md)
- [Superseded Multi-Sensor Note](../multi-sensor-contract.md)
