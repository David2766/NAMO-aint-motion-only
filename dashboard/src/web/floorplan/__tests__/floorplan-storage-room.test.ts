import { describe, expect, it } from "vitest";
import type { FloorplanStorageRoom, FloorplanStorageScale } from "../../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../../core/floorplan/floorplan-types";
import { currentStoredFloorplanRooms, storedRoomFromCandidate } from "../floorplan-storage-room";

const scale: FloorplanStorageScale = {
  widthMm: 8000,
  heightMm: 3000,
  outerBoundsPx: [10, 20, 400, 200],
  mmPerPxX: 20,
  mmPerPxY: 15
};

function rectCandidate(overrides: Partial<RoomCandidate> = {}): RoomCandidate {
  return {
    id: "room-1",
    name: "Room 1",
    kind: "room",
    confidence: 100,
    status: "confirmed",
    shape: "rect",
    rect: { x: 10, y: 20, width: 400, height: 200 },
    ...overrides
  };
}

describe("floorplan storage room helpers", () => {
  it("uses total floorplan size when candidate touches all outer bounds", () => {
    expect(storedRoomFromCandidate(rectCandidate(), scale, null, 8)).toEqual({
      id: "room-1",
      name: "Room 1",
      kind: "room",
      pointsPx: [[10, 20], [410, 20], [410, 220], [10, 220]],
      widthMm: 8000,
      heightMm: 3000,
      manualSize: false
    });
  });

  it("estimates room size from pixel bounds when candidate does not touch outer bounds", () => {
    expect(storedRoomFromCandidate(rectCandidate({
      rect: { x: 50, y: 60, width: 100, height: 80 }
    }), scale, null, 8)).toMatchObject({
      pointsPx: [[50, 60], [150, 60], [150, 140], [50, 140]],
      widthMm: 2000,
      heightMm: 1200,
      manualSize: false
    });
  });

  it("keeps previous manual room dimensions", () => {
    const previous: FloorplanStorageRoom = {
      id: "room-1",
      name: "Room 1",
      kind: "room",
      pointsPx: [],
      widthMm: 1234,
      heightMm: 567,
      manualSize: true
    };
    expect(storedRoomFromCandidate(rectCandidate({
      rect: { x: 50, y: 60, width: 100, height: 80 }
    }), scale, previous, 8)).toMatchObject({
      widthMm: 1234,
      heightMm: 567,
      manualSize: true
    });
  });

  it("converts a list of candidates using previous room lookup", () => {
    const candidates = [
      rectCandidate({ id: "room-1", rect: { x: 50, y: 60, width: 100, height: 80 } }),
      rectCandidate({ id: "room-2", name: "Room 2", rect: { x: 10, y: 20, width: 400, height: 200 } })
    ];
    const previousRooms: FloorplanStorageRoom[] = [{
      id: "room-1",
      name: "Room 1",
      kind: "room",
      pointsPx: [],
      widthMm: 1111,
      heightMm: 2222,
      manualSize: true
    }];
    expect(currentStoredFloorplanRooms(candidates, scale, previousRooms, 8)).toEqual([
      expect.objectContaining({ id: "room-1", widthMm: 1111, heightMm: 2222, manualSize: true }),
      expect.objectContaining({ id: "room-2", widthMm: 8000, heightMm: 3000, manualSize: false })
    ]);
  });
});
