import { describe, expect, it, vi } from "vitest";
import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import type { WebDeviceState } from "../core/types";
import {
  PlatformPairingError,
  type PlatformParticipantAssignment,
  type PlatformParticipantCapabilities,
  type PlatformParticipantsStatus,
  type PlatformSiteConfigurationStatus
} from "./api/platform-pairing";
import {
  PlatformCalibrationError,
  applyAutomaticCalibrationGroup,
  approvePlatformCalibration,
  buildPlatformCalibrationDisplayModel,
  buildPlatformCalibrationProposal,
  calibrationSampleReferencePointPx,
  capturePlatformCalibrationSample,
  rebaseCalibrationVersion
} from "./platform-calibration";

function floorplan(origin: [number, number], rotationDeg = 0): FloorplanStorageDocument {
  return {
    version: 2,
    floorplanId: "fp-12345678901234567890123456789012",
    geometryVersion: 4,
    image: { path: "/floorplan.webp", mime: "image/webp", width: 500, height: 400 },
    scale: { widthMm: 5000, heightMm: 4000, outerBoundsPx: [0, 0, 500, 400], mmPerPxX: 10, mmPerPxY: 10 },
    radar: { originPx: origin, rotationDeg, scale: 1, poseVersion: 3 },
    rooms: [{ id: "room-main", name: "Main", kind: "room", pointsPx: [[0, 0], [500, 0], [500, 400], [0, 400]] }],
    occlusion: { ignoredEdges: [] }
  };
}

function dividedFloorplan(origin: [number, number]): FloorplanStorageDocument {
  const document = floorplan(origin);
  document.rooms = [
    { id: "room-left", name: "Left", kind: "room", pointsPx: [[0, 0], [250, 0], [250, 400], [0, 400]] },
    { id: "room-right", name: "Right", kind: "room", pointsPx: [[250, 0], [500, 0], [500, 400], [250, 400]] }
  ];
  return document;
}

function state(x: number, y: number, activeTargets = 1): WebDeviceState {
  return {
    connected: true,
    updatedAt: 1,
    targets: Array.from({ length: activeTargets }, (_, index) => ({
      id: `target-${index}`,
      name: `Target ${index}`,
      color: "#fff",
      x: x + index,
      y,
      active: true
    }))
  };
}

function siteStatus(generation = 4) {
  return {
    ok: true,
    schemaVersion: 1,
    siteId: "site-main",
    authorityEpoch: 1,
    generation,
    configurationDigest: "0123456789abcdef",
    configurationConsistent: true,
    rosterRevision: generation,
    rosterCommitted: true,
    configurationOwnerNodeId: "node-a",
    localOwner: true,
    sharedWritesAllowed: true,
    memberCount: 2,
    activeMemberCount: 1,
    members: [
      {
        participantId: "node-a",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        leaderEligible: true,
        spatialProfile: null
      },
      {
        participantId: "node-b",
        participantType: "namo_node",
        trust: "community_unverified",
        membership: "quarantined",
        leaderEligible: false,
        spatialProfile: null
      }
    ],
    floorplan: {
      committed: true,
      floorplanId: "fp-12345678901234567890123456789012",
      geometryVersion: 4,
      configHash: 101,
      imageHash: 202
    },
    replication: null
  };
}

const calibrationCapabilities: PlatformParticipantCapabilities = {
  pointTracks: true,
  velocity: true,
  positionUncertainty: false,
  range: true,
  presence: true,
  motion: true,
  zonePresence: false,
  bridge: false,
  maxTargets: 3
};

function calibrationParticipantStatus(
  device: string,
  membership?: string,
  assignment?: PlatformParticipantAssignment
) {
  const projectedMembership = membership ??
    (device === "http://a" ? "quarantined" : "active");
  const directMembership = membership ?? "quarantined";
  return {
    ok: true,
    participants: [{
      participantId: device === "http://a" ? "node-b" : "node-a",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: projectedMembership,
      canonicalMembership: projectedMembership,
      directMembership,
      canonicalMember: true,
      directPair: true,
      capabilities: { ...calibrationCapabilities },
      assignment: assignment ?? null,
      credential: { provisioned: true, enabled: true, keyVersion: 1 }
    }]
  };
}

function initialCalibrationClientMethods() {
  return {
    getParticipants: vi.fn(async (device: string) => calibrationParticipantStatus(device)),
    reconfigureParticipant: vi.fn(async () => ({ ok: true, participants: [] }))
  };
}

function calibrationDescriptor(participantId: string) {
  return {
    protocolVersion: 1,
    participantId,
    participantType: "namo_node",
    product: "namo-spatial-sensor",
    softwareVersion: "test",
    capabilities: { ...calibrationCapabilities }
  };
}

