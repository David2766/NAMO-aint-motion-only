# NAMO Spatial Platform Implementation Roadmap

Status: **Draft under incremental verification. Production pairing, shared
Floorplan replication, calibration, bounded leadership, observation transport,
seven-input fusion, and the Home Assistant Integration are implemented. A
  five-Node single-Group Site has passed the complete new-Node pairing,
  Floorplan replication, calibration, and activation workflow plus physical
  single-role and paired-role-holder power-loss/rejoin smoke with automation
  continuity on the surviving strict majority. A seven-Node Site has passed
  sequential expansion, final 7/7 convergence, target resource checks, loss of
  strict majority at 3/7, fail-closed authority withdrawal, role re-election,
  and healthy 7/7 recovery. Four- and six-Node role matrices, seven-input
  overload, multi-Group, and rolling-update release matrices remain open. Official
  attestation, the Satellite SDK, and SmartThings are incomplete.**

Korean version: [implementation-roadmap.ko.md](implementation-roadmap.ko.md)

Last updated: 2026-08-26

This roadmap turns the architecture into independently verifiable work. A phase
is not complete because files or interfaces exist. It is complete only after its
exit criteria and tests pass without changing current standalone behavior.

## 1. Status Values

| Status | Meaning |
| --- | --- |
| `planned` | Scope documented; implementation has not started. |
| `in_progress` | Work or review is active; behavior is not release-ready. |
| `implemented` | Code exists and focused tests pass. |
| `verified` | Hardware/replay/compatibility gates pass. |
| `blocked` | A documented external or architectural blocker prevents progress. |

## 2. Invariants

These rules apply to every phase:

- Fusion disabled preserves current standalone outputs.
- `presence_tracker.cpp` remains the local tracker, not a cluster container.
- A standalone NAMO Node and a hierarchical multi-Group Site use the same
  stable Site, Space, Group, and participant identities.
- Site-wide traffic remains low-rate; high-rate observations and fusion remain
  inside one bounded Group.
- No unbounded container, queue, log, retry loop, or peer list is permitted.
- Transport failure never becomes a false unoccupied transition.
- Physical participant, Leader, IP, and display-name changes do not change logical
  Space automation identities.
- External Satellites cannot influence production state before pairing,
  capability validation, trust, and required calibration, and can never become
  Leaders or configuration replicas.
- Official attestation proves device provenance but does not replace local Site
  approval or observation-capability validation.
- Community NAMO Nodes remain supported through explicit local approval; they
  require a separate opt-in grant before becoming Leader-eligible.
- Current API endpoints are not redefined by a future design document.

## 3. Phase Summary

| Phase | Status | Scope |
| ---: | --- | --- |
| 0 | `implemented` | Architecture, protocol, ADR, and roadmap boundaries are fixed; later phases extend the implementation without reopening the core ownership and coordinate rules. |
| 1 | `implemented` | Local Observation Adapter; device smoke verification pending. |
| 2 | `implemented` | V1 fixtures, validation, bounded simulator, and host benchmark implemented; MessagePack selected for V1. |
| 3 | `in_progress` | 3A, 3B, and 3C are verified. Community discovery, secure pairing, connection management, and bilateral/offline cleanup UI passed target smoke. Official attestation and credential lifecycle remain in 3D. |
| 4 | `in_progress` | Fixed-size Site/Group coordinators, strict canonical-roster quorum, independent term/lease election, authenticated heartbeat transport, RAM-only endpoint tracking, role-cascade avoidance, and atomic published role snapshots are implemented. Same-revision recovery isolation, multi-Node three-role exclusion, bounded handover retry, pairing-poll resilience, the unified three-minute invitation lifetime, adjacent-roster spatial data-plane continuity, incumbent-preserving 5-to-6-to-7 Group roster expansion, and independent Site/Group handoff stability pass host regression tests. Physical five-Node role matrices and seven-Node strict-majority loss/recovery pass; four/six-Node and multi-Group validation remain open. |
| 5 | `in_progress` | Phase 5A shared geometry/replication and the 5B/5C connection, calibration, and activation workflow are verified, including adding a fifth physical Node to an existing four-Node Site. Phase 5D automatic handover is implemented; two-Node, five-Node, and seven-Node takeover/rejoin smoke passes, including authenticated seven-member convergence after Owner loss. Phase 5E now has a bounded directory, authenticated owner/Group metadata convergence, manifest-bound automatic Floorplan routing, and a queue-free serialized owner service; per-Floorplan pairing publication and handover remain. |
| 6 | `implemented` | The fixed-memory seven-source Shadow Fusion core, authenticated development transport/runtime, canonical handover-safe spatial profiles, bounded diagnostics, and production comparison capture are implemented. Repeated one-person captures no longer contain a sustained duplicate-track episode; a real two-person crossing capture remains a production-release gate. |
| 7 | `implemented` | Opt-in production fusion feeds target, presence, motion, Zone, and ESPHome paths through one fixed-size automation state. Complete sequenced snapshots, fail-closed invalidation, stable logical IDs, local diagnostics, and a non-extendable 30-second read-only handoff continuity window are implemented. Five-Node handover continuity and seven-Node quorum-loss/recovery smoke pass; mixed-upgrade and four/six-Node target smoke remain release verification. |
| 8 | `in_progress` | The internal-RAM/HTTP resource gate passes on standalone, two-Node, and steady seven-Node production builds. Each Node persists one bounded seven-member Group roster used by election, transport, and fusion; the Site Leader keeps at most eight authenticated low-rate Group summaries. Seven-Node and 8-by-7 host matrices pass, and physical seven-Node quorum-loss/recovery passes. Seven-input overload/packet-loss, physical four/six-Node role matrices, and multi-Group Site validation remain. |
| 9 | `in_progress` | Secure enrollment, connection management, calibration, upload and polygon-drawn Floorplans, multi-radar placement, physical-wall and door editing, shared occlusion rendering, fusion health, and fused-target display exist. Connected-device actions use one responsive settings dialog, and the pairing trace harness records the complete browser/device workflow. Full local-versus-fused diagnostics, Satellite workflow, and broader browser regression coverage remain. |
| 10 | `planned` | SmartThings Site/Space integration. |
| 11 | `implemented` | Home Assistant NAMO Integration. One entry per Site, one device per Space, Floorplan-less standalone fallback, bounded routing, authority fences, Group diagnostics, persistent-problem Repairs, stable shared automation areas, dynamic Space discovery, and legacy entity cleanup are implemented and pass focused host tests. Home Assistant container, DHCP, handover, HACS, and upgrade verification remain before `verified`. |
| 12 | `planned` | External Satellite SDK and conformance suite. |
| 13 | `in_progress` | Direct-upgrade floorplan-scale hydration, reboot regression coverage, signed OTA, and the volatile 10-minute one-install Developer alpha grant are implemented. Mixed direct/sequential target smoke, public beta migration policy, provisioned-device security smoke, documentation, and soak testing remain. |
| 14 | `planned` | Optional Radar Map Manager compatibility for standalone NAMO sensors through existing ESPHome X/Y entities. Fused Groups and NAMO advanced features remain outside this compatibility path. |
| 15 | `planned` | Optional bounded-consensus formal model and model-checking pass. Revisit after the executable role/authority contract is stable and before v1.0 or any quorum/availability-policy change. |

### Implemented Distributed-Control Corrections

These are release corrections to the current bounded control plane, not a new
architecture phase. The implementation and focused host regressions are complete;
physical verification outside the completed five-Node single-Group matrix remains
required before Phase 4 can return to `verified`:

- **P0 - same-revision recovery isolation:** the authority tracker blocks the
  rejected semantic candidate fingerprint rather than the numeric revision.
  A different quorum-backed source, digest, mode, or route at the same revision
  remains eligible to converge.
- **P1 - hard role-placement invariant:** a multi-Node Site cannot assign
  Configuration Owner, Site Leader, and Group Leader to one physical Node.
  Canonical assignment and live election use the same policy; one-Node
  standalone operation remains allowed.
- **P1 - resilient accepted-operation polling:** an accepted pairing mutation
  is issued once. Transient status-read failures use bounded backoff and final
  state reconciliation instead of replaying the mutation or immediately failing
  the UI.
- **P1 - one visible pairing lifetime:** the RAM invitation prompt, enrollment
  session, and dashboard countdown use the same 180-second lifetime.
- **P1 - bounded handover retry:** candidate construction or persistent-store
  failure schedules capped, jittered retry under the same semantic authority
  fingerprint. Eligibility or fingerprint changes restart the stability fence.
- **P1 - Owner-independent Group authority:** Group election-view identity no
  longer includes transient credential readiness. Credential completeness
  remains a hard candidate gate, while a surviving Group Leader keeps its term
  through Configuration Owner handover and former-Owner rejoin. Heartbeat v10
  provides the new digest and emits v9-compatible views during rolling update.
- **P1 - single-writer role terms:** heartbeat v11 binds election endorsement
  to the selected winner. Only that winner issues a new Site or Group term;
  followers adopt it without independently incrementing it. A surviving or
  suspended same Leader preserves its term, and a replacement advances it once.
  Current, v10, and v9 compatibility views are covered during rolling update.
- **P1 - stable bounded election identity:** heartbeat v12 binds the complete
  configured canonical roster, roster revision, and selected winner while
  keeping liveness, Directory state, and credential readiness as independent
  admission fences. Exact v11 compatibility is emitted during rolling update;
  mixed v11/v12 Site and Group contracts converge in host tests for three
  through seven Nodes.
- **P1 - non-destructive Owner credential refresh:** a canonical Owner or
  authority-epoch change cancels only an in-flight credential bundle and
  refreshes it against the new authority. It preserves authenticated endpoints,
  Site/Group coordinators and terms, summaries, and production fusion. A
  complete previously committed snapshot keeps heartbeat and observation
  traffic active through the bounded handover bootstrap, while an incomplete
  snapshot remains fail-closed. A destructive runtime reset remains reserved
  for a real Site-scope change or explicit platform reset.
- **P1 - vertical workflow proof:** the production dashboard pairing client now
  runs against a stateful firmware-contract harness from invitation and commit
  through Floorplan replication, calibration, bilateral activation, peer outage,
  and healthy rejoin. Native role tests independently exercise the C++ authority
  and persistence paths.
- **P1 - independent Leader handoff:** Site Leader replacement protects the
  current canonical Group Leader when a role-free or Owner-only candidate is
  available. The existing Group Leader and term therefore survive an unrelated
  Site handoff.
- **P1 - coherent published role state:** `/api/platform/groups` serializes one
  immutable Site/Group coordinator generation instead of reading mutable
  election state across an HTTP request.
