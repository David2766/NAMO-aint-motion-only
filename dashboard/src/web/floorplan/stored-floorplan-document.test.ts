import { describe, expect, it } from "vitest";
import type {
  FloorplanStorageDocument,
  FloorplanStorageRoom
} from "../../core/floorplan/floorplan-storage";
import {
  buildCurrentStoredFloorplanDocument,
  sanitizeStoredFloorplanDocument,
  storedRoomToCandidate,
  withLocalFloorplanRadarPlacement
} from "./stored-floorplan-document";

function storageRoomFixture(): FloorplanStorageRoom {
  return {
    id: "room_1",
    name: "Bedroom",
    kind: "room",
    pointsPx: [[10, 20], [110, 20], [110, 80], [10, 80]],
    widthMm: 1000,
    heightMm: 600,
    manualSize: false
  };
}

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
      originPx: [50, 100],
      rotationDeg: 0,
      scale: 1
    },
    rooms: [storageRoomFixture()],
    occlusion: {
      ignoredEdges: ["wall:0,0-10,0"]
    },
    wallSegments: [{
      id: "wall_1",
      axis: "horizontal",
      x1: 0,
      y1: 10,
      x2: 100,
      y2: 10,
      length: 100
    }],
    doors: [{
      id: "door_1",
      kind: "sliding",
      centerPx: [100, 40],
      widthPx: 80,
      rotationDeg: 90
    }],
    objects: []
  };
}

describe("stored floorplan document helpers", () => {
  it("converts stored rooms to editable room candidates", () => {
    const candidate = storedRoomToCandidate(storageRoomFixture());

    expect(candidate).toMatchObject({
      id: "room_1",
      name: "Bedroom",
      kind: "room",
      confidence: 100,
      status: "confirmed",
      shape: "polygon"
    });
    expect(candidate.rect).toEqual({ x: 10, y: 20, width: 100, height: 60 });
    expect(candidate.points).toEqual([[10, 20], [110, 20], [110, 80], [10, 80]]);
  });

  it("preserves occlusion and furniture objects while rebuilding the document", () => {
    const document = documentFixture();
    const object = {
      id: "object_1",
      asset: "bed",
      xPx: 12,
      yPx: 14,
      widthPx: 40,
      heightPx: 30,
      rotationDeg: 0
    };
    const next = buildCurrentStoredFloorplanDocument({
      document,
      candidates: [storedRoomToCandidate(storageRoomFixture())],
      scale: document.scale,
      previousRooms: document.rooms,
      touchTolerancePx: 8,
      objects: [object]
    });

    expect(next?.occlusion.ignoredEdges).toEqual(["wall:0,0-10,0"]);
    expect(next?.wallSegments).toEqual(document.wallSegments);
    expect(next?.doors).toEqual(document.doors);
    expect(next?.objects?.[0]).toMatchObject({ ...object, roomId: "room_1" });
  });

  it("sanitizes malformed stored furniture objects on load", () => {
    const document = {
      ...documentFixture(),
      objects: [
        0,
        {
          id: "object_1",
          asset: "bed",
          xPx: 12,
          yPx: 14,
          widthPx: 40,
          heightPx: 30,
          rotationDeg: 0
        },
        2
      ] as unknown as FloorplanStorageDocument["objects"]
    };

    expect(sanitizeStoredFloorplanDocument(document).objects).toEqual([{
      id: "object_1",
      asset: "bed",
      roomId: "room_1",
      xPx: 12,
      yPx: 14,
      widthPx: 40,
      heightPx: 30,
      rotationDeg: 0
    }]);
  });

  it("keeps only valid structured doors for the shared door overlay", () => {
    const document = {
      ...documentFixture(),
      doors: [
        documentFixture().doors?.[0],
        { id: "broken", kind: "sliding", centerPx: [10], widthPx: -1, rotationDeg: 0 }
      ] as unknown as FloorplanStorageDocument["doors"]
    };

    expect(sanitizeStoredFloorplanDocument(document).doors).toEqual(documentFixture().doors);
  });

  it("keeps legacy baked-door documents distinguishable from structured door documents", () => {
    const document = documentFixture();
    delete document.doors;

    expect(sanitizeStoredFloorplanDocument(document).doors).toBeUndefined();
  });

  it("keeps Node-local radar placement when shared geometry is loaded", () => {
    const document = documentFixture();
    const localized = withLocalFloorplanRadarPlacement(document, {
      originX: 91,
      originY: 47,
      rotation: 135,
      scale: 0.8,
      poseVersion: 22
    });

    expect(localized.radar).toMatchObject({
      originPx: [91, 47],
      rotationDeg: 135,
      scale: 0.8,
      poseVersion: 22
    });
    expect(localized.rooms).toBe(document.rooms);
    expect(document.radar.originPx).toEqual([50, 100]);
  });
});
