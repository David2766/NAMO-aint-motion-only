import {
  emitPairingTrace,
  pairingTraceErrorDetail,
  tracePairingPhase,
  type PairingTraceEmitter,
  type PairingTraceState
} from "../pairing-trace";

export type PlatformPairingRole = "leader" | "joiner";
export type PlatformPairingState = "idle" | "prepared" | "exchanged" | "verified" | "committed";

export interface PlatformPairingOffer {
  role: PlatformPairingRole;
  participantId: string;
  siteId: string;
  sessionId: string;
  nonce: string;
  publicKey: string;
}

export interface PlatformPairingStatus {
  ok: boolean;
  supported: boolean;
  busy: boolean;
  acceptedOperationId: number;
  completedOperationId: number;
  lastOperation: string;
  operationResult: string;
  commitResult: string;
  state: PlatformPairingState;
  role: PlatformPairingRole;
  remainingMs: number;
  peerVerified: boolean;
  peerParticipantId: string;
  leaderSiteId: string;
  offer: PlatformPairingOffer | null;
  localProof: string | null;
  confirmationCode: string | null;
  invitation?: {
    state: "pending" | "rejected";
    requesterAddress: string;
    remainingMs: number;
  } | null;
}

type PairingFetcher = typeof fetch;
type PairingSleep = (milliseconds: number) => Promise<void>;

export const PAIRING_INVITATION_DURATION_SECONDS = 180;
export const PAIRING_INVITATION_DURATION_MS =
  PAIRING_INVITATION_DURATION_SECONDS * 1000;

export interface PlatformPairingClientOptions {
  fetcher?: PairingFetcher;
  sleep?: PairingSleep;
  pollIntervalMs?: number;
  operationTimeoutMs?: number;
  traceEmitter?: PairingTraceEmitter;
}

export interface PlatformPairingConnection {
  local: PlatformPairingStatus;
  peer: PlatformPairingStatus;
  confirmationCode: string;
}

export interface PlatformPairingConfirmation {
  local: PlatformPairingStatus;
  peer: PlatformPairingStatus;
}

export interface PlatformDiscoveryCandidate {
  participantId: string;
  participantType: "namo_node";
  product: "namo-spatial-sensor";
  address: string;
  hostname: string;
  port: number;
  protocolMinimum: number;
  protocolMaximum: number;
}

export interface PlatformDiscoveryStatus {
  ok: boolean;
  supported: boolean;
  scanning: boolean;
  generation: number;
  result: "idle" | "ok" | "start_failed";
  candidates: PlatformDiscoveryCandidate[];
}

export interface PlatformDescriptor {
  protocolVersion: number;
  participantId: string;
  participantType: string;
  product: string;
  softwareVersion: string;
  capabilities?: PlatformParticipantCapabilities;
}

export interface PlatformParticipantCapabilities {
  pointTracks: boolean;
  velocity: boolean;
  positionUncertainty: boolean;
  range: boolean;
  presence: boolean;
  motion: boolean;
  zonePresence: boolean;
  bridge: boolean;
  maxTargets: number;
}

export interface PlatformParticipantAssignment {
  floorplanId: string;
  spaceId: string;
  groupId: string;
  geometryVersion: number;
  localPoseVersion: number;
  poseVersion: number;
  calibrationVersion: number;
  transform?: PlatformSpatialTransform | null;
}

export interface PlatformSpatialTransform {
  originXMillimeters: number;
  originYMillimeters: number;
  rotationCentiDegrees: number;
  residualMillimeters: number;
  calibrationSamples: number;
}

export interface PlatformParticipant {
  participantId: string;
  participantType: string;
  trust: string;
  membership: string;
  canonicalMembership?: string | null;
  directMembership?: string | null;
  leaderEligible?: boolean;
  canonicalMember?: boolean;
  directPair?: boolean;
  capabilities?: PlatformParticipantCapabilities | null;
  assignment?: PlatformParticipantAssignment | null;
  directAssignment?: PlatformParticipantAssignment | null;
  credential: {
    provisioned: boolean;
    enabled: boolean;
    keyVersion: number;
  };
  endpoint?: {
    address: string;
    port: number;
    authenticated: boolean;
    online: boolean;
    lastSeenAgeMs: number;
    siteConfigurationAuthorityEpoch?: number;
    siteConfigurationGeneration?: number;
    siteConfigurationDigest?: string;
    siteDirectoryRevision?: number;
    siteDirectoryDigest?: number;
    floorplanGeometryVersion?: number;
    floorplanConfigHash?: number;
    floorplanImageHash?: number;
    radarPoseVersion?: number;
    floorplanReplicaReady?: boolean;
    heartbeatWireVersion?: number;
    siteCredentialsComplete?: boolean;
  } | null;
  readiness?: {
    ready: boolean;
    missing: string[];
  };
}

export type PlatformMembershipView = "canonical" | "direct";

export interface PlatformParticipantsStatus {
  ok: boolean;
  managementAllowed?: boolean;
  registry?: {
    ready: boolean;
    generation: number;
    remoteCount: number;
    activeRemoteCount: number;
    maxRemoteParticipants: number;
    maxParticipants: number;
    projectedRemoteCount?: number;
  };
  pairing?: {
    supported: boolean;
    enabled: boolean;
    remainingMs: number;
    maxDurationMs: number;
  };
  siteReconciliation?: {
    state: "not_required" | "pending" | "complete" | "failed";
    result:
      | "idle"
      | "ok"
      | "invalid_state"
      | "build_failed"
      | "site_storage_failed"
      | "membership_storage_failed";
  };
  participants: PlatformParticipant[];
  heartbeat?: {
    supported: boolean;
    socketReady: boolean;
    port: number;
    endpoints: number;
    onlineEndpoints: number;
    received: number;
    authenticated: number;
    sent: number;
    rejected: number;
    addressChanges: number;
    lastResult: string;
  };
}

export interface PlatformSiteConfigurationStatus {
  ok: boolean;
  schemaVersion: number;
  siteId: string;
  authorityEpoch?: number;
  generation: number;
  configurationDigest?: string;
  configurationConsistent?: boolean;
  rosterRevision: number;
  rosterCommitted: boolean;
  configurationOwnerNodeId: string | null;
  localOwner: boolean;
  sharedWritesAllowed?: boolean;
  memberCount: number;
  activeMemberCount: number;
  members?: Array<{
    participantId: string;
    participantType: string;
    trust: string;
    membership: string;
    leaderEligible: boolean;
    spatialProfile?: {
      spaceId?: string;
      groupId?: string;
      poseVersion: number;
      calibrationVersion: number;
      transform: Pick<
        PlatformSpatialTransform,
        "originXMillimeters" | "originYMillimeters" | "rotationCentiDegrees"
      >;
    } | null;
  }>;
  floorplan: {
    committed: boolean;
    floorplanId: string | null;
    geometryVersion: number;
    configHash: number;
    imageHash: number;
  };
  replication: {
    supported: boolean;
    socketReady: boolean;
    state: string;
    result: string;
    validationResult?: string;
    mode?: "strict" | "automatic_owner_handover" | "authenticated_enrollment";
    operation?: "floorplan" | "site_directory" | string;
    ownerNodeId: string | null;
    remoteGeneration: number;
    transferredBytes: number;
    totalBytes: number;
    requests: number;
    responses: number;
    retries: number;
    rejected: number;
    autoPendingAuthorityEpoch?: number;
    autoPendingGeneration?: number;
    autoStartInMs?: number;
    autoRetryCount?: number;
    directoryAutoActive?: boolean;
    directoryAutoStartInMs?: number;
    directoryAutoRetryCount?: number;
    source?: {
      active: boolean;
      participantId: string | null;
      leaseRemainingMs: number;
      accepted: number;
      busy: number;
      completed: number;
      expired: number;
    };
  } | null;
  automaticHandover?: {
    state: "idle" | "blocked" | "waiting" | "ready" | "complete" | "failed";
    condition:
      | "not_required"
      | "owner_online"
      | "no_successor"
      | "not_designated_successor"
      | "configuration_conflict"
      | "not_site_leader"
      | "replica_not_ready"
      | "credentials_incomplete"
      | "eligible";
    successorNodeId: string | null;
    waitRemainingMs: number;
  };
}

export function pairingPeerRelationship(
  status: PlatformParticipantsStatus,
  localParticipantId: string
): "available" | "resumable" | "managed" {
  const directPeer = status.participants.find(
    (participant) => participant.participantId === localParticipantId &&
      participant.directPair === true
  );
  const otherDirectLink = status.participants.some(
    (participant) => participant.participantId !== localParticipantId &&
      participant.directPair === true &&
      participantMembershipInView(participant, "direct") !== "revoked"
  );
  if (directPeer &&
      participantMembershipInView(directPeer, "direct") === "quarantined" &&
      !otherDirectLink) {
    return "resumable";
  }
  if (directPeer &&
      participantMembershipInView(directPeer, "direct") !== "revoked") {
    return "managed";
  }

  const projectionAware = status.participants.some(
    (participant) => participant.directPair !== undefined ||
      participant.directMembership !== undefined ||
      participant.canonicalMembership !== undefined
  );
  if (projectionAware) {
    const managed = status.participants.some((participant) => {
      const canonicalMembership = participantMembershipInView(participant, "canonical");
      const directMembership = participantMembershipInView(participant, "direct");
      return (participant.canonicalMember === true && canonicalMembership !== "revoked") ||
        (participant.directPair === true && directMembership !== "revoked");
    });
    return managed ? "managed" : "available";
  }

  const links = status.participants.filter(
    (participant) => participant.membership !== "revoked" &&
      (participant.canonicalMember === true ||
       participant.credential?.provisioned === true)
  );
  if (links.length === 1 &&
      links[0].participantId === localParticipantId &&
      links[0].membership === "quarantined") {
    return "resumable";
  }
  return links.length === 0 ? "available" : "managed";
}