- **P1 - bounded automation continuity:** a forward Group Leader handoff may
  preserve the last complete automation state for at most 30 seconds as
  non-authoritative read-only data. The deadline cannot be restarted by
  firmware or Home Assistant transport failures, and unsafe configuration or
  spatial mismatches still fail closed immediately.
- **P1 - adjacent-roster transition continuity:** a current-protocol Node
  exactly one generation and roster revision behind or ahead in the same
  authority epoch may retain heartbeat and fusion only when credentials and
  the Floorplan/radar-pose contract still match. Shared writes, owner
  advertisements, and Group summaries remain fenced until the exact canonical
  revision converges. Epoch changes, non-adjacent revisions, legacy wire
  versions, and spatial mismatches cannot use this exception.
- **P1 - non-destructive Leader-eligibility rollout:** changing automatic
  replacement eligibility advances the canonical roster without resetting the
  current Site or Group term. The old and new adjacent views retain only
  matching spatial traffic while shared writes remain fenced, and the settled
  roster returns to the ordinary exact-revision contract.

Focused browser and native regression suites cover each correction, including
seeded one-to-seven-Node role cases. The final full host suite and process-level
two-to-seven-Node cluster matrix pass.
Five-Node role matrices and seven-Node strict-majority loss/recovery smoke also
pass; four/six-Node and multi-Group layouts remain. The separate survivor-membership
policy for operating a five-Node configuration with only two survivors remains
deferred; these corrections do not silently change the strict-majority contract.

## 4. Phase Details

### Phase 0: Design Freeze

Status: implemented. The ownership boundaries, coordinate convention, trust
classes, standalone compatibility rule, independent Site/Group Leader roles,
and hierarchical capacity target are fixed in the English/Korean architecture,
protocol, ADR, and roadmap. Later phase work may extend versioned contracts,
but it must not silently replace these boundaries.

Deliverables:

- approve domain names and ownership boundaries;
- resolve open coordinate-axis and angle-unit decisions;
- choose the first-release scope and explicit non-goals;
- mark the fixed-primary multi-sensor note as superseded;
- keep planned endpoints out of the current API contract.

Exit criteria:

- English and Korean architecture, protocol, roadmap, and ADR agree;
- no document describes planned behavior as currently available;
- the eight-Group Site and seven-input Group limits remain explicit, separate
  design and test targets.

### Phase 1: Local Observation Adapter

Status: implemented; device smoke verification is pending. The adapter is
internal shadow state only; it does not add a
network endpoint, require a Home Assistant custom integration, or alter the
standalone sensor's public state and local presence decisions.

Deliverables:

- adapt confirmed local tracks and bounded non-spatial evidence into an internal
  Observation model;
- keep local acquisition, filtering, and presence decisions unchanged;
- isolate future spatial code from the existing tracker.

Tests and exit criteria:

- current native tracker and replay tests remain green;
- Fusion off produces byte-for-byte equivalent stable public state fixtures;
- missing optional sensor hardware remains silently ignored.

### Phase 2: Protocol Prototype and Simulator

Status: implemented at the host-prototype boundary. The shared coordinate
convention, V1 logical fixtures, host-side validator, bounded Satellite sender,
latest-wins receive inbox, and JSON/MessagePack benchmark are implemented.
MessagePack is selected for V1 because the firmware already bundles a compatible
implementation and it avoids a private binary codec. This phase itself does not
enable production transport or Home Assistant integration; community pairing is
implemented separately in Phase 3C. Target-device
heap/timing/load validation is a mandatory gate before production transport is
enabled.

Deliverables:

- define versioned descriptor, Observation, Group state, and error fixtures;
- benchmark JSON debug and the bundled MessagePack data-plane candidate;
- implement a Python Satellite simulator before embedded peer traffic;
- define fixed limits for payload, tracks, rate, queue depth, and expiry.

Tests and exit criteria:

- malformed, oversized, stale, duplicate, reordered, and restarted streams;
- mixed optional capabilities;
- one, two, six, and seven simulated Group participants;
- MessagePack selected from measurements and repository dependency cost rather
  than wire size alone.

### Phase 3: Site Enrollment, Trust, and Attestation

Status: in progress. Work is split into three explicit boundaries so verified
storage and admission foundations are not confused with unfinished network trust:

- **3A Identity, Registry, and Readiness (`verified`):** stable identity,
  transactional membership/readiness storage, bounded local administration, and
  fail-closed shadow admission are implemented and verified on the target;
- **3B Authenticated Shadow Transport (`verified`):** the bounded authenticated
  host wire contract, golden vector, development receiver, abuse checks, target
  transport smoke, cleanup, and restart smoke passed without production-state
  authority;
- **3C Community Enrollment and Management (`verified`):** transactional
  participant credential ownership, secure community key exchange, on-demand
  discovery, the dedicated device-management dashboard, and retryable bilateral
  disconnect are implemented. Manual-address pairing and automatic discovery
  completed two-device target smoke. The management, bilateral disconnect, and
  offline cleanup UI also passed target-device verification.
- **3D Official Attestation and Credential Lifecycle (`planned`):** manufactured
  device provenance, certificate/root rotation, revocation, replacement, and
  recovery remain separate from community enrollment.

The currently implemented heap-free Participant Registry stores at most 19
remote participants plus the local NAMO Node. This is the legacy flat Site
storage boundary, not the target hierarchical capacity model. It implements
quarantine, explicit approval, revocation, trust/type
compatibility, and Leader eligibility rules. The runtime creates a stable
`nodeId` from the complete hardware MAC and a random standalone `siteId`.
Active V2 storage keeps immutable Node identity separate from mutable Site
membership and migrates the original combined V1 record without changing IDs.
Membership and registry changes use two slots and a checksummed commit pointer so
interrupted writes retain a complete old or new generation. Registry corruption
disables remote participation without disabling standalone sensing. Host tests
cover migration, commit repair, corruption, failed writes, immutable Node identity,
registry lifecycle restoration, and the 19-remote-plus-local capacity boundary.
Target verification exposed two independent embedded stack hazards: oversized
registry scratch values on the setup task and nested HTTP response construction on
the `httpd` task. Stores now own one bounded reusable workspace instead of placing
multiple blobs and snapshots on the task stack. HTTP routing reuses one server-owned
URL buffer, `/api/system/status` writes bounded response sections, and platform
status construction no longer uses a large local serialization array. In the
generated ELF, committed-load fell from 4,080 to 80 bytes, registry initialization
from 1,376 to 32 bytes, top-level request handling to 48 bytes, and platform status
construction to 80 bytes. Firmware `0.5.5-m` completed OTA boot, setup, soft restart,
registry/readiness restoration, and lifecycle smoke on the target without another
`httpd` stack overflow or rollback. The stable identity is exposed read-only through `/api/system/status` and
`/api/platform/descriptor`; `_namo._tcp` advertisements now feed an explicit,
three-second, at-most-19-candidate discovery scan, while manual address entry
remains available. Discovery does not grant trust or open enrollment. A heap-free
enrollment core now limits pairing windows
to five minutes, keeps at most one unapproved candidate in RAM, and closes the
window after successful approval. Approval persists derived trust while leaving
the participant quarantined; activation is a separate persisted mutation. Leader
opt-in, quarantine, revocation, and forget use the same transactional registry
path. Failed persistence does not replace the live registry, and
`official_verified` is rejected until an attestation verifier exists. A local
GET/POST participants API exposes the bounded window and registry lifecycle
without credentials; `pairingEnabled` is true only while that local window is
open. On-device lifecycle smoke completed stage, approval, readiness configuration,
activation, quarantine, forget, cleanup, transport abuse checks, and reboot
restoration. Fast-changing
registry generation numbers are intentionally not used as a completion claim.
Production observation authority is available only through the gated Phase 7
adapter in standard NAMO product builds; official-device attestation verification remains
unimplemented. Secure community enrollment and automatic discovery completed
two-device target smoke. The dedicated connection-management tab, bilateral
disconnect, and offline cleanup also passed target-device verification.

The next admission foundation is now implemented without enabling fusion. A
separate transactionally persisted readiness registry stores bounded capability,
Floorplan/Space/Group assignment, pose version, and calibration version for the
same 19 remote slots without changing the original registry storage format.
Approval still ends in quarantine. `activate` now fails with `not_ready` until a
presence-only participant has a complete non-spatial profile, or a spatial
participant additionally has non-zero pose and calibration versions. Changing an
active profile first persists quarantine. A heap-free shadow admission gate rejects
unknown/inactive participants, wrong Site or Group, capability violations,
pose/calibration version mismatches, expired samples, and duplicate/reordered
sequences. An explicit development build can now receive authenticated network
observations into bounded shadow diagnostics, but it cannot alter production
presence. Normal, beta, and release builds do not instantiate the receiver. Host
tests cover persistence rollback, reboot restoration, activation
gates, reconfiguration quarantine, cleanup, TTL, version, and sequence rejection.
`tools/participant_readiness_smoke.py` batches the device lifecycle into one run.
It never reboots the device. It only removes reserved `satellite-smoke-*` records,
closes pairing before cleanup, and verifies cleanup as part of that operation
instead of issuing a redundant final request. Reboot persistence remains a
separate manual target check after ESPHome marks the boot successful. This Phase
3 transport-smoke path keeps `fusionEnabled=false`; only the gated Phase 7
adapter in a standard product build may publish fused state.

The participant registry, readiness registry, and credential registry do not
consume the 20 KiB ESPHome NVS partition for repeated transactional writes. The
original six fixed 4 KiB sectors at the end of the radar data partition retain
their exact registry/readiness offsets. Three credential sectors were added
immediately before them and use the same two-slot plus checksummed-commit model.
On the first upgraded boot, a valid legacy NVS registry/readiness transaction is copied to
the new area before its legacy keys are removed. Failed migration leaves the NVS
copy intact and disables remote participation without affecting standalone
sensing. Host tests cover full 19-participant migration, interrupted migration,
and repeated full-capacity updates.

Embedded resource gates for the rest of Phase 3:

- no large request, response, registry, or readiness arrays on ESPHome task stacks;
- stream large JSON responses in bounded sections;
- keep persistence mutations transactional while avoiding redundant chained writes;
- inspect generated ELF stack frames before enabling each new firmware transport
  bundle;
- require standalone boot, soft restart, local sensing, and cleanup smoke on a real
  device before a firmware boundary is marked `verified`.

Current target-device smoke baseline, recorded through 2026-08-26:

