import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import {
  buildManualPhysicalWallSegments,
  ensureStoredPhysicalWalls,
  physicalWallOcclusionSegments
} from "./floorplan-physical-walls";

function room(id: string, points: Array<[number, number]>): RoomCandidate {
  return {
    id,
    name: id,
    kind: "room",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 100, height: 100 },
    points
  };
}

function documentFixture(overrides: Partial<FloorplanStorageDocument> = {}): FloorplanStorageDocument {
  return {
    version: 2,
    source: "manual",
    image: { path: "/floorplan.webp", mime: "image/webp", width: 300, height: 200 },
    scale: {
      widthMm: 3000,
      heightMm: 2000,
      outerBoundsPx: [10, 20, 200, 100],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: { originPx: [40, 80], rotationDeg: 0, scale: 1 },
    outline: { pointsPx: [[10, 20], [210, 20], [210, 120], [10, 120]] },
    rooms: [{
      id: "room_1",
      name: "Room",
      kind: "room",
      pointsPx: [[10, 20], [110, 20], [110, 120], [10, 120]]
    }],
    occlusion: { ignoredEdges: [] },
    ...overrides
  };
}

describe("floorplan physical walls", () => {
  it("freezes manual outline and room edges as independent physical walls", () => {
    const walls = buildManualPhysicalWallSegments(
      [[0, 0], [200, 0], [200, 100], [0, 100]],
      [room("room_1", [[0, 0], [100, 0], [100, 100], [0, 100]])]
    );

    expect(walls.some((wall) => wall.x1 === 100 && wall.x2 === 100)).toBe(true);
    expect(new Set(walls.map((wall) => `${wall.x1},${wall.y1}-${wall.x2},${wall.y2}`)).size).toBe(walls.length);
  });

  it("keeps diagonal manual walls instead of dropping them", () => {
    const walls = buildManualPhysicalWallSegments(
      [[0, 0], [100, 0], [150, 50], [0, 100]],
      []
    );

    expect(walls.some((wall) => wall.axis === "diagonal")).toBe(true);
  });

  it("hydrates a legacy manual document once and then uses walls instead of logical rooms", () => {
    const legacy = documentFixture({ wallSegments: [] });
    const hydrated = ensureStoredPhysicalWalls(legacy);
    const frozenWalls = physicalWallOcclusionSegments(hydrated);
    const logicalRoomMoved: FloorplanStorageDocument = {
      ...hydrated,
      rooms: [{
        ...hydrated.rooms[0],
        pointsPx: [[120, 20], [210, 20], [210, 120], [120, 120]]
      }]
    };

    expect(hydrated.wallSegments?.length).toBeGreaterThan(0);
    expect(physicalWallOcclusionSegments(logicalRoomMoved)).toEqual(frozenWalls);
  });

});
