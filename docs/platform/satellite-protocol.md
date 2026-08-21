# NAMO Satellite Protocol

Status: **Draft logical contract under incremental implementation. Host-side V1
fixtures, validation, bounded simulation, MessagePack selection, authenticated
768-byte observation envelopes and golden vectors, stable identity, the bounded
persistent registry, read-only descriptor discovery, and a bounded local
registry-administration API exist. Authenticated Shadow transport and an
explicitly opted-in NAMO-to-NAMO production fusion path use securely paired
community NAMO Node credentials. External Satellite admission, production
membership-key rotation, and official-device attestation verification do not
yet exist.**

Korean version: [satellite-protocol.ko.md](satellite-protocol.ko.md)

This document defines the logical contract that allows approved external
Satellites and NAMO Nodes to exchange observations inside a NAMO Spatial
Platform Site. Field names and examples are design candidates. They do not
describe currently available HTTP endpoints or firmware behavior unless a section
explicitly identifies an implemented read-only surface.

## 1. Protocol Principles

- A Satellite publishes observations, not final user-facing state.
- A Satellite is not a NAMO Node and cannot become a Leader or configuration
  replica.
- Product names and attestation status do not imply observation capabilities.
- A Site assigns membership and trust; a participant cannot self-authorize.
- Spatial observations identify their coordinate frame and calibration version.
- Every stream is bounded by rate, item count, TTL, and payload size.
- Old, duplicated, unauthenticated, or version-incompatible observations are
  rejected before fusion.
- The same contract supports one standalone NAMO Node and a bounded Group with
  one Group Leader plus at most six other NAMO Nodes or Satellites.

## 2. Participant and Message Identity

| Field | Purpose |
| --- | --- |
| `protocolVersion` | Wire-contract major version. |
| `siteId` | Site assigned during pairing. |
| `participantId` | Stable identity of either a NAMO Node or Satellite. Never an IP address. |
| `participantType` | `namo_node` or `satellite`. Immutable after enrollment. |
| `bootId` | Changes on every process or device boot. |
| `sequence` | Monotonic within one `bootId` and stream. |
| `groupId` | Fusion Group receiving the observation. |
| `observedAtMs` | Sender monotonic observation time. |
| `ttlMs` | Maximum age at which the observation may be used. |

A NAMO Node's `participantId` is its `nodeId`. An external Satellite receives a
`satelliteId` during enrollment and uses that value as its `participantId` on
shared envelopes. The separate names preserve role meaning while keeping one
bounded participant registry.

An IP address, hostname, display name, current Leader identity, certificate
serial number, or product name must not be used as a permanent Site, Space, or
automation entity identifier.

### 2.1 Local Identity Storage

The active V2 storage separates the immutable local NAMO `nodeId` from mutable
Site membership. The membership record contains `siteId`, standalone/enrolled
mode, and a monotonically changing generation. Both records have fixed
identifier limits, explicit storage versions, encoded lengths, and corruption
checksums. They never store an IP address, hostname, `bootId`, private key, or
certificate. `bootId` remains ephemeral and changes on every boot.

An absent record on firmware that predates the platform is uninitialized state,
not a legacy identity to reinterpret. Corrupt or unsupported records must not be
silently replaced with newly generated identities because that would break Site
membership and stable automation identity. Recovery and replacement require an
explicit control-plane operation. The checksum detects storage corruption; it is
not authentication and does not replace the pairing or attestation rules below.

Firmware upgrades migrate the original combined V1 identity without changing
either ID. The V1 record remains a recovery journal for an interrupted first V2
commit, but normal boots read the separated V2 records. Site membership uses two
slots and switches a checksummed commit pointer only after the inactive slot is
durable. A missing commit is repaired only when exactly one valid slot exists;
ambiguous or corrupt state fails closed. A deliberate full NVS erase or hardware
factory reset removes these records and therefore starts a new identity
lifecycle.

This record is private firmware storage, not a Satellite wire format. The shared
wire representation remains the versioned protocol defined by this document.

## 3. Discovery and Pairing

Discovery only locates a candidate. It does not grant trust.

1. A candidate advertises a local discovery service with its provisional
   identity, participant type, product, protocol range, and pairing state.
