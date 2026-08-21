import type { FloorplanStorageDocument, FloorplanStorageRoom } from "../core/floorplan/floorplan-storage";
import type { WebDeviceState, WebLocalTarget } from "../core/types";
import { findRoomContainingPoint } from "./floorplan/floorplan-room-context";
import type {
  PlatformDescriptor,
  PlatformParticipant,
  PlatformParticipantAssignment,
  PlatformParticipantCapabilities,
  PlatformParticipantReplicaExpectation,
  PlatformParticipantsStatus,
  PlatformMembershipView,
  PlatformSiteConfigurationStatus,
  PlatformSiteConvergenceExpectation,
  PlatformSpatialTransform
} from "./api/platform-pairing";
import { PlatformPairingError, siteConvergenceExpectation } from "./api/platform-pairing";
import { tracePairingPhase } from "./pairing-trace";
import { waitForSharedFloorplanWritable } from "./state/shared-floorplan-write";

const SCALE_ROUNDING_TOLERANCE = 0.011;
const MIN_ROTATION_BASELINE_MM = 800;
const MAX_TRANSLATION_MM = 750;
const MAX_ROTATION_CORRECTION_DEG = 12;
const MAX_RESIDUAL_MM = 350;
const RADAR_ROOM_PROBE_MM = 250;

type Point = [number, number];

export interface PlatformCalibrationSample {
  referenceLocalMm: Point;
  peerLocalMm: Point;
}

export interface PlatformCalibrationProposal {
  samples: number;
  floorplanId: string;
  geometryVersion: number;
  spaceId: string;
  groupId: string;
  translationMillimeters: number;
  rotationCorrectionDegrees: number;
  residualMillimeters: number;
  referenceAssignment: PlatformParticipantAssignment;
  peerAssignment: PlatformParticipantAssignment;
}

export interface PlatformCalibrationDisplayPoint {
  xPx: number;
  yPx: number;
}

export interface PlatformCalibrationDisplayRadar extends PlatformCalibrationDisplayPoint {
  rotationDegrees: number;
}

export interface PlatformCalibrationDisplayModel {
  referenceRadar: PlatformCalibrationDisplayRadar;
  peerRadar: PlatformCalibrationDisplayRadar;
  referenceTarget: PlatformCalibrationDisplayPoint | null;
  peerTarget: PlatformCalibrationDisplayPoint | null;
  referenceTargetCount: number;
  peerTargetCount: number;
  targetSeparationMillimeters: number | null;
  captureReady: boolean;
}

export function applyAutomaticCalibrationGroup(
  proposal: PlatformCalibrationProposal,
  site: PlatformSiteConfigurationStatus,
  referenceParticipantId: string,
  peerParticipantId: string
): PlatformCalibrationProposal {
  const groupCrossesSpace = (groupId: string, spaceId: string): boolean =>
    Boolean(site.members?.some((item) =>
      item.spatialProfile?.groupId === groupId && item.spatialProfile.spaceId !== spaceId
    ));
  const profileGroup = (participantId: string, spaceId: string): string | null => {
    const ownProfile = site.members?.find((item) => item.participantId === participantId)?.spatialProfile;
    if (ownProfile?.spaceId === spaceId && ownProfile.groupId?.trim() &&
        !groupCrossesSpace(ownProfile.groupId.trim(), spaceId)) {
      return ownProfile.groupId.trim();
    }
    const existing = site.members?.find((item) =>
      item.spatialProfile?.spaceId === spaceId && item.spatialProfile.groupId?.trim() &&
      !groupCrossesSpace(item.spatialProfile.groupId.trim(), spaceId)
    )?.spatialProfile?.groupId?.trim();
    return existing || null;
  };
  const referenceExisting = profileGroup(
    referenceParticipantId,
    proposal.referenceAssignment.spaceId
  );
  const peerExisting = profileGroup(
    peerParticipantId,
    proposal.peerAssignment.spaceId
  );
  let referenceGroupId = referenceExisting ?? proposal.referenceAssignment.groupId;
  let peerGroupId = peerExisting ?? proposal.peerAssignment.groupId;
  if (proposal.referenceAssignment.spaceId === proposal.peerAssignment.spaceId) {
    const sharedGroupId = referenceExisting ?? peerExisting ?? referenceGroupId;
    referenceGroupId = sharedGroupId;
    peerGroupId = sharedGroupId;
  }
  return {
    ...proposal,
    groupId: referenceGroupId,
    referenceAssignment: {
      ...proposal.referenceAssignment,
      groupId: referenceGroupId
    },
    peerAssignment: { ...proposal.peerAssignment, groupId: peerGroupId }
  };
}

export class PlatformCalibrationError extends Error {
  constructor(readonly code: string) {
    super(code);
  }
}