| Gate | Current result | Evidence required to keep it closed |
| --- | --- | --- |
| Standalone boot, local sensing, and soft restart | Passed | Serial boot without rollback or stack overflow |
| Participant lifecycle, cleanup, and post-restart lifecycle | Passed | `participant_readiness_smoke.py` before and after restart |
| Two-Node discovery, secure pairing, heartbeat loss, and DHCP address recovery | Passed | Both device-management views recover the live peer address |
| Repeated owner floorplan and local radar-placement saves | Passed | Saved state survives refresh without an `httpd` abort |
| Authenticated owner-to-follower floorplan pull and dashboard availability | Passed | Replica persists while Node-local radar pose and zones remain local |
| Internal heap fragmentation, HTTP stack, firmware, and storage margins | Passed | `device_resource_smoke.py` passed on the target; archive its JSON report with release validation |
| Two-Node snapshot calibration, approval, and bilateral activation | Passed | Both directions activate against one geometry revision and digest without rewriting local radar placement |
| Add, calibrate, and activate a fifth Node in an existing four-Node Site | Passed | The connection trace completed invitation, pairing commit, Floorplan replication, configure, bilateral activation, and final 5/5 canonical convergence |
| Sequentially expand one physical Group to seven Nodes | Passed | All seven Nodes reported the same authority epoch, generation, roster revision, and configuration digest; final state was healthy 7/7 with authoritative active fusion, healthy automation, and target resource margins intact |
| Canonical Leader agreement, manual takeover, and healthy rejoin | Passed | `site_leader_handover_smoke.py --handover` confirms takeover and rejoin after power restoration |
| Five-Node single-Group Owner/Site/Group role failure and rejoin | Passed | Single-role outages and all three paired role-holder absence combinations retained automation on the surviving 3/5 majority; no duplicate Site Leader, Group Leader, or concurrent configuration writer was observed. The 2026-08-19 rerun preserved fusion authority through Owner plus Site Leader loss/rejoin and returned to 5/5 |
| Five-Node automatic-replacement eligibility change and subsequent power loss/rejoin | Passed | The eligibility rollout retained Site and Group terms, avoided a false multi-device outage, and the following target power cycle returned to the healthy 5/5 roster |
| Seven-Node strict-majority loss and full rejoin | Passed | The 2026-08-26 capture removed authority at 3/7, then converged at authority epoch 6/generation 136, Site term 45, and Group term 41 after quorum returned. Final state was healthy 7/7 with canonical assignment and complete credentials. All 39 missing-authority samples occurred below quorum; no inactive-authority sample remained once authority existed |
| Seven-input overload, sustained packet loss, and multi-person load | Not verified | Steady operation and quorum recovery do not establish the processing and radio margins under worst-case observation load |
| Four- and six-Node role failure/rejoin matrices | Not verified | Host coverage exists, but the formal physical role-combination matrix has not been repeated at these two sizes |
| Eight-Group Site | Not verified | The 8-by-7 host matrix passes; physical multi-Group pairing, routing, handover, and resource smoke remain |

The default device resource gate is 32 KiB minimum internal heap, 16 KiB
largest internal block, 2 KiB minimum HTTP task stack headroom, 512 KiB firmware
slot headroom, 128 KiB storage headroom, and no more than a 32 KiB internal-free
drop during the bounded read-only exercise. These are initial safety gates, not
claims of verified hierarchical capacity.

Long-running real-use observation and bounded synthetic stress are separate.
`window` scope ignores an unchanged lifetime minimum that predates the run,
but still fails when a new lower lifetime minimum appears. Firmware records the
latest bounded operation boundary that observed a new internal-RAM minimum in
the PSRAM platform workspace.

Archive both target runs with:

```text
python tools/device_resource_smoke.py --device http://DEVICE_IP --mode observe --scope window --duration-seconds 3600 --interval 5 --output device-resource-observe.json
python tools/device_resource_smoke.py --device http://DEVICE_IP --mode stress --scope lifecycle --rounds 30 --concurrency 4 --output device-resource-stress.json
```

Use the repository failure-test wrapper for physical multi-Node capture. It
resolves the workspace and Python interpreter, records endpoint samples in a
timestamped session, writes operator markers from another terminal, and invokes
the analyzer against that same session:

```powershell
.\scripts\namo-failure-test.ps1 roles
.\scripts\namo-failure-test.ps1 capture -DurationHours 2
.\scripts\namo-failure-test.ps1 mark power_off_owner
.\scripts\namo-failure-test.ps1 analyze
.\scripts\namo-failure-test.ps1 session
```

Use repeated `-Device alias=http://DEVICE_IP` arguments for a different test
installation. An intentional `Ctrl+C` leaves an interrupted but analyzable
capture; markers are operator notes, while pass/fail decisions come from the
captured configuration, Group, fusion-authority, and system records.

Phase 3B now has a verified ADR, a 768-byte UDP-unicast authenticated envelope,
MessagePack/HMAC golden vector, host conformance and abuse tests, and an opt-in
receive-only firmware shadow path. The firmware path is development-only, bounded
to two packets per component loop and one pending observation per participant,
and has no production-state authority. Membership credentials remain separate
from Home Assistant API and manufacturer-attestation keys. Actual-device
transport, cleanup, soft-restart, and post-restart lifecycle smoke passed without
stack overflow or rollback, so 3B is verified.

Phase 3C now owns one bounded credential registry for the same nineteen remote
participants. It transactionally stores participant ID, opaque lookup token, key
version, enabled state, and one 32-byte transport key without putting snapshots
on task stacks. Observation transport references this owner instead of copying
all keys; its rate counters remain runtime-only. Public APIs expose metadata but
never token/key bytes. Revoke disables and forget erases a matching credential,
while registry membership remains the fail-closed authority. The development
smoke key is ephemeral. No raw-key HTTP mutation exists.

Secure community enrollment now adds one fixed-size RAM session and one
fixed-size command slot. It uses ephemeral P-256 ECDH, SHA-256 transcript
binding, HKDF-SHA256, role-specific HMAC proofs, and a six-digit code confirmed
on both dashboards. HTTP only queues operations; crypto and transactional writes
run from the component loop so the `httpd` stack does not inherit elliptic-curve
or storage frames. Successful confirmation stores the other Node as
`community_unverified`, leaves it quarantined, and enables its pair credential
for authenticated control-plane traffic. Readiness and explicit activation
remain separate authorization gates. The Joiner changes
Site membership only after registry and credential persistence, keeps
configuration ownership unresolved, and accepts the owner only through an
authenticated canonical pull. The Leader commits one owner before replication.
The same two devices may reverse roles only in a user-confirmed pairing commit
and when no third relationship would be orphaned. Removing the final
relationship resets the RAM pairing state and deterministically restores the
same Site as a one-member standalone configuration. It does not infer a remote
owner or create a new Site identity.
Enrollment into a mature Site now has a separately fenced, one-shot
`authenticated_enrollment` path. It survives reboot from the persisted
reciprocal quarantine and enabled credential, accepts only the exact canonical
Owner, preserves the established Owner Floorplan, and becomes unavailable after
convergence. Non-owner connection attempts are rejected with an actionable
result and the dashboard redirects to the online Configuration Owner. Lost
confirmation responses retain recoverable quarantine state instead of deleting
trust speculatively. Native and dashboard tests cover mature epoch adoption,
wrong source and roster rejection, ambiguous remnants, idempotent retry, Owner
redirection, canonical Floorplan preservation, and uncertain peer commit.
Standalone sensing and Home Assistant remain unchanged. Standard NAMO product
builds report the compiled fusion capability, but the Phase 7 adapter becomes
authoritative only after the Site and Group authority gates pass. Host tests
cover transcript agreement, proof/code rejection, expiry, one-slot backpressure,
deferred processing, and commit gating. The dashboard now provides a dedicated
two-column device-management tab. It starts one bounded discovery scan on entry
and lets the user explicitly run the same scan again. Already registered Nodes
stay out of the new-pairing list, and discovered addresses remain only in the
current dashboard session. Manual address entry remains the fallback. Disconnect
verifies peer identity and removes the relationship on the peer first. If the peer
does not respond, the local credential can be revoked and shown separately as a
pending disconnect until bilateral cleanup can be retried. Explicit local-only
deletion warns that the peer may retain its record. Host tests cover self filtering, compatibility
filtering, deduplication, the 19-remote-candidate bound, registered-candidate
filtering, bilateral operation ordering, identity mismatch, and remote cleanup
failure. Target smoke for the management, bilateral disconnect, and offline
cleanup UI is complete. Manufacturer attestation, key rotation, and replacement
recovery belong to 3D.

Deliverables:

- stable Site, NAMO Node, Satellite, and shared participant identities;
- discovery plus manual-address fallback;
- explicit pairing window, approval, revocation, and quarantine;
- `official_verified`, `community_unverified`, `satellite`, and `unknown` trust classes;
- per-device private key and certificate provisioning contract for manufactured
  NAMO devices;
- manufacturer root, challenge-response, rotation, revocation, replacement, and
  recovery rules;
- current bounded flat registry for 19 remote participants plus the local Node,
  retained until the Phase 8 hierarchy migration;
- Site configuration version and assigned Floorplan/Space/Group.
- bounded capability/readiness profiles and fail-closed activation admission;
- shadow observation admission policy without production-state authority;
- development-only authenticated UDP receiver and bounded diagnostics, disabled
  in normal, beta, and release builds.

Tests and exit criteria:

- DHCP address change does not create a new participant;
- unknown or revoked participant observations are rejected;
- restart changes `bootId` without changing stable participant identity;
- replayed, wrong-product, expired, untrusted-root, and revoked-certificate
  attestation fixtures fail;
- private keys are never returned through public API or diagnostics;
- attestation alone cannot assign membership, capabilities, or Group placement;
- a Community NAMO Node requires explicit approval and separate Leader opt-in;
- the twenty-first active participant is rejected with a bounded error;
- pairing-window expiry, candidate contention, persistence rollback, and reboot
  restoration preserve the last committed generation.
- incomplete or stale readiness cannot activate a participant or admit a spatial
  observation;
- active profile changes quarantine before the new profile can be admitted.
- the explicit transport-smoke image accepts valid HMAC/MessagePack observations,
  rejects bad authentication, rate overflow, and duplicate sequence without
  changing local presence, zones, Home Assistant state, or standalone restart.

### Phase 4: Groups and Leadership

Current implementation status:

- a heap-free coordinator derives Group membership from active readiness records;
- a one-node Site follows the same term and lease contract;
- deterministic election, Leader eligibility, heartbeat expiry, restart
  sequence, stale-term, and the current flat-registry memory bounds have host
  tests;