2. The user opens a bounded pairing window from an existing Site or creates a
   Site on a standalone NAMO Node.
3. The Site reads the candidate descriptor and capabilities.
4. For an official device, the Site issues a fresh attestation challenge and
   verifies the returned signature and certificate chain.
5. The user approves the candidate and assigns Floorplan, Space, and Group.
6. The Site issues the stable participant identity, membership data, and
   authentication material.
7. The participant enters `quarantined` until required placement and calibration
   are valid.
8. Activation is recorded as a versioned Site configuration change.

Manufacturer attestation can streamline verification but never bypasses the
explicit local pairing window or grants Group membership by itself. Manual
address entry remains a fallback when multicast discovery is unavailable.

Current firmware advertises
`_namo._tcp` on HTTP port 80 with the protocol range, stable Node participant
ID, participant type, product, descriptor path, `pairing=manual`, and identity
readiness. The same descriptor is available manually at
`/api/platform/descriptor`. Discovery grants no trust. The local
`/api/platform/participants` endpoint can exercise the bounded window and
transactional registry lifecycle, but it does not discover a peer, exchange Site
credentials, verify attestation, or carry observations.

The endpoint now also persists a bounded readiness profile containing declared
capabilities, Floorplan/Space/Group assignment, pose version, and calibration
version. Approval remains quarantined, and activation is rejected until the
profile is complete. A host-tested shadow admission core validates active
membership, Site/Group, capabilities, versions, TTL, boot identity, and sequence.
Firmware now has an opt-in development receiver that connects authenticated UDP
observations to that admission core and bounded shadow diagnostics. Standard
NAMO product builds reuse the bounded authenticated transport for active NAMO
Nodes and grant production authority only to the elected Group Leader. External
Satellite admission remains disabled.

## 4. Participant Descriptors

External Satellite example:

```json
{
  "protocolVersion": 1,
  "participantId": "satellite-bedroom-east",
  "participantType": "satellite",
  "product": "community-mmwave-bridge",
  "softwareVersion": "0.1.0",
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": true,
    "range": false,
    "presence": true,
    "motion": true,
    "zonePresence": false,
    "maxTargets": 3,
    "bridge": true
  },
  "protocolRange": {
    "minimum": 1,
    "maximum": 1
  }
}
```

Verified NAMO Device example:

```json
{
  "protocolVersion": 1,
  "participantId": "namo-bedroom-west",
  "participantType": "namo_node",
  "product": "namo-spatial-sensor",
  "softwareVersion": "1.0.0",
  "attestation": {
    "scheme": "namo-device-v1",
    "certificateChain": ["base64-device-cert", "base64-intermediate-cert"]
  },
  "capabilities": {
    "pointTracks": true,
    "velocity": true,
    "positionUncertainty": true,
    "presence": true,
    "motion": true,
    "maxTargets": 3
  },
  "protocolRange": {
    "minimum": 1,
    "maximum": 1
  }
}
```

Satellite descriptors do not contain `fusionLeaderEligible` or
`configReplicaEligible`. Those are Site-granted permissions available only to
NAMO Nodes. Capabilities are explicit. A receiver rejects a required capability
it cannot interpret and ignores unknown optional fields.

## 5. Observation Envelope

Draft JSON debug representation:

```json
{
  "protocolVersion": 1,
  "siteId": "site-home",
  "participantId": "satellite-bedroom-east",
  "participantType": "satellite",
  "bootId": "7d3e0d7c",
  "groupId": "group-bedroom",
  "sequence": 1842,
  "observedAtMs": 12345678,
  "ttlMs": 1500,
  "frame": {
    "id": "sensor-local",
    "poseVersion": 4,
    "calibrationVersion": 2
  },
  "tracks": [
    {
      "localTrackId": 7,
      "xMm": 840,
      "yMm": 2110,
      "vxMmS": -40,
      "vyMmS": 15,
      "qualityScore": 82,
      "sampleAgeMs": 28,
      "positionVarianceMm2": [14400, 0, 0, 32400]
    }
  ],
  "evidence": {
    "presence": true,
    "motion": false,
    "rangeMm": null,
    "zonePresence": []
  },
  "health": {
    "state": "healthy",
    "sampleAgeMs": 28
  }
}
```

