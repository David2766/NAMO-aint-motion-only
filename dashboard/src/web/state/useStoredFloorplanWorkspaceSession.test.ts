import { describe, expect, it, vi } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { FloorplanOcclusionSegment } from "../floorplan/floorplan-occlusion";
import { createStoredFloorplanWorkspaceSession } from "./useStoredFloorplanWorkspaceSession.svelte";
import type { StoredRadarPlacement } from "./useStoredRadarPlacementEditor.svelte";

function documentFixture(overrides: Partial<FloorplanStorageDocument> = {}): FloorplanStorageDocument {
  return {
    version: 1,
    image: {
      path: "/floorplan.webp",
      mime: "image/webp",
      width: 200,
      height: 400
    },
    scale: {
      widthMm: 2000,
      heightMm: 4000,
      outerBoundsPx: [10, 20, 100, 200],
      mmPerPxX: 20,
      mmPerPxY: 20
    },
    radar: {
      originPx: [50, 100],
      rotationDeg: 0,
      scale: 1.03
    },
    rooms: [],
    occlusion: {
      ignoredEdges: ["wall:a"]
    },
    objects: [],
    ...overrides
  };
}

function candidateFixture(overrides: Partial<RoomCandidate> = {}): RoomCandidate {
  const points = overrides.points ?? [[20, 30], [80, 30], [80, 90], [20, 90]] as Array<[number, number]>;
  return {
    id: "room_1",
    name: "Room 1",
    kind: "room",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 20, y: 30, width: 60, height: 60 },
    points,
    ...overrides
  };
}

function segmentFixture(): FloorplanOcclusionSegment {
  return {
    id: "stored-room-room_1-0",
    occlusionKey: "0,0,100,0",
    x1: 0,
    y1: 0,
    x2: 100,
    y2: 0,
    axis: "horizontal"
  };
}

function createHarness(options: {
  document?: FloorplanStorageDocument | null;
  candidates?: RoomCandidate[];
  selectedRoomId?: string;
  placement?: StoredRadarPlacement | null;
} = {}) {
  const wallProvider = vi.fn(() => [segmentFixture()]);
  const session = createStoredFloorplanWorkspaceSession({
    getDocument: () => options.document === undefined ? documentFixture() : options.document,
    getVisibleRoomCandidates: () => options.candidates ?? [candidateFixture()],
    getSelectedRoomId: () => options.selectedRoomId ?? "room_1",
    getRadarPlacement: () => options.placement ?? { originX: 50, originY: 100, rotation: 0 },
    getRoomWallSegments: wallProvider
  });
  return {
    session,
    wallProvider
  };
}

describe("stored floorplan workspace session", () => {
  it("builds the stored image frame style from the padded content bounds", () => {
    const { session } = createHarness();

    expect(session.imageFrameStyle()).toContain("aspect-ratio: 120 / 240");
  });

  it("crops the stored editor to the outline plus ten percent on each side", () => {
    const document = documentFixture({
      outline: {
        pointsPx: [[50, 100], [150, 100], [150, 200], [50, 200]]
      }
    });
    const { session } = createHarness({ document });

    expect(session.viewBounds()).toEqual({ x: 40, y: 90, width: 120, height: 120 });
    expect(session.imageFrameStyle()).toContain("aspect-ratio: 120 / 120");
  });

  it("clamps stored editor padding to the image boundary", () => {
    const document = documentFixture({
      outline: {
        pointsPx: [[0, 0], [100, 0], [100, 100], [0, 100]]
      }
    });
    const { session } = createHarness({ document });

    expect(session.viewBounds()).toEqual({ x: 0, y: 0, width: 110, height: 110 });
  });

  it("uses the selected room as the default furniture placement context", () => {
    const { session } = createHarness();

    expect(session.defaultFurniturePlacementContext()).toEqual({
      roomId: "room_1",
      bounds: { x: 20, y: 30, width: 60, height: 60 }
    });
  });

  it("falls back to the scale outer bounds when no room is selected", () => {
    const { session } = createHarness({ selectedRoomId: "" });

    expect(session.defaultFurniturePlacementContext()).toEqual({
      bounds: { x: 10, y: 20, width: 100, height: 200 }
    });
  });

  it("returns null furniture placement context when the document has no scale", () => {
    const document = documentFixture({ scale: null as never });
    const { session } = createHarness({ document });

    expect(session.defaultFurniturePlacementContext()).toBeNull();
  });

  it("maps visible room candidates to furniture room polygons", () => {
    const { session } = createHarness();

    expect(session.furnitureRooms()).toEqual([{
      id: "room_1",
      pointsPx: [[20, 30], [80, 30], [80, 90], [20, 90]]
    }]);
  });

  it("uses the stored-room segment prefix for radar room wall segments", () => {
    const placement = { originX: 12, originY: 34, rotation: 178 };
    const { session, wallProvider } = createHarness({ placement });

    expect(session.roomBoundarySegments()).toEqual([segmentFixture()]);
    expect(session.radarOcclusionSegments()).toEqual([segmentFixture()]);
    expect(wallProvider).toHaveBeenCalledWith(placement, "stored-room");
  });

  it("exposes stored radar scale, ignored occlusion edges, and furniture bounds", () => {
    const { session } = createHarness();

    expect(session.radarScalePercent()).toBe(103);
    expect(session.ignoredOcclusionSegmentIds()).toEqual(["wall:a"]);
    expect(session.furnitureBounds()).toEqual({ x: 10, y: 20, width: 100, height: 200 });
  });
});
