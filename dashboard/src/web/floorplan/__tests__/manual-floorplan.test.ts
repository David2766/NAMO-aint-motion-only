import { afterEach, describe, expect, it, vi } from "vitest";
import {
  MANUAL_FLOORPLAN_HEIGHT_PX,
  MANUAL_FLOORPLAN_MIN_SCALE_PERCENT,
  MANUAL_FLOORPLAN_MM_PER_PX,
  MANUAL_FLOORPLAN_SNAP_STEP_PX,
  MANUAL_FLOORPLAN_WIDTH_PX,
  manualFloorplanBounds,
  manualFloorplanDraftPointInsideOutline,
  manualFloorplanEdgeLabels,
  manualFloorplanMmPerPx,
  manualFloorplanPolygonInsideOutline,
  renderManualFloorplanImage,
  snapManualFloorplanPoint,
  validManualFloorplanPolygon
} from "../manual-floorplan";
import {
  manualFloorplanDoorsValid,
  manualFloorplanSlidingDoorOffsetPx,
  moveManualFloorplanDoor,
  placeManualFloorplanDoor,
  resizeManualFloorplanDoor,
  type ManualFloorplanDoor
} from "../manual-floorplan-doors";
import type { RoomCandidate } from "../../../core/floorplan/floorplan-types";

afterEach(() => vi.unstubAllGlobals());

function room(points: Array<[number, number]>): RoomCandidate {
  return {
    id: "room_1",
    name: "Room",
    kind: "unknown",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 300, height: 200 },
    points
  };
}