- `/api/platform/groups` exposes read-only shadow health without affecting
  production presence or fusion;
- the connection dashboard completes authenticated enrollment and bilateral
  removal, but it no longer derives readiness or activation from independently
  stored local floorplans. Enrolled peers remain quarantined until the Site
  configuration owner introduced in Phase 5 commits shared geometry and
  per-Node placement;
- normal firmware exchanges pairwise-HMAC authenticated heartbeats on UDP port
  49191 and feeds active, ready peers into the bounded shadow Group state;
- the ESP-IDF build reserves an explicit 20-socket lwIP budget for the existing
  HTTP/API/mDNS services plus heartbeat and replication UDP transports; target
  regression must still confirm that normal dashboard traffic no longer exhausts
  the HTTP accept path;
- peer IP addresses are learned by stable participant ID, authenticated from the
  UDP source, updated in RAM after DHCP changes, and never persisted;
- a separate heap-free Site coordinator elects exactly one active,
  Leader-eligible NAMO Node without receiving or forwarding target observations;
  its term and lease use the authenticated heartbeat's existing `siteTerm`;
- once a canonical roster is committed, it is the shared source of Site and
  Group Leader eligibility on every Node; pair-local participant records remain
  authentication and liveness boundaries only;
- multi-Node authority requires a strict majority of the configured canonical
  membership and a matching election-view digest. The majority is not
  recalculated from only the currently visible Nodes; changing survivor
  membership is a separate, explicit configuration policy;
- adopting a remote Leader preserves its authenticated term instead of
  incrementing it again. Only the selected local winner issues a replacement
  term newer than the surviving cluster's observed term; a restarted candidate
  cannot replace that term with its local reset value;
- an ineligible local Node cannot self-elect when no eligible Node is online,
  while active non-Leaders still contribute health;
- `/api/platform/groups` exposes the independent Site Leader snapshot alongside
  Group snapshots. The Phase 7 adapter may report active production authority
  only on the elected Group Leader after every runtime gate passes;
- `tools/site_leader_handover_smoke.py` validates reciprocal active/readiness/
  credential state, Site and Group leader agreement, and canonical successor
  eligibility before manual lease takeover and healthy rejoin. It never issues
  an API reboot or changes Leader eligibility;
- the connection dashboard lets the configuration owner explicitly allow or
  revoke fallback coordination for an active Community NAMO Node. It also shows
  a low-severity warning when only one Leader-eligible Node is currently online.
  This reuses canonical `leaderEligible`; future verified-device attestation can
  grant the same permission automatically without replacing the UI or schema;
- active two-Node Sites converge on the same Site and Group Leaders using the
  canonical roster. Target verification with
  `site_leader_handover_smoke.py --handover` passed manual takeover after
  Leader power loss and healthy convergence after power restoration.
  Authoritative shared-configuration commit and replication are implemented in
  Phase 5.
- a physical five-Node single-Group Site passed Configuration Owner, Site Leader,
  and Group Leader power-loss/rejoin smoke, including all three paired role-holder
  absence combinations. The surviving strict majority kept automation available;
  Group handoff used bounded continuity, former authorities rejoined without
  reclaiming their roles, and 252 captured role samples contained no duplicate
  Site Leader or Group Leader. Configuration convergence never enabled more than
  one writer.
- a physical single-Group Site was sequentially expanded to seven Nodes. Pairing,
  Floorplan replication, calibration, activation, final 7/7 canonical agreement,
  authoritative fusion, healthy automation, and target memory/stack/flash/storage
  checks passed. A subsequent seven-Node capture also passed strict-majority loss
  at 3/7, fail-closed authority withdrawal, fresh Site/Group terms after quorum
  recovery, and final healthy 7/7 convergence. Overload remains a separate gate.

Deliverables:

- low-rate Site topology and Group-local heartbeat;
- Group Leader eligibility restricted to approved NAMO Nodes and deterministic
  lease election;
- independent low-rate Site Leader restricted to approved NAMO Nodes;
- at least one NAMO Node required per active Fusion Group;
- Satellite leadership and configuration replication rejected by schema and runtime;
- term, lease, snapshot, and stale-authority handling.

Tests and exit criteria:

- Leader restart, isolation, recovery, and delayed old packets;
- no duplicate Leader is accepted for one term;
- occupancy does not falsely switch off during handover;
- a one-NAMO-Node Group follows the same authority contract.

### Phase 5: Geometry and Calibration

Current implementation status:

| Subphase | Status | Scope |
| --- | --- | --- |
| 5A | `verified` | Versioned Floorplan identity, authoritative manifest, bounded authenticated chunk replication, and atomic replica storage |
| 5B | `verified` | Owner-driven connection wizard, shared-floorplan choice, resume at Floorplan selection after cancellation, local pose/zone preservation, empty-floorplan replication, and physical fifth-Node addition to an existing four-Node Site |
| 5C | `verified` | Two-Node snapshots, bounded translation/rotation proposal, user approval, readiness persistence, bilateral activation, and target-device smoke that calibrates and activates a new Node in an existing four-Node Site |
| 5D | `implemented` | Deterministic automatic configuration-owner handover with a persisted authority epoch, semantic digest fencing, fail-closed split detection, guarded source-selected recovery, retained roster/manifest, authenticated convergence, failed-write suppression, and bounded owner-mediated multi-member credential provisioning. Directional keys are rebound after Owner changes through a bounded authenticated probe while stale follower transmission is suppressed; two-Node, five-Node, and seven-Node takeover/rejoin smoke pass while formal four/six-Node role-handover/rejoin validation remains |
| 5E | `in_progress` | One independent Configuration Owner per Floorplan, one owned Floorplan per physical NAMO Node, and a bounded owner directory. The codec, transactional ESP32 flash store, boot compatibility mirror, deterministic projection, heartbeat v6 metadata convergence, explicit and automatic manifest-bound routing, and queue-free one-follower owner lease are implemented; per-Floorplan pairing publication, per-Floorplan automatic handover, and the public directory API remain. |

The automatic Canonical Site Directory fork-repair loop has been removed.
Normal pairing, eligibility changes, and role handover must converge through
their ordinary enrollment, replication, and election contracts without
rewriting canonical authority. A user-selected `resolveConflict` operation
remains the explicit last-resort path for a proven persistent conflict.

The current single-Floorplan NAMO Node pairing scope closes in 5B and 5C. The
remaining 5E work publishes pairing results into a future multi-Floorplan
directory and adds per-Floorplan handover; it does not reopen the current
connection workflow.

The current 5D tracker suppresses repeated writes after a persistent handover
failure and may remain terminal until eligibility changes or reboot. The P1
correction gate replaces that terminal behavior with bounded, jittered,
authority-fenced retry; it does not permit an infinite flash-write loop.

Phases 5A through 5D describe the currently implemented single-Floorplan,
single-Configuration-Owner Site schema. They remain the compatibility baseline.
Phase 5E replaces the singular owner/manifest fields with a versioned bounded
Floorplan directory; it must not be inferred from the current API.

Phase 5E foundation now includes a separate `SiteDirectorySnapshot` with at
most eight Floorplan entries and eight Group entries, a 4 KiB checksummed codec,
an abstract two-slot commit store, three reserved ESP32 flash sectors for its
two slots and commit marker, and deterministic boot conversion of the current
single committed Floorplan without guessing Group leadership. The boot record
is marked as a compatibility mirror and is refreshed only when the legacy
single-Floorplan source changes; a future canonical multi-Floorplan directory
cannot be overwritten by that mirror path. Native tests cover maximum-size
round trips, duplicate owner/Leader rejection, corruption, commit-marker repair,
interrupted commits, shipped platform-sector address preservation, seven
participants per Group, and 56 aggregate Group-participant references across
eight Groups. That aggregate number is not a flat Site-wide device registry
limit. The elected local Site Leader now promotes the compatibility mirror to a
transactional single-Floorplan projection only after the Floorplan manifest,
all active member spatial assignments, Group sizes, and each Group's agreed
Leader are complete. This transitional record carries explicit provenance, is
refreshed on current configuration updates or deletion, and cannot overwrite a
future native multi-Floorplan directory. Explicit `sync` resolves the local
Group's Floorplan owner through that projection and binds the existing
authenticated chunk transfer to the Floorplan manifest. A wrong explicit owner
or mismatched returned manifest fails closed. Heartbeat v6 now separates the
shared Site-topology fence from independent Floorplan authority, advertises
bounded owner and Group metadata, and lets the Site Leader transactionally
promote or refresh the canonical directory. Automatic synchronization resolves
the local Group route through that directory and reuses the existing direct
owner transport. Pairing publication, independent per-Floorplan handover, the
public directory API, and physical multi-Floorplan smoke remain.

- floorplan document schema version 2 provides a persistent random
  `floorplanId`, an independent `geometryVersion`, and a per-Node radar
  `poseVersion`;
- legacy documents migrate on their next full save, and local geometry and pose
  edits advance only their respective revisions;
- the former dashboard-side local-document comparison and bilateral activation
  shortcut has been removed;
- a bounded, transactional Site-configuration record now persists the
  configuration owner, a sorted legacy flat roster of up to twenty members, roster
  revision, and the reserved floorplan manifest header;
- standalone Nodes bootstrap a one-member committed roster, while upgraded
  enrolled Nodes fail closed with unresolved ownership instead of guessing;
- an unresolved, uncommitted Node can explicitly claim local configuration
  ownership once; confirming a new pairing as Leader uses the same guarded path
  so the owner and canonical roster cannot become asymmetric;
- local ownership claims, confirmed role reversal, roster refresh, and
  last-relationship standalone recovery build one complete candidate roster and
  validate only the final snapshot, avoiding invalid half-transitioned owner
  state;
- the configuration owner can commit the current local non-revoked participant
  set through `/api/platform/site-configuration`;
- the configuration owner can commit an authoritative floorplan manifest using
  the current CONFIG/IMAGE hashes without uploading either payload again;
- paired Nodes replicate the canonical roster and floorplan directly from that
  owner over authenticated, bounded 384-byte UDP chunks;
- successful shared floorplan saves publish the authoritative manifest
  automatically, while Node-local radar pose and zone/tuning state do not;
- followers schedule newer generations in deterministic 2.5-second roster
  slots and use bounded whole-session backoff, preventing nineteen Nodes from
  requesting the owner at once;