export function buildPlatformCalibrationDisplayModel(
  referenceFloorplan: FloorplanStorageDocument,
  peerFloorplan: FloorplanStorageDocument,
  referenceState: WebDeviceState,
  peerState: WebDeviceState
): PlatformCalibrationDisplayModel {
  const referenceTransform = floorplanTransform(referenceFloorplan);
  const peerTransform = floorplanTransform(peerFloorplan);
  const referenceTargets = validActiveTargets(referenceState);
  const peerTargets = validActiveTargets(peerState);
  const referenceWorld = referenceTargets.length === 1
    ? applyTransform(referenceTransform, targetPoint(referenceTargets[0]))
    : null;
  const peerWorld = peerTargets.length === 1
    ? applyTransform(peerTransform, targetPoint(peerTargets[0]))
    : null;

  return {
    referenceRadar: {
      ...worldPointToReferencePixels(
        [referenceTransform.originXMillimeters, referenceTransform.originYMillimeters],
        referenceFloorplan
      ),
      rotationDegrees: referenceFloorplan.radar.rotationDeg
    },
    peerRadar: {
      ...worldPointToReferencePixels(
        [peerTransform.originXMillimeters, peerTransform.originYMillimeters],
        referenceFloorplan
      ),
      rotationDegrees: peerFloorplan.radar.rotationDeg
    },
    referenceTarget: referenceWorld
      ? worldPointToReferencePixels(referenceWorld, referenceFloorplan)
      : null,
    peerTarget: peerWorld
      ? worldPointToReferencePixels(peerWorld, referenceFloorplan)
      : null,
    referenceTargetCount: referenceTargets.length,
    peerTargetCount: peerTargets.length,
    targetSeparationMillimeters: referenceWorld && peerWorld
      ? Math.round(length(subtract(referenceWorld, peerWorld)))
      : null,
    captureReady: referenceState.connected && peerState.connected &&
      referenceTargets.length === 1 && peerTargets.length === 1
  };
}

export function calibrationSampleReferencePointPx(
  referenceFloorplan: FloorplanStorageDocument,
  sample: PlatformCalibrationSample
): PlatformCalibrationDisplayPoint {
  return worldPointToReferencePixels(
    applyTransform(floorplanTransform(referenceFloorplan), sample.referenceLocalMm),
    referenceFloorplan
  );
}

export function capturePlatformCalibrationSample(
  referenceState: WebDeviceState,
  peerState: WebDeviceState
): PlatformCalibrationSample {
  return {
    referenceLocalMm: targetPoint(singleActiveTarget(referenceState)),
    peerLocalMm: targetPoint(singleActiveTarget(peerState))
  };
}

export function buildPlatformCalibrationProposal(
  referenceFloorplan: FloorplanStorageDocument,
  peerFloorplan: FloorplanStorageDocument,
  samples: PlatformCalibrationSample[],
  calibrationVersion: number
): PlatformCalibrationProposal {
  validateFloorplanPair(referenceFloorplan, peerFloorplan);
  if (samples.length < 1 || samples.length > 2) throw new PlatformCalibrationError("calibration_sample_count");
  if (!Number.isInteger(calibrationVersion) || calibrationVersion < 1 || calibrationVersion > 0xffffffff) {
    throw new PlatformCalibrationError("calibration_version_invalid");
  }

  const referenceBase = floorplanTransform(referenceFloorplan);
  const peerBase = floorplanTransform(peerFloorplan);
  const referenceWorld = samples.map((sample) => applyTransform(referenceBase, sample.referenceLocalMm));
  let peerRotationDegrees = peerFloorplan.radar.rotationDeg;

  if (samples.length === 2) {
    const referenceDelta = subtract(referenceWorld[1], referenceWorld[0]);
    const peerDelta = subtract(samples[1].peerLocalMm, samples[0].peerLocalMm);
    if (length(referenceDelta) < MIN_ROTATION_BASELINE_MM || length(peerDelta) < MIN_ROTATION_BASELINE_MM) {
      throw new PlatformCalibrationError("calibration_baseline_too_short");
    }
    peerRotationDegrees = normalizeDegrees(
      radiansToDegrees(Math.atan2(referenceDelta[1], referenceDelta[0]) + Math.atan2(peerDelta[1], peerDelta[0]))
    );
  }

  const rotationCorrectionDegrees = normalizeDegrees(peerRotationDegrees - peerFloorplan.radar.rotationDeg);
  if (Math.abs(rotationCorrectionDegrees) > MAX_ROTATION_CORRECTION_DEG) {
    throw new PlatformCalibrationError("calibration_rotation_too_large");
  }

  const rotatedPeer = samples.map((sample) => rotateRadarPoint(sample.peerLocalMm, peerRotationDegrees));
  const origin: Point = [
    average(referenceWorld.map((point, index) => point[0] - rotatedPeer[index][0])),
    average(referenceWorld.map((point, index) => point[1] - rotatedPeer[index][1]))
  ];
  const peerTransform = transformFrom(origin, peerRotationDegrees, samples.length, 0);
  const residualMillimeters = Math.round(Math.sqrt(average(samples.map((sample, index) => {
    const corrected = applyTransform(peerTransform, sample.peerLocalMm);
    const dx = corrected[0] - referenceWorld[index][0];
    const dy = corrected[1] - referenceWorld[index][1];
    return dx * dx + dy * dy;
  }))));
  if (residualMillimeters > MAX_RESIDUAL_MM) {
    throw new PlatformCalibrationError("calibration_residual_too_large");
  }
  peerTransform.residualMillimeters = residualMillimeters;

  const baseOrigin: Point = [peerBase.originXMillimeters, peerBase.originYMillimeters];
  const translationMillimeters = Math.round(length(subtract(origin, baseOrigin)));
  if (translationMillimeters > MAX_TRANSLATION_MM) {
    throw new PlatformCalibrationError("calibration_translation_too_large");
  }

  const referencePointPx: { x: number; y: number } = {
    x: referenceWorld[0][0] / referenceFloorplan.scale.mmPerPxX,
    y: referenceWorld[0][1] / referenceFloorplan.scale.mmPerPxY
  };
  const captureRoom = findRoomContainingPoint(
    referenceFloorplan.rooms,
    referencePointPx,
    (candidate) => candidate.pointsPx
  );
  const referenceRoom = radarAssignmentRoom(referenceFloorplan, referenceBase);
  const peerRoom = radarAssignmentRoom(peerFloorplan, peerTransform);
  if (!captureRoom || !referenceRoom || !peerRoom) {
    throw new PlatformCalibrationError("calibration_room_not_found");
  }
  const captureSpaceId = boundedAssignmentId(captureRoom);
  const referenceSpaceId = boundedAssignmentId(referenceRoom);
  const peerSpaceId = boundedAssignmentId(peerRoom);
  const floorplanId = referenceFloorplan.floorplanId as string;
  const geometryVersion = referenceFloorplan.geometryVersion as number;

  return {
    samples: samples.length,
    floorplanId,
    geometryVersion,
    spaceId: captureSpaceId,
    groupId: referenceSpaceId,
    translationMillimeters,
    rotationCorrectionDegrees: round(rotationCorrectionDegrees, 2),
    residualMillimeters,
    referenceAssignment: {
      floorplanId,
      spaceId: referenceSpaceId,
      groupId: referenceSpaceId,
      geometryVersion,
      localPoseVersion: positivePoseVersion(peerFloorplan),
      poseVersion: positivePoseVersion(referenceFloorplan),
      calibrationVersion,
      transform: transformFrom(
        [referenceBase.originXMillimeters, referenceBase.originYMillimeters],
        referenceFloorplan.radar.rotationDeg,
        samples.length,
        residualMillimeters
      )
    },
    peerAssignment: {
      floorplanId,
      spaceId: peerSpaceId,
      groupId: peerSpaceId,
      geometryVersion,
      localPoseVersion: positivePoseVersion(referenceFloorplan),
      poseVersion: positivePoseVersion(peerFloorplan),
      calibrationVersion,
      transform: peerTransform
    }
  };
}

