# ADR-0003: Participant Credential Storage Boundary

Status: **Implemented and native-tested for the Phase 3C storage foundation**

Date: 2026-07-21

## Context

Phase 3B proved the authenticated observation envelope with a public development
key. Production traffic needs one secret per remote participant, bounded to the
existing Site limit of nineteen remote participants. The device has limited
internal RAM and previously suffered task-stack failures from large local
snapshots. Registry/readiness persistence already occupies the final six 4 KiB
sectors of the radar data partition, and deployed offsets must not move.

## Decision

- Add one heap-free `ParticipantCredentialRegistry`, bounded to nineteen records.
- A record contains participant ID, opaque 64-bit lookup token, key version,
  enabled state, and one 32-byte transport key.
- Persist the registry transactionally with two slots and one checksummed commit
  sector. Interrupted writes retain the previous committed generation.
- Reserve three new sectors immediately before the shipped six platform sectors.
  Existing registry/readiness absolute offsets remain unchanged.
- Keep credential storage separate from registry membership, readiness profiles,
  Home Assistant API keys, and manufacturer-attestation keys.
- Observation transport references the credential owner; it does not keep a
  second copy of all keys. Per-participant rate state remains runtime-only.
- The configuration owner derives no new long-lived key hierarchy. It reuses
  the established pair credentials and serves an authenticated, encrypted,
  generation-bound bundle over the existing Site-replication transport. A
  recipient receives only the owner, same-Group peers, active Site Leader
  candidates, and handover credentials required by its canonical role.
- Pair credentials are directional identities. A non-owner uses its owner-pair
  record for outbound signatures to every admitted peer; the owner uses the
  destination's direct record. This allows an owner-provisioned bounded Site
  mesh without pairwise user enrollment between every two Nodes.
- Configuration Owner handover rebinds those directional identities through a
  bounded key probe authenticated as `HMAC(key, localNodeId)`. Followers stop
  authenticated control/data transmission until the replacement bundle is
  committed, and discovery contributes an address but never authorizes the
  bundle. Commit also requires a complete bundle whose current-Owner record is
  the exact request credential.
- `revoke` disables a matching credential and `forget` erases it. Membership
  rejection remains authoritative if a secondary cleanup write fails.
- Public APIs expose only `provisioned`, `enabled`, `keyVersion`, generation, and
  count. Tokens and key bytes are never serialized.
- The development smoke key remains ephemeral and is never written into the
  credential transaction.
- No HTTP endpoint accepts raw keys. Secure community key agreement is handled
  by Phase 3C; rotation, recovery, and attestation transcript binding remain
  Phase 3D work.

## Embedded Constraints

- Codec and store workspaces are fixed-size and guarded by `static_assert`.
- Decode and recovery reuse caller/store-owned buffers; multi-kilobyte credential
  snapshots are not placed on setup or HTTP task stacks.
- Transactional bundle merge uses the PSRAM replication workspace for its
  registry scratch copy rather than placing that copy on the main-loop stack.
- Observation rate limiting uses a separate bounded state table smaller than
  512 bytes.
- Expanding platform storage reduces only maximum floorplan-image capacity by
  12 KiB; configured floorplan upload limits remain far below that boundary.

## Failure Semantics

- Invalid IDs, zero tokens/versions, duplicate tokens, malformed records, and
  checksums fail closed.
- Slot data is synchronized before its commit marker.
- A failed commit does not publish the new runtime generation.
- A corrupt committed slot falls back to the previous valid slot and repairs the
  commit marker.
- Credential-store failure disables authenticated remote transport but never
  disables standalone sensing.

## Verification

Native tests cover bounds, token collisions, enabled/disabled lookup, codec round
trip, corruption, transactional write ordering, failed-commit reboot behavior,
commit repair, and preservation of all six shipped platform-sector offsets.
Platform tests verify that public participant responses contain credential
metadata but no token or key field.

Credential-topology tests additionally cover a seven-member Fusion Group,
cross-Group low-frequency Leader authentication, directional sender-key
selection, partial-source rejection, unrelated-record rejection, and canonical
Directory fencing when the assigned Leader credential is absent.