- every Node derives the same automatic configuration-owner successor from the
  committed roster: the lexicographically smallest active Leader-eligible NAMO
  Node other than the current owner. The successor must also be the current
  Site Leader, hold the floorplan replica, retain enabled credentials for every
  active member, and remain eligible for 30 seconds before one transactional
  owner change is committed;
- heartbeat v5 fences Site state with authority epoch, generation, and semantic
  digest. Rolling updates keep periodic v4 liveness until the peer proves v5;
  mismatched or unfenced online peers cannot enter election, shared writes,
  directory reconciliation, or fusion;
- one fixed-size Site authority snapshot now classifies the local view as not
  ready, standalone, consistent, converging, or conflicting. Coordinator,
  write, directory, handover, fusion, and API guards consume that single
  decision instead of rescanning heartbeat endpoints independently;
- automatic Site-configuration replication now keeps its pending, active,
  blocked, and bounded-retry state in one fixed-size Site authority tracker in
  the PSRAM platform workspace. The existing authenticated transport and
  transactional storage paths remain unchanged;
- `site_configuration_conflict_repair.py` backs up both sides of a legacy
  equal-generation split, requires an explicit winning source, advances its
  authority epoch, and verifies exact convergence;
- an owner change is accepted only through an authenticated automatic pull
  from that deterministic successor. Manual sync cannot replace the owner, and
  successor pulls preserve the same 2.5-second roster staggering used for
  normal floorplan generations;
- when an automatic successor exists, the current owner accepts shared writes
  only while that successor is online at the same authority epoch, generation,
  configuration digest, and floorplan manifest. Detection and reads continue
  during a partition; shared writes pause
  so the previous and promoted owners cannot both advance canonical state.
  Explicit relationship removal remains available for recovery;
- CONFIG and IMAGE use inactive flash slots and one atomic pair commit, so an
  interrupted pull keeps the previous floorplan active without buffering a
  whole image in RAM;
- heartbeat version 5 reports the authority epoch, replicated generation,
  configuration digest, floorplan digest, and local radar pose. Versions 1
  through 4 remain readable during rolling upgrades, but an unfenced legacy
  peer cannot satisfy every current admission guard;
- equal fenced-revision divergence and stale owner epochs fail closed, and a
  NAMO Node cannot activate until its authenticated online digest matches the
  canonical manifest exactly;
- replicated shared geometry retains each Node's local radar pose sidecar;
- host tests cover the replication codec, stable follower slots, bounded
  backoff, stale/equal-generation conflicts, digest admission, bounded transfer
  state, and transactional replica storage;
- two-device target smoke of a complete owner-to-follower floorplan pull,
  reboot persistence, and dashboard availability is complete;
- the device-management dashboard now drives pairing from one guided workflow:
  select the device, wait up to three minutes for explicit permission on the
  other device, verify one code, choose the shared floorplan, and start
  authenticated replication;
- the workflow preserves each device's radar pose and local zones, requires an
  explicit choice when both devices contain different floorplans, blocks a
  move that would orphan existing connections, and offers a direct return link
  instead of exposing Site, owner, or Node terminology;
- pairing completion now requires both devices to report the same Site,
  configuration owner, floorplan identity, geometry revision, and CONFIG/IMAGE
  hashes. A `returnTo` query remains visible only while the pairing session is
  active or its host matches the invitation or authenticated participant
  endpoint;
- deleting the authoritative floorplan publishes an empty manifest, and
  followers remove the replicated CONFIG and IMAGE when that generation is
  committed;
- the configuration-owner dashboard captures the same single target on both
  Nodes, proposes bounded translation from one snapshot and optional rotation
  from a second separated snapshot, reports residual error, and requires user
  approval before configuring and activating both directions;
- approved effective transforms are versioned readiness data. They do not
  rewrite either Node's local Floorplan radar pose. Partial activation failure
  quarantines both directions;
- direct upgrades from pre-platform firmware preserve the local radar
  placement and calibration sidecars. If only their scale is missing, firmware
  hydrates independent X/Y scale from the stored canonical Floorplan before
  refreshing the owner's canonical spatial profiles. The migration is
  deterministic across reboot and does not rewrite the Floorplan or local
  placement;
- transform metadata extends the existing readiness contract without replacing
  it. Current records without a transform remain readable, while readiness
  versions 1 and 2 fail closed until recalibration because they lack geometry
  and both pose baselines. The future spatial-fusion admission path, not generic
  participant activation, requires an approved transform;
- an observation whose pose or calibration version differs from approved
  readiness is rejected by spatial admission. Active spatial relationships now
  persist the approved geometry plus both local and remote pose revisions.
  Changing shared geometry or either radar pose clears calibration and
  quarantines at most one affected relationship per loop while retaining its
  secure pairing. Reconfiguration of an approved calibration uses the same
  fail-closed quarantine path;
- the automatic handover core retains all dependent members and refuses to
  promote a successor that lacks any required pairwise credential. A two-Node
  Site therefore has a complete path today. Larger Sites now prepare the
  bounded credential topology automatically through the configuration owner.
  Steady seven-Node authenticated topology and seven-member Owner handover/rejoin
  passed target validation. Four/six-Node formal matrices remain open, and the
  implementation blocks instead of silently dropping members;
- host tests cover confirmed crossed-owner role reversal,
  authoritative-manifest convergence, target-aware return-link cleanup, transform
  axes, direct-upgrade scale hydration, reboot persistence, solver bounds,
  bilateral activation rollback, and the
  manual leader-handover smoke tool. Two-device calibration/activation and
  Leader-handover target smoke passed. Automatic two-Node configuration-owner
  takeover and healthy rejoin also passed target smoke. Adding, calibrating,
  and activating a fifth Node in an existing four-Node Site, followed by role
  loss and healthy rejoin, also passed. Formal single-peer role-reversal target
  smoke, similar-angle and different-angle placement verification, and four- or
  six-Node credential-topology handover/rejoin validation remain.

Target multi-Floorplan extension for Phase 5E:

- each active Floorplan has exactly one persisted Configuration Owner, and one
  physical NAMO Node owns at most one active Floorplan;
- each Floorplan has independent owner generation, payload digest, assignment,
  write gate, and automatic handover state;
- a NAMO Node pulls only its assigned Floorplan directly from that Floorplan's
  Configuration Owner and commits CONFIG/IMAGE to its own inactive flash slots;
- a Configuration Owner serves one active replication session at a time.
  Other eligible Nodes wait in deterministic bounded slots and retry with
  bounded whole-session backoff;
- the Site Leader stores only bounded Floorplan owner/digest/route/health and
  Group-summary metadata. It never stores or relays Floorplan payloads by role;
- a physical Node that is both Site Leader and a Floorplan Configuration Owner
  serves payloads only under its Configuration Owner role;
- migration from the current singular owner/manifest schema is transactional
  and preserves the existing Floorplan, local radar pose, zones, calibration,
  and last committed replica on failure.

Deliverables:

- versioned local-to-Floorplan transform;
- shared Site Floorplan and Space geometry;
- overlap snapshot capture and residual report;
- user-approved bounded translation/rotation correction;
- explicit invalidation after pose, geometry, or calibration changes
  (`implemented`; target smoke pending).

Tests and exit criteria:

- transform round trips and axis fixtures;
- similar-angle and different-angle placements;
- large residual requests physical or UI placement correction;
- uncalibrated or stale-version observations cannot enter spatial fusion.

Target verification after both Nodes show `Active`:

```text
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B --handover
python tools/site_leader_handover_smoke.py --device-a http://DEVICE_A --device-b http://DEVICE_B --configuration-owner-handover --timeout-seconds 90
python tools/site_configuration_conflict_repair.py --source http://SELECTED_SOURCE --peer http://OTHER_DEVICE
python tools/site_configuration_conflict_repair.py --source http://SELECTED_SOURCE --peer http://OTHER_DEVICE --confirm --timeout-seconds 90
```

The handover mode never reboots a device. It pauses for the operator to power the
current Leader off, verifies lease takeover on the follower, then pauses for
power-on and verifies healthy convergence.
The configuration-owner mode uses the same manual power control, waits for the
30-second stability guard, and verifies that both devices converge on the new
owner, generation, roster revision, and floorplan manifest after rejoin.
The conflict-repair command is a dry run without `--confirm`; it still backs up
both devices and prints the selected source digest before any authority change.

### Phase 6: Seven-Source Shadow Fusion

Status: implemented. A fixed-memory host-tested core accepts at most seven
already-admitted world-coordinate sources with three tracks each. It performs
bounded spatial association, maintains fused identities independently of local
track IDs, discounts correlated same-angle observations, and weights
different-angle geometry. The follower sends authenticated observations
directly to the elected Group Leader, which joins one local and at most six
admitted remote observations through a bounded runtime and exposes read-only
diagnostics from `GET /api/platform/groups`. The source capacity is tied to
`GroupMembershipView::MAX_PARTICIPANTS` by `static_assert`. Standard NAMO product
builds enable the runtime with persisted credentials; development smoke uses a
separate non-production authority adapter.

Deliverables:

- fixed-size spatial gating and bounded association: implemented in the host
  core;
- fused-track lifecycle independent of local track IDs: implemented in the host
  core;
- correlated-error policy for similar viewing angles: implemented in the host
  core;
- weighted geometry for different viewing angles: implemented in the host core;
- opt-in receive/runtime bridge on the elected Group Leader: implemented;
- authenticated NAMO sender with a shared fixed UDP socket,
  500 ms send gate, no retransmission, and fail-closed readiness checks:
  implemented;
- bounded Shadow output and cycle diagnostics that never grant production
  authority by themselves: implemented;
- canonical per-Node approved transforms in authenticated Site replication,
  including preservation across deterministic configuration-owner handover:
  implemented;
- read-only PC-side comparison of production presence/target count and active
  Shadow output, with optional schema-versioned per-sample JSONL capture:
  implemented. Version 2 retains production coordinates, bounded Shadow tracks,
  per-cycle association diagnostics, and overcount episodes without adding
  device-side history.

Tests and exit criteria:

- duplicate observation of one person becomes one fused candidate: host test
  passes;
- two nearby people do not collapse without adequate evidence: host test
  passes;
- crossing, temporary loss, coordinate jumps, asymmetric sensor quality, local
  ID replacement, and fixed-capacity behavior: host tests pass;
- external comparison capture exposes every disagreement with current
  production state: host tests and the first two-Node target capture pass. The
  first 120-second capture produced 101 active samples, a 1.98% presence
  mismatch rate, and a 16.83% target-count mismatch rate. After tightening
  continuity and single-source admission, the repeated 101-sample capture
  reduced target-count mismatch to 8.91%, overcount to 1.98%, and maximum
  fusion cycle time from 31,463 to 8,744 microseconds. No sustained duplicate
  episode remained in the observed occupied interval.