interface CalibrationClient {
  getParticipants(device: string): Promise<PlatformParticipantsStatus>;
  configureParticipant(
    device: string,
    participantId: string,
    capabilities: PlatformParticipantCapabilities,
    assignment: PlatformParticipantAssignment,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus>;
  reconfigureParticipant(
    device: string,
    participantId: string,
    capabilities: PlatformParticipantCapabilities,
    assignment: PlatformParticipantAssignment,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus>;
  activateParticipant(
    device: string,
    participantId: string,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus>;
  setCommunityFallback?(
    device: string,
    participantId: string,
    eligible: boolean
  ): Promise<PlatformParticipantsStatus>;
  quarantineParticipant(
    device: string,
    participantId: string,
    membershipView?: PlatformMembershipView
  ): Promise<PlatformParticipantsStatus>;
  getSiteConfiguration(device: string): Promise<PlatformSiteConfigurationStatus>;
  syncSiteConfiguration(device: string, ownerNodeId?: string): Promise<unknown>;
  waitForSiteConvergence(
    device: string,
    expected: PlatformSiteConvergenceExpectation
  ): Promise<unknown>;
  waitForParticipantReplica(
    device: string,
    participantId: string,
    expected: PlatformParticipantReplicaExpectation
  ): Promise<unknown>;
  waitForParticipantEndpoint(device: string, participantId: string): Promise<unknown>;
}

export async function waitForPlatformCalibrationBootstrap(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceParticipantId: string,
  peerParticipantId: string,
  referenceNodePoseVersion: number,
  peerNodePoseVersion: number
): Promise<PlatformSiteConfigurationStatus> {
  if (!Number.isInteger(referenceNodePoseVersion) || referenceNodePoseVersion < 1 ||
      !Number.isInteger(peerNodePoseVersion) || peerNodePoseVersion < 1) {
    throw new PlatformCalibrationError("calibration_site_not_ready");
  }

  const canonical = await tracePairingPhase(
    "calibration.bootstrap.owner_read",
    () => client.getSiteConfiguration(referenceDevice),
    { peer: peerDevice }
  );
  if (canonical.configurationOwnerNodeId !== referenceParticipantId) {
    throw new PlatformCalibrationError("calibration_site_not_ready");
  }
  const expectedSite = siteConvergenceExpectation(canonical);
  const expectedReplica: PlatformParticipantReplicaExpectation = {
    siteConfigurationAuthorityEpoch: expectedSite.authorityEpoch,
    siteConfigurationGeneration: expectedSite.generation,
    siteConfigurationDigest: expectedSite.configurationDigest,
    floorplanGeometryVersion: expectedSite.geometryVersion,
    floorplanConfigHash: expectedSite.configHash,
    floorplanImageHash: expectedSite.imageHash
  };

  await tracePairingPhase(
    "calibration.bootstrap.endpoints",
    () => Promise.all([
      client.waitForParticipantEndpoint(referenceDevice, peerParticipantId),
      client.waitForParticipantEndpoint(peerDevice, referenceParticipantId)
    ]),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    "calibration.bootstrap.sync",
    () => requestCalibrationSiteSync(client, peerDevice, referenceParticipantId),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    "calibration.bootstrap.site_convergence",
    () => client.waitForSiteConvergence(peerDevice, expectedSite),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    "calibration.bootstrap.replicas",
    () => Promise.all([
      client.waitForParticipantReplica(referenceDevice, peerParticipantId, {
        ...expectedReplica,
        radarPoseVersion: peerNodePoseVersion
      }),
      client.waitForParticipantReplica(peerDevice, referenceParticipantId, {
        ...expectedReplica,
        radarPoseVersion: referenceNodePoseVersion
      })
    ]),
    { peer: peerDevice }
  );
  return canonical;
}

export async function approvePlatformCalibration(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  proposal: PlatformCalibrationProposal,
  options: { prepareCommunityFallback?: boolean } = {}
): Promise<void> {
  const referenceCapabilities = referenceDescriptor.capabilities;
  const peerCapabilities = peerDescriptor.capabilities;
  if (!referenceCapabilities?.pointTracks || !peerCapabilities?.pointTracks) {
    throw new PlatformCalibrationError("calibration_spatial_capability_required");
  }
  assertCalibrationPoseContract(proposal);
  const baseline = await waitForPlatformCalibrationBootstrap(
    client,
    referenceDevice,
    peerDevice,
    referenceDescriptor.participantId,
    peerDescriptor.participantId,
    proposal.referenceAssignment.poseVersion,
    proposal.peerAssignment.poseVersion
  );
  if (!baseline.localOwner ||
      baseline.configurationOwnerNodeId !== referenceDescriptor.participantId ||
      !baseline.floorplan.committed ||
      baseline.floorplan.floorplanId !== proposal.floorplanId) {
    throw new PlatformCalibrationError("calibration_site_not_ready");
  }
  const automaticProposal = rebaseCalibrationVersion(
    applyAutomaticCalibrationGroup(
      proposal,
      baseline,
      referenceDescriptor.participantId,
      peerDescriptor.participantId
    ),
    baseline,
    referenceDescriptor.participantId,
    peerDescriptor.participantId
  );

  const [referenceParticipants, peerParticipants] = await tracePairingPhase(
    "calibration.membership_read",
    () => Promise.all([
      client.getParticipants(referenceDevice),
      client.getParticipants(peerDevice)
    ]),
    { peer: peerDevice }
  );
  const ownerViewPeer = referenceParticipants.participants.find(
    (participant) => participant.participantId === peerDescriptor.participantId
  );
  const peerViewReference = peerParticipants.participants.find(
    (participant) => participant.participantId === referenceDescriptor.participantId
  );
  if (!ownerViewPeer || !peerViewReference) {
    throw new PlatformCalibrationError("calibration_membership_inconsistent");
  }

  const calibrationMode = resolvePlatformCalibrationMode(
    baseline,
    referenceDescriptor.participantId,
    peerDescriptor.participantId,
    ownerViewPeer,
    peerViewReference
  );
  if (calibrationMode === "recalibration") {
    await approveActivePlatformRecalibration(
      client,
      referenceDevice,
      peerDevice,
      referenceDescriptor,
      peerDescriptor,
      automaticProposal,
      ownerViewPeer.capabilities,
      ownerViewPeer.assignment,
      peerViewReference.capabilities,
      peerViewReference.assignment
    );
    if (options.prepareCommunityFallback === true) {
      await enableCommunityFallback(
        client,
        referenceDevice,
        peerDevice,
        peerDescriptor.participantId
      );
      await tracePairingPhase(
        "calibration.convergence_final",
        () => convergeCalibrationSite(
          client,
          referenceDevice,
          peerDevice,
          referenceDescriptor,
          peerDescriptor,
          automaticProposal.referenceAssignment.poseVersion,
          automaticProposal.peerAssignment.poseVersion
        ),
        { peer: peerDevice }
      );
    }
    return;
  }

  await approveInitialPlatformCalibration(
    client,
    referenceDevice,
    peerDevice,
    referenceDescriptor,
    peerDescriptor,
    automaticProposal,
    baseline,
    peerViewReference,
    options.prepareCommunityFallback === true
  );
}

function resolvePlatformCalibrationMode(
  baseline: PlatformSiteConfigurationStatus,
  referenceParticipantId: string,
  peerParticipantId: string,
  ownerViewPeer: PlatformParticipant,
  peerViewReference: PlatformParticipant
): "initial" | "recalibration" {
  const ownerMember = baseline.members?.find(
    (member) => member.participantId === referenceParticipantId
  );
  const peerMember = baseline.members?.find(
    (member) => member.participantId === peerParticipantId
  );
  const projectionMismatch =
    !ownerMember || ownerMember.membership !== "active" ||
    !peerMember ||
    participantMembership(ownerViewPeer, "canonical") !== peerMember.membership ||
    participantMembership(peerViewReference, "canonical") !== ownerMember.membership ||
    ownerViewPeer.canonicalMember === false ||
    peerViewReference.canonicalMember === false ||
    ownerViewPeer.directPair === false ||
    peerViewReference.directPair === false;
  if (projectionMismatch) {
    throw new PlatformCalibrationError("calibration_membership_inconsistent");
  }

  if (peerMember.membership === "quarantined") return "initial";
  if (peerMember.membership === "active") return "recalibration";
  throw new PlatformCalibrationError("calibration_membership_inconsistent");
}

function participantMembership(
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

async function enableCommunityFallback(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  peerParticipantId: string
): Promise<void> {
  if (!client.setCommunityFallback) {
    throw new PlatformCalibrationError("automatic_fallback_unavailable");
  }
  await tracePairingPhase(
    "calibration.fallback",
    () => client.setCommunityFallback!(
      referenceDevice,
      peerParticipantId,
      true
    ),
    { peer: peerDevice }
  );
}

async function approveInitialPlatformCalibration(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  proposal: PlatformCalibrationProposal,
  baseline: PlatformSiteConfigurationStatus,
  peerViewReference: PlatformParticipant,
  prepareCommunityFallback: boolean
): Promise<void> {
  const referenceCapabilities = referenceDescriptor.capabilities!;
  const peerCapabilities = peerDescriptor.capabilities!;
  const peerDirectActive = participantMembership(peerViewReference, "direct") === "active";
  if (peerDirectActive) {
    await tracePairingPhase(
      "calibration.configure.peer",
      () => client.reconfigureParticipant(
        peerDevice,
        referenceDescriptor.participantId,
        referenceCapabilities,
        proposal.referenceAssignment,
        "direct"
      ),
      { mode: "reconfigure", peer: peerDevice }
    );
  } else {
    await tracePairingPhase(
      "calibration.configure.peer",
      () => client.configureParticipant(
        peerDevice,
        referenceDescriptor.participantId,
        referenceCapabilities,
        proposal.referenceAssignment,
        "direct"
      ),
      { mode: "configure", peer: peerDevice }
    );
  }
  await tracePairingPhase(
    "calibration.configure.owner",
    () => runOwnerMutationAfterContinuity(
      client,
      referenceDevice,
      () => client.configureParticipant(
        referenceDevice,
        peerDescriptor.participantId,
        peerCapabilities,
        proposal.peerAssignment,
        "canonical"
      ),
      baseline
    ),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    "calibration.activation_readiness",
    () => waitForCalibrationActivationReadiness(
      client,
      referenceDevice,
      peerDevice,
      referenceDescriptor,
      peerDescriptor,
      baseline,
      proposal.referenceAssignment.poseVersion,
      proposal.peerAssignment.poseVersion
    ),
    { peer: peerDevice }
  );

  await tracePairingPhase(
    "calibration.activate.peer",
    () => client.activateParticipant(
      peerDevice,
      referenceDescriptor.participantId,
      "direct"
    ),
    { peer: peerDevice }
  );
  const ownerParticipants = await tracePairingPhase(
    "calibration.activate.owner",
    () => runOwnerMutationAfterContinuity(
      client,
      referenceDevice,
      () => client.activateParticipant(
        referenceDevice,
        peerDescriptor.participantId,
        "canonical"
      )
    ),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    "calibration.activation_sync_fanout",
    () => requestExistingCalibrationPeersSync(
      client,
      ownerParticipants,
      peerDescriptor.participantId,
      referenceDescriptor.participantId
    ),
    { peer: peerDevice }
  );
  await tracePairingPhase(
    prepareCommunityFallback
      ? "calibration.convergence_after_activation"
      : "calibration.convergence_final",
    () => convergeCalibrationSite(
      client,
      referenceDevice,
      peerDevice,
      referenceDescriptor,
      peerDescriptor,
      proposal.referenceAssignment.poseVersion,
      proposal.peerAssignment.poseVersion
    ),
    { peer: peerDevice }
  );
  if (prepareCommunityFallback) {
    await enableCommunityFallback(
      client,
      referenceDevice,
      peerDevice,
      peerDescriptor.participantId
    );
    await tracePairingPhase(
      "calibration.convergence_final",
      () => convergeCalibrationSite(
        client,
        referenceDevice,
        peerDevice,
        referenceDescriptor,
        peerDescriptor,
        proposal.referenceAssignment.poseVersion,
        proposal.peerAssignment.poseVersion
      ),
      { peer: peerDevice }
    );
  }
}

async function approveActivePlatformRecalibration(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  proposal: PlatformCalibrationProposal,
  previousPeerCapabilities: PlatformParticipantCapabilities | null | undefined,
  previousPeerAssignment: PlatformParticipantAssignment | null | undefined,
  previousReferenceCapabilities: PlatformParticipantCapabilities | null | undefined,
  previousReferenceAssignment: PlatformParticipantAssignment | null | undefined
): Promise<void> {
  if (!previousPeerCapabilities || !previousPeerAssignment ||
      !previousReferenceCapabilities || !previousReferenceAssignment) {
    throw new PlatformCalibrationError("calibration_membership_inconsistent");
  }
  const referenceCapabilities = referenceDescriptor.capabilities!;
  const peerCapabilities = peerDescriptor.capabilities!;
  const ownerBaselineCapabilities = cloneCapabilities(previousPeerCapabilities);
  const ownerBaselineAssignment = cloneAssignment(previousPeerAssignment);
  const peerBaselineCapabilities = cloneCapabilities(previousReferenceCapabilities);
  const peerBaselineAssignment = cloneAssignment(previousReferenceAssignment);
  let peerAttempted = false;
  let ownerAttempted = false;

  try {
    ownerAttempted = true;
    await runOwnerMutationAfterContinuity(
      client,
      referenceDevice,
      () => client.reconfigureParticipant(
        referenceDevice,
        peerDescriptor.participantId,
        peerCapabilities,
        proposal.peerAssignment,
        "canonical"
      )
    );
    peerAttempted = true;
    await client.reconfigureParticipant(
      peerDevice,
      referenceDescriptor.participantId,
      referenceCapabilities,
      proposal.referenceAssignment,
      "direct"
    );
    await convergeCalibrationSite(
      client,
      referenceDevice,
      peerDevice,
      referenceDescriptor,
      peerDescriptor,
      proposal.referenceAssignment.poseVersion,
      proposal.peerAssignment.poseVersion
    );
  } catch (cause) {
    const restored = await restoreActiveCalibrationPair(
      client,
      referenceDevice,
      peerDevice,
      referenceDescriptor,
      peerDescriptor,
      ownerAttempted,
      peerAttempted,
      ownerBaselineCapabilities,
      ownerBaselineAssignment,
      peerBaselineCapabilities,
      peerBaselineAssignment,
      proposal.peerAssignment,
      proposal.referenceAssignment
    );
    if (!restored) {
      throw new PlatformCalibrationError("calibration_rollback_incomplete");
    }
    throw cause;
  }
}

async function restoreActiveCalibrationPair(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  ownerAttempted: boolean,
  peerAttempted: boolean,
  ownerCapabilities: PlatformParticipantCapabilities,
  ownerAssignment: PlatformParticipantAssignment,
  peerCapabilities: PlatformParticipantCapabilities,
  peerAssignment: PlatformParticipantAssignment,
  attemptedOwnerAssignment: PlatformParticipantAssignment,
  attemptedPeerAssignment: PlatformParticipantAssignment
): Promise<boolean> {
  let restored = true;
  if (ownerAttempted) {
    const rollbackAssignment = forwardRollbackAssignment(
      ownerAssignment,
      attemptedOwnerAssignment
    );
    if (!rollbackAssignment) return false;
    try {
      await runOwnerMutationAfterContinuity(
        client,
        referenceDevice,
        () => client.reconfigureParticipant(
          referenceDevice,
          peerDescriptor.participantId,
          ownerCapabilities,
          rollbackAssignment,
          "canonical"
        )
      );
    } catch {
      restored = false;
    }
  }
  if (peerAttempted) {
    const rollbackAssignment = forwardRollbackAssignment(
      peerAssignment,
      attemptedPeerAssignment
    );
    if (!rollbackAssignment) return false;
    try {
      await client.reconfigureParticipant(
        peerDevice,
        referenceDescriptor.participantId,
        peerCapabilities,
        rollbackAssignment,
        "direct"
      );
    } catch {
      restored = false;
    }
  }
  return restored;
}

function forwardRollbackAssignment(
  baseline: PlatformParticipantAssignment,
  attempted: PlatformParticipantAssignment
): PlatformParticipantAssignment | null {
  const currentVersion = Math.max(
    baseline.calibrationVersion,
    attempted.calibrationVersion
  );
  if (currentVersion >= 0xffffffff) return null;
  return {
    ...cloneAssignment(baseline),
    calibrationVersion: currentVersion + 1
  };
}

function cloneCapabilities(
  capabilities: PlatformParticipantCapabilities
): PlatformParticipantCapabilities {
  return { ...capabilities };
}

function cloneAssignment(assignment: PlatformParticipantAssignment): PlatformParticipantAssignment {
  return {
    ...assignment,
    transform: assignment.transform ? { ...assignment.transform } : assignment.transform
  };
}

function assertCalibrationPoseContract(proposal: PlatformCalibrationProposal): void {
  const referenceNodePoseVersion = proposal.referenceAssignment.poseVersion;
  const peerNodePoseVersion = proposal.peerAssignment.poseVersion;
  if (!Number.isInteger(referenceNodePoseVersion) || referenceNodePoseVersion < 1 ||
      !Number.isInteger(peerNodePoseVersion) || peerNodePoseVersion < 1 ||
      proposal.referenceAssignment.localPoseVersion !== peerNodePoseVersion ||
      proposal.peerAssignment.localPoseVersion !== referenceNodePoseVersion) {
    throw new PlatformCalibrationError("calibration_pose_contract_invalid");
  }
}

export function rebaseCalibrationVersion(
  proposal: PlatformCalibrationProposal,
  site: PlatformSiteConfigurationStatus,
  referenceParticipantId: string,
  peerParticipantId: string
): PlatformCalibrationProposal {
  const assignmentVersion = (
    participantId: string,
    assignment: PlatformParticipantAssignment
  ): number => {
    const profile = site.members?.find(
      (member) => member.participantId === participantId
    )?.spatialProfile;
    if (!profile) return Math.max(1, assignment.calibrationVersion);
    const unchanged = profile.spaceId === assignment.spaceId &&
      profile.groupId === assignment.groupId &&
      profile.poseVersion === assignment.poseVersion &&
      profile.transform.originXMillimeters === assignment.transform?.originXMillimeters &&
      profile.transform.originYMillimeters === assignment.transform?.originYMillimeters &&
      profile.transform.rotationCentiDegrees === assignment.transform?.rotationCentiDegrees;
    if (unchanged) return profile.calibrationVersion;
    if (profile.calibrationVersion >= 0xffffffff) {
      throw new PlatformCalibrationError("calibration_version_invalid");
    }
    return profile.calibrationVersion + 1;
  };
  const referenceCalibrationVersion = assignmentVersion(
    referenceParticipantId,
    proposal.referenceAssignment
  );
  const peerCalibrationVersion = assignmentVersion(
    peerParticipantId,
    proposal.peerAssignment
  );
  if (proposal.referenceAssignment.calibrationVersion === referenceCalibrationVersion &&
      proposal.peerAssignment.calibrationVersion === peerCalibrationVersion) {
    return proposal;
  }
  return {
    ...proposal,
    referenceAssignment: {
      ...proposal.referenceAssignment,
      calibrationVersion: referenceCalibrationVersion
    },
    peerAssignment: {
      ...proposal.peerAssignment,
      calibrationVersion: peerCalibrationVersion
    }
  };
}

async function runOwnerMutationAfterContinuity<T>(
  client: CalibrationClient,
  ownerDevice: string,
  mutation: () => Promise<T>,
  initialSite?: PlatformSiteConfigurationStatus
): Promise<T> {
  if (initialSite?.sharedWritesAllowed === false) {
    await waitForSharedFloorplanWritable({
      readSite: () => client.getSiteConfiguration(ownerDevice)
    });
  }
  try {
    return await mutation();
  } catch (cause) {
    if (!isContinuityUnavailable(cause)) throw cause;
    await waitForSharedFloorplanWritable({
      readSite: () => client.getSiteConfiguration(ownerDevice)
    });
    return mutation();
  }
}

function isContinuityUnavailable(cause: unknown): boolean {
  return cause instanceof PlatformPairingError &&
    cause.code === "site_configuration_continuity_unavailable";
}

async function convergeCalibrationSite(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  referencePoseVersion: number,
  peerPoseVersion: number
): Promise<void> {
  const canonical = await client.getSiteConfiguration(referenceDevice);
  const ownerNodeId = canonical.configurationOwnerNodeId;
  const floorplan = canonical.floorplan;
  if (!canonical.localOwner || ownerNodeId !== referenceDescriptor.participantId ||
      !floorplan.committed || !floorplan.floorplanId) {
    throw new PlatformCalibrationError("calibration_site_not_ready");
  }

  const siteExpectation = siteConvergenceExpectation(canonical);
  const replicaExpectation: PlatformParticipantReplicaExpectation = {
    siteConfigurationAuthorityEpoch: siteExpectation.authorityEpoch,
    siteConfigurationGeneration: canonical.generation,
    siteConfigurationDigest: siteExpectation.configurationDigest,
    floorplanGeometryVersion: floorplan.geometryVersion,
    floorplanConfigHash: floorplan.configHash,
    floorplanImageHash: floorplan.imageHash
  };

  await requestCalibrationSiteSync(client, peerDevice, ownerNodeId);
  await client.waitForSiteConvergence(peerDevice, siteExpectation);
  await Promise.all([
    client.waitForParticipantReplica(
      referenceDevice,
      peerDescriptor.participantId,
      {
        ...replicaExpectation,
        radarPoseVersion: peerPoseVersion
      }
    ),
    client.waitForParticipantReplica(
      peerDevice,
      referenceDescriptor.participantId,
      {
        ...replicaExpectation,
        radarPoseVersion: referencePoseVersion
      }
    )
  ]);
  await waitForSharedFloorplanWritable({
    readSite: () => client.getSiteConfiguration(referenceDevice),
    timeoutMs: 60000
  });
}

async function waitForCalibrationActivationReadiness(
  client: CalibrationClient,
  referenceDevice: string,
  peerDevice: string,
  referenceDescriptor: PlatformDescriptor,
  peerDescriptor: PlatformDescriptor,
  canonical: PlatformSiteConfigurationStatus,
  referencePoseVersion: number,
  peerPoseVersion: number
): Promise<void> {
  const ownerNodeId = canonical.configurationOwnerNodeId;
  const floorplan = canonical.floorplan;
  if (!canonical.localOwner || ownerNodeId !== referenceDescriptor.participantId ||
      !floorplan.committed || !floorplan.floorplanId) {
    throw new PlatformCalibrationError("calibration_site_not_ready");
  }

  const siteExpectation = siteConvergenceExpectation(canonical);
  const replicaExpectation: PlatformParticipantReplicaExpectation = {
    siteConfigurationAuthorityEpoch: siteExpectation.authorityEpoch,
    siteConfigurationGeneration: canonical.generation,
    siteConfigurationDigest: siteExpectation.configurationDigest,
    floorplanGeometryVersion: floorplan.geometryVersion,
    floorplanConfigHash: floorplan.configHash,
    floorplanImageHash: floorplan.imageHash
  };

  await requestCalibrationSiteSync(client, peerDevice, ownerNodeId);
  await client.waitForSiteConvergence(peerDevice, siteExpectation);
  await Promise.all([
    client.waitForParticipantReplica(referenceDevice, peerDescriptor.participantId, {
      ...replicaExpectation,
      radarPoseVersion: peerPoseVersion
    }),
    client.waitForParticipantReplica(peerDevice, referenceDescriptor.participantId, {
      ...replicaExpectation,
      radarPoseVersion: referencePoseVersion
    })
  ]);
}

async function requestExistingCalibrationPeersSync(
  client: CalibrationClient,
  participants: PlatformParticipantsStatus,
  joiningParticipantId: string,
  ownerNodeId: string
): Promise<void> {
  const devices = new Set(
    participants.participants
      .filter((participant) =>
        participant.participantId !== joiningParticipantId &&
        participantMembership(participant, "canonical") === "active" &&
        participant.endpoint?.authenticated === true &&
        participant.endpoint.online === true &&
        Boolean(participant.endpoint.address)
      )
      .map((participant) => participant.endpoint!.address)
  );
  await Promise.allSettled(
    [...devices].map((device) => client.syncSiteConfiguration(device, ownerNodeId))
  );
}

function isTransientSiteSync(cause: unknown): boolean {
  return cause instanceof PlatformPairingError &&
    (cause.code === "owner_offline" || cause.code === "busy");
}

async function requestCalibrationSiteSync(
  client: CalibrationClient,
  peerDevice: string,
  ownerNodeId: string
): Promise<void> {
  try {
    await client.syncSiteConfiguration(peerDevice, ownerNodeId);
  } catch (cause) {
    if (!isTransientSiteSync(cause)) throw cause;
  }
}

function validateFloorplanPair(reference: FloorplanStorageDocument, peer: FloorplanStorageDocument): void {
  if (!reference.floorplanId || reference.floorplanId !== peer.floorplanId ||
      !reference.geometryVersion || reference.geometryVersion !== peer.geometryVersion) {
    throw new PlatformCalibrationError("calibration_floorplan_mismatch");
  }
  for (const document of [reference, peer]) {
    const { widthMm, heightMm, outerBoundsPx, mmPerPxX, mmPerPxY } = document.scale;
    const widthPx = outerBoundsPx?.[2];
    const heightPx = outerBoundsPx?.[3];
    if (![widthMm, heightMm, widthPx, heightPx, mmPerPxX, mmPerPxY].every(
      (value) => Number.isFinite(value) && value > 0
    )) {
      throw new PlatformCalibrationError("calibration_scale_invalid");
    }
    if (!scaleValueMatches(mmPerPxX, widthMm / widthPx) ||
        !scaleValueMatches(mmPerPxY, heightMm / heightPx)) {
      throw new PlatformCalibrationError("calibration_scale_invalid");
    }
  }
  const referenceScale = [
    reference.scale.widthMm,
    reference.scale.heightMm,
    ...reference.scale.outerBoundsPx,
    reference.scale.mmPerPxX,
    reference.scale.mmPerPxY
  ];
  const peerScale = [
    peer.scale.widthMm,
    peer.scale.heightMm,
    ...peer.scale.outerBoundsPx,
    peer.scale.mmPerPxX,
    peer.scale.mmPerPxY
  ];
  if (referenceScale.some((value, index) => !scaleValueMatches(value, peerScale[index]))) {
    throw new PlatformCalibrationError("calibration_floorplan_mismatch");
  }
}

function scaleValueMatches(actual: number, expected: number): boolean {
  return Math.abs(actual - expected) <= SCALE_ROUNDING_TOLERANCE;
}

function singleActiveTarget(state: WebDeviceState): WebLocalTarget {
  if (!state.connected) throw new PlatformCalibrationError("calibration_device_offline");
  const targets = calibrationTargets(state).filter((target) => target.active);
  if (targets.length !== 1) throw new PlatformCalibrationError("calibration_requires_one_target");
  return targets[0];
}

function validActiveTargets(state: WebDeviceState): WebLocalTarget[] {
  return calibrationTargets(state).filter(
    (target) => target.active && Number.isFinite(target.x) && Number.isFinite(target.y)
  );
}

function calibrationTargets(state: WebDeviceState): WebLocalTarget[] {
  if (Array.isArray(state.local?.targets)) return state.local.targets;
  return state.fusion?.authoritative === true ? [] : state.targets;
}

function targetPoint(target: WebLocalTarget): Point {
  if (!Number.isFinite(target.x) || !Number.isFinite(target.y)) {
    throw new PlatformCalibrationError("calibration_target_invalid");
  }
  return [target.x, target.y];
}

function worldPointToReferencePixels(
  point: Point,
  referenceFloorplan: FloorplanStorageDocument
): PlatformCalibrationDisplayPoint {
  return {
    xPx: point[0] / referenceFloorplan.scale.mmPerPxX,
    yPx: point[1] / referenceFloorplan.scale.mmPerPxY
  };
}

function floorplanTransform(document: FloorplanStorageDocument): PlatformSpatialTransform {
  return transformFrom(
    [
      document.radar.originPx[0] * document.scale.mmPerPxX,
      document.radar.originPx[1] * document.scale.mmPerPxY
    ],
    document.radar.rotationDeg,
    1,
    0
  );
}

function transformFrom(origin: Point, rotationDegrees: number, samples: number, residual: number): PlatformSpatialTransform {
  return {
    originXMillimeters: Math.round(origin[0]),
    originYMillimeters: Math.round(origin[1]),
    rotationCentiDegrees: Math.round(normalizeDegrees(rotationDegrees) * 100),
    residualMillimeters: Math.round(residual),
    calibrationSamples: samples
  };
}

function applyTransform(transform: PlatformSpatialTransform, point: Point): Point {
  const rotated = rotateRadarPoint(point, transform.rotationCentiDegrees / 100);
  return [transform.originXMillimeters + rotated[0], transform.originYMillimeters + rotated[1]];
}

function rotateRadarPoint([x, y]: Point, rotationDegrees: number): Point {
  const angle = rotationDegrees * Math.PI / 180;
  return [x * Math.cos(angle) + y * Math.sin(angle), x * Math.sin(angle) - y * Math.cos(angle)];
}

function boundedAssignmentId(room: FloorplanStorageRoom): string {
  if (/^[A-Za-z0-9._:-]{1,40}$/.test(room.id)) return room.id;
  let hash = 2166136261;
  for (const character of room.id) {
    hash ^= character.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return `room-${(hash >>> 0).toString(16).padStart(8, "0")}`;
}

function radarAssignmentRoom(
  document: FloorplanStorageDocument,
  transform: PlatformSpatialTransform
): FloorplanStorageRoom | null {
  const probes: Point[] = [
    applyTransform(transform, [0, RADAR_ROOM_PROBE_MM]),
    applyTransform(transform, [0, 0])
  ];
  for (const probe of probes) {
    const room = findRoomContainingPoint(
      document.rooms,
      {
        x: probe[0] / document.scale.mmPerPxX,
        y: probe[1] / document.scale.mmPerPxY
      },
      (candidate) => candidate.pointsPx
    );
    if (room) return room;
  }
  return null;
}

function positivePoseVersion(document: FloorplanStorageDocument): number {
  const value = document.radar.poseVersion;
  if (!Number.isInteger(value) || (value ?? 0) <= 0) throw new PlatformCalibrationError("calibration_pose_missing");
  return value as number;
}

function subtract(a: Point, b: Point): Point {
  return [a[0] - b[0], a[1] - b[1]];
}

function length(point: Point): number {
  return Math.hypot(point[0], point[1]);
}

function average(values: number[]): number {
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

function normalizeDegrees(value: number): number {
  let normalized = value % 360;
  if (normalized > 180) normalized -= 360;
  if (normalized <= -180) normalized += 360;
  return normalized;
}

function radiansToDegrees(value: number): number {
  return value * 180 / Math.PI;
}

function round(value: number, digits: number): number {
  const factor = 10 ** digits;
  return Math.round(value * factor) / factor;
}