describe("manual floorplan geometry", () => {
  it("snaps points to the 0.5 metre grid while preserving ordinary angles", () => {
    const first = snapManualFloorplanPoint([], { x: 103, y: 96 });
    expect(first).toEqual({ x: 100, y: 100 });

    const second = snapManualFloorplanPoint([[100, 100]], { x: 224, y: 143 });
    expect(second).toEqual({ x: 200, y: 150 });
    expect(MANUAL_FLOORPLAN_SNAP_STEP_PX * MANUAL_FLOORPLAN_MM_PER_PX).toBe(500);
  });

  it("soft-snaps near horizontal, vertical, and diagonal edges", () => {
    expect(snapManualFloorplanPoint([[100, 100]], { x: 302, y: 109 })).toEqual({ x: 300, y: 100 });
    expect(snapManualFloorplanPoint([[100, 100]], { x: 109, y: 302 })).toEqual({ x: 100, y: 300 });
    expect(snapManualFloorplanPoint([[100, 100]], { x: 302, y: 292 })).toEqual({ x: 300, y: 300 });
  });

  it("aligns a new corner with any previously placed corner axis", () => {
    expect(snapManualFloorplanPoint([[120, 130], [330, 130]], { x: 129, y: 310 }))
      .toEqual({ x: 120, y: 300 });
  });

  it("formats open and closed edge lengths to one decimal metre", () => {
    expect(manualFloorplanEdgeLabels([[0, 0], [100, 0], [100, 50]])).toEqual([
      { x: 50, y: 0, angleDeg: 0, text: "1.0 m" },
      { x: 100, y: 25, angleDeg: 90, text: "0.5 m" }
    ]);
    expect(manualFloorplanEdgeLabels([[0, 0], [100, 0], [100, 100]], true)).toHaveLength(3);
  });

  it("accepts concave outlines and rejects common drawing mistakes", () => {
    const lShape = [[0, 0], [200, 0], [200, 100], [100, 100], [100, 200], [0, 200]] as Array<[number, number]>;
    expect(validManualFloorplanPolygon(lShape)).toBe(true);
    expect(manualFloorplanBounds(lShape)).toEqual({ x: 0, y: 0, width: 200, height: 200 });
    expect(validManualFloorplanPolygon([[0, 0], [100, 100], [0, 100], [100, 0]])).toBe(false);
    expect(validManualFloorplanPolygon([[0, 0], [100, 0], [100, 0], [0, 100]])).toBe(false);
  });

  it("provides a whole-home drawing area instead of the old 12 by 8 metre canvas", () => {
    expect(MANUAL_FLOORPLAN_WIDTH_PX * MANUAL_FLOORPLAN_MM_PER_PX).toBe(30_000);
    expect(MANUAL_FLOORPLAN_HEIGHT_PX * MANUAL_FLOORPLAN_MM_PER_PX).toBe(20_000);
    expect(MANUAL_FLOORPLAN_WIDTH_PX * manualFloorplanMmPerPx(MANUAL_FLOORPLAN_MIN_SCALE_PERCENT))
      .toBe(120_000);
    expect(MANUAL_FLOORPLAN_HEIGHT_PX * manualFloorplanMmPerPx(MANUAL_FLOORPLAN_MIN_SCALE_PERCENT))
      .toBe(80_000);
  });

  it("keeps the snap grid at half a metre across work scales", () => {
    const mmPerPx = manualFloorplanMmPerPx(25);
    const point = snapManualFloorplanPoint([], { x: 14, y: 24 }, undefined, undefined, mmPerPx);
    expect(point).toEqual({ x: 12.5, y: 25 });
    expect(point.x * mmPerPx).toBe(500);
  });

  it("accepts rooms inside a concave outline and rejects rooms that leave it", () => {
    const outline = [[0, 0], [300, 0], [300, 100], [180, 100], [180, 240], [0, 240]] as Array<[number, number]>;
    const inside = [[20, 20], [150, 20], [150, 200], [20, 200]] as Array<[number, number]>;
    const crossingNotch = [[140, 40], [260, 40], [260, 180], [140, 180]] as Array<[number, number]>;

    expect(manualFloorplanPolygonInsideOutline(inside, outline)).toBe(true);
    expect(manualFloorplanPolygonInsideOutline(crossingNotch, outline)).toBe(false);
    expect(manualFloorplanDraftPointInsideOutline([], [20, 20], outline)).toBe(true);
    expect(manualFloorplanDraftPointInsideOutline([], [260, 180], outline)).toBe(false);
    expect(manualFloorplanDraftPointInsideOutline([[260, 50]], [140, 180], outline)).toBe(false);
  });

  it("places hinged and sliding doors directly on outer and room walls", () => {
    const outline = [[0, 0], [400, 0], [400, 300], [0, 300]] as Array<[number, number]>;
    const rooms = [room([[20, 20], [200, 20], [200, 250], [20, 250]])];
    const hinged = placeManualFloorplanDoor({ x: 170, y: 12 }, outline, rooms, [], "hinged", 10);
    expect(hinged.reason).toBeNull();
    expect(hinged.door).toMatchObject({ kind: "hinged", centerPx: [145, 20], widthPx: 90, rotationDeg: 0 });

    const sliding = placeManualFloorplanDoor({ x: 192, y: 130 }, outline, rooms, [], "sliding", 10);
    expect(sliding.reason).toBeNull();
    expect(sliding.door).toMatchObject({ kind: "sliding", centerPx: [200, 130], widthPx: 160, rotationDeg: 90 });
  });

  it("rejects points away from walls, short walls, and overlapping doors", () => {
    const outline = [[0, 0], [400, 0], [400, 300], [0, 300]] as Array<[number, number]>;
    expect(placeManualFloorplanDoor({ x: 200, y: 150 }, outline, [], [], "hinged", 10).reason)
      .toBe("wall_not_found");

    const tinyRoom = [room([[100, 100], [160, 100], [160, 140], [100, 140]])];
    expect(placeManualFloorplanDoor({ x: 130, y: 105 }, [], tinyRoom, [], "hinged", 10).reason)
      .toBe("wall_too_short");

    const existing: ManualFloorplanDoor[] = [{
      id: "door_1",
      kind: "hinged",
      centerPx: [170, 0],
      widthPx: 90,
      rotationDeg: 0
    }];
    expect(placeManualFloorplanDoor({ x: 180, y: 4 }, outline, [], existing, "sliding", 10).reason)
      .toBe("door_overlap");
  });

  it("invalidates a door when its supporting wall is moved away", () => {
    const outline = [[0, 0], [400, 0], [400, 300], [0, 300]] as Array<[number, number]>;
    const door: ManualFloorplanDoor = {
      id: "door_1",
      kind: "hinged",
      centerPx: [180, 100],
      widthPx: 90,
      rotationDeg: 0
    };
    expect(manualFloorplanDoorsValid([door], outline, [room([[20, 100], [300, 100], [300, 250], [20, 250]])], 10))
      .toBe(true);
    expect(manualFloorplanDoorsValid([door], outline, [room([[20, 130], [300, 130], [300, 250], [20, 250]])], 10))
      .toBe(false);
  });

  it("resizes a selected door along its supporting wall without moving the opposite end", () => {
    const outline = [[0, 0], [400, 0], [400, 300], [0, 300]] as Array<[number, number]>;
    const door: ManualFloorplanDoor = {
      id: "door_1",
      kind: "hinged",
      centerPx: [200, 0],
      widthPx: 90,
      rotationDeg: 0
    };
    const resized = resizeManualFloorplanDoor(door, "end", { x: 300, y: 18 }, outline, [], [door], 10);

    expect(resized.reason).toBeNull();
    expect(resized.door).toMatchObject({ centerPx: [227.5, 0], widthPx: 145, rotationDeg: 0 });
  });

  it("moves a door only along the wall segment that already supports it", () => {
    const outline = [[0, 0], [400, 0], [400, 300], [0, 300]] as Array<[number, number]>;
    const door: ManualFloorplanDoor = {
      id: "door_1",
      kind: "hinged",
      centerPx: [200, 0],
      widthPx: 90,
      rotationDeg: 0
    };
    const moved = moveManualFloorplanDoor(door, { x: 390, y: 280 }, outline, [], [door], 10);

    expect(moved.reason).toBeNull();
    expect(moved.door).toMatchObject({ centerPx: [345, 0], widthPx: 90, rotationDeg: 0 });
  });

  it("keeps sliding-door thickness independent from its adjustable length", () => {
    expect(manualFloorplanSlidingDoorOffsetPx(10)).toBe(8);
    expect(manualFloorplanSlidingDoorOffsetPx(10)).toBe(manualFloorplanSlidingDoorOffsetPx(10));
  });

  it("bakes only frozen physical room walls into the manual WebP", async () => {
    const context = {
      fillStyle: "",
      strokeStyle: "",
      lineWidth: 0,
      lineJoin: "miter",
      fillRect: vi.fn(),
      save: vi.fn(),
      restore: vi.fn(),
      beginPath: vi.fn(),
      moveTo: vi.fn(),
      lineTo: vi.fn(),
      closePath: vi.fn(),
      fill: vi.fn(),
      stroke: vi.fn(),
      fillText: vi.fn()
    };
    vi.stubGlobal("document", {
      createElement: () => ({
        width: 0,
        height: 0,
        getContext: () => context,
        toBlob: (callback: BlobCallback) => callback(new Blob(["manual"], { type: "image/webp" }))
      })
    });

    await renderManualFloorplanImage({
      rooms: [room([[0, 0], [100, 0], [100, 100], [0, 100]])],
      outline: [[0, 0], [200, 0], [200, 150], [0, 150]]
    });

    expect(context.stroke).toHaveBeenCalledTimes(2);
    expect(context.fill).not.toHaveBeenCalled();
    expect(context.fillText).not.toHaveBeenCalled();
  });
});
