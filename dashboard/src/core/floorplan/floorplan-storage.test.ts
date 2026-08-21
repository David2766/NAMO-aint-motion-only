import { describe, expect, it } from "vitest";
import {
  advanceFloorplanAutomationAreasRevision,
  buildFloorplanStorageDocument,
  ensureFloorplanStorageIdentity,
  FLOORPLAN_STORAGE_VERSION,
  sanitizeFloorplanAutomationAreas,
  type FloorplanStorageBuildInput,
  type FloorplanStorageDocument
} from "./floorplan-storage";

function buildInput(): FloorplanStorageBuildInput {
  return {
    image: {
      width: 200,
      height: 120
    },
    scale: {
      widthMm: 2000,
      heightMm: 1200,
      outerBounds: { x: 0, y: 0, width: 200, height: 120 },
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: {
      originX: 50,
      originY: 100,
      rotation: 0,
      scale: 1
    },
    rooms: [{
      id: "room_1",
      name: "Room",
      kind: "room",
      confidence: 100,
      status: "confirmed",
      shape: "polygon",
      rect: { x: 0, y: 0, width: 100, height: 120 },
      points: [[0, 0], [100, 0], [100, 120], [0, 120]]
    }],
    wallSegments: [{
      id: "wall_1",
      axis: "vertical",
      x1: 12.345,
      y1: 20,
      x2: 12.345,
      y2: 90,
      length: 70
    }],
    doors: [{
      id: "door_1",
      kind: "sliding",
      centerPx: [100.126, 40.994],
      widthPx: 80.555,
      rotationDeg: 90
    }]
  };
}

describe("floorplan storage document builder", () => {
  it("stores detected wall candidates separately from confirmed room polygons", () => {
    const document = buildFloorplanStorageDocument(buildInput());

    expect(document.rooms[0].pointsPx).toEqual([[0, 0], [100, 0], [100, 120], [0, 120]]);
    expect(document.wallSegments).toEqual([{
      id: "wall_1",
      axis: "vertical",
      x1: 12.35,
      y1: 20,
      x2: 12.35,
      y2: 90,
      length: 70
    }]);
    expect(document.doors).toEqual([{
      id: "door_1",
      kind: "sliding",
      centerPx: [100.13, 40.99],
      widthPx: 80.56,
      rotationDeg: 90
    }]);
  });

  it("stores a manual floorplan source and its locked outer outline", () => {
    const input = buildInput();
    input.source = "manual";
    input.outline = [[0, 0], [200, 0], [200, 120], [100, 120], [100, 80], [0, 80]];

    const document = buildFloorplanStorageDocument(input);

    expect(document.source).toBe("manual");
    expect(document.outline?.pointsPx).toEqual(input.outline);
  });

  it("creates a persistent identity and independent initial revisions", () => {
    const first = buildFloorplanStorageDocument(buildInput());
    const second = buildFloorplanStorageDocument(buildInput());

    expect(first.version).toBe(FLOORPLAN_STORAGE_VERSION);
    expect(first.floorplanId).toMatch(/^fp-[0-9a-f]{32}$/);
    expect(first.floorplanId).not.toBe(second.floorplanId);
    expect(first.geometryVersion).toBe(1);
    expect(first.radar.poseVersion).toBe(1);
  });

  it("upgrades legacy documents without changing an existing identity", () => {
    const legacy = buildFloorplanStorageDocument(buildInput()) as FloorplanStorageDocument;
    legacy.version = 1;
    delete legacy.floorplanId;
    delete legacy.geometryVersion;
    delete legacy.radar.poseVersion;

    const upgraded = ensureFloorplanStorageIdentity(legacy);
    const repeated = ensureFloorplanStorageIdentity(upgraded);

    expect(upgraded.version).toBe(FLOORPLAN_STORAGE_VERSION);
    expect(upgraded.floorplanId).toMatch(/^fp-[0-9a-f]{32}$/);
    expect(upgraded.geometryVersion).toBe(1);
    expect(upgraded.radar.poseVersion).toBe(1);
    expect(repeated).toBe(upgraded);
  });

  it("keeps automation-area revisions independent from geometry", () => {
    const document = buildFloorplanStorageDocument(buildInput());
    document.automationAreas = {
      schemaVersion: 1,
      revision: 4,
      areas: [{
        areaId: "desk",
        name: "Desk",
        spaceId: "office",
        enabled: true,
        pointsPx: [[10, 10], [80, 10], [80, 70], [10, 70]]
      }]
    };

    const updated = advanceFloorplanAutomationAreasRevision(document);

    expect(updated.automationAreas?.revision).toBe(5);
    expect(updated.geometryVersion).toBe(document.geometryVersion);
    expect(updated.radar.poseVersion).toBe(document.radar.poseVersion);
  });

  it("rejects duplicate and over-capacity automation areas", () => {
    const area = (areaId: string) => ({
      areaId,
      name: areaId,
      spaceId: "office",
      enabled: true,
      pointsPx: [[0, 0], [10, 0], [10, 10]] as Array<[number, number]>
    });

    expect(sanitizeFloorplanAutomationAreas({
      schemaVersion: 1,
      revision: 1,
      areas: [area("desk"), area("desk")]
    })).toBeUndefined();
    expect(sanitizeFloorplanAutomationAreas({
      schemaVersion: 1,
      revision: 1,
      areas: Array.from({ length: 7 }, (_, index) => area(`area-${index}`))
    })).toBeUndefined();
  });
});