export function pairingSetupRelationship(
  localStatus: PlatformParticipantsStatus,
  remoteStatus: PlatformParticipantsStatus,
  localParticipantId: string,
  remoteParticipantId: string
): "new" | "resume" | "blocked" {
  const remoteRelationship = pairingPeerRelationship(
    remoteStatus,
    localParticipantId
  );
  const localPeer = localStatus.participants.find(
    (participant) => participant.participantId === remoteParticipantId &&
      participantMembershipInView(participant, "direct") !== "revoked" &&
      participant.directPair !== false &&
      (participant.canonicalMember === true ||
       participant.credential?.provisioned === true)
  );
  if (remoteRelationship === "resumable") {
    return localPeer &&
      participantMembershipInView(localPeer, "direct") === "quarantined"
      ? "resume"
      : "blocked";
  }
  if (remoteRelationship === "available") {
    return localPeer ? "blocked" : "new";
  }
  return "blocked";
}

export function pairingKeepsCanonicalFloorplan(
  site: PlatformSiteConfigurationStatus
): boolean {
  return site.localOwner && site.rosterCommitted && site.memberCount > 1 &&
    site.floorplan.committed && Boolean(site.floorplan.floorplanId);
}

export interface PairingConnectionGuard {
  allowed: boolean;
  reason: "" | "owner" | "continuity";
  ownerAddress: string;
}

export function pairingConnectionGuard(
  site: PlatformSiteConfigurationStatus,
  participants: PlatformParticipantsStatus
): PairingConnectionGuard {
  if (!site.localOwner) {
    const owner = participants.participants.find(
      (participant) => participant.participantId === site.configurationOwnerNodeId
    );
    return {
      allowed: false,
      reason: "owner",
      ownerAddress: owner ? participantOnlineAddress(owner) : ""
    };
  }
  if (site.configurationConsistent === false ||
      site.sharedWritesAllowed === false) {
    return { allowed: false, reason: "continuity", ownerAddress: "" };
  }
  return { allowed: true, reason: "", ownerAddress: "" };
}

export interface PlatformGroupsStatus {
  ok: boolean;
  supported: boolean;
  capacity?: {
    maxGroups: number;
    maxGroupParticipants: number;
  };
  site?: {
    leaderNodeId: string;
    localLeader: boolean;
    participantsOnline: number;
    participantsExpected: number;
    quorumRequired: number;
    quorumAvailable: boolean;
    health: string;
  };
  groups?: Array<{
    groupId: string | null;
    configured: boolean;
    term: number;
    leaderNodeId: string;
    localLeader: boolean;
    assignmentRevision: number;
    canonicalAssignment: boolean;
    credentialsComplete: boolean;
    leaseRemainingMs: number;
    participantsOnline: number;
    participantsExpected: number;
    quorumRequired: number;
    quorumAvailable: boolean;
    health: string;
  }>;
  siteGroupSummary?: {
    localAuthority: boolean;
    capacity: number;
    configuredGroups: number;
    reportingGroups: number;
    generation: number;
    rejected: number;
    expired: number;
  };
  siteGroups?: Array<{
    groupId: string;
    spaceId: string | null;
    floorplanId: string;
    leaderNodeId: string;
    assignmentRevision: number;
    term: number;
    leaseRemainingMs: number;
    participantsOnline: number;
    participantsExpected: number;
    health: string;
    route: {
      schemaVersion: 1;
      available: boolean;
      local: boolean;
      address: string | null;
      port: number;
      statePath: string;
      lastSeenAgeMs: number | null;
    };
  }>;
  siteSpaceSummary?: {
    shadowOnly: boolean;
    localAuthority: boolean;
    capacity: number;
    reportingSpaces: number;
    generation: number;
    admitted: number;
    rejected: number;
    expired: number;
  };
  siteSpaces?: Array<{
    spaceId: string;
    groupId: string;
    leaderNodeId: string;
    term: number;
    sequence: number;
    sourceSequence: number;
    ageMs: number;
    presence: boolean;
    motion: boolean;
    targetCount: number;
    health: string;
  }>;
  productionFusion?: {
    buildEnabled: boolean;
    authoritative: boolean;
    state: string;
    sourceSequence: number;
    targetCount: number;
  };
}

export interface PlatformSiteConvergenceExpectation {
  siteId: string;
  authorityEpoch: number;
  generation: number;
  configurationDigest: string;
  configurationOwnerNodeId: string;
  floorplanId: string;
  geometryVersion: number;
  configHash: number;
  imageHash: number;
}

export interface PlatformParticipantReplicaExpectation {
  siteConfigurationAuthorityEpoch: number;
  siteConfigurationGeneration: number;
  siteConfigurationDigest: string;
  floorplanGeometryVersion: number;
  floorplanConfigHash: number;
  floorplanImageHash: number;
  radarPoseVersion?: number;
}

export interface PlatformPairDisconnectResult {
  local: PlatformParticipantsStatus;
  peer: PlatformParticipantsStatus;
}

export interface PlatformSiteMemberRemovalResult {
  owner: PlatformParticipantsStatus;
  reset: {
    ok: boolean;
    rebootRequired: boolean;
    rebootInMs: number;
  } | null;
}

export interface PlatformSiteLeaveResult {
  owner: PlatformParticipantsStatus;
  reset: {
    ok: boolean;
    rebootRequired: boolean;
    rebootInMs: number;
  };
}

export interface PlatformPairingDisplay {
  state: PlatformPairingState;
  peerParticipantIds: string[];
}

export type PairingFloorplanUsage = "none" | "local" | "shared" | "received";

export function pairingFloorplanUsage(
  hasLocalFloorplan: boolean,
  connectedParticipantCount: number,
  localOwner: boolean,
  configurationOwnerNodeId: string | null | undefined
): PairingFloorplanUsage {
  if (!hasLocalFloorplan) return "none";
  if (connectedParticipantCount <= 0) return "local";
  if (localOwner) return "shared";
  if (configurationOwnerNodeId) return "received";
  return "local";
}

export function availablePairingCandidates(
  candidates: PlatformDiscoveryCandidate[],
  participants: PlatformParticipant[]
): PlatformDiscoveryCandidate[] {
  return candidates.filter((candidate) => !participantIsRegistered(participants, candidate.participantId));
}

export function rememberDiscoveryCandidates(
  remembered: PlatformDiscoveryCandidate[],
  discovered: PlatformDiscoveryCandidate[]
): PlatformDiscoveryCandidate[] {
  if (discovered.length === 0) return remembered;

  const merged = [...remembered];
  const indexes = new Map(merged.map((candidate, index) => [candidate.participantId, index]));
  for (const candidate of discovered) {
    const index = indexes.get(candidate.participantId);
    if (index === undefined) {
      indexes.set(candidate.participantId, merged.length);
      merged.push(candidate);
    } else {
      merged[index] = candidate;
    }
  }
  return merged;
}

export function participantIsRegistered(
  participants: PlatformParticipant[],
  participantId: string
): boolean {
  return participants.some(
    (participant) => participant.participantId === participantId &&
      (participant.canonicalMember === true ||
       participant.credential?.provisioned === true)
  );
}

export function participantIsOnline(participant: PlatformParticipant): boolean {
  return participant.endpoint?.online === true;
}

export type PairingCalibrationAvailability = "hidden" | "waiting" | "ready";

export type PairingSetupResumeAvailability = "hidden" | "waiting" | "ready";

export function pairingSetupResumeAvailability(
  site: PlatformSiteConfigurationStatus | null,
  participant: PlatformParticipant,
  knownAddress: string
): PairingSetupResumeAvailability {
  const resumable = site?.localOwner === true &&
    site.floorplan.committed &&
    participant.directPair === true &&
    participantMembershipInView(participant, "direct") === "quarantined" &&
    participant.credential?.provisioned === true &&
    Boolean(knownAddress.trim());
  if (!resumable || !site) return "hidden";
  return "ready";
}

export function pairingCalibrationAvailability(
  site: PlatformSiteConfigurationStatus | null,
  participant: PlatformParticipant
): PairingCalibrationAvailability {
  if (!site?.localOwner || !site.floorplan.committed ||
      participant.membership === "revoked" ||
      participant.credential?.provisioned !== true) return "hidden";
  const endpoint = participant.endpoint;
  return pairingCalibrationConvergenceReady(site, participant) &&
    endpoint?.floorplanReplicaReady === true &&
    endpoint.floorplanGeometryVersion === site.floorplan.geometryVersion &&
    endpoint.floorplanConfigHash === site.floorplan.configHash &&
    endpoint.floorplanImageHash === site.floorplan.imageHash
    ? "ready"
    : "waiting";
}

function pairingCalibrationConvergenceReady(
  site: PlatformSiteConfigurationStatus,
  participant: PlatformParticipant
): boolean {
  const endpoint = participant.endpoint;
  return endpoint?.authenticated === true &&
    participantIsOnline(participant) &&
    endpoint.siteConfigurationAuthorityEpoch === site.authorityEpoch &&
    endpoint.siteConfigurationGeneration === site.generation &&
    endpoint.siteConfigurationDigest === site.configurationDigest &&
    (endpoint.siteDirectoryRevision ?? 0) > 0 &&
    (endpoint.siteDirectoryDigest ?? 0) > 0 &&
    site.configurationConsistent === true &&
    Boolean(participantOnlineAddress(participant));
}

