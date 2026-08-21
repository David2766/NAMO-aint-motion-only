import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import { floorplanPxToRadarPoint } from "../../core/floorplan/radar-floorplan-transform";
import { pointInPolygon } from "../../core/geometry";
import type { PlatformParticipantAssignment, PlatformParticipantCapabilities } from "./platform-pairing";
import { mockPairingError, mockPairingJson } from "./mock-platform-pairing";
import {
  MOCK_LOCAL_NODE_ID,
  MOCK_MAX_PEERS,
  MOCK_SITE_ID,
  type MockPeer,
  type MockPlatformOptions,
  mockPeerAddress,
  mockPeerId,
  mockPlatformState,
  persistMockPlatformState,
  removeMockPlatformParticipant
} from "./mock-platform-state";

export function mockParticipantsStatus(index: number, options: MockPlatformOptions) {
  const peers = mockPlatformState.peers;
  const visible =
    index === 0
      ? peers.map((peer) => participantView(peer.index, peer, options))
      : peers.some((peer) => peer.index === index)
        ? [participantView(0, mockPlatformState.peers.find((peer) => peer.index === index)!, options)]
        : [];
  return {
    ok: true,
    managementAllowed: index === 0,
    registry: {
      ready: true,
      generation: mockPlatformState.generation,
      remoteCount: visible.length,
      activeRemoteCount: visible.filter((participant) => participant.membership === "active").length,
      maxRemoteParticipants: MOCK_MAX_PEERS,
      maxParticipants: MOCK_MAX_PEERS + 1,
      projectedRemoteCount: visible.length
    },
    pairing: {
      supported: true,
      enabled: false,
      remainingMs: 0,
      maxDurationMs: 180000
    },
    participants: visible
  };
}

export function mutateMockParticipant(
  deviceIndex: number,
  form: URLSearchParams,
  options: MockPlatformOptions
): Response {
  const action = String(form.get("action") ?? "");
  if (action === "open") return mockPairingJson(mockParticipantsStatus(deviceIndex, options));
  const targetId = String(form.get("participantId") ?? "");
  const peerIndex =
    targetId === MOCK_LOCAL_NODE_ID
      ? deviceIndex
      : (mockPlatformState.peers.find((peer) => mockPeerId(peer.index) === targetId)?.index ?? 0);
  const peer = mockPlatformState.peers.find((candidate) => candidate.index === peerIndex);
  if (!peer) return mockPairingError(404, "participant_not_found");

  if (action === "revoke") peer.membership = "revoked";
  if (action === "forget") {
    removeMockPlatformParticipant(mockPeerId(peer.index));
    return mockPairingJson(mockParticipantsStatus(deviceIndex, options));
  }
  if (action === "activate") peer.membership = "active";
  if (action === "quarantine") peer.membership = "quarantined";
  if (action === "set_community_leader") peer.leaderEligible = form.get("eligible") === "true";
  if (action === "configure" || action === "reconfigure") {
    const capabilities = capabilitiesFromForm(form);
    const assignment = assignmentFromForm(form);
    if (targetId === MOCK_LOCAL_NODE_ID) {
      mockPlatformState.localCapabilities = capabilities;
      mockPlatformState.localAssignment = assignment;
    } else {
      peer.capabilities = capabilities;
      peer.assignment = assignment;
    }
    peer.membership = action === "reconfigure" ? "active" : "quarantined";
  }
  mockPlatformState.generation += 1;
  persistMockPlatformState();
  return mockPairingJson(mockParticipantsStatus(deviceIndex, options));
}

