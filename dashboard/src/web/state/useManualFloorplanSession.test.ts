import { describe, expect, it } from "vitest";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import { createManualFloorplanSession } from "./useManualFloorplanSession.svelte";

function polygonCandidate(points: Array<[number, number]>): RoomCandidate {
  return {
    id: "manual",
    name: "Manual",
    kind: "unknown",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 300, height: 200 },
    points
  };
}

describe("manual floorplan session", () => {
  it("quietly expands the fixed canvas to 120 by 80 metres before outline confirmation", () => {
    const session = createManualFloorplanSession();
    session.start();

    expect(session.scalePercent).toBe(100);
    expect(session.mmPerPx).toBe(10);
    session.setScalePercent(25);
    expect(session.scalePercent).toBe(25);
    expect(session.mmPerPx).toBe(40);
    expect(session.width * session.mmPerPx).toBe(120_000);
    expect(session.height * session.mmPerPx).toBe(80_000);
  });

  it("uses the same 25 percent work-scale steps for zooming in and out", () => {
    const session = createManualFloorplanSession();
    session.start();

    session.zoomScale(1);
    expect(session.scalePercent).toBe(125);
    expect(session.mmPerPx).toBe(8);
    session.zoomScale(1);
    expect(session.scalePercent).toBe(150);
    session.zoomScale(-1);
    expect(session.scalePercent).toBe(125);
    session.zoomScale(-1);
    expect(session.scalePercent).toBe(100);
    session.zoomScale(-1);
    expect(session.scalePercent).toBe(75);
  });

  it("locks the physical scale when the outer outline is confirmed", () => {
    const session = createManualFloorplanSession();
    session.start();
    session.setScalePercent(50);
    session.captureOutline(polygonCandidate([[0, 0], [300, 0], [300, 200], [0, 200]]));

    session.setScalePercent(25);
    expect(session.scalePercent).toBe(50);
    expect(session.scaleInput()).toMatchObject({ widthMm: 6000, heightMm: 4000 });
  });

  it("keeps outer-boundary creation separate from room creation", () => {
    const session = createManualFloorplanSession();
    session.start();

    expect(session.phase).toBe("outline");
    expect(session.captureOutline(polygonCandidate([[0, 0], [300, 0], [300, 200], [0, 200]]))).toBe(true);
    expect(session.phase).toBe("rooms");
    expect(session.outlinePoints).toEqual([[0, 0], [300, 0], [300, 200], [0, 200]]);
    expect(session.draftValid([[20, 20], [120, 20], [120, 100], [20, 100]])).toBe(true);
    expect(session.draftValid([[250, 20], [350, 20], [350, 100], [250, 100]])).toBe(false);
  });

  it("restores the original outline when editing is cancelled", () => {
    const session = createManualFloorplanSession();
    session.start();
    session.captureOutline(polygonCandidate([[0, 0], [300, 0], [300, 200], [0, 200]]));

    session.beginOutlineEdit();
    session.moveOutlinePoint(1, { x: 250, y: 20 });
    expect(session.outlinePoints[1]).not.toEqual([300, 0]);

    session.cancelOutlineEdit();
    expect(session.outlinePoints).toEqual([[0, 0], [300, 0], [300, 200], [0, 200]]);
    expect(session.outlineEditing).toBe(false);
  });

  it("uses the same metric and angle snap while editing room vertices", () => {
    const session = createManualFloorplanSession();
    session.start();
    session.captureOutline(polygonCandidate([[0, 0], [500, 0], [500, 400], [0, 400]]));

    expect(session.snapEditPoint([[100, 100], [300, 100], [300, 300]], 1, { x: 304, y: 109 }))
      .toEqual({ x: 300, y: 100 });
    expect(session.snapEditPoint([[100, 100], [300, 100], [300, 300]], 0, { x: 109, y: 104 }))
      .toEqual({ x: 100, y: 100 });
  });

  it("keeps door placement transactional and restricted to known walls", () => {
    const session = createManualFloorplanSession();
    session.start();
    session.captureOutline(polygonCandidate([[0, 0], [500, 0], [500, 400], [0, 400]]));
    const rooms = [polygonCandidate([[50, 50], [300, 50], [300, 300], [50, 300]])];

    session.startDoorPlacement();
    expect(session.doorPlacementActive).toBe(true);
    expect(session.placeDoor({ x: 180, y: 56 }, rooms)).toBe(true);
    expect(session.doors).toHaveLength(1);
    expect(session.selectedDoorId).toBe(session.doors[0].id);
    expect(session.previewDoor({ x: 294, y: 190 }, rooms)?.centerPx[0]).toBe(300);
    expect(session.resizeDoor(session.doors[0].id, "end", { x: 260, y: 72 }, rooms)).toBe(true);
    expect(session.doors[0].widthPx).toBeGreaterThan(90);
    expect(session.moveDoor(session.doors[0].id, { x: 230, y: 180 }, rooms)).toBe(true);
    expect(session.doors[0].centerPx[1]).toBe(50);
    session.setDoorPlacementKind("sliding");
    expect(session.placeDoor({ x: 294, y: 190 }, rooms)).toBe(true);
    expect(session.doors.map((door) => door.kind)).toEqual(["hinged", "sliding"]);

    session.cancelDoorPlacement();
    expect(session.doors).toEqual([]);

    session.startDoorPlacement();
    expect(session.placeDoor({ x: 180, y: 56 }, rooms)).toBe(true);
    session.finishDoorPlacement();
    expect(session.doors).toHaveLength(1);
    expect(session.roomsValid(rooms)).toBe(true);
    session.removeDoor(session.doors[0].id);
    expect(session.doors).toEqual([]);
  });
});
