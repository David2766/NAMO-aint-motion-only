# ADR-0002: Authenticated Observation Transport

Status: **Verified for the Phase 3B shadow implementation**

Date: 2026-07-21

Korean version: [0002-authenticated-observation-transport.ko.md](0002-authenticated-observation-transport.ko.md)

## Context

NAMO must carry bounded observations from one Group Leader and at most six
other sensing participants without
making Home Assistant, SmartThings, a broker, or a permanent coordinator a
runtime requirement. The transport must not block local sensing, grow queues
without a limit, or let an unauthenticated LAN sender reach observation
admission. The ESP32-S3 target also has a small internal-RAM and task-stack
budget even when PSRAM is present.

The logical Observation V1 contract and MessagePack codec already exist in the
host simulator. Participant identity, membership, readiness, and fail-closed
admission are separate concerns and remain authoritative.

## Decision

- Discovery continues to use `_namo._tcp` mDNS with manual address fallback.
- Pairing and low-rate configuration continue to use HTTP.
- Observation V1 uses UDP unicast directed to the current Group receiver.
- UDP multicast, broadcast, acknowledgement, and retransmission are not used for
  observations. A newer observation replaces an older pending observation.
- Each remote participant sends at most two observations per second. Six remote
  participants therefore have a design ceiling of 12 observation datagrams per
  second at the Group Leader.
- The complete authenticated datagram is at most 768 bytes. It contains a fixed
  24-byte network-order prefix, at most 712 bytes of MessagePack, and a full
  32-byte HMAC-SHA256 tag.
- The prefix contains magic, envelope version, message kind, flags, prefix size,
  key version, a 64-bit participant lookup token, payload length, and a reserved
  field. Unsupported or non-zero reserved fields fail closed.
- The lookup token is the first 64 bits of SHA-256 over `participantId`. It is a
  bounded key lookup aid, not identity or authentication. The authenticated
  MessagePack payload remains the source of participant identity.
- HMAC covers the prefix and complete MessagePack payload. Receivers enforce the
  packet bound and prefix, resolve an enabled participant key, verify HMAC, then
  parse MessagePack and apply descriptor/readiness/admission rules in that order.
- Transport keys are exactly 32 bytes and scoped to one Site participant. They
  are separate from the Home Assistant API key and manufacturer-attestation
  private key. Key issuance, persistence, rotation, and redaction are Phase 3C
  work; transport remains disabled when no valid key is available.
- Phase 3B output is shadow-only. Accepted remote observations may update
  diagnostics but cannot change production presence, targets, Zones, Home
  Assistant entities, or SmartThings state.

The host implementation and golden vector live in `tools/platform-sim`. The
firmware now contains an opt-in receive-only smoke path using the same envelope,
HMAC, MessagePack schema, and admission core. Normal, beta, and release builds do
not instantiate its UDP receiver. The smoke image uses one public test key and
must not be distributed.

## Consequences

Positive:

- fixed packet, rate, participant, and latest-wins limits map directly to bounded
  embedded storage;
- HMAC is checked before untrusted MessagePack parsing;
- no broker or external integration is required for standalone operation;
- packet loss does not create a retry queue or delay local tracking;
- the same logical MessagePack payload remains usable by external Satellites.

Costs:

- UDP does not provide delivery guarantees, ordering, or confidentiality;
- membership credential provisioning must be completed before firmware traffic
  can be authenticated;
- receiver diagnostics and hardware tests must distinguish packet loss,
  authentication failure, expiry, and participant admission failure;
- passive LAN observers can inspect payloads. Adding confidentiality requires a
  later ADR and cannot silently change the V1 envelope.

## Rejected Alternatives

### Unauthenticated UDP

Rejected because Site and participant identifiers are not secrets and do not
prevent spoofing.

### HTTP for Every Observation

Rejected because connection and request handling overhead would put high-rate
traffic on the existing HTTP task and repeat the resource pressure already found
during target testing.

### WebSocket or Persistent TCP Per Participant

Rejected for V1 because connection state, buffering, reconnect behavior, and
head-of-line blocking add RAM and lifecycle complexity. They may be revisited if
measured UDP behavior is inadequate.

### MQTT or Mandatory Broker

Rejected because a single NAMO Node must remain fully functional without a
broker or home-automation platform.

## Verification Gates

- wrong key, modified payload, unknown key version, malformed prefix, truncation,
  and oversize packets are rejected before logical admission;
- duplicate, reordered, replayed-boot, expired, and restarted streams retain the
  existing bounded semantics;
- three-track packets remain within 768 bytes;
- six remote participants at two packets per second retain at most one pending
  observation per participant;
- firmware adds one fixed receive buffer and a bounded per-loop packet budget;
- real-device tests measure internal heap, task stack, parse time, packet drops,
  standalone restart, and local sensing before Phase 3B is marked verified.

Host verification and the firmware implementation are complete. Actual-device
verification completed authenticated/admitted traffic, malformed authentication,
rate limiting, duplicate rejection, cleanup, standalone sensing, soft restart,
and post-restart lifecycle smoke without stack overflow or rollback. Phase 3B is
therefore verified; production credential exchange and authority remain Phase 3C.