export function mockSiteStatus(index: number, options: MockPlatformOptions) {
  const floorplan = options.floorplan(window.location.origin);
  const revision = canonicalSiteRevision();
  const peers = mockPlatformState.peers.filter((peer) => peer.membership !== "revoked");
  const localProfile = profileFor(mockPlatformState.localAssignment, floorplan.document, 0);
  return {
    ok: true,
    schemaVersion: 2,
    siteId: MOCK_SITE_ID,
    authorityEpoch: 1,
    generation: revision.generation,
    configurationDigest: revision.digest,
    configurationConsistent: true,
    rosterRevision: mockPlatformState.generation,
    rosterCommitted: true,
    configurationOwnerNodeId: MOCK_LOCAL_NODE_ID,
    localOwner: index === 0,
    sharedWritesAllowed: true,
    memberCount: peers.length + 1,
    activeMemberCount: 1 + peers.filter((peer) => peer.membership === "active").length,
    members: [
      memberView(MOCK_LOCAL_NODE_ID, "active", true, localProfile),
      ...peers.map((peer) =>
        memberView(
          mockPeerId(peer.index),
          peer.membership,
          peer.leaderEligible,
          profileFor(peer.assignment, floorplan.document, peer.index)
        )
      )
    ],
    floorplan: {
      committed: floorplan.configured,
      floorplanId: floorplan.document?.floorplanId ?? null,
      geometryVersion: floorplan.document?.geometryVersion ?? 0,
      configHash: floorplan.configHash,
      imageHash: floorplan.imageHash
    },
    replication: null,
    automaticHandover: {
      state: "idle",
      condition: "not_required",
      successorNodeId: null,
      waitRemainingMs: 0
    }
  };
}

export function mutateMockSite(index: number, form: URLSearchParams, options: MockPlatformOptions): Response {
  const action = String(form.get("action") ?? "");
  if (action === "sync" && index > 0) {
    const peer = mockPlatformState.peers.find((candidate) => candidate.index === index);
    if (peer) {
      peer.floorplanSynced = true;
      const document = options.floorplan(window.location.origin).document;
      if (document) ensureMockRadarPlacement(index, document);
    }
  }
  if (action === "commitFloorplan" || action === "sync") mockPlatformState.generation += 1;
  persistMockPlatformState();
  return mockPairingJson(mockSiteStatus(index, options));
}

export function mockGroupsStatus() {
  const active = mockPlatformState.peers.filter((peer) => peer.membership === "active").length;
  const expected = active + 1;
  const groupLeader = mockPlatformState.peers.find((peer) => peer.membership === "active")?.index ?? 0;
  return {
    ok: true,
    supported: true,
    site: {
      leaderNodeId: MOCK_LOCAL_NODE_ID,
      localLeader: true,
      participantsOnline: expected,
      participantsExpected: expected,
      health: active > 0 ? "healthy" : "standalone"
    },
    groups:
      active > 0
        ? [
            {
              groupId: "mock_room",
              leaderParticipantId: groupLeader === 0 ? MOCK_LOCAL_NODE_ID : mockPeerId(groupLeader),
              localLeader: groupLeader === 0,
              participantsOnline: expected,
              participantsExpected: expected,
              health: "healthy"
            }
          ]
        : [],
    productionFusion: {
      buildEnabled: true,
      authoritative: active > 0,
      state: active > 0 ? "active" : "degraded",
      sourceSequence: mockPlatformState.generation,
      targetCount: active > 0 ? 3 : 0
    }
  };
}

export function mockDeviceConfig(index: number, options: MockPlatformOptions) {
  const floorplan = options.floorplan(index === 0 ? window.location.origin : `http://${mockPeerAddress(index)}`);
  const document = floorplan.document;
  const radar = document
    ? {
        originX: document.radar.originPx[0],
        originY: document.radar.originPx[1],
        rotation: document.radar.rotationDeg,
        scale: document.radar.scale,
        poseVersion: document.radar.poseVersion ?? 1
      }
    : undefined;
  return {
    version: 1,
    integrationMode: "unknown",
    legacyPresenceFallback: false,
    zones: [],
    calibrationZones: [],
    floorplan: {
      enabled: floorplan.configured,
      hasImage: floorplan.configured,
      ...(radar ? { radar } : {})
    }
  };
}

