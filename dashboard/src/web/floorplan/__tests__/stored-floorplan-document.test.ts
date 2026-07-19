import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument, FloorplanStorageObject } from "../../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../../core/floorplan/floorplan-types";
import {
  buildCurrentStoredFloorplanDocument,
  buildStoredFloorplanRooms,
  sanitizeStoredFloorplanObjects
} from "../stored-floorplan-document";

function documentFixture(): FloorplanStorageDocument {
  return {
    version: 1,
    image: {
      path: "/floorplan.webp",
      mime: "image/webp",
      width: 200,
      height: 120
    },
    scale: {
      widthMm: 2000,
      heightMm: 1200,
      outerBoundsPx: [0, 0, 200, 120],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: {
      originPx: [100, 100],
      rotationDeg: 0,
      scale: 1
    },
    rooms: [
      {
        id: "room_1",
        name: "Bedroom",
        kind: "room",
        pointsPx: [[0, 0], [100, 0], [100, 120], [0, 120]],
        widthMm: 999,
        heightMm: 888,
        manualSize: true
      }
    ],
    occlusion: {
      ignoredEdges: ["room_1:top"]
    },
    wallSegments: [{
      id: "wall_1",
      axis: "vertical",
      x1: 10,
      y1: 0,
      x2: 10,
      y2: 120,
      length: 120
    }],
    objects: []
  };
}

function roomCandidate(): RoomCandidate {
  return {
    id: "room_1",
    name: "Bedroom Edited",
    kind: "room",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 100, height: 120 },
    points: [[0, 0], [100, 0], [100, 120], [0, 120]]
  };
}

describe("stored floorplan document builder", () => {
  it("builds storage rooms while preserving manual dimensions from previous rooms", () => {
    const document = documentFixture();

    const rooms = buildStoredFloorplanRooms({
      candidates: [roomCandidate()],
      scale: document.scale,
      previousRooms: document.rooms,
      touchTolerancePx: 8
    });

    expect(rooms).toEqual([
      {
        id: "room_1",
        name: "Bedroom Edited",
        kind: "room",
        pointsPx: [[0, 0], [100, 0], [100, 120], [0, 120]],
        widthMm: 999,
        heightMm: 888,
        manualSize: true
      }
    ]);
  });

  it("returns an updated document with rebuilt rooms, occlusion, and preserved objects", () => {
    const document = documentFixture();
    const object: FloorplanStorageObject = {
      id: "desk_1",
      asset: "desk",
      xPx: 210,
      yPx: 20,
      widthPx: 30,
      heightPx: 20,
      rotationDeg: 0
    };

    const next = buildCurrentStoredFloorplanDocument({
      document,
      candidates: [roomCandidate()],
      scale: document.scale,
      previousRooms: document.rooms,
      touchTolerancePx: 8,
      objects: [object],
    });

    expect(next?.rooms).toHaveLength(1);
    expect(next?.occlusion.ignoredEdges).toEqual(["room_1:top"]);
    expect(next?.wallSegments).toEqual(document.wallSegments);
    expect(next?.objects).toEqual([object]);
  });

  it("drops malformed furniture objects instead of persisting map indexes", () => {
    const object: FloorplanStorageObject = {
      id: "desk_1",
      asset: "desk",
      xPx: 20,
      yPx: 20,
      widthPx: 30,
      heightPx: 20,
      rotationDeg: 0
    };

    expect(sanitizeStoredFloorplanObjects([0, object, 2], documentFixture().rooms)).toEqual([{
      ...object,
      roomId: "room_1"
    }]);
  });

  it("returns null when no document or scale is available", () => {
    expect(buildCurrentStoredFloorplanDocument({
      document: null,
      candidates: [],
      scale: undefined,
      previousRooms: [],
      touchTolerancePx: 8,
      objects: [],
    })).toBeNull();
  });
});