export function siteConvergenceExpectation(
  site: PlatformSiteConfigurationStatus
): PlatformSiteConvergenceExpectation {
  const ownerNodeId = site.configurationOwnerNodeId;
  const floorplanId = site.floorplan.floorplanId;
  if (!site.localOwner || !ownerNodeId || !site.floorplan.committed || !floorplanId ||
      !Number.isInteger(site.authorityEpoch) || (site.authorityEpoch ?? 0) < 1 ||
      !/^[0-9a-f]{16}$/i.test(site.configurationDigest ?? "")) {
    throw new PlatformPairingError(
      "site_configuration_unavailable",
      503,
      "site_configuration_unavailable"
    );
  }
  return {
    siteId: site.siteId,
    authorityEpoch: site.authorityEpoch as number,
    generation: site.generation,
    configurationDigest: site.configurationDigest as string,
    configurationOwnerNodeId: ownerNodeId,
    floorplanId,
    geometryVersion: site.floorplan.geometryVersion,
    configHash: site.floorplan.configHash,
    imageHash: site.floorplan.imageHash
  };
}

function siteConvergenceReplicationFailure(
  status: PlatformSiteConfigurationStatus,
  expected: PlatformSiteConvergenceExpectation
): string | null {
  const replication = status.replication;
  if (!replication ||
      (replication.state !== "failed" && replication.state !== "conflict")) {
    return null;
  }
  if (replication.operation && replication.operation !== "floorplan") return null;
  if (replication.ownerNodeId &&
      replication.ownerNodeId !== expected.configurationOwnerNodeId) {
    return null;
  }

  const validation = replication.validationResult;
  if (validation && validation !== "not_evaluated") {
    if (replication.remoteGeneration > 0 &&
        replication.remoteGeneration !== expected.generation) {
      return null;
    }
    return validation;
  }
  const result = replication.result;
  return result && result !== "idle" && result !== "ok" ? result : null;
}

function siteConvergenceFailureCanRecover(code: string): boolean {
  return code === "busy" ||
    code === "owner_offline" ||
    code === "owner_mismatch" ||
    code === "stale_remote";
}

export function communityFallbackControlAvailable(
  participant: PlatformParticipant,
  localOwnsConfiguration: boolean
): boolean {
  return localOwnsConfiguration &&
    participant.participantType === "namo_node" &&
    participant.trust === "community_unverified" &&
    participant.membership === "active";
}

export function onlineLeaderEligibleCount(
  configuration: PlatformSiteConfigurationStatus | null,
  participants: PlatformParticipant[],
  localNodeId: string
): number {
  if (!configuration?.members?.length || !localNodeId) return 0;
  return configuration.members.filter((member) => {
    if (member.membership !== "active" || !member.leaderEligible) return false;
    if (member.participantId === localNodeId) return true;
    const participant = participants.find(
      (candidate) => candidate.participantId === member.participantId
    );
    return participant ? participantIsOnline(participant) : false;
  }).length;
}

export function shouldOfferCommunityFallbackPreparation(
  configuration: PlatformSiteConfigurationStatus | null,
  participants: PlatformParticipant[],
  localNodeId: string,
  participantId: string
): boolean {
  if (!configuration?.localOwner || !localNodeId || !participantId) return false;
  const localMember = configuration.members?.find(
    (member) => member.participantId === localNodeId
  );
  if (localMember?.membership !== "active" || !localMember.leaderEligible) return false;

  const participant = participants.find(
    (candidate) => candidate.participantId === participantId
  );
  const membership = participant?.canonicalMembership ?? participant?.membership;
  if (!participant || participant.participantType !== "namo_node" ||
      participant.trust !== "community_unverified" ||
      membership !== "quarantined" || participant.leaderEligible === true) {
    return false;
  }

  const anotherReadyCandidate = configuration.members?.some((member) => {
    if (member.participantId === localNodeId || member.membership !== "active" ||
        !member.leaderEligible) return false;
    const candidate = participants.find(
      (value) => value.participantId === member.participantId
    );
    return candidate?.endpoint?.online === true &&
      candidate.endpoint.authenticated === true &&
      candidate.endpoint.siteCredentialsComplete === true &&
      candidate.readiness?.ready === true;
  }) ?? false;
  return !anotherReadyCandidate;
}

export function participantOnlineAddress(participant: PlatformParticipant): string {
  return participantIsOnline(participant) ? participant.endpoint?.address ?? "" : "";
}

export function participantUsesSiteMemberRemoval(
  participant: PlatformParticipant
): boolean {
  return participant.canonicalMember === true ||
    (participant.membership === "revoked" &&
      participant.directPair === true &&
      participant.credential.provisioned === true &&
      participant.credential.enabled === false);
}

export class PlatformPairingError extends Error {
  constructor(
    message: string,
    readonly status = 0,
    readonly code = "pairing_request_failed"
  ) {
    super(message);
  }
}

function participantConfigurationMatches(
  participant: PlatformParticipant | undefined,
  capabilities: PlatformParticipantCapabilities,
  assignment: PlatformParticipantAssignment,
  expectedMembership: "active" | "quarantined" | undefined,
  membershipView: PlatformMembershipView
): boolean {
  const actualAssignment = membershipView === "direct"
    ? participant?.directAssignment
    : participant?.assignment;
  if (!participant?.capabilities || !actualAssignment ||
      (expectedMembership !== undefined &&
       participantMembershipInView(participant, membershipView) !== expectedMembership)) return false;
  const actualCapabilities = participant.capabilities;
  const capabilitiesMatch =
    actualCapabilities.pointTracks === capabilities.pointTracks &&
    actualCapabilities.velocity === capabilities.velocity &&
    actualCapabilities.positionUncertainty === capabilities.positionUncertainty &&
    actualCapabilities.range === capabilities.range &&
    actualCapabilities.presence === capabilities.presence &&
    actualCapabilities.motion === capabilities.motion &&
    actualCapabilities.zonePresence === capabilities.zonePresence &&
    actualCapabilities.bridge === capabilities.bridge &&
    actualCapabilities.maxTargets === capabilities.maxTargets;
  const assignmentMatches =
    actualAssignment.floorplanId === assignment.floorplanId &&
    actualAssignment.spaceId === assignment.spaceId &&
    actualAssignment.groupId === assignment.groupId &&
    actualAssignment.geometryVersion === assignment.geometryVersion &&
    actualAssignment.localPoseVersion === assignment.localPoseVersion &&
    actualAssignment.poseVersion === assignment.poseVersion &&
    actualAssignment.calibrationVersion === assignment.calibrationVersion;
  if (!capabilitiesMatch || !assignmentMatches) return false;
  if (!assignment.transform) return true;
  const actualTransform = actualAssignment.transform;
  if (!actualTransform) return false;
  const authoritativeTransformMatches =
    actualTransform.originXMillimeters === assignment.transform.originXMillimeters &&
    actualTransform.originYMillimeters === assignment.transform.originYMillimeters &&
    actualTransform.rotationCentiDegrees === assignment.transform.rotationCentiDegrees;
  if (!authoritativeTransformMatches || membershipView === "canonical")
    return authoritativeTransformMatches;
  return actualTransform.residualMillimeters === assignment.transform.residualMillimeters &&
    actualTransform.calibrationSamples === assignment.transform.calibrationSamples;
}

function legacyDirectConfigurationResponseMatches(
  participant: PlatformParticipant | undefined,
  capabilities: PlatformParticipantCapabilities,
  expectedMembership: "active" | "quarantined"
): boolean {
  if (!participant?.capabilities || participant.directAssignment !== undefined ||
      participantMembershipInView(participant, "direct") !== expectedMembership) return false;
  const actual = participant.capabilities;
  return actual.pointTracks === capabilities.pointTracks &&
    actual.velocity === capabilities.velocity &&
    actual.positionUncertainty === capabilities.positionUncertainty &&
    actual.range === capabilities.range &&
    actual.presence === capabilities.presence &&
    actual.motion === capabilities.motion &&
    actual.zonePresence === capabilities.zonePresence &&
    actual.bridge === capabilities.bridge &&
    actual.maxTargets === capabilities.maxTargets;
}

function participantMembershipInView(
  participant: PlatformParticipant,
  view: PlatformMembershipView
): string | null {
  if (view === "canonical") {
    if (participant.canonicalMembership !== undefined) return participant.canonicalMembership;
    return participant.membership;
  }
  if (participant.directMembership !== undefined) return participant.directMembership;
  return participant.membership;
}

function pairingPollReadCanRetry(cause: unknown): boolean {
  return cause instanceof TypeError ||
    (cause instanceof PlatformPairingError &&
      (cause.status === 0 || cause.status === 408 || cause.status === 429 ||
       cause.status >= 500));
}