The compact data-plane representation may use shorter numeric keys or a fixed
layout, but it must preserve this logical meaning.

## 6. Observation Types and Authority

### 6.1 Point Tracks

A `pointTracks` participant may publish bounded local tracks. Coordinates are
relative to the declared sensor frame. A track requires position, local track
identity, observation time, and a quality score. Velocity and uncertainty are
optional only when the descriptor says they are unavailable.

A local track identity is valid only for one participant boot. It is not a
person ID and must not be exposed as one. An accepted confirmed track may start
or maintain occupancy and may create a spatial fusion candidate.

`qualityScore` is a bounded 0-100 implementation heuristic. It is not a
calibrated probability and consumers must not present it as one. V1 uncertainty
uses the row-major covariance matrix `[xx, xy, yx, yy]` in square millimetres.
The current NAMO tracker supplies diagonal variance, so its wire mapping uses
zero for both cross terms instead of copying the internal C++ struct layout.

### 6.2 Range

A range observation constrains distance from a known sensor pose. It cannot
determine a two-dimensional target or start occupancy by itself. Range evidence
is fused only when its pose and calibration are current.

### 6.3 Presence and Motion

Trusted motion evidence such as PIR may start or maintain non-spatial occupancy
under an explicit Site policy, but it cannot create coordinates or increase
target count.

Presence-only evidence such as LD2410C may only confirm or maintain an occupancy
session that has already been armed by accepted point-track or motion evidence.
It cannot start or re-arm a session, create a coordinate, increase target count,
or mark a spatial Zone occupied. Expired evidence and old target-drop reasons do
not satisfy the arming requirement.

### 6.4 Zone Presence

Zone evidence identifies a stable `zoneId` already present in the Site
configuration. A Satellite cannot create arbitrary Zones through observation
messages. Zone evidence authority must be explicitly granted and does not imply
spatial target coordinates.

## 7. Coordinate Convention

- Distance units are millimetres.
- Velocity units are millimetres per second.
- Time durations are milliseconds.
- `sensor-local` has its origin at the radar, positive X to the radar's right,
  and positive Y forward, away from the radar face.
- Floorplan pixels have their origin at the image's top-left, positive X to the
  right, and positive Y down.
- `rotationDeg` is measured clockwise from Floorplan up to sensor-local
  positive Y. Angles are degrees.
- The Site-owned Floorplan transform uses separate `mmPerPxX` and `mmPerPxY`
  values and the radar `scaleFactor`; implementations must not replace them
  with one averaged scale.
- Pose and calibration changes increment their versions.

For a sensor-local point `(radarX, radarY)`, the existing dashboard contract is:

```text
angle = rotationDeg * pi / 180
dx = radarX * scaleFactor / mmPerPxX
dy = -radarY * scaleFactor / mmPerPxY
floorplanX = originX + dx*cos(angle) - dy*sin(angle)
floorplanY = originY + dx*sin(angle) + dy*cos(angle)
```

The inverse transform is defined by the shared V1 coordinate fixtures in
`tools/platform-sim/fixtures-v1.json`. Future world-coordinate messages require
an explicit frame ID and transform version; a receiver must never infer axes
from the participant product name.

## 8. Timing, Ordering, and Expiry

- `sequence` increases monotonically within one `bootId`.
- A changed `bootId` resets sequence ordering without reusing old state.
- The receiver rejects duplicate or lower sequence values.
- `ttlMs` limits how long an observation may influence fusion.
- Lost real-time observations are not retransmitted after they are stale.
- Clock offset must not be hidden. A participant publishes time-quality
  information or the receiver derives age from arrival and heartbeat timing.
- `observedAtMs` is an unsigned 32-bit sender monotonic value in V1. Receivers
  calculate short elapsed intervals with modulo-2^32 subtraction. A changed
  `bootId` resets all prior timing and ordering state; timestamps from different
  boots are never compared.

Leader states also include `term`. A state from a lower term is rejected even if
its sequence is numerically higher.

## 9. Group State Contract

Draft logical state consumed by dashboards and platform integrations:

```json
{
  "protocolVersion": 1,
  "siteId": "site-home",
  "floorplanId": "floorplan-main",
  "spaceId": "space-bedroom",
  "groupId": "group-bedroom",
  "configVersion": 12,
  "term": 7,
  "sequence": 991,
  "leaderNodeId": "namo-bedroom-west",
  "observedAtMs": 12345999,
  "ttlMs": 3000,
  "occupancy": true,
  "motion": false,
  "targetCount": 1,
  "zones": [
    {
      "zoneId": "zone-bed",
      "occupancy": true,
      "targetCount": 1
    }
  ],
  "health": "healthy",
  "participantsOnline": 2,
  "participantsExpected": 2
}
```

Platform adapters consume this state. They do not consume individual
observations to reproduce fusion.

## 10. Authority and Source Changes

The current Leader is a NAMO Node transport endpoint, not a stable automation
identity. A consumer changes source only when it sees a newer valid term or the
previous lease expires. It must not switch repeatedly merely because another
endpoint responds faster.

A non-Leader NAMO Node may return a structured `group_not_leader` response
containing the known Leader identity and term. A Satellite cannot serve Group
state. A new Leader sends a complete Group snapshot before incremental changes.

## 11. Transport Candidates

The protocol separates semantics from transport so external implementations are
not tied to an ESPHome detail.

| Traffic | Initial candidate | Requirement |
| --- | --- | --- |
| Discovery | mDNS plus manual fallback | Low rate, no trust grant |
| Pairing/configuration | HTTP/JSON | Reliable, authenticated |
| Heartbeat/authority | Bounded unicast message | Low rate |
| Observations | Authenticated MessagePack over UDP unicast | Standard payload, bounded datagram, shadow-only first |
| Debugging | JSON snapshot | Human-readable and optional |
| HA/ST state | HTTP snapshot, later push stream | Stable Group state only |

### 11.1 Observation Datagram V1

ADR-0002 fixes the Phase 3B host contract. One datagram is at most 768 bytes:

| Part | Size | Purpose |
| --- | ---: | --- |
| Prefix | 24 bytes | Magic, envelope version, kind, flags, prefix size, key version, participant token, payload length, reserved field |
| MessagePack | 1..712 bytes | Logical Observation V1 payload |
| Authentication tag | 32 bytes | Full HMAC-SHA256 over prefix and payload |

All integer prefix fields use network byte order. A paired connection uses its
opaque 64-bit participant token only to select a bounded credential candidate;
the receiver then confirms that the authenticated credential's participant ID
matches the payload. The public smoke fixture retains its deterministic test
token. The receiver checks packet bounds and prefix fields, resolves an enabled
32-byte participant key, verifies HMAC, parses MessagePack, then applies
identity, readiness, TTL, sequence, and admission rules. Authentication failure
never reaches logical admission.

Observation delivery uses UDP unicast without acknowledgements or retries. Each
participant sends at most two datagrams per second, and the receiver retains at
most one pending observation per participant. After authentication, the
receiver permits one additional datagram per fixed one-second window to absorb
normal scheduler and network jitter; this does not increase the sender's
two-datagram contract. Transactional participant
credential storage and secure pairing now exist. The current `--transport-smoke`
build enables a
development-only authenticated sender/receiver on UDP `49190`, plus one public
RAM-only smoke identity/key. A follower sends only to its authenticated online
Group Leader after active membership, readiness, local pose, calibration, and
credential checks pass. The sender reuses the receive socket, emits at most once
per 500 ms, and never acknowledges or retransmits. This path is only for
actual-device resource, admission, and Shadow Fusion verification and has no
production-state authority. The standard NAMO product build removes the public
smoke credential, accepts only persisted active NAMO Node
credentials, and lets the elected Group Leader publish the bounded fused result.
Normal builds and external Satellite participants have no production
observation authority.

Run the target smoke only after uploading that explicit development build:

```powershell
python tools/participant_readiness_smoke.py --device http://192.168.29.104 --transport
```