export function mockDeviceState(index: number, options: MockPlatformOptions) {
  const elapsed = (Date.now() % 120000) / 1000;
  const referenceDocument = options.floorplan(window.location.origin).document;
  const deviceDocument = options.floorplan(
    index === 0 ? window.location.origin : `http://${mockPeerAddress(index)}`
  ).document;
  const worldPoint = referenceDocument ? movingPointInsideRadarRoom(referenceDocument, elapsed) : { x: 0, y: 1800 };
  const offset =
    index === 0 || !referenceDocument ? { x: 0, y: 0 } : mockTargetOffset(index, elapsed, referenceDocument);
  const targetWorldPoint = {
    x: worldPoint.x + offset.x,
    y: worldPoint.y + offset.y
  };
  const [x, y] = deviceDocument
    ? floorplanPxToRadarPoint(targetWorldPoint, {
        placement: {
          originX: deviceDocument.radar.originPx[0],
          originY: deviceDocument.radar.originPx[1],
          rotation: deviceDocument.radar.rotationDeg
        },
        scale: deviceDocument.scale
      })
    : [targetWorldPoint.x, targetWorldPoint.y];
  const target = {
    id: "target_1",
    name: "T1",
    color: "#ff6b7a",
    x: Math.round(x),
    y: Math.round(y),
    active: true,
    fused: false,
    sourceCount: 0
  };
  return {
    connected: true,
    updatedAt: Date.now(),
    presence: true,
    motion: true,
    pirMotion: true,
    targetCount: 1,
    movingTargetCount: 1,
    stillTargetCount: 0,
    fusion: { buildEnabled: true, authoritative: false, state: "degraded" },
    targets: [target],
    local: {
      presence: true,
      motion: true,
      targetCount: 1,
      targets: [{ x: target.x, y: target.y, active: true }]
    }
  };
}

export function ensureMockRadarPlacement(index: number, document: FloorplanStorageDocument) {
  if (index === 0) return document.radar;
  const peer = mockPlatformState.peers.find((candidate) => candidate.index === index);
  if (!peer || !peer.floorplanSynced) return document.radar;
  if (!peer.radarPlacement) {
    peer.radarPlacement = randomRadarPlacement(document);
    persistMockPlatformState();
  }
  return {
    ...document.radar,
    originPx: peer.radarPlacement.originPx,
    rotationDeg: peer.radarPlacement.rotationDeg,
    poseVersion: document.radar.poseVersion ?? 1
  };
}

function randomRadarPlacement(document: FloorplanStorageDocument) {
  const room = radarRoom(document);
  const bounds = polygonBounds(room?.pointsPx ?? outerBoundsPoints(document));
  for (let attempt = 0; attempt < 64; attempt += 1) {
    const point: [number, number] = [
      bounds.minX + bounds.width * (0.12 + Math.random() * 0.76),
      bounds.minY + bounds.height * (0.12 + Math.random() * 0.76)
    ];
    if (!room || pointInPolygon({ x: point[0], y: point[1] }, room.pointsPx)) {
      return {
        originPx: point,
        rotationDeg: Math.round((Math.random() * 360 - 180) * 10) / 10
      };
    }
  }
  const safe = safestRoomPoint(room?.pointsPx ?? outerBoundsPoints(document));
  return {
    originPx: [safe.x, safe.y] as [number, number],
    rotationDeg: Math.round((Math.random() * 360 - 180) * 10) / 10
  };
}

function movingPointInsideRadarRoom(document: FloorplanStorageDocument, elapsed: number) {
  const points = radarRoom(document)?.pointsPx ?? outerBoundsPoints(document);
  const safe = safestRoomPoint(points);
  const clearance = Math.max(0, safe.clearance);
  const maxOffsetPx = 300 / Math.max(0.001, Math.min(document.scale.mmPerPxX, document.scale.mmPerPxY));
  const radius = Math.max(0, clearance - maxOffsetPx - 2) * 0.62;
  return {
    x: safe.x + Math.sin(elapsed / 4.2) * radius,
    y: safe.y + Math.cos(elapsed / 5.1) * radius * 0.72
  };
}