The authenticated transport, two-Node Leader agreement, configuration-owner
takeover, healthy rejoin, one-person stationary capture, and enter/exit capture
remain regression gates. A real two-person crossing capture is still required
before broad production release, but it does not keep the bounded Shadow core
open.

Target comparison command:

```text
python tools/shadow_fusion_compare.py --device http://GROUP_LEADER --duration-seconds 120 --output shadow-compare.jsonl
```

### Phase 7: Production Fusion and Automation State

Status: implemented. Standard NAMO product builds now include the authenticated
production observation transport. A fixed-size
production adapter publishes only confirmed/coasting tracks, converts
world-coordinate output back to the elected Group Leader's local radar frame,
and feeds the existing target, Zone, state, and Home Assistant count paths.
Presence and motion are only strengthened by authoritative fused evidence.
Any runtime, transport, readiness, source-count, or transform failure reports
`degraded` and preserves the existing local tracker path. Normal builds remain
unchanged and default to production fusion off.

Current implementation status:

- the elected Group Leader publishes bounded confirmed/coasting production
  targets and the existing presence, motion, target-count, Zone, and Home
  Assistant entity paths consume that authoritative output;
- followers continue sending authenticated observations while retaining their
  local sensing and compatibility entities;
- `GET /api/state` exposes compact `fusion` runtime metadata and per-target
  `fused`/`sourceCount` evidence without creating a second coordinate stream;
- the `automation` field of `GET /api/state` exposes one complete, fixed-size logical Group
  snapshot with stable Site/Space/Zone identity, Leader boot and term,
  transport and source sequences, health, occupancy, motion, target count, and
  the complete Zone list. Its `local` field retains the local diagnostic
  values;
- the dashboard hides fusion UI when the build or spatial setup is absent,
  otherwise shows the active/sending/degraded state, active peer radar
  placements on the shared Floorplan, and corroborated targets as
  `T1 · Fused` / `T1 · 융합`;
- the elected Group Leader sends a fixed-size, pairwise-authenticated Fusion
  Snapshot V1 directly to each ready Group participant. A non-computing Node
  keeps only the latest snapshot, rejects stale authority and sequences,
  reprojects canonical world targets through its own approved transform, and
  exposes the result from its local `/api/state`. The dashboard no longer
  fetches another Node by IP, and missing or expired snapshots fall back to
  local display;
- established observation pairs use a bounded release margin, and fused-track
  positions use fixed-memory predictive smoothing with a separate one-source
  dropout response. The dashboard interpolates only 300 ms between 500 ms
  samples to reduce visible steps without adding sluggish control lag;
- host tests cover fail-closed adapter authority, bounded output, coordinate
  conversion, and state serialization. Two-Node target smoke confirms
  authenticated send/admit flow and authoritative fused-track output.
  `site_leader_handover_smoke.py --fusion-snapshot` additionally requires the
  non-computing Node's own `/api/state` to become authoritative and verifies
  its authenticated snapshot source and rejection counters.
- one fixed-size `FusionAutomationState` normalizes the local Group Leader
  output and admitted remote Fusion Snapshot. Target, Zone, state, and ESPHome
  entity paths consume that same output on every Node;
- host regressions reject stale terms and sequences across loss, preserve the
  last accepted ordering fence, convert remote coordinates and velocity into
  the local frame, and serialize unavailable occupancy and Zone values as
  `null` rather than false. Disabling fusion preserves canonical logical IDs;
- the production-fusion resource baseline is archived under Phase 8. Mixed
  direct/sequential upgrade, restart, and handover target smoke remain release
  verification rather than unfinished protocol work.

Deliverables:

- opt-in Fusion Group output;
- occupancy, motion, fused target count, Zone state, transitions, and health;
- complete Group snapshot followed by sequenced updates;
- stable Site/Space/Zone logical identity;
- current local state remains available for diagnostics and compatibility.

Tests and exit criteria:

- no stale term or sequence can change automation state;
- transport loss yields `degraded` or `unavailable`, not unoccupied;
- one-to-two participant expansion preserves Space IDs;
- Fusion can be disabled without migration loss.

### Phase 8: Hierarchical Capacity and Resource Verification

Current foundation:

- authenticated fixed-size Fusion Snapshot fan-out reuses the existing UDP
  transport and pairwise credentials. Each receiving Node keeps only the
  latest snapshot in a PSRAM-first bounded cache and serves it through its
  own `/api/state`; no browser-to-Leader state request or unbounded queue is
  required;
- the first staged internal-RAM reclamation moves the bounded
  `ParticipantReadinessStore` workspace to PSRAM while retaining the small
  storage backend and 256-byte flash I/O staging buffer in internal RAM.
  Allocation failure disables remote readiness instead of silently consuming
  internal RAM. Initial device resource smoke found no progressive heap loss
  or largest-block failure, but the HTTP task fell below its 2 KiB stack gate
  on standalone and two-Node builds. Fixed endpoint attribution identified
  `/api/system/status` and `/api/stats` handlers as the low-water paths. System
  status now streams JSON through one bounded PSRAM-first workspace and caches
  immutable image metadata before HTTP registration. Statistics keep their
  heatmap cells and separate response/persistence workspaces in PSRAM-first
  fixed-lifetime storage, compute all four rolling summaries in one daily-history
  pass, and stream current heatmap RLE without constructing a temporary string.
  The firmware raises the ESP-IDF HTTP task stack from the inherited 4,352-byte
  request to a bounded 6,144 bytes in internal RAM. All local HTTP route families
  are fixed-code classified and measured at request entry and handler completion.
  Firmware 0.5.5-h device smoke measured 1.2-1.3 KiB HTTP task headroom before
  these two changes. Firmware 0.5.5-j passed the complete gate on one
  standalone and both Nodes of an active production-fusion pair: minimum
  internal heap was 74.8, 53.5, and 61.0 KiB; largest free blocks were 62,
  62, and 62 KiB; HTTP task headroom was 3.1, 3.1, and 3.0 KiB; and progressive
  internal-heap drop was 8.4, 3.9, and 7.1 KiB. The pass establishes the current
  standalone/two-Node baseline, not four/six-Node capacity. Firmware 0.5.5-l
  retained the gate after adding the bounded Site directory: the standalone
  Node passed three consecutive 30-round, concurrency-four runs with 72.4 KiB
  minimum internal heap and 3.1 KiB HTTP stack headroom; the two active fusion
  Nodes passed with 52.4/59.8 KiB and 3.0/3.1 KiB respectively, and their
  connections remained online;
- the completed internal-RAM reclamation step moves the canonical remote
  participant, readiness, and Site configuration snapshots, the
  current flat Site and Group coordinator arrays, Site configuration
  update scratch, Fusion Snapshot and remote-observation scratch, participant
  registry and Site configuration store workspaces,
  heartbeat/replication/observation transport objects and fixed packet buffers,
  observation parser, inbox, Fusion Snapshot cache, and Shadow Fusion runtime
  into PSRAM-first fixed-lifetime objects. Every allocation logs the component,
  memory region, size, internal and PSRAM free space, and largest blocks. PSRAM
  failure with internal fallback, complete allocation failure, transport
  dependency failure, socket failure, and replication failure have distinct
  staged logs. FreeRTOS and HTTP task stacks, lwIP socket objects, UART/DMA
  paths, and credential snapshots, registries, and store workspaces remain in
  internal RAM. Firmware 0.5.5-j standalone boot logs confirmed PSRAM allocation
  for the 19,040-byte platform runtime, 2,664-byte participant-registry store,
  4,192-byte Site-configuration store, 8,356-byte readiness store, and the
  heartbeat, replication, observation, and fusion workspaces;
- Site replication now keeps its packet/control object below 2 KiB and allocates
  the two 4 KiB configuration blobs plus decoded snapshot as one PSRAM-only
  workspace. Missing PSRAM disables remote replication with an explicit staged
  log instead of falling back to internal RAM; local sensing and the active
  Floorplan remain available;
- the fixed-memory fusion core and runtime now accept one local plus up to six
  authenticated remote NAMO observations. Input extraction, six remote
  observation records, participant/readiness selection scratch, runtime input,
  and the seven-source fusion scratch are bounded and kept in PSRAM-resident
  workspaces. Source ordering is deterministic, duplicate participant tokens
  are rejected, association uses bounded incremental candidate sets, and output
  remains capped at six tracks. Native regressions cover one, two, four, six,
  and seven inputs, deterministic dropping of excess independent measurements,
  and fail-closed rejection before an eighth source can access the bounded
  arrays. Steady seven-Node resource verification and strict-majority
  loss/recovery pass; overload, timing, and sustained packet-loss verification
  at the supported boundary remain required;
- a configured Group with only one online Node remains a Group instead of
  being rewritten as standalone. After deterministic Leader election, its
  valid local observation is admitted as a one-source production input;
  automation remains available while Site health reports the missing
  configured members as degraded. Native regression covers local-only startup,
  remote loss without track-identity reset, and remote rejoin;
- fixed-memory host tests cover a Site Leader that avoids the configuration
  owner when possible and deterministic Group Leader placement in the current
  flat candidate set while avoiding both roles;
- the configuration owner and then the Site Leader remain bounded fallback
  candidates when no ordinary candidate is available, preserving standalone
  and degraded operation;
- Site-configuration storage version 3 persists approved `spaceId` and
  `groupId` values in every NAMO Node's canonical spatial profile. Coordinator
  election, observation send and receive admission, fusion input selection,
  and Fusion Snapshot fan-out select only the local Node's canonical Group.
  Version 1 and 2 records remain readable and are promoted only when every
  valid readiness assignment identifies the same Group. Ambiguous multi-Group
  records are never resolved by calibration revision order and fail closed.
  Native tests cover the current 4 KiB maximum-length flat encoding,
  legacy-record recovery, and separation of multiple Groups;
- automatic calibration assignment now derives each radar's Space from its own
  Floorplan placement and facing direction rather than from the captured
  person's room. Existing room-derived Group IDs remain valid, multiple Group
  IDs are normalized only within the requested Space, and calibration across
  different Spaces never causes a transitive Group merge. A future bounded
  cross-Space compositor may consume low-rate Group results, but no adjacency
  graph or Site-wide raw-coordinate fusion is part of the current runtime;