The receiver reads at most two datagrams per component loop, uses a 769-byte
fixed receive buffer to detect oversize packets, keeps one expiring shadow item
per remote participant, and exports counters through
`GET /api/platform/participants`. It creates no task and never writes observation
traffic to NVS or the radar data partition. The sender builds MessagePack and
HMAC in one fixed 768-byte buffer. With three covariance-bearing tracks it omits
only the optional per-track `sampleAgeMs`; the observation-level age remains
authoritative.

Credential persistence uses a bounded nineteen-record registry with two slots
and one checksummed commit sector. Its three sectors grow toward lower addresses
without relocating the six shipped registry/readiness sectors. Runtime transport
references that owner instead of copying every key. The public participant API
returns only provisioned/enabled/key-version metadata; participant tokens and key
bytes are never returned. The development smoke key is RAM-only.

V1 uses MessagePack. The host benchmark is repeatable with:

```text
python -m pip install -r tools/platform-sim/requirements.txt
python tools/platform-sim/benchmark.py --iterations 2000
```

Median results from 5,000 iterations on Windows/Python 3.11 on 2026-07-21 are
shown below. These compare wire size and host implementations; they do not stand
in for ESP32 heap or parsing measurements.

| Observation | JSON | MessagePack |
| --- | ---: | ---: |
| Presence only | 416 bytes / encode 20.8 us / decode 19.0 us | 330 / 16.5 / 13.1 |
| One point track | 553 / 34.3 / 31.1 | 427 / 24.7 / 24.1 |
| Three point tracks | 831 / 55.0 / 49.5 | 627 / 41.7 / 39.8 |

The current ESPHome build already includes ArduinoJson 7.4.2 with MessagePack
support and a PSRAM-backed document allocator. A previous fixed-binary spike
reached 130 bytes for one track, but required 655 lines of duplicate C++ codec
and validation code. It was removed instead of becoming a private wire format.
CBOR was also dropped because it would add another implementation without a
meaningful repository-level advantage over the already bundled MessagePack.

At the current 500 ms observation schedule, at most six remote participants
produce 12 observation packets per second at the Group Leader. Before this
capacity is declared supported, an actual-device gate must measure peak
internal heap, PSRAM use, parse time, and dropped packets at that load. If
MessagePack fails the gate, first consider compact numeric keys; a private fixed
codec is the last fallback.

Run the protocol, codec, and boundary suite with:

```text
cd dashboard
npm run test:platform
```


## 12. Security and Device Attestation

### 12.1 Common Trust Rules

- Pairing requires explicit local user action and a bounded time window.
- Membership credentials are scoped to one Site and participant.
- Observation authentication covers identity, boot ID, sequence, payload, and
  expiry metadata.
- Replay protection survives packet reordering but rejects reused sequences.
- Revoked participants are ignored even when still reachable on the LAN.
- Secrets, certificates where inappropriate, and private addresses are redacted
  from diagnostics.
- Bridges are not trusted more than their declared source and capability.

### 12.2 Official Device Attestation

Each manufactured NAMO device receives a unique private key and device
certificate. The certificate chains to a NAMO manufacturer root. The key must be
generated or provisioned through a controlled manufacturing process and stored
so normal firmware APIs cannot export it.

During pairing:

1. the Site sends a fresh random nonce and requested transcript context;
2. the device signs the challenge with its device private key;
3. the Site verifies the signature, certificate chain, product claim, validity,
   and revocation state;
4. the Site records `official_verified` and the certificate identity;
5. Site-specific membership credentials are issued separately.

The attestation signature must bind the nonce, protocol version, provisional
identity, and transcript context to prevent replay or cross-protocol use. The
private key never leaves the device. Manufacturer roots are versioned and
distributed with the NAMO Core Runtime; rotation and revocation behavior must be
defined before production provisioning.

Attestation does not assign capabilities, Floorplan, Group, or calibration. It
does not replace local Site approval.

### 12.3 Community NAMO Nodes

A NAMO Core Runtime built by a user may enroll as `community_unverified` after
explicit local administrator approval. It can provide normal NAMO Node behavior,
but Leader eligibility is disabled until the administrator separately opts in.
It cannot display or advertise itself as an official Verified NAMO Device.

This distinction preserves an open AGPL-compatible implementation path while
keeping official hardware provenance meaningful.

### 12.4 External Satellites