function mockTargetOffset(index: number, elapsed: number, document: FloorplanStorageDocument) {
  const distanceMm = 100 + ((index * 73) % 201);
  const angle = index * 1.17 + Math.sin(elapsed / 9) * 0.18;
  return {
    x: (Math.cos(angle) * distanceMm) / document.scale.mmPerPxX,
    y: (Math.sin(angle) * distanceMm) / document.scale.mmPerPxY
  };
}

function radarRoom(document: FloorplanStorageDocument) {
  const [x, y] = document.radar.originPx;
  return document.rooms.find((room) => pointInPolygon({ x, y }, room.pointsPx)) ?? document.rooms[0] ?? null;
}

function outerBoundsPoints(document: FloorplanStorageDocument): Array<[number, number]> {
  const [x, y, width, height] = document.scale.outerBoundsPx;
  return [
    [x, y],
    [x + width, y],
    [x + width, y + height],
    [x, y + height]
  ];
}

function polygonBounds(points: Array<[number, number]>) {
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  return { minX, minY, width: maxX - minX, height: maxY - minY };
}

function safestRoomPoint(points: Array<[number, number]>) {
  const bounds = polygonBounds(points);
  let best = {
    x: bounds.minX + bounds.width / 2,
    y: bounds.minY + bounds.height / 2,
    clearance: -1
  };
  for (let row = 1; row < 10; row += 1) {
    for (let column = 1; column < 10; column += 1) {
      const candidate = {
        x: bounds.minX + bounds.width * (column / 10),
        y: bounds.minY + bounds.height * (row / 10)
      };
      if (!pointInPolygon(candidate, points)) continue;
      const clearance = minimumEdgeDistance(candidate, points);
      if (clearance > best.clearance) best = { ...candidate, clearance };
    }
  }
  return best;
}

function minimumEdgeDistance(point: { x: number; y: number }, points: Array<[number, number]>) {
  let minimum = Number.POSITIVE_INFINITY;
  for (let index = 0; index < points.length; index += 1) {
    const [x1, y1] = points[index];
    const [x2, y2] = points[(index + 1) % points.length];
    const dx = x2 - x1;
    const dy = y2 - y1;
    const lengthSquared = dx * dx + dy * dy;
    const projection =
      lengthSquared > 0 ? Math.max(0, Math.min(1, ((point.x - x1) * dx + (point.y - y1) * dy) / lengthSquared)) : 0;
    minimum = Math.min(minimum, Math.hypot(point.x - (x1 + projection * dx), point.y - (y1 + projection * dy)));
  }
  return Number.isFinite(minimum) ? minimum : 0;
}

function participantView(index: number, peer: MockPeer, options: MockPlatformOptions) {
  const canonical = options.floorplan(window.location.origin);
  const replica = index === 0 ? canonical : options.floorplan(`http://${mockPeerAddress(index)}`);
  const assignment = index === 0 ? mockPlatformState.localAssignment : peer.assignment;
  const capabilities = index === 0 ? mockPlatformState.localCapabilities : peer.capabilities;
  const site = canonicalSiteRevision();
  return {
    participantId: index === 0 ? MOCK_LOCAL_NODE_ID : mockPeerId(index),
    participantType: "namo_node",
    trust: "community_unverified",
    membership: peer.membership,
    canonicalMembership: peer.membership,
    directMembership: peer.membership,
    leaderEligible: peer.leaderEligible,
    canonicalMember: true,
    directPair: true,
    capabilities: capabilities ?? null,
    assignment: assignment ?? null,
    directAssignment: assignment ?? null,
    credential: { provisioned: true, enabled: true, keyVersion: 1 },
    endpoint: {
      address: index === 0 ? new URL(window.location.origin).hostname : mockPeerAddress(index),
      port: 80,
      authenticated: true,
      online: true,
      lastSeenAgeMs: 0,
      siteConfigurationAuthorityEpoch: 1,
      siteConfigurationGeneration: site.generation,
      siteConfigurationDigest: site.digest,
      siteDirectoryRevision: mockPlatformState.generation,
      siteDirectoryDigest: mockPlatformState.generation,
      floorplanGeometryVersion: canonical.document?.geometryVersion ?? 0,
      floorplanConfigHash: canonical.configHash,
      floorplanImageHash: canonical.imageHash,
      radarPoseVersion: assignment?.poseVersion ?? 1,
      floorplanReplicaReady: replica.configured,
      heartbeatWireVersion: 2,
      siteCredentialsComplete: true
    },
    readiness: {
      ready: peer.membership === "active" && replica.configured && Boolean(assignment),
      missing: peer.membership === "active" && replica.configured && assignment ? [] : ["spatial_profile"]
    }
  };
}

