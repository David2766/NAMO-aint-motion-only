import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
import {
  canonicalLocalRadarPlacement,
  floorplanMutationDecision,
  floorplanCoordinateSize,
  floorplanReplacementRelocations,
  runFloorplanReplacementAction
} from "./floorplan-replacement";

function document(): FloorplanStorageDocument {
  return {
    version: 2,
    floorplanId: "fp-new",
    geometryVersion: 1,
    image: { path: "/floorplan.webp", mime: "image/webp", width: 1000, height: 600 },
    scale: {
      widthMm: 8000,
      heightMm: 4800,
      outerBoundsPx: [100, 50, 800, 480],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: { originPx: [500, 300], rotationDeg: 0, scale: 1, poseVersion: 1 },
    rooms: [],
    occlusion: { ignoredEdges: [] }
  };
}

function site(): PlatformSiteConfigurationStatus {
  return {
    ok: true,
    schemaVersion: 1,
    siteId: "site-home",
    generation: 7,
    rosterRevision: 4,
    rosterCommitted: true,
    configurationOwnerNodeId: "node-a",
    localOwner: true,
    memberCount: 3,
    activeMemberCount: 3,
    floorplan: {
      committed: true,
      floorplanId: "fp-old",
      geometryVersion: 9,
      configHash: 1,
      imageHash: 2
    },
    members: [
      member("node-a", 1200, 800),
      member("node-b", 11000, 1000),
      member("node-c", -1, 7000)
    ],
    replication: null
  };
}

function member(participantId: string, x: number, y: number) {
  return {
    participantId,
    participantType: "namo_node",
    trust: "official_verified",
    membership: "active",
    leaderEligible: true,
    spatialProfile: {
      spaceId: "room-main",
      groupId: "group-main",
      poseVersion: 3,
      calibrationVersion: 5,
      transform: {
        originXMillimeters: x,
        originYMillimeters: y,
        rotationCentiDegrees: 9000
      }
    }
  };
}

describe("floorplan replacement", () => {
  it("keeps replacement and deletion authority decisions in one policy", () => {
    const follower = site();
    follower.localOwner = false;
    expect(floorplanMutationDecision(follower, "node-a", "replace")).toBe(
      "owner_required"
    );
    expect(floorplanMutationDecision(follower, "node-a", "delete")).toBe(
      "owner_required"
    );

    const connectedOwner = site();
    expect(floorplanMutationDecision(connectedOwner, "node-a", "replace")).toBe(
      "allowed"
    );
    expect(floorplanMutationDecision(connectedOwner, "node-a", "delete")).toBe(
      "connected_delete_blocked"
    );

    const standaloneOwner = site();
    standaloneOwner.members = [standaloneOwner.members![0]];
    standaloneOwner.memberCount = 1;
    standaloneOwner.activeMemberCount = 1;
    expect(floorplanMutationDecision(standaloneOwner, "node-a", "delete")).toBe(
      "allowed"
    );
  });

  it("uses the complete image coordinate frame instead of only the outline", () => {
    expect(floorplanCoordinateSize(document())).toEqual({ widthMm: 10000, heightMm: 6000 });
  });

  it("centers every out-of-bounds radar and preserves in-bounds radars", () => {
    expect(floorplanReplacementRelocations(site(), document())).toEqual([
      { participantId: "node-b", originXMillimeters: 5000, originYMillimeters: 3000 },
      { participantId: "node-c", originXMillimeters: 5000, originYMillimeters: 3000 }
    ]);
  });

  it("uses the canonical local transform after shared replacement", () => {
    const current = site();
    current.floorplan.floorplanId = "fp-new";
    current.floorplan.geometryVersion = 1;
    expect(canonicalLocalRadarPlacement(current, document(), "node-a")).toEqual({
      originX: 120,
      originY: 80,
      rotation: 90,
      scale: 1,
      poseVersion: 3
    });
  });

  it.each([2, 3, 4, 5, 6, 7])(
    "runs one ordered replacement action for %i devices",
    async (memberCount) => {
      const current = site();
      current.members = Array.from({ length: memberCount }, (_, index) =>
        member(`node-${index}`, index % 2 === 0 ? 1000 : 12000, 1000)
      );
      current.memberCount = memberCount;
      current.activeMemberCount = memberCount;
      const calls: string[] = [];
      const result = await runFloorplanReplacementAction({
        site: current,
        document: document(),
        prepare: async () => { calls.push("prepare"); return current; },
        savePayloads: async () => { calls.push("save"); },
        commitManifest: async (bounds) => {
          calls.push(`commit:${bounds.widthMm}x${bounds.heightMm}`);
          return current;
        },
        finalizeLocalState: async () => { calls.push("finalize"); }
      });
      expect(calls).toEqual(["prepare", "save", "commit:10000x6000", "finalize"]);
      expect(result.relocatedParticipantIds).toEqual(
        Array.from({ length: memberCount }, (_, index) => index)
          .filter((index) => index % 2 === 1)
          .map((index) => `node-${index}`)
      );
    }
  );

  it.each([
    ["prepare", "prepare"],
    ["save", "save"],
    ["commit", "commit"]
  ] as const)("stops at a failed %s boundary", async (_label, failAt) => {
    const calls: string[] = [];
    await expect(runFloorplanReplacementAction({
      site: site(),
      document: document(),
      prepare: async () => {
        calls.push("prepare");
        if (failAt === "prepare") throw new Error("failed");
      },
      savePayloads: async () => {
        calls.push("save");
        if (failAt === "save") throw new Error("failed");
      },
      commitManifest: async () => {
        calls.push("commit");
        if (failAt === "commit") throw new Error("failed");
        return site();
      },
      finalizeLocalState: async () => { calls.push("finalize"); }
    })).rejects.toThrow("failed");
    expect(calls[calls.length - 1]).toBe(failAt);
    expect(calls).not.toContain("finalize");
  });

  it("uses the post-continuity site snapshot for relocation decisions", async () => {
    const stale = site();
    stale.members = [member("node-a", 1000, 1000)];
    const current = site();
    const result = await runFloorplanReplacementAction({
      site: stale,
      document: document(),
      prepare: async () => current,
      savePayloads: async () => {},
      commitManifest: async () => current,
      finalizeLocalState: async () => {}
    });
    expect(result.relocatedParticipantIds).toEqual(["node-b", "node-c"]);
  });

  it("keeps a committed replacement successful when local finalization fails", async () => {
    const failure = new Error("local state failed");
    const result = await runFloorplanReplacementAction({
      site: site(),
      document: document(),
      prepare: async () => site(),
      savePayloads: async () => {},
      commitManifest: async () => site(),
      finalizeLocalState: async () => { throw failure; }
    });
    expect(result.relocatedParticipantIds).toEqual(["node-b", "node-c"]);
    expect(result.localFinalizationError).toBe(failure);
  });
});