Satellites use Site-issued membership authentication after explicit approval.
They remain observer-only regardless of vendor certificate or protocol
conformance. Protocol conformance proves compatibility, not NAMO manufacture or
Leader authority.

The security design must finalize key provisioning, secure storage, root
rotation, participant revocation, replacement, backup boundaries, and recovery
before the protocol is enabled in production.

## 13. Versioning

- `protocolVersion` changes only for incompatible wire semantics.
- Schema additions are optional unless explicitly marked required.
- Capability versions describe feature-level compatibility.
- Site configuration has an independent monotonic `configVersion`.
- Pose and calibration have independent versions.
- Attestation scheme and trust-policy versions are independent of observation
  protocol versions.
- Mixed-version behavior must be covered by conformance fixtures.

## 14. Resource Limits

The implementation contract must define:

- at most eight active Fusion Groups in one Site;
- exactly one Configuration Owner for each active Floorplan;
- at most one active Floorplan owned by one physical NAMO Node;
- at most one active Floorplan replication session per Configuration Owner,
  with bounded deterministic waiting and retry;
- one Group Leader plus at most six other active NAMO Nodes or Satellites in
  one Fusion Group;
- at most one Group Leader role per physical ESP32-S3;
- minimum one NAMO Node per active Fusion Group;
- maximum observations per participant per second;
- maximum tracks and evidence items per observation;
- maximum payload size;
- maximum accepted observation age;
- fixed queue depth and overflow policy;
- fused-track limit and association budget.

The current local relationship registry has a separate flat capacity of 20
participants including the local Node. That is a pairing/storage boundary, not
the product Group capacity. The production Group contract admits at most seven
sensing participants,
three point tracks per observation, six Zone evidence items, TTL from 100 to
10000 ms, two observations per participant per second, a 768-byte authenticated
datagram, and one latest pending observation per remote participant. The raw
JSON/debug codec retains a separate 4,096-byte pre-decode test bound and is not
the production data plane. Fused-track and association budgets remain bounded
embedded limits and release benchmark gates.

The receive inbox never compares a sender's `observedAtMs` directly with the
receiver clock. Sender monotonic-clock offset is unknown, so TTL uses
`health.sampleAgeMs`, transport-provided age, and receiver time elapsed after
arrival. Recent sequence history is bounded to eight entries per participant,
recent boot IDs to four, and reordered packets never roll current state back.

V1 JSON fixtures reject `NaN` and infinite numbers. Track velocity and position
uncertainty must match their descriptor capabilities. Error `details` contain
at most eight bounded scalar values; nested diagnostic payloads are not valid
protocol errors.

Exceeding a limit produces a bounded warning or rejected observation. It must
not grow memory without a limit or block local presence tracking.

Floorplan images and geometry are not Site Leader payloads. An assigned NAMO
Node pulls its Floorplan directly from that Floorplan's Configuration Owner and
commits it locally through inactive flash slots. The Site Leader receives only
the bounded owner, generation/digest, Group assignment, route, and health
metadata. Different Floorplans have independent owners and handover state.

## 15. External SDK and Conformance

The first public SDK should be a small Python reference Satellite that can:

- advertise and pair;
- publish point, presence, motion, and range fixtures;
- simulate delay, loss, restart, and stale packets;
- validate capability and version negotiation;
- run against a conformance test server without NAMO hardware.

Later C++ and ESPHome examples may reuse the same fixtures. Passing protocol
conformance does not certify spatial accuracy, official NAMO manufacture, or
Leader eligibility; those remain separate.

The current `tools/platform-sim` code provides a deterministic sender,
loss/reorder/restart scenarios, a legacy 20-participant storage limit, a
latest-wins inbox, authenticated MessagePack datagrams, a byte-exact golden
vector, and codec benchmarks. It does not advertise, pair, open sockets,
publish device data, or act as a Home Assistant custom integration.

## 16. Related Documents

- [Spatial Platform Architecture](architecture.md)
- [Implementation Roadmap](implementation-roadmap.md)
- [Current API Contract](../api-contract.md)
- [ADR-0002: Authenticated Observation Transport](../adr/0002-authenticated-observation-transport.md)