function baseUrl(value: string): string {
  const trimmed = value.trim().replace(/\/+$/, "");
  if (!trimmed) return window.location.origin;
  if (/^https?:\/\//i.test(trimmed)) return trimmed;
  return `http://${trimmed}`;
}

async function responseJson<T>(response: Response): Promise<T> {
  let body: Record<string, unknown> = {};
  try {
    body = (await response.json()) as Record<string, unknown>;
  } catch {
    // The HTTP status remains useful when a device closes the connection early.
  }
  if (!response.ok) {
    const info = body.errorInfo as { code?: string } | undefined;
    const code = info?.code ?? String(body.error ?? "pairing_request_failed");
    throw new PlatformPairingError(code, response.status, code);
  }
  return body as T;
}

export function createPlatformPairingClient(options: PlatformPairingClientOptions = {}) {
  const fetcher = options.fetcher ?? fetch;
  const sleep = options.sleep ?? ((milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds)));
  const pollIntervalMs = options.pollIntervalMs ?? 150;
  const operationTimeoutMs = options.operationTimeoutMs ?? 10000;
  const traceEmitter = options.traceEmitter ?? emitPairingTrace;

  function traceEvent(
    phase: string,
    state: PairingTraceState,
    detail?: Record<string, unknown>,
    durationMs?: number
  ): void {
    traceEmitter(phase, state, detail, durationMs);
  }

  function traced<T>(
    phase: string,
    operation: () => Promise<T>,
    detail?: Record<string, unknown>
  ): Promise<T> {
    return tracePairingPhase(phase, operation, detail, traceEmitter);
  }

  async function getStatus(device: string): Promise<PlatformPairingStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/pairing`, {
      cache: "no-store"
    });
    return responseJson<PlatformPairingStatus>(response);
  }

  async function getDiscovery(device: string): Promise<PlatformDiscoveryStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/discovery`, {
      cache: "no-store"
    });
    return responseJson<PlatformDiscoveryStatus>(response);
  }

  async function getDescriptor(device: string): Promise<PlatformDescriptor> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/descriptor`, {
      cache: "no-store"
    });
    return responseJson<PlatformDescriptor>(response);
  }

  async function getParticipants(device: string): Promise<PlatformParticipantsStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/participants`, {
      cache: "no-store"
    });
    return responseJson<PlatformParticipantsStatus>(response);
  }

  async function waitForParticipantReplica(
    device: string,
    participantId: string,
    expected: PlatformParticipantReplicaExpectation,
    timeoutMs = 60000
  ): Promise<PlatformParticipant> {
    const deadline = Date.now() + timeoutMs;
    let lastParticipant: PlatformParticipant | undefined;
    while (Date.now() <= deadline) {
      try {
        const participants = await getParticipants(device);
        const participant = participants.participants.find(
          (item) => item.participantId === participantId
        );
        lastParticipant = participant;
        const endpoint = participant?.endpoint;
        if (participant &&
            endpoint?.authenticated &&
            endpoint?.online &&
            endpoint.floorplanReplicaReady &&
            endpoint.siteConfigurationAuthorityEpoch === expected.siteConfigurationAuthorityEpoch &&
            endpoint.siteConfigurationGeneration === expected.siteConfigurationGeneration &&
            endpoint.siteConfigurationDigest === expected.siteConfigurationDigest &&
            (endpoint.siteDirectoryRevision ?? 0) > 0 &&
            (endpoint.siteDirectoryDigest ?? 0) > 0 &&
            endpoint.floorplanGeometryVersion === expected.floorplanGeometryVersion &&
            endpoint.floorplanConfigHash === expected.floorplanConfigHash &&
            endpoint.floorplanImageHash === expected.floorplanImageHash &&
            (expected.radarPoseVersion === undefined ||
             endpoint.radarPoseVersion === expected.radarPoseVersion)) {
          return participant;
        }
      } catch (cause) {
        if (!pairingPollReadCanRetry(cause)) throw cause;
      }
      await sleep(Math.max(pollIntervalMs, 300));
    }
    const endpoint = lastParticipant?.endpoint;
    if (!lastParticipant)
      throw new PlatformPairingError("participant_not_found", 404, "participant_not_found");
    if (!endpoint?.online)
      throw new PlatformPairingError("endpoint_offline", 408, "endpoint_offline");
    if (!endpoint.authenticated)
      throw new PlatformPairingError("endpoint_offline", 408, "endpoint_offline");
    if (!endpoint.floorplanReplicaReady)
      throw new PlatformPairingError("replica_not_ready", 408, "replica_not_ready");
    if ((endpoint.siteDirectoryRevision ?? 0) <= 0 ||
        (endpoint.siteDirectoryDigest ?? 0) <= 0) {
      throw new PlatformPairingError(
        "site_directory_not_ready",
        408,
        "site_directory_not_ready"
      );
    }
    if (expected.radarPoseVersion !== undefined &&
        endpoint.radarPoseVersion !== expected.radarPoseVersion) {
      throw new PlatformPairingError(
        "radar_pose_version_mismatch",
        409,
        "radar_pose_version_mismatch"
      );
    }
    if (endpoint.siteConfigurationAuthorityEpoch !== expected.siteConfigurationAuthorityEpoch ||
        endpoint.siteConfigurationGeneration !== expected.siteConfigurationGeneration ||
        endpoint.siteConfigurationDigest !== expected.siteConfigurationDigest) {
      throw new PlatformPairingError("site_revision_changed", 409, "site_revision_changed");
    }
    if (endpoint.floorplanGeometryVersion !== expected.floorplanGeometryVersion ||
        endpoint.floorplanConfigHash !== expected.floorplanConfigHash ||
        endpoint.floorplanImageHash !== expected.floorplanImageHash) {
      throw new PlatformPairingError(
        "floorplan_revision_mismatch",
        409,
        "floorplan_revision_mismatch"
      );
    }
    throw new PlatformPairingError("floorplan_replica_timeout", 408, "floorplan_replica_timeout");
  }

  async function waitForParticipantEndpoint(
    device: string,
    participantId: string,
    timeoutMs = 60000
  ): Promise<PlatformParticipant> {
    const deadline = Date.now() + timeoutMs;
    let lastParticipant: PlatformParticipant | undefined;
    while (Date.now() <= deadline) {
      try {
        const participants = await getParticipants(device);
        const participant = participants.participants.find(
          (item) => item.participantId === participantId
        );
        lastParticipant = participant;
        if (participant?.endpoint?.authenticated === true &&
            participant.endpoint.online === true &&
            Boolean(participant.endpoint.address)) {
          return participant;
        }
      } catch (cause) {
        if (!pairingPollReadCanRetry(cause)) throw cause;
      }
      await sleep(Math.max(pollIntervalMs, 300));
    }
    if (!lastParticipant)
      throw new PlatformPairingError("participant_not_found", 404, "participant_not_found");
    throw new PlatformPairingError("endpoint_offline", 408, "endpoint_offline");
  }

  async function getSiteConfiguration(device: string): Promise<PlatformSiteConfigurationStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/site-configuration`, {
      cache: "no-store"
    });
    return responseJson<PlatformSiteConfigurationStatus>(response);
  }

  async function getGroups(device: string): Promise<PlatformGroupsStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/platform/groups`, {
      cache: "no-store"
    });
    return responseJson<PlatformGroupsStatus>(response);
  }

  async function waitForSiteConvergence(
    device: string,
    expected: PlatformSiteConvergenceExpectation,
    timeoutMs = 60000
  ): Promise<PlatformSiteConfigurationStatus> {
    const deadline = Date.now() + timeoutMs;
    let lastStatus: PlatformSiteConfigurationStatus | undefined;
    while (Date.now() <= deadline) {
      try {
        const status = await getSiteConfiguration(device);
        lastStatus = status;
        if (status.siteId === expected.siteId &&
            status.authorityEpoch === expected.authorityEpoch &&
            status.generation === expected.generation &&
            status.configurationDigest === expected.configurationDigest &&
            status.configurationConsistent === true &&
            status.configurationOwnerNodeId === expected.configurationOwnerNodeId &&
            !status.localOwner &&
            status.floorplan.committed &&
            status.floorplan.floorplanId === expected.floorplanId &&
            status.floorplan.geometryVersion === expected.geometryVersion &&
            status.floorplan.configHash === expected.configHash &&
            status.floorplan.imageHash === expected.imageHash) {
          return status;
        }
        const failure = siteConvergenceReplicationFailure(status, expected);
        if (failure && !siteConvergenceFailureCanRecover(failure)) {
          throw new PlatformPairingError(
            failure,
            409,
            failure
          );
        }
      } catch (cause) {
        if (!pairingPollReadCanRetry(cause)) throw cause;
      }
      await sleep(Math.max(pollIntervalMs, 500));
    }
    const finalFailure = lastStatus
      ? siteConvergenceReplicationFailure(lastStatus, expected)
      : null;
    if (finalFailure) {
      throw new PlatformPairingError(finalFailure, 408, finalFailure);
    }
    throw new PlatformPairingError(
      "site_configuration_sync_timeout",
      408,
      "site_configuration_sync_timeout"
    );
  }

  async function getDeviceState(device: string): Promise<import("../../core/types").WebDeviceState> {
    const response = await fetcher(`${baseUrl(device)}/api/state`, { cache: "no-store" });
    return responseJson<import("../../core/types").WebDeviceState>(response);
  }

  async function getFloorplan(device: string): Promise<import("../../core/floorplan/floorplan-storage").FloorplanStorageDocument> {
    const response = await fetcher(`${baseUrl(device)}/api/floorplan`, { cache: "no-store" });
    return responseJson<import("../../core/floorplan/floorplan-storage").FloorplanStorageDocument>(response);
  }

  async function postForm<T>(device: string, path: string, fields: Record<string, string>): Promise<T> {
    const body = new URLSearchParams(fields);
    const response = await fetcher(`${baseUrl(device)}${path}`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: body.toString()
    });
    return responseJson<T>(response);
  }

  async function queue(device: string, action: string, fields: Record<string, string> = {}): Promise<number> {
    const accepted = await postForm<{ operationId: number }>(device, "/api/platform/pairing", {
      action,
      ...fields
    });
    if (!Number.isInteger(accepted.operationId) || accepted.operationId <= 0) {
      throw new PlatformPairingError("invalid_operation_id");
    }
    return accepted.operationId;
  }

  async function waitForOperation(device: string, operationId: number): Promise<PlatformPairingStatus> {
    const deadline = Date.now() + operationTimeoutMs;
    let consecutiveReadFailures = 0;
    while (Date.now() <= deadline) {
      try {
        const status = await getStatus(device);
        consecutiveReadFailures = 0;
        if (status.completedOperationId === operationId && !status.busy) {
          if (status.operationResult !== "ok") {
            throw new PlatformPairingError(status.operationResult, 409, status.operationResult);
          }
          if (status.lastOperation === "confirm" && status.commitResult !== "ok") {
            throw new PlatformPairingError(status.commitResult, 409, status.commitResult);
          }
          return status;
        }
      } catch (cause) {
        if (!pairingPollReadCanRetry(cause)) throw cause;
        consecutiveReadFailures += 1;
        if (consecutiveReadFailures >= 5) throw cause;
        const retryDelay = Math.min(
          Math.max(pollIntervalMs, 100) * (2 ** (consecutiveReadFailures - 1)),
          1000
        );
        await sleep(retryDelay);
        continue;
      }
      await sleep(pollIntervalMs);
    }
    throw new PlatformPairingError("pairing_operation_timeout", 408, "pairing_operation_timeout");
  }

  async function run(device: string, action: string, fields: Record<string, string> = {}): Promise<PlatformPairingStatus> {
    return waitForOperation(device, await queue(device, action, fields));
  }

  async function openWindow(device: string): Promise<void> {
    await postForm(device, "/api/platform/participants", {
      action: "open",
      durationSeconds: "180"
    });
  }

  async function prepare(device: string, role: PlatformPairingRole): Promise<PlatformPairingStatus> {
    await openWindow(device);
    return run(device, "prepare", { role, durationSeconds: "180" });
  }

  async function prepareJoiner(device: string): Promise<PlatformPairingStatus> {
    return prepare(device, "joiner");
  }

  async function requestInvitation(device: string, requesterAddress: string): Promise<void> {
    await postForm(device, "/api/platform/pairing", {
      action: "invite",
      requesterAddress,
      durationSeconds: String(PAIRING_INVITATION_DURATION_SECONDS)
    });
  }

  async function rejectInvitation(device: string): Promise<PlatformPairingStatus> {
    await postForm(device, "/api/platform/pairing", { action: "rejectInvite" });
    return getStatus(device);
  }

  async function dismissInvitation(device: string): Promise<PlatformPairingStatus> {
    await postForm(device, "/api/platform/pairing", { action: "dismissInvite" });
    return getStatus(device);
  }

  async function scan(device: string): Promise<PlatformDiscoveryStatus> {
    await postForm(device, "/api/platform/discovery", { action: "scan" });
    const deadline = Date.now() + operationTimeoutMs;
    while (Date.now() <= deadline) {
      const discovery = await getDiscovery(device);
      if (!discovery.scanning) {
        if (discovery.result === "idle") {
          await sleep(Math.max(pollIntervalMs, 300));
          continue;
        }
        if (discovery.result !== "ok") {
          throw new PlatformPairingError(
            `discovery_${discovery.result}`,
            503,
            `discovery_${discovery.result}`
          );
        }
        return discovery;
      }
      await sleep(Math.max(pollIntervalMs, 300));
    }
    throw new PlatformPairingError("discovery_timeout", 408, "discovery_timeout");
  }

  async function connectLeader(localDevice: string, peerDevice: string): Promise<PlatformPairingConnection> {
    const localPrepared = await prepare(localDevice, "leader");
    const peerPrepared = await getStatus(peerDevice);
    if (peerPrepared.state !== "prepared" || peerPrepared.role !== "joiner" || !peerPrepared.offer) {
      throw new PlatformPairingError("peer_not_ready", 409, "peer_not_ready");
    }
    if (!localPrepared.offer) throw new PlatformPairingError("local_offer_missing");

    const localExchanged = await run(localDevice, "exchange", offerFields(peerPrepared.offer));
    const peerExchanged = await run(peerDevice, "exchange", offerFields(localPrepared.offer));
    if (!localExchanged.localProof || !peerExchanged.localProof) {
      throw new PlatformPairingError("pairing_proof_missing");
    }
    await run(localDevice, "verify", { proof: peerExchanged.localProof });
    await run(peerDevice, "verify", { proof: localExchanged.localProof });
    const local = await getStatus(localDevice);
    const peer = await getStatus(peerDevice);
    if (!local.confirmationCode || local.confirmationCode !== peer.confirmationCode) {
      throw new PlatformPairingError("confirmation_code_mismatch", 409, "confirmation_code_mismatch");
    }
    return { local, peer, confirmationCode: local.confirmationCode };
  }

  async function confirm(device: string, confirmationCode: string): Promise<PlatformPairingStatus> {
    return run(device, "confirm", { confirmationCode });
  }

  async function confirmPair(
    localDevice: string,
    peerDevice: string,
    confirmationCode: string
  ): Promise<PlatformPairingConfirmation> {
    const local = await confirm(localDevice, confirmationCode);
    try {
      const peer = await confirm(peerDevice, confirmationCode);
      return { local, peer };
    } catch (cause) {
      try {
        const peer = await getStatus(peerDevice);
        if (peer.state === "committed" && peer.commitResult === "ok" &&
            peer.peerVerified && peer.leaderSiteId === local.leaderSiteId) {
          return { local, peer };
        }
      } catch {
        throw new PlatformPairingError(
          "pairing_confirmation_uncertain",
          503,
          "pairing_confirmation_uncertain"
        );
      }
      const peerParticipantId = local.peerParticipantId;
      if (peerParticipantId) {
        try {
          await removeParticipant(localDevice, peerParticipantId);
        } catch {
          // Keep the original pairing error. A remaining quarantined record is
          // visible in device management and can be removed safely there.
        }
      }
      throw cause;
    }
  }

  async function cancel(device: string): Promise<PlatformPairingStatus> {
    return run(device, "cancel");
  }

  async function participantAction(
    device: string,
    action: "activate" | "quarantine" | "revoke" | "forget" | "set_community_leader",
    participantId: string,
    extra: Record<string, string> = {},
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus> {
    const verificationView = membershipView ?? "canonical";
    return runParticipantMutation(
      device,
      () => postForm<PlatformParticipantsStatus>(device, "/api/platform/participants", {
        action,
        participantId,
        ...extra
      }),
      (status) => {
        const participant = status.participants.find(
          (candidate) => candidate.participantId === participantId
        );
        if (action === "forget") {
          if (participant === undefined) return true;
          if (membershipView === "direct") return participant.directPair === false;
          if (membershipView === "canonical") return participant.canonicalMember === false;
          return false;
        }
        if (!participant) return false;
        const membership = participantMembershipInView(participant, verificationView);
        if (action === "activate") return membership === "active";
        if (action === "quarantine") return membership === "quarantined";
        if (action === "revoke") return membership === "revoked";
        return participant.leaderEligible === (extra.eligible === "true");
      },
      membershipView !== undefined
    );
  }

  async function setCommunityFallback(
    device: string,
    participantId: string,
    eligible: boolean
  ): Promise<PlatformParticipantsStatus> {
    return participantAction(device, "set_community_leader", participantId, {
      eligible: String(eligible)
    }, "canonical");
  }

  async function configureParticipant(
    device: string,
    participantId: string,
    capabilities: PlatformParticipantCapabilities,
    assignment: PlatformParticipantAssignment,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus> {
    return writeParticipantConfiguration(
      device,
      "configure",
      participantId,
      capabilities,
      assignment,
      membershipView
    );
  }

  async function reconfigureParticipant(
    device: string,
    participantId: string,
    capabilities: PlatformParticipantCapabilities,
    assignment: PlatformParticipantAssignment,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus> {
    return writeParticipantConfiguration(
      device,
      "reconfigure",
      participantId,
      capabilities,
      assignment,
      membershipView
    );
  }

  async function writeParticipantConfiguration(
    device: string,
    action: "configure" | "reconfigure",
    participantId: string,
    capabilities: PlatformParticipantCapabilities,
    assignment: PlatformParticipantAssignment,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus> {
    const transform = assignment.transform;
    const verificationView = membershipView ?? "canonical";
    const expectedMembership = action === "reconfigure" ? "active" : "quarantined";
    return runParticipantMutation(
      device,
      () => postForm<PlatformParticipantsStatus>(device, "/api/platform/participants", {
        action,
        participantId,
        pointTracks: String(capabilities.pointTracks),
        velocity: String(capabilities.velocity),
        positionUncertainty: String(capabilities.positionUncertainty),
        range: String(capabilities.range),
        presence: String(capabilities.presence),
        motion: String(capabilities.motion),
        zonePresence: String(capabilities.zonePresence),
        bridge: String(capabilities.bridge),
        maxTargets: String(capabilities.maxTargets),
        automaticGroup: "true",
        floorplanId: assignment.floorplanId,
        spaceId: assignment.spaceId,
        groupId: assignment.groupId,
        geometryVersion: String(assignment.geometryVersion),
        localPoseVersion: String(assignment.localPoseVersion),
        poseVersion: String(assignment.poseVersion),
        calibrationVersion: String(assignment.calibrationVersion),
        ...(transform ? {
          originXMillimeters: String(transform.originXMillimeters),
          originYMillimeters: String(transform.originYMillimeters),
          rotationCentiDegrees: String(transform.rotationCentiDegrees),
          residualMillimeters: String(transform.residualMillimeters),
          calibrationSamples: String(transform.calibrationSamples)
        } : {})
      }),
      (status) => {
        const participant = status.participants.find(
          (candidate) => candidate.participantId === participantId
        );
        return participantConfigurationMatches(
          participant,
          capabilities,
          assignment,
          expectedMembership,
          verificationView
        );
      },
      membershipView !== undefined,
      verificationView === "direct"
        ? (status) => legacyDirectConfigurationResponseMatches(
            status.participants.find((candidate) => candidate.participantId === participantId),
            capabilities,
            expectedMembership
          )
        : undefined
    );
  }

  async function runParticipantMutation(
    device: string,
    mutation: () => Promise<PlatformParticipantsStatus>,
    applied: (status: PlatformParticipantsStatus) => boolean,
    verifySuccessfulResponse = false,
    acceptSuccessfulResponse?: (status: PlatformParticipantsStatus) => boolean
  ): Promise<PlatformParticipantsStatus> {
    let lastCause: unknown;
    if (verifySuccessfulResponse) {
      try {
        const current = await getParticipants(device);
        if (applied(current)) return current;
      } catch {
        // A failed preflight read must not prevent the bounded mutation itself.
      }
    }
    for (let attempt = 0; attempt < 3; attempt += 1) {
      let successfulResponseNeedsConvergence = false;
      try {
        const status = await mutation();
        if (!verifySuccessfulResponse || applied(status) ||
            acceptSuccessfulResponse?.(status)) return status;
        successfulResponseNeedsConvergence = true;
        lastCause = new PlatformPairingError(
          "participant_mutation_not_applied",
          409,
          "participant_mutation_not_applied"
        );
      } catch (cause) {
        lastCause = cause;
        const retryable = cause instanceof TypeError ||
          (cause instanceof PlatformPairingError && cause.code === "busy");
        if (!retryable) throw cause;
      }
      const convergenceReads = successfulResponseNeedsConvergence ? 3 : 1;
      for (let readAttempt = 0; readAttempt < convergenceReads; readAttempt += 1) {
        if (readAttempt > 0) await sleep(Math.max(pollIntervalMs, 300));
        try {
          const status = await getParticipants(device);
          if (applied(status)) return status;
        } catch {
          // Preserve the mutation result while bounded readback continues.
        }
      }
      if (successfulResponseNeedsConvergence) throw lastCause;
      if (attempt < 2) await sleep(Math.max(pollIntervalMs, 300));
    }
    throw lastCause;
  }

  async function removeParticipant(
    device: string,
    participantId: string,
    scope: "all" | "direct" = "all"
  ): Promise<PlatformParticipantsStatus> {
    let current = await getParticipants(device);
    let participant = current.participants.find((item) => item.participantId === participantId);
    if (!participant) return current;
    if (scope === "direct" && participant.directPair === false) return current;
    const mutationView: PlatformMembershipView =
      participant.directPair === true ? "direct" : "canonical";

    if (participantMembershipInView(participant, mutationView) !== "revoked") {
      await participantAction(device, "revoke", participantId, {}, mutationView);
      current = await getParticipants(device);
      participant = current.participants.find((item) => item.participantId === participantId);
      if (participant &&
          participantMembershipInView(participant, mutationView) !== "revoked" &&
          !(mutationView === "direct" && participant.directPair === false)) {
        throw new PlatformPairingError("participant_revoke_incomplete", 409, "participant_revoke_incomplete");
      }
    }

    if (participant &&
        participantMembershipInView(participant, mutationView) === "revoked") {
      await participantAction(device, "forget", participantId, {}, mutationView);
      current = await getParticipants(device);
    }
    if (current.participants.some((item) => {
      if (item.participantId !== participantId) return false;
      if (mutationView === "direct") return item.directPair === true;
      return item.canonicalMember === true;
    })) {
      throw new PlatformPairingError("participant_forget_incomplete", 409, "participant_forget_incomplete");
    }
    return current;
  }

  async function revokeParticipant(
    device: string,
    participantId: string
  ): Promise<PlatformParticipantsStatus> {
    return participantAction(device, "revoke", participantId);
  }

  async function forgetParticipant(
    device: string,
    participantId: string
  ): Promise<PlatformParticipantsStatus> {
    return participantAction(device, "forget", participantId);
  }

  async function disconnectPair(
    localDevice: string,
    peerDevice: string,
    localParticipantId: string,
    peerParticipantId: string
  ): Promise<PlatformPairDisconnectResult> {
    const peerDescriptor = await getDescriptor(peerDevice);
    if (peerDescriptor.participantId !== peerParticipantId) {
      throw new PlatformPairingError("peer_identity_mismatch", 409, "peer_identity_mismatch");
    }

    const localBefore = await getParticipants(localDevice);
    const localRecord = localBefore.participants.find(
      (item) => item.participantId === peerParticipantId
    );
    if (!localRecord) {
      throw new PlatformPairingError("local_pairing_record_missing", 404, "local_pairing_record_missing");
    }
    if (localRecord.directPair === false) {
      throw new PlatformPairingError("direct_pair_not_found", 409, "direct_pair_not_found");
    }

    // Remove an existing direct record from the peer first. A canonical-only
    // projection is not peer-local state, so the Owner can continue with its
    // canonical removal without sending an invalid peer mutation.
    const peer = await removeParticipant(peerDevice, localParticipantId, "direct");
    const local = await removeParticipant(localDevice, peerParticipantId);
    return { local, peer };
  }

  function siteMemberState(
    site: PlatformSiteConfigurationStatus,
    participantId: string
  ): string | null {
    return site.members?.find(
      (member) => member.participantId === participantId
    )?.membership ?? null;
  }

  function removalParticipantTrace(
    status: PlatformParticipantsStatus,
    participantId: string
  ): Record<string, unknown> {
    const participant = status.participants.find(
      (candidate) => candidate.participantId === participantId
    );
    return {
      registryGeneration: status.registry?.generation ?? null,
      participantCount: status.participants.length,
      targetPresent: participant !== undefined,
      targetMembership: participant?.membership ?? null,
      targetCanonicalMembership: participant?.canonicalMembership ?? null,
      targetDirectMembership: participant?.directMembership ?? null,
      targetCanonicalMember: participant?.canonicalMember ?? false,
      targetDirectPair: participant?.directPair ?? false,
      targetCredentialProvisioned: participant?.credential.provisioned ?? false,
      targetCredentialEnabled: participant?.credential.enabled ?? false,
      targetOnline: participant?.endpoint?.online ?? false,
      targetAuthenticated: participant?.endpoint?.authenticated ?? false
    };
  }

  function removalSiteTrace(
    site: PlatformSiteConfigurationStatus,
    participantId: string
  ): Record<string, unknown> {
    return {
      siteId: site.siteId,
      authorityEpoch: site.authorityEpoch ?? null,
      generation: site.generation,
      rosterRevision: site.rosterRevision,
      configurationDigest: site.configurationDigest ?? null,
      configurationConsistent: site.configurationConsistent ?? null,
      configurationOwnerNodeId: site.configurationOwnerNodeId,
      localOwner: site.localOwner,
      sharedWritesAllowed: site.sharedWritesAllowed ?? null,
      memberCount: site.memberCount,
      activeMemberCount: site.activeMemberCount,
      targetMembership: siteMemberState(site, participantId),
      replicationState: site.replication?.state ?? null,
      replicationResult: site.replication?.result ?? null
    };
  }

  function siteRevisionMatches(
    status: PlatformSiteConfigurationStatus,
    expected: PlatformSiteConfigurationStatus
  ): boolean {
    return status.siteId === expected.siteId &&
      status.authorityEpoch === expected.authorityEpoch &&
      status.generation === expected.generation &&
      status.rosterRevision === expected.rosterRevision &&
      status.configurationOwnerNodeId === expected.configurationOwnerNodeId &&
      (!expected.configurationDigest ||
        status.configurationDigest === expected.configurationDigest);
  }

  async function waitForSiteMemberMutationReplication(
    ownerDevice: string,
    ownerParticipantId: string,
    targetParticipantId: string,
    originalMemberCount: number,
    ownerParticipants: PlatformParticipantsStatus,
    expected: PlatformSiteConfigurationStatus,
    expectedMembership: "revoked" | "absent"
  ): Promise<void> {
    const requiredCopies = originalMemberCount <= 2
      ? 1
      : Math.floor(originalMemberCount / 2) + 1;
    if (requiredCopies === 1) {
      traceEvent("removal.replication.sample", "event", {
        requiredCopies,
        matchingCopies: 1,
        replicaCount: 0,
        expectedMembership,
        result: "owner_copy_sufficient"
      });
      return;
    }

    const replicaAddresses = ownerParticipants.participants
      .filter((participant) =>
        participant.participantId !== targetParticipantId &&
        participant.canonicalMember === true &&
        participantMembershipInView(participant, "canonical") === "active" &&
        participant.endpoint?.authenticated === true &&
        participant.endpoint.online === true &&
        Boolean(participant.endpoint.address)
      )
      .map((participant) => participant.endpoint?.address ?? "");
    const replicationTimeoutMs = operationTimeoutMs < 0
      ? operationTimeoutMs
      : Math.max(operationTimeoutMs, 30000);
    const deadline = Date.now() + replicationTimeoutMs;
    let attempt = 0;
    let lastSample = "";

    do {
      attempt += 1;
      const ownerStatus = await getSiteConfiguration(ownerDevice);
      if (!ownerStatus.localOwner ||
          ownerStatus.configurationOwnerNodeId !== ownerParticipantId) {
        traceEvent("removal.replication.guard", "error", {
          reason: "owner_changed",
          attempt,
          ...removalSiteTrace(ownerStatus, targetParticipantId)
        });
        throw new PlatformPairingError(
          "site_member_removal_owner_changed",
          409,
          "site_member_removal_owner_changed"
        );
      }
      if (!siteRevisionMatches(ownerStatus, expected)) {
        traceEvent("removal.replication.guard", "error", {
          reason: "revision_superseded",
          attempt,
          expectedAuthorityEpoch: expected.authorityEpoch ?? null,
          expectedGeneration: expected.generation,
          expectedRosterRevision: expected.rosterRevision,
          ...removalSiteTrace(ownerStatus, targetParticipantId)
        });
        throw new PlatformPairingError(
          "site_member_removal_superseded",
          409,
          "site_member_removal_superseded"
        );
      }

      let matchingCopies = 1;
      let failedReads = 0;
      let revisionMismatches = 0;
      let membershipMismatches = 0;
      const replicas = await Promise.allSettled(
        replicaAddresses.map((address) => getSiteConfiguration(address))
      );
      for (const replica of replicas) {
        if (replica.status !== "fulfilled") {
          failedReads += 1;
          continue;
        }
        if (!siteRevisionMatches(replica.value, expected)) {
          revisionMismatches += 1;
          continue;
        }
        const membership = siteMemberState(replica.value, targetParticipantId);
        if ((expectedMembership === "revoked" && membership === "revoked") ||
            (expectedMembership === "absent" && membership === null)) {
          matchingCopies += 1;
        } else {
          membershipMismatches += 1;
        }
      }
      const sampleState = {
        requiredCopies,
        matchingCopies,
        replicaCount: replicaAddresses.length,
        failedReads,
        revisionMismatches,
        membershipMismatches,
        expectedMembership
      };
      const sampleSignature = JSON.stringify(sampleState);
      if (sampleSignature !== lastSample || attempt % 10 === 0) {
        traceEvent("removal.replication.sample", "event", { attempt, ...sampleState });
        lastSample = sampleSignature;
      }
      if (matchingCopies >= requiredCopies) return;
      await sleep(Math.max(pollIntervalMs, 500));
    } while (Date.now() <= deadline);

    throw new PlatformPairingError(
      "site_member_removal_replication_pending",
      408,
      "site_member_removal_replication_pending"
    );
  }

  function isStandaloneSite(
    site: PlatformSiteConfigurationStatus,
    participantId: string,
    previousSiteId: string
  ): boolean {
    return site.siteId !== previousSiteId &&
      site.localOwner &&
      site.configurationOwnerNodeId === participantId &&
      site.memberCount === 1 &&
      site.activeMemberCount === 1 &&
      siteMemberState(site, participantId) === "active";
  }

  async function waitForStandaloneSite(
    device: string,
    participantId: string,
    previousSiteId: string
  ): Promise<PlatformSiteConfigurationStatus> {
    const timeoutMs = operationTimeoutMs < 0
      ? operationTimeoutMs
      : Math.max(operationTimeoutMs, 30000);
    const deadline = Date.now() + timeoutMs;
    let attempt = 0;
    let lastSample = "";
    do {
      attempt += 1;
      try {
        const site = await getSiteConfiguration(device);
        const sampleState = removalSiteTrace(site, participantId);
        const sampleSignature = JSON.stringify(sampleState);
        if (sampleSignature !== lastSample || attempt % 10 === 0) {
          traceEvent("removal.peer_standalone.sample", "event", {
            attempt,
            ...sampleState
          });
          lastSample = sampleSignature;
        }
        if (isStandaloneSite(site, participantId, previousSiteId)) return site;
      } catch (cause) {
        traceEvent("removal.peer_standalone.read_error", "event", {
          attempt,
          ...pairingTraceErrorDetail(cause)
        });
        if (!pairingPollReadCanRetry(cause)) throw cause;
      }
      await sleep(Math.max(pollIntervalMs, 500));
    } while (Date.now() <= deadline);
    throw new PlatformPairingError(
      "site_member_reset_unconfirmed",
      408,
      "site_member_reset_unconfirmed"
    );
  }

  async function removeSiteMember(
    ownerDevice: string,
    peerDevice: string,
    ownerParticipantId: string,
    peerParticipantId: string
  ): Promise<PlatformSiteMemberRemovalResult> {
    const [peerDescriptor, ownerSiteBefore, peerSiteBefore, ownerBefore] =
      await traced("removal.preflight.read", () => Promise.all([
          getDescriptor(peerDevice),
          getSiteConfiguration(ownerDevice),
          getSiteConfiguration(peerDevice),
          getParticipants(ownerDevice)
        ]), { ownerParticipantId, peerParticipantId });
    traceEvent("removal.preflight.state", "event", {
      ownerParticipantId,
      peerParticipantId,
      peerDescriptorParticipantId: peerDescriptor.participantId,
      ownerSite: removalSiteTrace(ownerSiteBefore, peerParticipantId),
      peerSite: removalSiteTrace(peerSiteBefore, peerParticipantId),
      ownerParticipant: removalParticipantTrace(ownerBefore, peerParticipantId)
    });
    if (peerDescriptor.participantId !== peerParticipantId) {
      throw new PlatformPairingError(
        "peer_identity_mismatch",
        409,
        "peer_identity_mismatch"
      );
    }
    if (!ownerSiteBefore.localOwner ||
        ownerSiteBefore.configurationOwnerNodeId !== ownerParticipantId) {
      throw new PlatformPairingError(
        "site_member_removal_owner_changed",
        409,
        "site_member_removal_owner_changed"
      );
    }
    const ownerRecord = ownerBefore.participants.find(
      (participant) => participant.participantId === peerParticipantId
    );
    const ownerCanonicalMember = ownerRecord?.canonicalMember === true;
    const ownerRemovalPending = ownerRecord !== undefined &&
      ownerRecord.canonicalMember !== true &&
      ownerRecord.directPair === true &&
      participantMembershipInView(ownerRecord, "direct") === "revoked" &&
      ownerRecord.credential.provisioned === true &&
      ownerRecord.credential.enabled === false &&
      siteMemberState(ownerSiteBefore, peerParticipantId) === null;
    if (!ownerRecord || (!ownerCanonicalMember && !ownerRemovalPending)) {
      throw new PlatformPairingError(
        "site_member_not_canonical",
        409,
        "site_member_not_canonical"
      );
    }

    let peerStandalone = isStandaloneSite(
      peerSiteBefore,
      peerParticipantId,
      ownerSiteBefore.siteId
    );
    if (!peerStandalone &&
        (peerSiteBefore.siteId !== ownerSiteBefore.siteId ||
         peerSiteBefore.localOwner ||
         peerSiteBefore.configurationOwnerNodeId !== ownerParticipantId)) {
      throw new PlatformPairingError(
        "site_member_removal_site_mismatch",
        409,
        "site_member_removal_site_mismatch"
      );
    }

    let reset: PlatformSiteMemberRemovalResult["reset"] = null;
    if (!peerStandalone) {
      const peerSiteCurrent = await traced(
        "removal.peer.refresh",
        () => getSiteConfiguration(peerDevice),
        { peerParticipantId }
      );
      peerStandalone = isStandaloneSite(
        peerSiteCurrent,
        peerParticipantId,
        ownerSiteBefore.siteId
      );
      if (!peerStandalone &&
          (peerSiteCurrent.siteId !== ownerSiteBefore.siteId ||
           peerSiteCurrent.localOwner ||
           peerSiteCurrent.configurationOwnerNodeId !== ownerParticipantId)) {
        throw new PlatformPairingError(
          "site_member_removal_site_mismatch",
          409,
          "site_member_removal_site_mismatch"
        );
      }
      if (!peerStandalone) {
        let resetFailure: unknown;
        try {
          reset = await traced(
            "removal.peer.reset",
            () => postForm<NonNullable<PlatformSiteMemberRemovalResult["reset"]>>(
              peerDevice,
              "/api/system/reset",
              { connections: "true" }
            ),
            { peerParticipantId }
          );
        } catch (cause) {
          // A successful reset can close the HTTP connection during reboot. Keep
          // a definite HTTP error so it is not replaced by a generic timeout.
          resetFailure = cause;
        }
        try {
          await traced(
            "removal.peer.standalone_wait",
            () => waitForStandaloneSite(
              peerDevice,
              peerParticipantId,
              ownerSiteBefore.siteId
            ),
            { peerParticipantId, previousSiteId: ownerSiteBefore.siteId }
          );
        } catch (cause) {
          if (resetFailure instanceof PlatformPairingError) throw resetFailure;
          throw cause;
        }
      }
    } else {
      traceEvent("removal.peer.already_standalone", "event", {
        peerParticipantId,
        ...removalSiteTrace(peerSiteBefore, peerParticipantId)
      });
    }

    let [ownerSiteCurrent, owner] = await traced(
      "removal.owner.post_reset_readback",
      () => Promise.all([
        getSiteConfiguration(ownerDevice),
        getParticipants(ownerDevice)
      ]),
      { ownerParticipantId, peerParticipantId }
    );
    traceEvent("removal.owner.post_reset_state", "event", {
      site: removalSiteTrace(ownerSiteCurrent, peerParticipantId),
      participant: removalParticipantTrace(owner, peerParticipantId)
    });
    if (!ownerSiteCurrent.localOwner ||
        ownerSiteCurrent.configurationOwnerNodeId !== ownerParticipantId) {
      throw new PlatformPairingError(
        "site_member_removal_owner_changed",
        409,
        "site_member_removal_owner_changed"
      );
    }
    const ownerRecordCurrent = owner.participants.find(
      (participant) => participant.participantId === peerParticipantId
    );
    if (ownerRecordCurrent?.canonicalMember === true) {
      const removalView: PlatformMembershipView =
        ownerRecordCurrent.directPair === true ? "direct" : "canonical";
      owner = await traced(
        "removal.owner.revoke",
        () => participantAction(
          ownerDevice,
          "revoke",
          peerParticipantId,
          {},
          removalView
        ),
        { peerParticipantId, removalView }
      );
      ownerSiteCurrent = await traced(
        "removal.owner.revoke_readback",
        () => getSiteConfiguration(ownerDevice),
        { peerParticipantId, removalView }
      );
      traceEvent("removal.owner.revoked_state", "event", {
        site: removalSiteTrace(ownerSiteCurrent, peerParticipantId),
        participant: removalParticipantTrace(owner, peerParticipantId)
      });
    }
    const removedMembership = siteMemberState(ownerSiteCurrent, peerParticipantId);
    if (removedMembership !== null && removedMembership !== "revoked") {
      throw new PlatformPairingError(
        "site_member_revoke_incomplete",
        409,
        "site_member_revoke_incomplete"
      );
    }
    const originalMemberCount = Math.max(
      ownerSiteBefore.memberCount + (ownerRemovalPending ? 1 : 0),
      peerSiteBefore.memberCount
    );
    await traced(
      "removal.owner.replication_wait",
      () => waitForSiteMemberMutationReplication(
        ownerDevice,
        ownerParticipantId,
        peerParticipantId,
        originalMemberCount,
        owner,
        ownerSiteCurrent,
        removedMembership === "revoked" ? "revoked" : "absent"
      ),
      {
        peerParticipantId,
        originalMemberCount,
        expectedMembership: removedMembership === "revoked" ? "revoked" : "absent"
      }
    );

    const ownerCurrent = await traced(
      "removal.owner.pre_forget_readback",
      () => getParticipants(ownerDevice),
      { peerParticipantId }
    );
    const ownerRecordFinal = ownerCurrent.participants.find(
      (participant) => participant.participantId === peerParticipantId
    );
    if (ownerRecordFinal) {
      const removalView = ownerRecordFinal.directPair === true ? "direct" : "canonical";
      owner = await traced(
        "removal.owner.forget",
        () => participantAction(
          ownerDevice,
          "forget",
          peerParticipantId,
          {},
          removalView
        ),
        { peerParticipantId, removalView }
      );
    } else {
      owner = ownerCurrent;
    }
    traceEvent("removal.complete", "event", {
      peerParticipantId,
      ownerParticipant: removalParticipantTrace(owner, peerParticipantId)
    });
    return { owner, reset };
  }

  async function leaveSite(
    localDevice: string,
    ownerDevice: string,
    localParticipantId: string,
    ownerParticipantId: string
  ): Promise<PlatformSiteLeaveResult> {
    const [localSite, ownerDescriptor, ownerSite, ownerBefore] = await Promise.all([
      getSiteConfiguration(localDevice),
      getDescriptor(ownerDevice),
      getSiteConfiguration(ownerDevice),
      getParticipants(ownerDevice)
    ]);
    if (localSite.localOwner ||
        localSite.configurationOwnerNodeId !== ownerParticipantId) {
      throw new PlatformPairingError("site_leave_owner_mismatch", 409, "site_leave_owner_mismatch");
    }
    if (ownerDescriptor.participantId !== ownerParticipantId) {
      throw new PlatformPairingError("peer_identity_mismatch", 409, "peer_identity_mismatch");
    }

    const ownerListsLocal = ownerBefore.participants.some(
      (participant) => participant.participantId === localParticipantId
    );
    if (ownerListsLocal &&
        (!ownerSite.localOwner || ownerSite.siteId !== localSite.siteId)) {
      throw new PlatformPairingError("site_leave_owner_mismatch", 409, "site_leave_owner_mismatch");
    }

    const owner = ownerListsLocal
      ? await removeParticipant(ownerDevice, localParticipantId)
      : ownerBefore;
    if (owner.participants.some(
      (participant) => participant.participantId === localParticipantId
    )) {
      throw new PlatformPairingError("site_leave_owner_still_lists_node", 409, "site_leave_owner_still_lists_node");
    }

    const reset = await postForm<PlatformSiteLeaveResult["reset"]>(
      localDevice,
      "/api/system/reset",
      { connections: "true" }
    );
    return { owner, reset };
  }

  async function commitLocalFloorplan(device: string): Promise<PlatformSiteConfigurationStatus> {
    const response = await fetcher(`${baseUrl(device)}/api/floorplan`, {
      cache: "no-store"
    });
    const floorplan = await responseJson<{
      floorplanId?: string;
      geometryVersion?: number;
      image?: { width?: number; height?: number };
      scale?: { mmPerPxX?: number; mmPerPxY?: number };
    }>(response);
    const geometryVersion = floorplan.geometryVersion;
    const coordinateWidthMm = Math.round((floorplan.image?.width ?? 0) * (floorplan.scale?.mmPerPxX ?? 0));
    const coordinateHeightMm = Math.round((floorplan.image?.height ?? 0) * (floorplan.scale?.mmPerPxY ?? 0));
    if (!floorplan.floorplanId || !Number.isInteger(geometryVersion) ||
        (geometryVersion ?? 0) <= 0 || !Number.isSafeInteger(coordinateWidthMm) ||
        coordinateWidthMm <= 0 || !Number.isSafeInteger(coordinateHeightMm) ||
        coordinateHeightMm <= 0) {
      throw new PlatformPairingError("floorplan_manifest_invalid", 409, "floorplan_manifest_invalid");
    }
    return commitFloorplanManifest(
      device,
      floorplan.floorplanId,
      geometryVersion as number,
      coordinateWidthMm,
      coordinateHeightMm
    );
  }

  async function commitFloorplanManifest(
    device: string,
    floorplanId: string,
    geometryVersion: number,
    coordinateWidthMm: number,
    coordinateHeightMm: number
  ): Promise<PlatformSiteConfigurationStatus> {
    if (!floorplanId || !Number.isInteger(geometryVersion) || geometryVersion <= 0 ||
        !Number.isSafeInteger(coordinateWidthMm) || coordinateWidthMm <= 0 ||
        coordinateWidthMm > 0x7fffffff ||
        !Number.isSafeInteger(coordinateHeightMm) || coordinateHeightMm <= 0 ||
        coordinateHeightMm > 0x7fffffff) {
      throw new PlatformPairingError("floorplan_manifest_invalid", 409, "floorplan_manifest_invalid");
    }
    return postForm<PlatformSiteConfigurationStatus>(device, "/api/platform/site-configuration", {
      action: "commitFloorplan",
      floorplanId,
      geometryVersion: String(geometryVersion),
      coordinateWidthMm: String(coordinateWidthMm),
      coordinateHeightMm: String(coordinateHeightMm)
    });
  }

  async function claimSiteOwnership(device: string): Promise<PlatformSiteConfigurationStatus> {
    return postForm<PlatformSiteConfigurationStatus>(device, "/api/platform/site-configuration", {
      action: "claimOwnership"
    });
  }

  async function syncSiteConfiguration(
    device: string,
    ownerNodeId = ""
  ): Promise<PlatformSiteConfigurationStatus> {
    return postForm<PlatformSiteConfigurationStatus>(device, "/api/platform/site-configuration", {
      action: "sync",
      ...(ownerNodeId ? { ownerNodeId } : {})
    });
  }

  return {
    getStatus,
    getDiscovery,
    getDescriptor,
    getParticipants,
    waitForParticipantReplica,
    waitForParticipantEndpoint,
    getSiteConfiguration,
    getGroups,
    waitForSiteConvergence,
    getDeviceState,
    getFloorplan,
    scan,
    prepareJoiner,
    requestInvitation,
    rejectInvitation,
    dismissInvitation,
    connectLeader,
    confirm,
    confirmPair,
    cancel,
    revokeParticipant,
    forgetParticipant,
    setCommunityFallback,
    configureParticipant,
    reconfigureParticipant,
    activateParticipant: (
      device: string,
      participantId: string,
      membershipView?: PlatformMembershipView
    ) => participantAction(
      device,
      "activate",
      participantId,
      { automaticGroup: "true" },
      membershipView
    ),
    quarantineParticipant: (
      device: string,
      participantId: string,
      membershipView?: PlatformMembershipView
    ) => participantAction(device, "quarantine", participantId, {}, membershipView),
    disconnectPair,
    removeSiteMember,
    leaveSite,
    claimSiteOwnership,
    commitFloorplanManifest,
    commitLocalFloorplan,
    syncSiteConfiguration
  };
}

export function pairingDisplay(
  status: PlatformPairingStatus | null,
  participantsStatus: PlatformParticipantsStatus | null
): PlatformPairingDisplay {
  const peerParticipantIds = (participantsStatus?.participants ?? [])
    .filter((participant) => participant.membership !== "revoked" &&
      (participant.canonicalMember === true ||
       participant.credential?.provisioned === true))
    .map((participant) => participant.participantId);

  if (status && status.state !== "idle" && status.state !== "committed") {
    return {
      state: status.state,
      peerParticipantIds: status.peerParticipantId ? [status.peerParticipantId] : []
    };
  }

  return {
    state: peerParticipantIds.length > 0 ? "committed" : "idle",
    peerParticipantIds
  };
}

export function pairingReturnFlowActive(
  returnTo: string,
  status: PlatformPairingStatus | null,
  participantsStatus: PlatformParticipantsStatus | null
): boolean {
  const returnHost = addressHost(returnTo);
  if (!returnHost) return false;

  if (status?.role === "joiner" &&
      status.state !== "idle" &&
      status.state !== "committed") {
    return true;
  }

  if (status?.invitation?.state === "pending" &&
      addressHost(status.invitation.requesterAddress) === returnHost) {
    return true;
  }

  const connected = (participantsStatus?.participants ?? []).filter(
    (participant) =>
      participant.membership !== "revoked" &&
      (participant.canonicalMember === true ||
       participant.credential?.provisioned === true)
  );
  return connected.some((participant) => {
    const endpointHost = addressHost(participant.endpoint?.address ?? "");
    if (endpointHost) return endpointHost === returnHost;
    return connected.length === 1 &&
      participant.participantId === status?.peerParticipantId;
  });
}

function addressHost(value: string): string {
  const trimmed = value.trim();
  if (!trimmed) return "";
  try {
    const url = new URL(
      /^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`
    );
    return url.hostname.toLowerCase();
  } catch {
    return "";
  }
}

function offerFields(offer: PlatformPairingOffer): Record<string, string> {
  return {
    role: offer.role,
    participantId: offer.participantId,
    siteId: offer.siteId,
    sessionId: offer.sessionId,
    nonce: offer.nonce,
    publicKey: offer.publicKey
  };
}
