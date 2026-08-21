import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../core/floorplan/floorplan-storage";
import { radarPointToFloorplanPx } from "../core/floorplan/radar-floorplan-transform";
import type { WebDeviceState } from "../core/types";
import type {
  PlatformGroupsStatus,
  PlatformSiteConfigurationStatus
} from "./api/platform-pairing";
import {
  platformFloorplanTargets,
  platformFusionSummary,
  platformPeerRadarMarkers
} from "./platform-radar-display";

const floorplan: FloorplanStorageDocument = {
  version: 2,
  floorplanId: "fp-test",
  geometryVersion: 4,
  image: { path: "/floorplan.webp", mime: "image/webp", width: 400, height: 600 },
  scale: {
    widthMm: 4000,
    heightMm: 6000,
    outerBoundsPx: [0, 0, 400, 600],
    mmPerPxX: 10,
    mmPerPxY: 10
  },
  radar: { originPx: [50, 500], rotationDeg: 0, scale: 1, poseVersion: 3 },
  rooms: [],
  occlusion: { ignoredEdges: [] }
};

const site: PlatformSiteConfigurationStatus = {
  ok: true,
  schemaVersion: 1,
  siteId: "site-test",
  generation: 5,
  rosterRevision: 3,
  rosterCommitted: true,
  configurationOwnerNodeId: "node-a",
  localOwner: true,
  memberCount: 2,
  activeMemberCount: 2,
  members: [
    {
      participantId: "node-a",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      leaderEligible: true,
      spatialProfile: {
        poseVersion: 3,
        calibrationVersion: 2,
        transform: {
          originXMillimeters: 500,
          originYMillimeters: 5000,
          rotationCentiDegrees: 0
        }
      }
    },
    {
      participantId: "node-b",
      participantType: "namo_node",
      trust: "community_unverified",
      membership: "active",
      leaderEligible: true,
      spatialProfile: {
        poseVersion: 2,
        calibrationVersion: 2,
        transform: {
          originXMillimeters: 3200,
          originYMillimeters: 800,
          rotationCentiDegrees: 9000
        }
      }
    }
  ],
  floorplan: {
    committed: true,
    floorplanId: "fp-test",
    geometryVersion: 4,
    configHash: 1,
    imageHash: 2
  },
  replication: null
};

describe("platformPeerRadarMarkers", () => {
  it("maps only active peer radar transforms into the shared floorplan", () => {
    expect(platformPeerRadarMarkers(site, floorplan, "node-a")).toEqual([
      {
        participantId: "node-b",
        xPx: 320,
        yPx: 80,
        rotationDegrees: 90
      }
    ]);
  });

  it("rejects stale geometry rather than drawing a radar in the wrong floorplan", () => {
    expect(platformPeerRadarMarkers(
      {
        ...site,
        floorplan: { ...site.floorplan, geometryVersion: 5 }
      },
      floorplan,
      "node-a"
    )).toEqual([]);
  });
});

describe("platformFloorplanTargets", () => {
  const fusedState: WebDeviceState = {
    connected: true,
    targets: [],
    updatedAt: 100,
    fusion: { buildEnabled: true, authoritative: true, state: "active" }
  };

  it("projects a fused local target through the canonical spatial profile", () => {
    const correctedSite: PlatformSiteConfigurationStatus = {
      ...site,
      members: site.members?.map((member) => member.participantId === "node-a"
        ? {
            ...member,
            spatialProfile: {
              ...member.spatialProfile!,
              transform: {
                originXMillimeters: 1000,
                originYMillimeters: 5000,
                rotationCentiDegrees: 0
              }
            }
          }
        : member)
    };
    const targets = [{
      id: "fused-1",
      name: "T1",
      color: "#fff",
      x: 0,
      y: 1000,
      active: true,
      fused: true,
      sourceCount: 2
    }];

    expect(platformFloorplanTargets(
      fusedState,
      targets,
      correctedSite,
      floorplan,
      "node-a"
    )).toEqual([{ ...targets[0], x: 500, y: 1000 }]);
  });

  it("preserves local targets when fusion is not authoritative", () => {
    const targets = [{
      id: "local-1",
      name: "T1",
      color: "#fff",
      x: 200,
      y: 1200,
      active: true
    }];
    expect(platformFloorplanTargets(
      { ...fusedState, fusion: { buildEnabled: true, authoritative: false, state: "degraded" } },
      targets,
      site,
      floorplan,
      "node-a"
    )).toBe(targets);
  });

  it("places different calibrated local coordinates on the same floorplan point", () => {
    const targetA = {
      id: "fused-1",
      name: "T1",
      color: "#fff",
      x: 1000,
      y: 1000,
      active: true,
      fused: true
    };
    const targetB = { ...targetA, x: 0, y: -1000 };
    const floorplanA = {
      ...floorplan,
      radar: { ...floorplan.radar, originPx: [150, 500] as [number, number], rotationDeg: 0 }
    };
    const floorplanB = {
      ...floorplan,
      radar: { ...floorplan.radar, originPx: [300, 400] as [number, number], rotationDeg: 90 }
    };
    const siteFor = (
      nodeId: string,
      originXMillimeters: number,
      originYMillimeters: number,
      rotationCentiDegrees: number
    ): PlatformSiteConfigurationStatus => ({
      ...site,
      members: [{
        ...site.members![0],
        participantId: nodeId,
        spatialProfile: {
          ...site.members![0].spatialProfile!,
          transform: { originXMillimeters, originYMillimeters, rotationCentiDegrees }
        }
      }]
    });
    const displayPoint = (
      target: typeof targetA,
      localFloorplan: FloorplanStorageDocument,
      localSite: PlatformSiteConfigurationStatus,
      nodeId: string
    ) => {
      const [projected] = platformFloorplanTargets(
        fusedState,
        [target],
        localSite,
        localFloorplan,
        nodeId
      );
      return radarPointToFloorplanPx([projected.x, projected.y], {
        placement: {
          originX: localFloorplan.radar.originPx[0],
          originY: localFloorplan.radar.originPx[1],
          rotation: localFloorplan.radar.rotationDeg
        },
        scale: localFloorplan.scale,
        scaleFactor: localFloorplan.radar.scale
      });
    };

    expect(displayPoint(
      targetA,
      floorplanA,
      siteFor("node-a", 1000, 5000, 0),
      "node-a"
    )).toEqual({ x: 200, y: 400 });
    expect(displayPoint(
      targetB,
      floorplanB,
      siteFor("node-b", 3000, 4000, 9000),
      "node-b"
    )).toEqual({ x: 200, y: 400 });
  });

  it("hides authoritative markers when the canonical local profile is unavailable", () => {
    expect(platformFloorplanTargets(
      fusedState,
      [{ id: "fused-1", name: "T1", color: "#fff", x: 0, y: 1000, active: true }],
      { ...site, members: site.members?.filter((member) => member.participantId !== "node-a") },
      floorplan,
      "node-a"
    )).toEqual([]);
  });
});