function capabilitiesFromForm(form: URLSearchParams): PlatformParticipantCapabilities {
  return {
    pointTracks: form.get("pointTracks") === "true",
    velocity: form.get("velocity") === "true",
    positionUncertainty: form.get("positionUncertainty") === "true",
    range: form.get("range") === "true",
    presence: form.get("presence") === "true",
    motion: form.get("motion") === "true",
    zonePresence: form.get("zonePresence") === "true",
    bridge: form.get("bridge") === "true",
    maxTargets: Number(form.get("maxTargets") ?? 3)
  };
}

function assignmentFromForm(form: URLSearchParams): PlatformParticipantAssignment {
  const transformPresent = form.has("originXMillimeters");
  return {
    floorplanId: String(form.get("floorplanId") ?? ""),
    spaceId: String(form.get("spaceId") ?? ""),
    groupId: String(form.get("groupId") ?? ""),
    geometryVersion: Number(form.get("geometryVersion") ?? 1),
    localPoseVersion: Number(form.get("localPoseVersion") ?? 1),
    poseVersion: Number(form.get("poseVersion") ?? 1),
    calibrationVersion: Number(form.get("calibrationVersion") ?? 1),
    transform: transformPresent
      ? {
          originXMillimeters: Number(form.get("originXMillimeters") ?? 0),
          originYMillimeters: Number(form.get("originYMillimeters") ?? 0),
          rotationCentiDegrees: Number(form.get("rotationCentiDegrees") ?? 0),
          residualMillimeters: Number(form.get("residualMillimeters") ?? 0),
          calibrationSamples: Number(form.get("calibrationSamples") ?? 2)
        }
      : null
  };
}

function canonicalSiteRevision() {
  const generation = mockPlatformState.generation;
  return { generation, digest: generation.toString(16).padStart(16, "0") };
}

function memberView(
  id: string,
  membership: string,
  leaderEligible: boolean,
  spatialProfile: ReturnType<typeof profileFor>
) {
  return {
    participantId: id,
    participantType: "namo_node",
    trust: "community_unverified",
    membership,
    leaderEligible,
    spatialProfile
  };
}

function profileFor(
  assignment: PlatformParticipantAssignment | undefined,
  document: FloorplanStorageDocument | null,
  index: number
) {
  if (assignment) {
    return {
      spaceId: assignment.spaceId,
      groupId: assignment.groupId,
      poseVersion: assignment.poseVersion,
      calibrationVersion: assignment.calibrationVersion,
      transform: assignment.transform ?? null
    };
  }
  if (!document) return null;
  const x = Math.round(document.radar.originPx[0] * document.scale.mmPerPxX);
  const y = Math.round(document.radar.originPx[1] * document.scale.mmPerPxY);
  return {
    spaceId: document.rooms[0]?.id ?? "mock_room",
    groupId: document.rooms[0]?.id ?? "mock_room",
    poseVersion: 1,
    calibrationVersion: 1,
    transform: {
      originXMillimeters: x,
      originYMillimeters: y,
      rotationCentiDegrees: index === 0 ? Math.round(document.radar.rotationDeg * 100) : 0,
      residualMillimeters: 0,
      calibrationSamples: 2
    }
  };
}