- an authenticated RAM-only Shadow path now lets each current Group Leader
  publish one aggregate Space summary every two seconds to the elected Site Leader.
  The fixed eight-Space cache contains only presence, motion, target count,
  Group term, sequence, and identity. It contains no target coordinates, raw
  observations, Zone state, history, or dynamic allocation, and it is not yet
  an automation authority. Stale terms, former Leaders, mismatched Spaces,
  reordered packets, and a ninth Space fail closed;
- the Site Leader's existing bounded siteGroups directory now preserves every
  committed Group identity and exposes an ephemeral automation route whenever
  matching live Group evidence exists. Local Group
  Leaders resolve against the current Site Leader origin; remote Group Leaders
  expose only the IPv4 address learned from their authenticated heartbeat plus
  the fixed /api/state path. Routes are fenced by the surrounding Group
  Leader, term, and canonical Space identity, are never persisted, and become
  unavailable without removing the Group when summary or endpoint evidence
  expires;
- each Node now keeps one Group-local A/B roster in three reserved platform
  sectors. The fixed PSRAM runtime snapshot contains at most the local Node and
  six peers; the coordinator, observation sender and receiver admission,
  Fusion Snapshot fan-out, and fusion-input extraction all consume that same
  roster. A direct upgrade migrates its one local Group from the legacy flat
  Site configuration exactly once. An eighth sensing participant is rejected
  before commit, and a legacy oversized roster is reduced deterministically so
  excluded Nodes fail closed instead of constructing different local views;
- the native matrix covers the seven-member Group limit, Group Leader handoff
  with interrupted roster commits, and eight independent seven-member Group
  rosters while the Site directory retains only eight low-rate summaries. This
  establishes the bounded storage and runtime contracts; physical eight-Group
  pairing and resource smoke remain Phase 8 release gates;
- the existing Site directory now admits a RAM-only cache of at most eight
  low-rate Group Leader summaries. A report is accepted only from the
  directory-assigned Group Leader when Group ID, assignment revision, term,
  and member count agree. The Site Leader stores only Floorplan/Leader routing,
  online/expected counts, health, and a bounded lease. It clears this cache on
  Site Leader handover and never persists or relays target data. Heartbeat v6
  carries the optional authenticated summary, Floorplan route, and separated
  Site-topology fence only to the elected Site Leader; periodic v5/v4
  compatibility packets preserve mixed-firmware liveness. Native
  tests cover 1/8 Group admission, ninth-Group rejection, stale authority,
  expiry, and handover clearing. Physical multi-Group Site smoke remains.
- canonical Site-directory delivery now reuses the existing authenticated,
  384-byte chunked UDP replication transport instead of adding another
  protocol or user workflow. Every active, Leader-eligible successor that is
  not the Site Leader polls the elected Site Leader every 30 seconds, with
  deterministic initial staggering and bounded retry backoff. The receiver
  compares topology revision plus a semantic hash,
  then commits through the existing A/B directory store; local persistence
  generation is not treated as distributed authority. Floorplan CONFIG and
  IMAGE still come directly from each Floorplan's Configuration Owner and are
  never relayed by the Site Leader. Native codec, authorization, stale,
  split-revision, compatibility-mirror, and transactional-store regressions
  pass. Mixed-firmware and multi-Group physical convergence smoke remain.
- each Configuration Owner now serializes Floorplan CONFIG/IMAGE service with
  one fixed-size 2.5-second follower lease and no waiting queue. Competing
  authenticated followers receive a backward-compatible busy response and
  retry through the existing whole-session slots and bounded backoff. The
  lease is refreshed by the active follower, released on completion or a
  terminal response, and expires after abandonment. Native tests cover six
  simultaneous competitors, ownership, completion, and expiry; physical
  multi-follower transfer smoke remains.
- heartbeat v7 now advertises the committed Site Directory revision and digest.
  Site election prefers the freshest fenced Directory, waits one bounded lease
  at multi-candidate boot before assuming missing peers are offline, and uses
  deterministic elected-authority repair for equal-revision divergence.
  Initial enrollment keeps the local compatibility mirror outside role
  coordination and normally fills it from the already authenticated
  Configuration Owner before role authority is admitted. A stale same-Site
  canonical copy does not block activation; ordinary election and replication
  fencing keep the Node non-authoritative until convergence. In a Site with
  more than two active members, a
  non-Site-Leader Owner may serve only when live quorum exists and the elected
  Site Leader advertises the exact same Directory revision and digest. The
  one- and two-member reset fallback remains bounded to the Owner or sole
  canonical peer.
  Canonical reconciliation prunes removed Groups and Floorplans instead of
  retaining permanent unavailable routes. The native failure matrix now keeps
  persisted Directory state and covers stale reboot plus equal-revision
  partition convergence; focused native tests cover deletion pruning and mixed
  v6/v7 heartbeat compatibility.

Target hierarchy:

- one Site Leader maintains at most eight low-rate Group directory and health
  records;
- every active Floorplan has one independent Configuration Owner, and one
  physical NAMO Node owns at most one active Floorplan;
- each Configuration Owner serializes replica delivery to one follower session
  while the Site Leader retains metadata only;
- one Group Leader receives and fuses one local plus at most six remote sensing
  inputs;
- one physical ESP32-S3 leads at most one Fusion Group;
- the Site Leader never receives raw observations or fused-target streams. It
  may retain the bounded low-rate aggregate Space Shadow above, but does not
  relay it or use it as production automation authority;
- Home Assistant and SmartThings resolve Group routes through the Site Leader,
  then read each logical Space state directly from its Group Leader;
- configuration owner plus Site Leader and configuration owner plus Group
  Leader are valid placements. Site Leader plus Group Leader is degraded
  fallback only. All three roles may share one Node only when its Group has no
  other eligible candidate, and that emergency placement yields when one appears.

Deliverables:

- load harness with physical NAMO Nodes and simulated Satellites;
- fixed metrics for network, heap, CPU, queue overflow, and handover;
- fixed-size Site Group-summary directory and Group-local participant registry;
- fixed-size Floorplan owner/digest/route directory with per-Floorplan
  generation and handover state;
- bounded one-session Configuration Owner scheduler with deterministic waiting
  slots and backoff;
- canonical Node-to-Group assignments in Site configuration;
- association candidate reduction before expensive matching;
- documented fused-track and per-cycle processing limits.

Tests and exit criteria:

- one Fusion Group with one, two, four, six, and seven active sensing inputs;
- one Site with one, six, and eight active Group Leader summaries;
- one Site with eight one-Group Floorplans and eight independent Configuration
  Owners without placing payloads in Site Leader state;
- concurrent replica requests converge through one owner-side session without
  losing the previous local replica or growing an unbounded queue;
- an eighth sensing participant in one Group is rejected in the UI and ignored
  fail-closed by firmware without disturbing the seven admitted members;
- a ninth Group is rejected without growing Site state;
- every Node converges on the same canonical Group assignments and
  anti-affinity placement;
- seed-recorded 1-7 Node fault sequences reproduce candidate loss during
  convergence, same-revision conflict, stale restart, repeated former-Owner
  rejoin, transient partition, and delayed packets;
- Group Leader loss, Site Leader loss, DHCP change, packet loss, and healthy
  rejoin preserve logical Space identity;
- the degraded triple-role fallback passes the same resource gates, is used
  only without another eligible Group candidate, and yields when one appears;
- peak resource usage stays inside an approved safety margin;
- local tracking remains responsive under overload and packet loss.

The physical seven-Node steady-state and quorum-loss/recovery baseline may be
reported as verified. Passing the remaining Phase 8 load matrix is required
before documentation may claim verified seven-input overload capacity or an
eight-Group physical Site.

### Phase 9: Dashboard Workflow

Status: in progress. The dedicated connection workflow, secure community
pairing, bilateral removal, shared-Floorplan selection and replication,
calibration/activation wizard, fallback-Leader controls, multiple radar
placements, production-fusion status, and fused-target markers are implemented.
Users can also draw a polygonal whole-Floorplan outline, rooms, physical walls,
hinged or sliding doors, and radar placement on a snapped metric canvas. The
saved view and editor consume the same physical-wall and occlusion geometry.
The standalone dashboard stays unchanged when no multi-sensor setup exists.

`scripts/capture-pairing.ps1` and `tools/pairing_trace_capture.py` correlate
browser stages, API operations, participant state, Site authority, and final
convergence so a delayed or failed pairing can be diagnosed without inferring
its cause from the final UI message alone.

Remaining dashboard scope includes a bounded local-observation versus fused
result comparison, residual/health diagnostics, official and Satellite trust
workflows, and complete onboarding/restore/role-change browser regression
coverage.

Deliverables:

- create or join Site;
- approve, assign, quarantine, calibrate, disable, and remove NAMO Nodes and Satellites;
- show official verification, community approval, and Satellite trust states
  without implying that protocol compatibility means official manufacture;
- require a separate confirmation before granting Leader eligibility to a
  Community NAMO Node;
- place multiple sensors on one Floorplan;
- compare local observations, fused targets, residuals, and health;
- preserve the current uncluttered workflow for a standalone user.

Tests and exit criteria:

- onboarding, refresh, unsaved-change, restore, and role-change browser tests;
- one vertical browser-to-firmware workflow covers pairing commit, shared
  Floorplan replication, calibration, bilateral activation, role loss, and
  healthy rejoin without replaying an accepted mutation after a transient poll
  failure;
- the invitation prompt and cryptographic enrollment session expose one
  consistent three-minute user countdown;
- mobile and desktop layout verification;
- no configuration screen writes an unversioned Site change.

### Phase 10: SmartThings Edge

Deliverables:

- retain legacy physical-device behavior;
- create one stable Site LAN device;
- create stable Space and Zone children from logical IDs;
- use standard occupancy and motion capabilities where possible;
- discover the current Site Leader and read its bounded Group directory;
- poll each current Group Leader directly, not every participant;
- never proxy observations or fused state through the Site Leader;
- switch endpoint without recreating SmartThings devices.

Tests and exit criteria:

- child keys survive Leader and IP changes;
- offline does not emit unoccupied;
- driver and Hub restart preserve children;
- current single-device users continue to receive state.

### Phase 11: Home Assistant Integration

Implementation guide: [Home Assistant Integration](../home-assistant-integration.md)

Status: implemented; target verification remains. `custom_components/namo` consumes the existing bounded Site
Group directory and complete Space automation snapshot without adding a firmware
proxy. Zeroconf and manual setup deduplicate by stable Site identity; Space
entity IDs remain stable across address and Leader changes. A standalone device
without a Floorplan is accepted through the explicit empty-Floorplan sentinel
instead of being reported unavailable.