describe("platform fusion display", () => {
  const groups: PlatformGroupsStatus = {
    ok: true,
    supported: true,
    site: {
      leaderNodeId: "node-a",
      localLeader: false,
      participantsOnline: 2,
      participantsExpected: 2,
      quorumRequired: 2,
      quorumAvailable: true,
      health: "healthy"
    },
    groups: [{
      groupId: "group-a",
      configured: true,
      term: 4,
      leaderNodeId: "node-a",
      localLeader: false,
      assignmentRevision: 4,
      canonicalAssignment: true,
      credentialsComplete: true,
      leaseRemainingMs: 5000,
      participantsOnline: 2,
      participantsExpected: 2,
      quorumRequired: 2,
      quorumAvailable: true,
      health: "healthy"
    }],
    productionFusion: {
      buildEnabled: true,
      authoritative: false,
      state: "not_group_leader",
      sourceSequence: 0,
      targetCount: 0
    }
  };
  it("shows a sequentially upgraded follower from the platform snapshot even when its local state is degraded", () => {
    expect(platformFusionSummary(
      {
        connected: true,
        targets: [],
        updatedAt: 100,
        fusion: {
          buildEnabled: true,
          authoritative: false,
          state: "degraded"
        }
      },
      groups,
      { ...site, localOwner: false }
    )).toEqual({
      visible: true,
      sensorCount: 2,
      state: "sending"
    });
  });

  it("keeps the summary hidden until canonical profiles exist", () => {
    expect(platformFusionSummary(
      {
        connected: true,
        targets: [],
        updatedAt: 100,
        fusion: {
          buildEnabled: true,
          authoritative: false,
          state: "degraded"
        }
      },
      groups,
      {
        ...site,
        members: site.members?.map((member) => ({
          ...member,
          spatialProfile: undefined
        }))
      }
    )).toEqual({
      visible: false,
      sensorCount: 0,
      state: "sending"
    });
  });

  it("shows a received authoritative snapshot from the local state API", () => {
    const localState: WebDeviceState = {
      connected: true,
      targets: [
        {
          id: "fused-1",
          name: "T1",
          color: "#fff",
          x: 4700,
          y: 4100,
          rawX: 4600,
          rawY: 4000,
          active: true,
          fused: true,
          sourceCount: 2
        }
      ],
      updatedAt: 200,
      presence: true,
      motion: true,
      targetCount: 1,
      movingTargetCount: 1,
      stillTargetCount: 0,
      fusion: { buildEnabled: true, authoritative: false, state: "healthy" },
      automation: {
        schemaVersion: 1,
        complete: true,
        health: "healthy",
        available: true,
        siteId: "site-a",
        spaceId: "room-a",
        groupId: "group-a",
        leaderNodeId: "node-a",
        bootId: "boot-a",
        source: "group_leader_snapshot",
        groupTerm: 4,
        sequence: 8,
        sourceSequence: 12,
        ageMs: 100,
        presence: true,
        motion: true,
        targetCount: 1,
        zones: []
      }
    };
    expect(platformFusionSummary(
      localState,
      groups,
      site
    )).toEqual({
      visible: true,
      sensorCount: 2,
      state: "active"
    });
  });

  it("does not show stale fusion authority when automation is unavailable", () => {
    expect(platformFusionSummary(
      {
        connected: true,
        targets: [],
        updatedAt: 300,
        fusion: { buildEnabled: true, authoritative: true, state: "active" },
        automation: {
          schemaVersion: 1,
          complete: true,
          health: "unavailable",
          available: false,
          presence: null,
          motion: null,
          targetCount: null,
          zones: []
        }
      },
      groups,
      site
    )).toEqual({
      visible: true,
      sensorCount: 2,
      state: "sending"
    });
  });

  it("does not call a site follower a fusion sender without a configured local Group", () => {
    expect(platformFusionSummary(
      {
        connected: true,
        targets: [],
        updatedAt: 400,
        fusion: { buildEnabled: true, authoritative: false, state: "degraded" }
      },
      {
        ...groups,
        groups: [{
          ...groups.groups![0],
          groupId: null,
          configured: false,
          leaderNodeId: "",
          localLeader: true,
          participantsOnline: 1,
          participantsExpected: 1,
          health: "standalone"
        }]
      },
      site
    )).toEqual({
      visible: true,
      sensorCount: 2,
      state: "fallback"
    });
  });
});
