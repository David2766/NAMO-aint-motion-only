# ADR 0004: Secure Community Pairing

- Status: Implemented and host-tested; physical multi-Node resilience remains open
- Date: 2026-07-21

## Context

NAMO Nodes need a local way to establish pairwise observation credentials
without requiring HACS, cloud service, or a central coordinator. The ESP32 HTTP
task previously suffered stack overflows, so elliptic-curve work and persistent
transactions cannot run directly inside request handlers. Manufacturer
attestation is also not available yet, so this path must not claim official
device provenance.

## Decision

Community pairing uses an explicit three-minute enrollment session on both
devices, opposite Leader/Joiner roles, ephemeral P-256 ECDH, SHA-256 transcript
binding, HKDF-SHA256 key derivation, role-specific HMAC proofs, and a six-digit
short authentication string. The initiating browser presents the comparison
and invokes both devices' confirmation APIs after the user confirms a match.

One fixed-size RAM session and one fixed-size command slot are allowed. HTTP
POST requests enqueue work and return `200` with `accepted: true` and an
`operationId`; the component loop performs crypto and persistence. Public
responses expose only ephemeral offers, proofs, the confirmation code, and
lifecycle metadata. All private and derived secret bytes are wiped on
cancellation, expiry, or successful commit.

The dashboard invitation prompt, cryptographic enrollment session, and visible
countdown use one 180-second lifetime. The invitation remains RAM-only and does
not open enrollment or establish trust by itself.

Successful confirmation persists the remote Node as `community_unverified`, an
enabled pairwise credential for authenticated control-plane traffic, and
quarantined membership. The Joiner changes to
the Leader Site only after registry and credential persistence succeeds.
Readiness and explicit activation remain separate authorization gates.
Pairing alone therefore leaves the peer quarantined. Production observation
authority is enabled only after the canonical Floorplan replica, radar pose and
calibration, readiness, roster commit, and bilateral activation gates pass.

## Consequences

- Standalone operation and Home Assistant continue without pairing or HACS.
- A partial failure cannot make a remote participant active.
- The flow supports manual IP entry and does not depend on multicast discovery.
- Community pairing proves possession of the ephemeral transcript key, not NAMO
  manufacturing provenance.
- Local HTTP means users must compare the code on independently opened device
  pages on a trusted network until attestation and stronger channel protection
  are implemented.
- Key rotation, replacement recovery, and manufacturer attestation remain open
  Phase 3C work.