Canonical shared automation areas are stored in the Floorplan configuration,
edited through the existing detection-area UI only by the Configuration Owner,
replicated by the existing configuration path, and exposed with stable
Site/Floorplan/area identity. Other Nodes render them read-only. Entry migration
removes preview-era duplicate devices and local Zone entities without recreating
Space entities.

Fifty-seven focused host contract and entity tests pass. They cover route bounds, standalone fallback with and without a
Floorplan, multiple Spaces under one Site, stable entity registration, shared
area identity, stale authority, ordering, dynamic Space addition, legacy entity
cleanup, unavailable reason semantics, delayed Repairs, recovery, and version 3
registry migration. This completes the code and focused-test boundary for Phase
11. Cross-Space entry/exit events remain deliberately deferred until transition
authority is canonical and are not part of this completion boundary.

Space diagnostics reuse the bounded Group directory's online/expected counts
and the complete automation snapshot's health and age. One Site diagnostic
aggregates only those bounded records. A short restart remains silent; a Site
that stays non-healthy for 30 seconds creates one Repairs issue and clears it
after recovery.

Deliverables:

- `custom_components/namo` as the development source, exported to the separate
  `NAMO-Home-Assistant` repository;
- HACS Custom Repository installation from the dedicated integration repository
  without waiting for default listing;
- one config entry per `siteId`;
- one Home Assistant device per stable `spaceId`, including one local Space for
  a one-member standalone Site;
- Zeroconf discovery and manual fallback;
- stable Space occupancy, motion, target count, and health entities;
- one bounded Site status entity and delayed persistent-problem Repairs issue;
- expose canonical Floorplan-backed shared automation areas as stable Group
  entities while keeping device-local reduced/filter/exclusion areas private;
- resolve the bounded Group directory from the current Site Leader;
- read each Space state directly from its current Group Leader;
- route internally across Site or Group Leader changes without using the Site
  Leader as a state proxy.

Tests and exit criteria:

Implemented and host-tested:

- Site discovery deduplication and one config entry per stable Site identity;
- single-to-multi expansion and dynamic Space addition keep entity unique IDs;
- stale authority and out-of-order snapshots are ignored;
- timeout becomes unavailable without a false state transition;
- shared area identity survives Configuration Owner and Leader changes;
- preview-era Site devices and local Zone entities are removed by migration;
- existing ESPHome physical entities remain independent by contract.

Target verification required before `verified`:

- fresh HACS Custom Repository installation and integration reload in the target
  Home Assistant container;
- Zeroconf discovery, manual fallback, duplicate setup rejection, and DHCP/IP
  address update against physical devices;
- short and long Site Leader, Group Leader, and Configuration Owner outages with
  stable devices/entities, unavailable semantics, Repairs delay, and recovery;
- an existing preview installation upgraded in place without duplicate, orphaned,
  or renamed Space and shared-area entities.

### Phase 12: External Satellite SDK

Deliverables:

- Python reference Satellite and bridge example;
- machine-readable schema and protocol fixtures;
- conformance server and failure simulation;
- minimal C++ or ESPHome reference after the protocol stabilizes;
- contributor guide for capabilities, pairing, calibration, and security.

Tests and exit criteria:

- a third-party simulator joins as an observer-only Satellite without NAMO tracker code;
- presence-only sources cannot create spatial targets;
- presence-only sources cannot start or re-arm occupancy and may only maintain
  an already armed session;
- a Satellite cannot advertise or receive Leader or configuration-replica authority;
- incompatible required features fail clearly;
- examples pass the same fixtures as firmware.

### Phase 13: Public Beta and Migration

Current foundation:

- a Developer-signed package is rejected before opening the OTA partition
  unless the user granted one installation for at most 10 minutes. The grant
  is RAM-only, is consumed by a successful alpha installation, and does not
  affect unattended Release-signed packages;
- direct upgrades with preserved pairing/readiness and a legacy local radar
  sidecar recover the missing axis scale from the canonical Floorplan at boot;
- current dashboard saves persist both axis scales while keeping the legacy
  scalar average readable;
- frontend fixtures cover sequential upgrades with valid canonical profiles,
  and native tests cover direct-upgrade hydration plus simulated reboot;
- signed OTA is implemented through a fixed P-256 package, preflight trust
  verification, exact upload matching, flashed-partition digest verification,
  A/B Trust Store persistence, Root-authorized release/developer key rotation,
  trust-before-boot activation, Root recovery from a Release-pinned epoch,
  per-release anti-rollback epochs, P-256 replacement-key ID validation,
  signed Developer builds, exact multipart route fencing, and dashboard
  no-downgrade routing. Host tests and a non-destructive 320-byte physical
  mutation and route-confusion smoke tool are complete; provisioned firmware
  compilation and physical-device security smoke remain release gates;
- final compatibility verification requires one firmware build installed on a
  sequentially upgraded pair and a direct-upgrade pair in the same smoke
  session. This target gate is not complete until both pairs expose canonical
  profiles, fusion status, peer radar placement, and fused target rendering.

Deliverables:

- opt-in migration from standalone Site to Fusion Group;
- rollback and backup rules;
- API, dashboard, HA, SmartThings, and Satellite compatibility matrix;
- security, privacy, attestation provisioning, root rotation, revocation, and
  Community NAMO Node guidance;
- release notes that distinguish design target from verified support.
- package-provided release notes in the one-shot firmware completion dialog;
  the completion dialog ships first without an empty placeholder section.

Tests and exit criteria:

- 24-hour minimum real-room soak tests for both viewing-angle patterns;
- upgrade, downgrade, backup, restore, participant removal, and factory reset;
- valid Release and Developer packages, every-byte mutation, unknown and
  revoked keys, rollback, interrupted upload, interrupted Trust Store commit,
  Root recovery, reboot, and resource smoke on a provisioned physical device;
- Developer-signed alpha packages require an explicit volatile 10-minute,
  one-install grant; Release packages remain unattended and unaffected;
- no automatic rewrite of existing HA or SmartThings automations.

### Phase 14: Radar Map Manager Standalone Compatibility

Status: planned and explicitly deferred. This is an optional compatibility
layer, not a prerequisite for NAMO production fusion or Home Assistant support.

Scope:

- support one standalone NAMO radar through the existing ESPHome Target 1-3 X/Y
  entities, millimetre units, and MAC-suffixed entity prefix;
- document a tested RMM profile using 2D placement and no second tracking or
  smoothing layer;
- add no firmware HTTP coordinate API and no duplicate coordinate stream to the
  NAMO Home Assistant integration;
- do not advertise a fused Group as multiple RMM radars. Fusion, shared areas,
  handover, and Site diagnostics remain NAMO-only features;
- treat RMM as a version-pinned, best-effort external compatibility target.

Tests and exit criteria:

- RMM discovers or can manually select the standalone Target X/Y entity prefix;
- target appearance, movement, disappearance, reboot, reconnect, and unavailable
  recovery work without a zero-coordinate ghost;
- 2D orientation and millimetre units match the physical radar view;
- compare the current two-second entity update with a one-second option and only
  change it if visual improvement passes firmware and Home Assistant resource
  regression gates;
- a fused Site remains clearly unsupported by this compatibility path.

### Phase 15: Bounded Consensus Formal Verification

Status: planned and trigger-based. This optional assurance phase did not delay
the completed P0/P1 implementation and does not replace native, browser, virtual-cluster, or
physical-device tests. Revisit it after the executable role and authority
contract is stable, and before v1.0 or any change to quorum, survivor
reconfiguration, or role-availability semantics.

Scope:

- describe the one-to-seven-Node control plane as a small TLA+ state machine;
- model Configuration Owner, Site Leader, Group Leader, canonical membership,
  authority epoch/generation, term, lease, election-view digest, credential
  completeness, partition, power loss, candidate loss, and stale rejoin;
- check safety properties including single authority per canonical view,
  monotonic authority, no stale mutation, fail-closed minority behavior, and no
  three-role placement on one physical Node in a multi-Node Site;
- check liveness under explicit fairness assumptions: a stable eligible quorum
  eventually elects roles, a failed candidate does not leave handover stuck, and
  a stale rejoining Node eventually converges as a follower;
- keep Alloy optional for static membership and role-placement relations only;
  do not introduce a second formal model unless it finds a concrete gap that the
  temporal model cannot express clearly;
- preserve every useful counterexample as a seed or fixture in the executable
  host fault-sequence suite;
- do not generate production firmware from the model or treat model success as
  evidence for HTTP, flash, stack, timing, or radio correctness.

Entry and exit criteria:

- the same-revision recovery, role anti-affinity, pairing polling, and bounded
  handover-retry regressions remain green;
- the safety, liveness, and current survivor-availability contracts exist as
  executable assertions and seeded one-to-seven-Node host tests;
- model checking finds no safety counterexample within the declared bounded
  state space and no liveness counterexample under the documented fairness
  assumptions;
- every discovered counterexample receives an executable regression test before
  the model is considered resolved;
- physical multi-Node failure and rejoin smoke remains a release gate regardless
  of the formal-model result.

## 5. Release Slices

| Slice | Content | User-visible effect |
| --- | --- | --- |
| A | Phases 0-3 | No fusion; platform foundation only. |
| B | Phases 4-6 | Bounded seven-source Shadow Fusion and diagnostics. |
| C | Phase 7 and dashboard minimum | Opt-in bounded seven-source production fusion. |
| D | Phases 10-11 | Stable SmartThings and HA Space automation. |
| E | Phases 8 and 12 | Verified hierarchical capacity and open Satellite SDK. |
| F | Phase 13 | Public platform beta. |
| G | Phase 14 | Optional standalone Radar Map Manager compatibility. |
| H | Phase 15 | Optional bounded-consensus assurance before v1.0 or a quorum-contract change. |

## 6. Stop Conditions

Implementation pauses for design review when:

- standalone behavior changes without an explicit migration;
- a planned API is used before its contract and fixtures exist;
- Leader handover can emit false unoccupied;
- an unbounded allocation or queue appears in a real-time path;
- third-party input can bypass pairing or trust;
- Site growth requires all-to-all high-rate traffic or makes the Site Leader
  relay Group observations or fused targets;
- one ESP32-S3 must lead more than one Fusion Group;
- manufacturer attestation becomes a silent requirement for local community
  NAMO Node operation;
- attestation is treated as capability, membership, or calibration proof;
- HA or SmartThings identity depends on current Leader or IP;
- a large correction hides invalid physical placement.

## 7. Documentation Rule

When a phase starts, mark it `in_progress`. Mark it `implemented` only with
focused tests, and `verified` only after its stated hardware, replay, and
compatibility gates pass. Update both language versions in the same change.