function calibrationProposal() {
  return buildPlatformCalibrationProposal(
    floorplan([100, 100]),
    floorplan([105, 100]),
    [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
    2
  );
}

function capacitySiteStatus(
  totalMembers: number,
  peerMembership: "active" | "quarantined"
): PlatformSiteConfigurationStatus {
  const existingIds = Array.from(
    { length: totalMembers - 2 },
    (_, index) => `node-existing-${index + 1}`
  );
  const member = (participantId: string, membership: "active" | "quarantined", index: number) => ({
    participantId,
    participantType: "namo_node",
    trust: index % 2 === 0 ? "official_verified" : "community_unverified",
    membership,
    leaderEligible: membership === "active" && index % 3 !== 2,
    spatialProfile: membership === "active" ? {
      spaceId: "room-main",
      groupId: "room-main",
      poseVersion: index + 1,
      calibrationVersion: 1,
      transform: {
        originXMillimeters: index * 250,
        originYMillimeters: index * 100,
        rotationCentiDegrees: 0
      }
    } : null
  });
  const members = [
    member("node-owner", "active", 0),
    ...existingIds.map((participantId, index) => member(participantId, "active", index + 1)),
    member("node-joining", peerMembership, totalMembers - 1)
  ];
  return {
    ...siteStatus(),
    configurationOwnerNodeId: "node-owner",
    memberCount: totalMembers,
    activeMemberCount: peerMembership === "active" ? totalMembers : totalMembers - 1,
    members
  };
}

function capacityParticipantStatus(
  totalMembers: number,
  device: string,
  peerMembership: "active" | "quarantined",
  proposal: ReturnType<typeof calibrationProposal>
): PlatformParticipantsStatus {
  const existingIds = Array.from(
    { length: totalMembers - 2 },
    (_, index) => `node-existing-${index + 1}`
  );
  const localId = device === "http://owner" ? "node-owner" : "node-joining";
  const targetId = device === "http://owner" ? "node-joining" : "node-owner";
  const participantIds = ["node-owner", ...existingIds, "node-joining"]
    .filter((participantId) => participantId !== localId);
  if (totalMembers % 2 === 1) participantIds.reverse();
  return {
    ok: true,
    participants: participantIds.map((participantId, index) => {
      const isTarget = participantId === targetId;
      const membership = participantId === "node-joining" ? peerMembership : "active";
      const directMembership = isTarget ? peerMembership : null;
      const assignment = !isTarget || peerMembership !== "active"
        ? null
        : device === "http://owner"
          ? { ...proposal.peerAssignment, calibrationVersion: 1 }
          : { ...proposal.referenceAssignment, calibrationVersion: 1 };
      return {
        participantId,
        participantType: "namo_node",
        trust: index % 2 === 0 ? "official_verified" : "community_unverified",
        membership,
        canonicalMembership: membership,
        directMembership,
        leaderEligible: membership === "active",
        canonicalMember: true,
        directPair: isTarget,
        capabilities: { ...calibrationCapabilities },
        assignment,
        credential: {
          provisioned: isTarget || membership === "active",
          enabled: isTarget || membership === "active",
          keyVersion: 1
        }
      };
    })
  };
}

describe("platform calibration", () => {
  it.each(
    [2, 3, 4, 5, 6, 7].flatMap((totalMembers) => [
      [totalMembers, "quarantined" as const],
      [totalMembers, "active" as const]
    ])
  )("selects the canonical calibration path with %i members and a %s peer", async (
    totalMembers,
    peerMembership
  ) => {
    const proposal = calibrationProposal();
    const configureParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const reconfigureParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const activateParticipant = vi.fn(async (
      _device: string,
      _participantId: string,
      _view?: string
    ) => ({ ok: true, participants: [] }));
    const quarantineParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const client = {
      getParticipants: vi.fn(async (device: string) => capacityParticipantStatus(
        totalMembers, device, peerMembership, proposal
      )),
      configureParticipant,
      reconfigureParticipant,
      activateParticipant,
      quarantineParticipant,
      getSiteConfiguration: vi.fn(async () => capacitySiteStatus(totalMembers, peerMembership)),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-joining" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-joining" }))
    };

    await approvePlatformCalibration(
      client,
      "http://owner",
      "http://joining",
      calibrationDescriptor("node-owner"),
      calibrationDescriptor("node-joining"),
      proposal
    );

    if (peerMembership === "quarantined") {
      expect(configureParticipant).toHaveBeenCalledTimes(2);
      expect(activateParticipant).toHaveBeenCalledTimes(2);
      expect(reconfigureParticipant).not.toHaveBeenCalled();
    } else {
      expect(reconfigureParticipant).toHaveBeenCalledTimes(2);
      expect(configureParticipant).not.toHaveBeenCalled();
      expect(activateParticipant).not.toHaveBeenCalled();
    }
    expect(quarantineParticipant).not.toHaveBeenCalled();
  });

  it("resumes initial calibration when the peer direct pair is already active", async () => {
    const proposal = calibrationProposal();
    const owner = calibrationParticipantStatus("http://a");
    const peer = calibrationParticipantStatus("http://b");
    peer.participants[0].directMembership = "active";
    const configureParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const reconfigureParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const activateParticipant = vi.fn(async (
      _device: string,
      _participantId: string,
      _view?: string
    ) => ({ ok: true, participants: [] }));
    const setCommunityFallback = vi.fn(async () => ({ ok: true, participants: [] }));
    const waitForSiteConvergence = vi.fn(async () => ({ ok: true }));
    const client = {
      getParticipants: vi.fn(async (device: string) => device === "http://a" ? owner : peer),
      configureParticipant,
      reconfigureParticipant,
      activateParticipant,
      setCommunityFallback,
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration: vi.fn(async () => siteStatus()),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence,
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await approvePlatformCalibration(
      client,
      "http://a",
      "http://b",
      calibrationDescriptor("node-a"),
      calibrationDescriptor("node-b"),
      proposal,
      { prepareCommunityFallback: true }
    );

    expect(reconfigureParticipant).toHaveBeenCalledWith(
      "http://b",
      "node-a",
      expect.anything(),
      expect.anything(),
      "direct"
    );
    expect(configureParticipant).toHaveBeenCalledWith(
      "http://a",
      "node-b",
      expect.anything(),
      expect.anything(),
      "canonical"
    );
    expect(activateParticipant.mock.calls.map(([device, participantId, view]) => [
      device, participantId, view
    ])).toEqual([
      ["http://b", "node-a", "direct"],
      ["http://a", "node-b", "canonical"]
    ]);
    expect(setCommunityFallback).toHaveBeenCalledOnce();
    expect(setCommunityFallback).toHaveBeenCalledWith(
      "http://a",
      "node-b",
      true
    );
    const ownerActivationOrder = activateParticipant.mock.invocationCallOrder[1];
    const fallbackOrder = setCommunityFallback.mock.invocationCallOrder[0];
    expect(ownerActivationOrder).toBeLessThan(fallbackOrder);
    expect(waitForSiteConvergence.mock.invocationCallOrder.some(
      (order) => order > ownerActivationOrder && order < fallbackOrder
    )).toBe(true);
    expect(setCommunityFallback.mock.invocationCallOrder[0]).toBeLessThan(
      waitForSiteConvergence.mock.invocationCallOrder[
        waitForSiteConvergence.mock.invocationCallOrder.length - 1
      ]
    );
  });

  it("preserves fallback consent when retrying after activation already completed", async () => {
    const proposal = calibrationProposal();
    const reconfigureParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const activateParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const setCommunityFallback = vi.fn(async () => ({ ok: true, participants: [] }));
    const waitForSiteConvergence = vi.fn(async () => ({ ok: true }));
    const client = {
      getParticipants: vi.fn(async (device: string) => capacityParticipantStatus(
        2, device, "active", proposal
      )),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      reconfigureParticipant,
      activateParticipant,
      setCommunityFallback,
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration: vi.fn(async () => capacitySiteStatus(2, "active")),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence,
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-joining" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-joining" }))
    };

    await approvePlatformCalibration(
      client,
      "http://owner",
      "http://joining",
      calibrationDescriptor("node-owner"),
      calibrationDescriptor("node-joining"),
      proposal,
      { prepareCommunityFallback: true }
    );

    expect(activateParticipant).not.toHaveBeenCalled();
    expect(reconfigureParticipant).toHaveBeenCalledTimes(2);
    expect(setCommunityFallback).toHaveBeenCalledWith(
      "http://owner",
      "node-joining",
      true
    );
    const lastReconfigurationOrder = Math.max(
      ...reconfigureParticipant.mock.invocationCallOrder
    );
    const fallbackOrder = setCommunityFallback.mock.invocationCallOrder[0];
    expect(waitForSiteConvergence.mock.invocationCallOrder.some(
      (order) => order > lastReconfigurationOrder && order < fallbackOrder
    )).toBe(true);
    expect(waitForSiteConvergence.mock.invocationCallOrder.some(
      (order) => order > fallbackOrder
    )).toBe(true);
  });

  it("rejects stale, revoked, non-canonical, and non-direct calibration projections", async () => {
    const proposal = calibrationProposal();
    const scenarios: Array<{
      name: string;
      mutate: (
        site: PlatformSiteConfigurationStatus,
        owner: PlatformParticipantsStatus,
        peer: PlatformParticipantsStatus
      ) => void;
    }> = [
      {
        name: "owner has already promoted only its local projection",
        mutate: (_site, owner) => {
          const participant = owner.participants.find(
            (item) => item.participantId === "node-joining"
          )!;
          participant.membership = "active";
          participant.canonicalMembership = "active";
        }
      },
      {
        name: "joining peer still projects the Owner as quarantined",
        mutate: (_site, _owner, peer) => {
          const participant = peer.participants.find(
            (item) => item.participantId === "node-owner"
          )!;
          participant.membership = "quarantined";
          participant.canonicalMembership = "quarantined";
        }
      },
      {
        name: "canonical joining member has been revoked",
        mutate: (site) => {
          site.members!.find((item) => item.participantId === "node-joining")!.membership = "revoked";
        }
      },
      {
        name: "owner projection is not canonical",
        mutate: (_site, owner) => {
          owner.participants.find((item) => item.participantId === "node-joining")!.canonicalMember = false;
        }
      },
      {
        name: "joining peer no longer has a direct Owner pair",
        mutate: (_site, _owner, peer) => {
          peer.participants.find((item) => item.participantId === "node-owner")!.directPair = false;
        }
      },
      {
        name: "joining peer is missing the canonical Owner projection",
        mutate: (_site, _owner, peer) => {
          peer.participants = peer.participants.filter((item) => item.participantId !== "node-owner");
        }
      }
    ];

    for (const scenario of scenarios) {
      const site = capacitySiteStatus(7, "quarantined");
      const owner = capacityParticipantStatus(7, "http://owner", "quarantined", proposal);
      const peer = capacityParticipantStatus(7, "http://joining", "quarantined", proposal);
      scenario.mutate(site, owner, peer);
      const client = {
        getParticipants: vi.fn(async (device: string) => device === "http://owner" ? owner : peer),
        configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
        reconfigureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
        activateParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
        quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
        getSiteConfiguration: vi.fn(async () => site),
        syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
        waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
        waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-joining" })),
        waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-joining" }))
      };

      await expect(approvePlatformCalibration(
        client,
        "http://owner",
        "http://joining",
        calibrationDescriptor("node-owner"),
        calibrationDescriptor("node-joining"),
        proposal
      ), scenario.name).rejects.toThrowError(
        new PlatformCalibrationError("calibration_membership_inconsistent")
      );
      expect(client.configureParticipant, scenario.name).not.toHaveBeenCalled();
      expect(client.reconfigureParticipant, scenario.name).not.toHaveBeenCalled();
    }
  });

  it("inherits an existing Group without replacing room assignments", () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const site = {
      ...siteStatus(),
      members: [
        {
          participantId: "node-a",
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          leaderEligible: true,
          spatialProfile: {
            spaceId: "room-main",
            groupId: "legacy-bedroom-group",
            poseVersion: 1,
            calibrationVersion: 1,
            transform: {
              originXMillimeters: 0,
              originYMillimeters: 0,
              rotationCentiDegrees: 0
            }
          }
        }
      ]
    };
    const automatic = applyAutomaticCalibrationGroup(
      proposal, site, "node-a", "node-b"
    );
    expect(automatic.groupId).toBe("legacy-bedroom-group");
    expect(automatic.referenceAssignment.groupId).toBe("legacy-bedroom-group");
    expect(automatic.peerAssignment.groupId).toBe("legacy-bedroom-group");
    expect(automatic.referenceAssignment.spaceId).toBe("room-main");
    expect(automatic.peerAssignment.spaceId).toBe("room-main");
  });

  it("keeps radars in different spaces out of the same automatic Group", () => {
    const proposal = buildPlatformCalibrationProposal(
      dividedFloorplan([200, 200]),
      dividedFloorplan([260, 200]),
      [{ referenceLocalMm: [600, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    expect(proposal.spaceId).toBe("room-right");
    expect(proposal.referenceAssignment).toMatchObject({
      spaceId: "room-left",
      groupId: "room-left"
    });
    expect(proposal.peerAssignment).toMatchObject({
      spaceId: "room-right",
      groupId: "room-right"
    });

    const automatic = applyAutomaticCalibrationGroup(
      proposal,
      {
        ...siteStatus(),
        members: [
          {
            participantId: "node-a",
            participantType: "namo_node",
            trust: "official_verified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: {
              spaceId: "room-left",
              groupId: "legacy-left",
              poseVersion: 1,
              calibrationVersion: 1,
              transform: {
                originXMillimeters: 2000,
                originYMillimeters: 2000,
                rotationCentiDegrees: 0
              }
            }
          },
          {
            participantId: "node-b",
            participantType: "namo_node",
            trust: "official_verified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: {
              spaceId: "room-right",
              groupId: "legacy-right",
              poseVersion: 1,
              calibrationVersion: 1,
              transform: {
                originXMillimeters: 2600,
                originYMillimeters: 2000,
                rotationCentiDegrees: 0
              }
            }
          }
        ]
      },
      "node-a",
      "node-b"
    );
    expect(automatic.referenceAssignment.groupId).toBe("legacy-left");
    expect(automatic.peerAssignment.groupId).toBe("legacy-right");

    const contaminated = applyAutomaticCalibrationGroup(
      proposal,
      {
        ...siteStatus(),
        members: [
          {
            participantId: "node-a",
            participantType: "namo_node",
            trust: "official_verified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: {
              spaceId: "room-left",
              groupId: "shared-legacy",
              poseVersion: 1,
              calibrationVersion: 1,
              transform: { originXMillimeters: 2000, originYMillimeters: 2000, rotationCentiDegrees: 0 }
            }
          },
          {
            participantId: "node-b",
            participantType: "namo_node",
            trust: "official_verified",
            membership: "active",
            leaderEligible: true,
            spatialProfile: {
              spaceId: "room-right",
              groupId: "shared-legacy",
              poseVersion: 1,
              calibrationVersion: 1,
              transform: { originXMillimeters: 2600, originYMillimeters: 2000, rotationCentiDegrees: 0 }
            }
          }
        ]
      },
      "node-a",
      "node-b"
    );
    expect(contaminated.referenceAssignment.groupId).toBe("room-left");
    expect(contaminated.peerAssignment.groupId).toBe("room-right");
  });

  it("captures only one simultaneous target per device", () => {
    expect(capturePlatformCalibrationSample(state(100, 1200), state(80, 1210))).toEqual({
      referenceLocalMm: [100, 1200],
      peerLocalMm: [80, 1210]
    });
    expect(() => capturePlatformCalibrationSample(state(100, 1200, 2), state(80, 1210)))
      .toThrowError(new PlatformCalibrationError("calibration_requires_one_target"));
  });

  it("uses local radar targets when authoritative fusion replaces top-level targets", () => {
    const authoritative = {
      ...state(4000, 3000),
      fusion: { authoritative: true },
      local: {
        targets: [{ x: 100, y: 1200, active: true }]
      }
    } satisfies WebDeviceState;
    const peer = state(80, 1210);

    expect(capturePlatformCalibrationSample(authoritative, peer)).toEqual({
      referenceLocalMm: [100, 1200],
      peerLocalMm: [80, 1210]
    });
    expect(buildPlatformCalibrationDisplayModel(
      floorplan([100, 100]),
      floorplan([105, 100]),
      authoritative,
      peer
    ).referenceTarget).toEqual({ xPx: 110, yPx: -20 });
  });

  it("fails closed when an authoritative legacy state omits local radar targets", () => {
    const authoritative = {
      ...state(4000, 3000),
      fusion: { authoritative: true }
    } satisfies WebDeviceState;

    expect(() => capturePlatformCalibrationSample(authoritative, state(80, 1210)))
      .toThrowError(new PlatformCalibrationError("calibration_requires_one_target"));
    expect(buildPlatformCalibrationDisplayModel(
      floorplan([100, 100]),
      floorplan([105, 100]),
      authoritative,
      state(80, 1210)
    ).captureReady).toBe(false);
  });

  it("projects both live detections onto the shared floorplan without changing their source poses", () => {
    const reference = floorplan([100, 100]);
    const peer = floorplan([105, 100]);
    const model = buildPlatformCalibrationDisplayModel(
      reference,
      peer,
      state(0, 1000),
      state(0, 1000)
    );

    expect(model).toMatchObject({
      referenceRadar: { xPx: 100, yPx: 100, rotationDegrees: 0 },
      peerRadar: { xPx: 105, yPx: 100, rotationDegrees: 0 },
      referenceTarget: { xPx: 100, yPx: 0 },
      peerTarget: { xPx: 105, yPx: 0 },
      referenceTargetCount: 1,
      peerTargetCount: 1,
      targetSeparationMillimeters: 50,
      captureReady: true
    });
    expect(reference.radar.originPx).toEqual([100, 100]);
    expect(peer.radar.originPx).toEqual([105, 100]);
  });

  it("blocks visual capture readiness unless both online devices have one valid target", () => {
    const reference = floorplan([100, 100]);
    const peer = floorplan([105, 100]);
    const multipleTargets = buildPlatformCalibrationDisplayModel(
      reference,
      peer,
      state(0, 1000, 2),
      state(0, 1000)
    );
    const offlineState = { ...state(0, 1000), connected: false };
    const offline = buildPlatformCalibrationDisplayModel(
      reference,
      peer,
      offlineState,
      state(0, 1000)
    );

    expect(multipleTargets.captureReady).toBe(false);
    expect(multipleTargets.referenceTarget).toBeNull();
    expect(multipleTargets.referenceTargetCount).toBe(2);
    expect(offline.captureReady).toBe(false);
  });

  it("places the captured baseline marker in reference floorplan coordinates", () => {
    expect(calibrationSampleReferencePointPx(
      floorplan([100, 100]),
      { referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }
    )).toEqual({ xPx: 100, yPx: 0 });
  });

  it("proposes a bounded translation without rewriting either radar pose", () => {
    const reference = floorplan([100, 100]);
    const peer = floorplan([105, 100]);
    peer.radar.poseVersion = 5;
    const proposal = buildPlatformCalibrationProposal(reference, peer, [{
      referenceLocalMm: [0, 1000],
      peerLocalMm: [0, 1000]
    }], 2);

    expect(proposal.translationMillimeters).toBe(50);
    expect(proposal.rotationCorrectionDegrees).toBe(0);
    expect(proposal.referenceAssignment).toMatchObject({
      geometryVersion: 4,
      localPoseVersion: 5,
      poseVersion: 3
    });
    expect(proposal.peerAssignment).toMatchObject({
      geometryVersion: 4,
      localPoseVersion: 3,
      poseVersion: 5
    });
    expect(proposal.peerAssignment.transform).toMatchObject({
      originXMillimeters: 1000,
      originYMillimeters: 1000,
      calibrationSamples: 1
    });
    expect(peer.radar.originPx).toEqual([105, 100]);
  });

  it("uses a second separated capture to recover peer rotation", () => {
    const reference = floorplan([100, 100]);
    const peer = floorplan([100, 100], 5);
    const proposal = buildPlatformCalibrationProposal(reference, peer, [
      { referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] },
      { referenceLocalMm: [1000, 1000], peerLocalMm: [1000, 1000] }
    ], 3);

    expect(proposal.rotationCorrectionDegrees).toBe(-5);
    expect(proposal.residualMillimeters).toBe(0);
    expect(proposal.peerAssignment.transform?.rotationCentiDegrees).toBe(0);
    expect(proposal.samples).toBe(2);
  });

  it("rejects mismatched floorplans and implausible corrections", () => {
    const peer = floorplan([200, 100]);
    expect(() => buildPlatformCalibrationProposal(floorplan([100, 100]), peer, [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 1)).toThrowError(new PlatformCalibrationError("calibration_translation_too_large"));

    peer.floorplanId = "fp-other";
    expect(() => buildPlatformCalibrationProposal(floorplan([100, 100]), peer, [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 1)).toThrowError(new PlatformCalibrationError("calibration_floorplan_mismatch"));
  });

  it("accepts a shared floorplan with independent horizontal and vertical scale", () => {
    const reference = floorplan([100, 100]);
    const peer = floorplan([105, 100]);
    for (const document of [reference, peer]) {
      document.scale.heightMm = 8000;
      document.scale.mmPerPxY = 20;
    }

    const proposal = buildPlatformCalibrationProposal(reference, peer, [{
      referenceLocalMm: [0, 1000],
      peerLocalMm: [0, 1000]
    }], 1);

    expect(proposal.translationMillimeters).toBe(50);
    expect(proposal.residualMillimeters).toBe(0);
  });

  it("rejects invalid or divergent shared floorplan scale data", () => {
    const invalid = floorplan([100, 100]);
    invalid.scale.mmPerPxX = 0;
    expect(() => buildPlatformCalibrationProposal(invalid, floorplan([105, 100]), [{
      referenceLocalMm: [0, 1000],
      peerLocalMm: [0, 1000]
    }], 1)).toThrowError(new PlatformCalibrationError("calibration_scale_invalid"));

    const reference = floorplan([100, 100]);
    const peer = floorplan([105, 100]);
    peer.scale.widthMm = 5500;
    peer.scale.mmPerPxX = 11;
    expect(() => buildPlatformCalibrationProposal(reference, peer, [{
      referenceLocalMm: [0, 1000],
      peerLocalMm: [0, 1000]
    }], 1)).toThrowError(new PlatformCalibrationError("calibration_floorplan_mismatch"));
  });

  it("keeps the staged state and preserves the owner activation error", async () => {
    const referenceFloorplan = floorplan([100, 100]);
    const peerFloorplan = floorplan([105, 100]);
    peerFloorplan.radar.poseVersion = 5;
    const proposal = buildPlatformCalibrationProposal(referenceFloorplan, peerFloorplan, [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 2);
    const configureParticipant = vi.fn(async (_device: string, _participantId: string) => ({
      ok: true,
      participants: []
    }));
    const activateParticipant = vi.fn()
      .mockResolvedValueOnce({ ok: true, participants: [] })
      .mockRejectedValueOnce(new Error("activation_failed"));
    const quarantineParticipant = vi.fn(async () => ({ ok: true, participants: [] }));
    const getSiteConfiguration = vi.fn(async () => siteStatus());
    const syncSiteConfiguration = vi.fn(async () => ({ ok: true }));
    const waitForSiteConvergence = vi.fn(async () => ({ ok: true }));
    const waitForParticipantReplica = vi.fn(async () => ({ participantId: "node-b" }));
    const waitForParticipantEndpoint = vi.fn(async () => ({ participantId: "node-b" }));
    const descriptor = (participantId: string) => ({
      protocolVersion: 1,
      participantId,
      participantType: "namo_node",
      product: "namo-spatial-sensor",
      softwareVersion: "test",
      capabilities: {
        pointTracks: true, velocity: true, positionUncertainty: false,
        range: true, presence: true, motion: true, zonePresence: false,
        bridge: false, maxTargets: 3
      }
    });

    await expect(approvePlatformCalibration(
      {
        ...initialCalibrationClientMethods(),
        configureParticipant,
        activateParticipant,
        quarantineParticipant,
        getSiteConfiguration,
        syncSiteConfiguration,
        waitForSiteConvergence,
        waitForParticipantReplica,
        waitForParticipantEndpoint
      },
      "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).rejects.toThrow("activation_failed");
    expect(configureParticipant).toHaveBeenCalledTimes(2);
    expect(configureParticipant.mock.calls.map(([device, participantId]) => [device, participantId])).toEqual([
      ["http://b", "node-a"],
      ["http://a", "node-b"]
    ]);
    expect(syncSiteConfiguration).toHaveBeenCalledTimes(2);
    for (const call of syncSiteConfiguration.mock.calls) {
      expect(call).toEqual(["http://b", "node-a"]);
    }
    expect(waitForSiteConvergence).toHaveBeenCalledTimes(2);
    expect(waitForParticipantEndpoint).toHaveBeenCalledTimes(2);
    expect(waitForParticipantReplica).toHaveBeenCalledTimes(4);
    expect(waitForParticipantReplica.mock.calls.slice(0, 2)).toEqual([
      ["http://a", "node-b", {
        siteConfigurationGeneration: 4,
        siteConfigurationAuthorityEpoch: 1,
        siteConfigurationDigest: "0123456789abcdef",
        floorplanGeometryVersion: 4,
        floorplanConfigHash: 101,
        floorplanImageHash: 202,
        radarPoseVersion: 5
      }],
      ["http://b", "node-a", {
        siteConfigurationGeneration: 4,
        siteConfigurationAuthorityEpoch: 1,
        siteConfigurationDigest: "0123456789abcdef",
        floorplanGeometryVersion: 4,
        floorplanConfigHash: 101,
        floorplanImageHash: 202,
        radarPoseVersion: 3
      }]
    ]);
    expect(activateParticipant).toHaveBeenCalledTimes(2);
    expect(activateParticipant.mock.calls.map(([device, participantId, view]) => [
      device, participantId, view
    ])).toEqual([
      ["http://b", "node-a", "direct"],
      ["http://a", "node-b", "canonical"]
    ]);
    expect(quarantineParticipant).not.toHaveBeenCalled();
  });

  it("reconfigures an active pair without activation or quarantine", async () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const descriptor = (participantId: string) => ({
      protocolVersion: 1,
      participantId,
      participantType: "namo_node",
      product: "namo-spatial-sensor",
      softwareVersion: "test",
      capabilities: calibrationCapabilities
    });
    const ownerBaseline = { ...proposal.peerAssignment, calibrationVersion: 1 };
    const peerBaseline = { ...proposal.referenceAssignment, calibrationVersion: 1 };
    const reconfigureParticipant = vi.fn(async (
      _device: string,
      _participantId: string,
      _capabilities: PlatformParticipantCapabilities,
      _assignment: PlatformParticipantAssignment
    ) => ({ ok: true, participants: [] }));
    const client = {
      getParticipants: vi.fn(async (device: string) => calibrationParticipantStatus(
        device,
        "active",
        device === "http://a" ? ownerBaseline : peerBaseline
      )),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      reconfigureParticipant,
      activateParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration: vi.fn(async () => ({
        ...siteStatus(),
        activeMemberCount: 2,
        members: siteStatus().members.map((member) => ({ ...member, membership: "active" }))
      })),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await approvePlatformCalibration(
      client,
      "http://a",
      "http://b",
      descriptor("node-a"),
      descriptor("node-b"),
      proposal
    );

    expect(reconfigureParticipant.mock.calls.map(([device, participantId]) => [device, participantId])).toEqual([
      ["http://a", "node-b"],
      ["http://b", "node-a"]
    ]);
    expect(client.configureParticipant).not.toHaveBeenCalled();
    expect(client.activateParticipant).not.toHaveBeenCalled();
    expect(client.quarantineParticipant).not.toHaveBeenCalled();
  });

  it("restores both active assignments when recalibration fails halfway", async () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const descriptor = (participantId: string) => ({
      protocolVersion: 1,
      participantId,
      participantType: "namo_node",
      product: "namo-spatial-sensor",
      softwareVersion: "test",
      capabilities: calibrationCapabilities
    });
    const ownerBaseline = { ...proposal.peerAssignment, calibrationVersion: 1 };
    const peerBaseline = { ...proposal.referenceAssignment, calibrationVersion: 1 };
    const writtenVersions: number[] = [];
    const reconfigureParticipant = vi.fn(async (
      _device: string,
      _participantId: string,
      _capabilities: PlatformParticipantCapabilities,
      assignment: PlatformParticipantAssignment
    ) => {
      writtenVersions.push(assignment.calibrationVersion);
      if (writtenVersions.length === 2) throw new Error("owner_write_failed");
      return { ok: true, participants: [] };
    });
    const client = {
      getParticipants: vi.fn(async (device: string) => calibrationParticipantStatus(
        device,
        "active",
        device === "http://a" ? ownerBaseline : peerBaseline
      )),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      reconfigureParticipant,
      activateParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration: vi.fn(async () => ({
        ...siteStatus(),
        activeMemberCount: 2,
        members: siteStatus().members.map((member) => ({ ...member, membership: "active" }))
      })),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await expect(approvePlatformCalibration(
      client,
      "http://a",
      "http://b",
      descriptor("node-a"),
      descriptor("node-b"),
      proposal
    )).rejects.toThrow("owner_write_failed");

    expect(writtenVersions).toEqual([2, 2, 3, 3]);
    expect(client.activateParticipant).not.toHaveBeenCalled();
    expect(client.quarantineParticipant).not.toHaveBeenCalled();
  });

  it("rejects a mirrored assignment whose Node pose contract is inconsistent", async () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    proposal.referenceAssignment.localPoseVersion = 99;
    const getSiteConfiguration = vi.fn(async () => siteStatus());
    const descriptor = (participantId: string) => ({
      protocolVersion: 1,
      participantId,
      participantType: "namo_node",
      product: "namo-spatial-sensor",
      softwareVersion: "test",
      capabilities: {
        pointTracks: true, velocity: true, positionUncertainty: false,
        range: true, presence: true, motion: true, zonePresence: false,
        bridge: false, maxTargets: 3
      }
    });

    await expect(approvePlatformCalibration(
      {
        ...initialCalibrationClientMethods(),
        configureParticipant: vi.fn(),
        activateParticipant: vi.fn(),
        quarantineParticipant: vi.fn(),
        getSiteConfiguration,
        syncSiteConfiguration: vi.fn(),
        waitForSiteConvergence: vi.fn(),
        waitForParticipantReplica: vi.fn(),
        waitForParticipantEndpoint: vi.fn()
      },
      "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).rejects.toThrowError(new PlatformCalibrationError("calibration_pose_contract_invalid"));
    expect(getSiteConfiguration).not.toHaveBeenCalled();
  });

  it("does not add quarantine revisions after an uncertain initial activation", async () => {
    const proposal = buildPlatformCalibrationProposal(floorplan([100, 100]), floorplan([105, 100]), [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 2);
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const descriptor = (participantId: string) => ({
      protocolVersion: 1, participantId, participantType: "namo_node",
      product: "namo-spatial-sensor", softwareVersion: "test", capabilities
    });
    const attempts = new Map<string, number>();
    const quarantineParticipant = vi.fn(async (device: string, participantId: string) => {
      const count = (attempts.get(device) ?? 0) + 1;
      attempts.set(device, count);
      if (device === "http://a" && count === 1) {
        throw new TypeError("response lost after quarantine");
      }
      return {
        ok: true,
        participants: [{
          participantId,
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "quarantined",
          credential: { provisioned: true, enabled: true, keyVersion: 1 }
        }]
      };
    });
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      activateParticipant: vi.fn()
        .mockResolvedValueOnce({ ok: true, participants: [] })
        .mockRejectedValueOnce(new Error("activation_failed")),
      quarantineParticipant,
      getSiteConfiguration: vi.fn(async () => siteStatus()),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await expect(approvePlatformCalibration(
      client, "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).rejects.toThrow("activation_failed");
    expect(attempts.size).toBe(0);
    expect(quarantineParticipant).not.toHaveBeenCalled();
  });

  it("does not replace the primary activation failure with a rollback error", async () => {
    const proposal = buildPlatformCalibrationProposal(floorplan([100, 100]), floorplan([105, 100]), [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 2);
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const descriptor = (participantId: string) => ({
      protocolVersion: 1, participantId, participantType: "namo_node",
      product: "namo-spatial-sensor", softwareVersion: "test", capabilities
    });
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      activateParticipant: vi.fn()
        .mockResolvedValueOnce({ ok: true, participants: [] })
        .mockRejectedValueOnce(new Error("activation_failed")),
      quarantineParticipant: vi.fn(async (_device: string, participantId: string) => ({
        ok: true,
        participants: [{
          participantId,
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "active",
          credential: { provisioned: true, enabled: true, keyVersion: 1 }
        }]
      })),
      getSiteConfiguration: vi.fn(async () => siteStatus()),
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await expect(approvePlatformCalibration(
      client, "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).rejects.toThrow("activation_failed");
    expect(client.quarantineParticipant).not.toHaveBeenCalled();
  });

  it("keeps configure staged and waits for one canonical activation revision", async () => {
    const proposal = buildPlatformCalibrationProposal(floorplan([100, 100]), floorplan([105, 100]), [{
      referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000]
    }], 2);
    const calls: string[] = [];
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const descriptor = (participantId: string) => ({
      protocolVersion: 1, participantId, participantType: "namo_node",
      product: "namo-spatial-sensor", softwareVersion: "test", capabilities
    });
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async (_device: string, participantId: string) => {
        calls.push(`configure:${participantId}`);
        return { ok: true, participants: [] };
      }),
      syncSiteConfiguration: vi.fn(async (_device: string, ownerNodeId?: string) => {
        expect(ownerNodeId).toBe("node-a");
        calls.push("sync");
        return { ok: true };
      }),
      getSiteConfiguration: vi.fn()
        .mockImplementationOnce(async () => {
          calls.push("site:3");
          return siteStatus(3);
        })
        .mockImplementationOnce(async () => {
          calls.push("site:4");
          return siteStatus(4);
        })
        .mockImplementationOnce(async () => {
          calls.push("site:4:owner-ready");
          return siteStatus(4);
        }),
      waitForSiteConvergence: vi.fn(async (_device: string, expected: { generation: number }) => {
        calls.push(`converged:${expected.generation}`);
        return {};
      }),
      waitForParticipantReplica: vi.fn(async (
        device: string,
        _participantId: string,
        expected: { siteConfigurationGeneration: number }
      ) => {
        calls.push(`replica:${device}:${expected.siteConfigurationGeneration}`);
        return {};
      }),
      waitForParticipantEndpoint: vi.fn(async () => {
        calls.push("endpoint");
        return {};
      }),
      activateParticipant: vi.fn(async (_device: string, participantId: string) => {
        calls.push(`activate:${participantId}`);
        return { ok: true, participants: [] };
      }),
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] }))
    };

    await approvePlatformCalibration(
      client, "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    );
    expect(calls).toEqual([
      "site:3",
      "endpoint", "endpoint",
      "sync", "converged:3",
      "replica:http://a:3", "replica:http://b:3",
      "configure:node-a", "configure:node-b",
      "sync", "converged:3",
      "replica:http://a:3", "replica:http://b:3",
      "activate:node-a", "activate:node-b",
      "site:4", "sync", "converged:4",
      "replica:http://a:4", "replica:http://b:4",
      "site:4:owner-ready"
    ]);
    expect(client.quarantineParticipant).not.toHaveBeenCalled();
  });

  it("nudges existing active peers after activation without making their sync authoritative", async () => {
    const proposal = calibrationProposal();
    const syncSiteConfiguration = vi.fn(async (device: string) => {
      if (device === "192.168.1.31") throw new Error("offline");
      return { ok: true };
    });
    const ownerParticipants = {
      ok: true,
      participants: [
        {
          participantId: "node-b",
          participantType: "namo_node",
          trust: "community_unverified",
          membership: "active",
          canonicalMembership: "active",
          credential: { provisioned: true, enabled: true, keyVersion: 1 },
          endpoint: {
            address: "192.168.1.20", port: 49191, authenticated: true,
            online: true, lastSeenAgeMs: 10
          }
        },
        ...[30, 31].map((suffix) => ({
          participantId: `node-existing-${suffix}`,
          participantType: "namo_node",
          trust: "official_verified",
          membership: "active",
          canonicalMembership: "active",
          credential: { provisioned: true, enabled: true, keyVersion: 1 },
          endpoint: {
            address: `192.168.1.${suffix}`, port: 49191, authenticated: true,
            online: true, lastSeenAgeMs: 10
          }
        }))
      ]
    };
    let ownerActivation = false;
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      activateParticipant: vi.fn(async (device: string) => {
        if (device === "http://a") {
          ownerActivation = true;
          return ownerParticipants;
        }
        return { ok: true, participants: [] };
      }),
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration: vi.fn(async () => siteStatus(ownerActivation ? 5 : 4)),
      syncSiteConfiguration,
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await expect(approvePlatformCalibration(
      client,
      "http://a",
      "http://b",
      calibrationDescriptor("node-a"),
      calibrationDescriptor("node-b"),
      proposal
    )).resolves.toBeUndefined();

    expect(syncSiteConfiguration).toHaveBeenCalledWith("192.168.1.30", "node-a");
    expect(syncSiteConfiguration).toHaveBeenCalledWith("192.168.1.31", "node-a");
    expect(syncSiteConfiguration).not.toHaveBeenCalledWith("192.168.1.20", "node-a");
  });

  it("waits for owner continuity and retries a raced owner activation once", async () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const descriptor = (participantId: string) => ({
      protocolVersion: 1, participantId, participantType: "namo_node",
      product: "namo-spatial-sensor", softwareVersion: "test", capabilities
    });
    const continuityError = new PlatformPairingError(
      "site_configuration_continuity_unavailable",
      409,
      "site_configuration_continuity_unavailable"
    );
    let ownerActivationAttempts = 0;
    const activateParticipant = vi.fn(async (device: string) => {
      if (device === "http://a" && ownerActivationAttempts++ === 0) {
        throw continuityError;
      }
      return { ok: true, participants: [] };
    });
    const getSiteConfiguration = vi.fn()
      .mockResolvedValueOnce({ ...siteStatus(3), sharedWritesAllowed: false })
      .mockResolvedValueOnce({ ...siteStatus(3), sharedWritesAllowed: true })
      .mockResolvedValue(siteStatus(4));
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      activateParticipant,
      quarantineParticipant: vi.fn(async () => ({ ok: true, participants: [] })),
      getSiteConfiguration,
      syncSiteConfiguration: vi.fn(async () => ({ ok: true })),
      waitForSiteConvergence: vi.fn(async () => ({ ok: true })),
      waitForParticipantReplica: vi.fn(async () => ({ participantId: "node-b" })),
      waitForParticipantEndpoint: vi.fn(async () => ({ participantId: "node-b" }))
    };

    await approvePlatformCalibration(
      client,
      "http://a",
      "http://b",
      descriptor("node-a"),
      descriptor("node-b"),
      proposal
    );

    expect(activateParticipant).toHaveBeenCalledTimes(3);
    expect(client.quarantineParticipant).not.toHaveBeenCalled();
  });

  it("keeps calibration versions independent for each Site member", () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const site = {
      ...siteStatus(),
      members: [{
        participantId: "node-a",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        leaderEligible: true,
        spatialProfile: {
          spaceId: "room-main",
          groupId: "room-main",
          poseVersion: 3,
          calibrationVersion: 2,
          transform: {
            originXMillimeters: 1000,
            originYMillimeters: 1000,
            rotationCentiDegrees: 0
          }
        }
      }]
    };

    const rebased = rebaseCalibrationVersion(proposal, site, "node-a", "node-b");
    expect(rebased.referenceAssignment.calibrationVersion).toBe(2);
    expect(rebased.peerAssignment.calibrationVersion).toBe(2);
  });

  it("does not bump an unchanged hub when another peer is calibrated", () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      1
    );
    const reference = proposal.referenceAssignment;
    const site = {
      ...siteStatus(),
      members: [{
        participantId: "node-hub",
        participantType: "namo_node",
        trust: "official_verified",
        membership: "active",
        leaderEligible: true,
        spatialProfile: {
          spaceId: reference.spaceId,
          groupId: reference.groupId,
          poseVersion: reference.poseVersion,
          calibrationVersion: 4,
          transform: {
            originXMillimeters: reference.transform!.originXMillimeters,
            originYMillimeters: reference.transform!.originYMillimeters,
            rotationCentiDegrees: reference.transform!.rotationCentiDegrees
          }
        }
      }]
    };

    const rebased = rebaseCalibrationVersion(
      proposal,
      site,
      "node-hub",
      "node-new-peer"
    );
    expect(rebased.referenceAssignment.calibrationVersion).toBe(4);
    expect(rebased.peerAssignment.calibrationVersion).toBe(1);
  });

  it("recovers a quarantined calibration transaction without a Group directory route", async () => {
    const proposal = buildPlatformCalibrationProposal(
      floorplan([100, 100]),
      floorplan([105, 100]),
      [{ referenceLocalMm: [0, 1000], peerLocalMm: [0, 1000] }],
      2
    );
    const capabilities = {
      pointTracks: true, velocity: true, positionUncertainty: false,
      range: true, presence: true, motion: true, zonePresence: false,
      bridge: false, maxTargets: 3
    };
    const descriptor = (participantId: string) => ({
      protocolVersion: 1, participantId, participantType: "namo_node",
      product: "namo-spatial-sensor", softwareVersion: "test", capabilities
    });
    let generation = 3;
    let canonicalCalibrationVersion = 1;
    let ownerViewMembership = "quarantined";
    let peerViewMembership = "quarantined";
    let failConfiguredSync = true;
    let failConfiguredConvergence = true;
    const configuredVersions: number[] = [];
    const syncOwners: Array<string | undefined> = [];
    const currentSite = () => ({
      ...siteStatus(generation),
      members: [
        {
          participantId: "node-a", participantType: "namo_node",
          trust: "official_verified", membership: "active", leaderEligible: true,
          spatialProfile: {
            spaceId: "room-main", groupId: "room-main", poseVersion: 3,
            calibrationVersion: canonicalCalibrationVersion,
            transform: { originXMillimeters: 1000, originYMillimeters: 1000, rotationCentiDegrees: 0 }
          }
        },
        {
          participantId: "node-b", participantType: "namo_node",
          trust: "community_unverified", membership: ownerViewMembership, leaderEligible: false,
          spatialProfile: {
            spaceId: "room-main", groupId: "room-main", poseVersion: 3,
            calibrationVersion: canonicalCalibrationVersion,
            transform: { originXMillimeters: 1050, originYMillimeters: 1000, rotationCentiDegrees: 0 }
          }
        }
      ]
    });
    const participantStatus = (participantId: string, membership: string) => ({
      ok: true,
      participants: [{
        participantId,
        participantType: "namo_node",
        trust: "community_unverified",
        membership,
        credential: { provisioned: true, enabled: true, keyVersion: 1 }
      }]
    });
    const client = {
      ...initialCalibrationClientMethods(),
      configureParticipant: vi.fn(async (
        device: string,
        participantId: string,
        _capabilities: unknown,
        assignment: { calibrationVersion: number }
      ) => {
        configuredVersions.push(assignment.calibrationVersion);
        if (device === "http://a") {
          canonicalCalibrationVersion = assignment.calibrationVersion;
          ownerViewMembership = "quarantined";
          generation += 1;
        } else {
          peerViewMembership = "quarantined";
        }
        return participantStatus(participantId, "quarantined");
      }),
      activateParticipant: vi.fn(async (device: string, participantId: string) => {
        if (device === "http://a") {
          ownerViewMembership = "active";
          generation += 1;
        } else {
          peerViewMembership = "active";
        }
        return participantStatus(participantId, "active");
      }),
      quarantineParticipant: vi.fn(async (device: string, participantId: string) => {
        if (device === "http://a") {
          ownerViewMembership = "quarantined";
          generation += 1;
        } else {
          peerViewMembership = "quarantined";
        }
        return participantStatus(participantId, "quarantined");
      }),
      getSiteConfiguration: vi.fn(async () => currentSite()),
      syncSiteConfiguration: vi.fn(async (_device: string, ownerNodeId?: string) => {
        syncOwners.push(ownerNodeId);
        if (failConfiguredSync && canonicalCalibrationVersion === 2) {
          failConfiguredSync = false;
          throw new PlatformPairingError("owner_offline", 408, "owner_offline");
        }
        return currentSite();
      }),
      waitForSiteConvergence: vi.fn(async () => {
        if (canonicalCalibrationVersion === 2 && failConfiguredConvergence) {
          failConfiguredConvergence = false;
          throw new PlatformPairingError("owner_offline", 408, "owner_offline");
        }
        return currentSite();
      }),
      waitForParticipantReplica: vi.fn(async () => participantStatus("node-b", "quarantined").participants[0]),
      waitForParticipantEndpoint: vi.fn(async () => participantStatus("node-b", "quarantined").participants[0])
    };

    await expect(approvePlatformCalibration(
      client, "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).rejects.toThrow("owner_offline");
    expect(ownerViewMembership).toBe("quarantined");
    expect(peerViewMembership).toBe("quarantined");

    await expect(approvePlatformCalibration(
      client, "http://a", "http://b", descriptor("node-a"), descriptor("node-b"), proposal
    )).resolves.toBeUndefined();
    expect(configuredVersions).toEqual([1, 2, 2, 3]);
    expect(syncOwners.every((owner) => owner === "node-a")).toBe(true);
    expect(ownerViewMembership).toBe("active");
    expect(peerViewMembership).toBe("active");
  });
});
