import { describe, expect, it } from "vitest";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import { createFloorplanRoomScaleSession } from "./useFloorplanRoomScaleSession.svelte";

const text = {
  sizeRequired: "size required",
  roomSizesCalculated: (count: number) => `calculated ${count}`,
  roomSizeHint: "input total size",
  preciseSizeHint: "precise hint"
};

function roomFixture(overrides: Partial<RoomCandidate> = {}): RoomCandidate {
  const points = overrides.points ?? [[0, 0], [100, 0], [100, 50], [0, 50]] as Array<[number, number]>;
  return {
    id: "room_1",
    name: "Room 1",
    kind: "unknown",
    confidence: 100,
    status: "candidate",
    shape: "polygon",
    rect: {
      x: Math.min(...points.map(([x]) => x)),
      y: Math.min(...points.map(([, y]) => y)),
      width: Math.max(...points.map(([x]) => x)) - Math.min(...points.map(([x]) => x)),
      height: Math.max(...points.map(([, y]) => y)) - Math.min(...points.map(([, y]) => y))
    },
    points,
    ...overrides
  };
}

function createSession(candidates: RoomCandidate[] = [roomFixture()]) {
  return createFloorplanRoomScaleSession({
    getRoomCandidates: () => candidates,
    getText: () => text,
    outerBoundsTouchTolerancePx: 8
  });
}

describe("floorplan room scale session", () => {
  it("normalizes total size input and calculates the floorplan scale", () => {
    const session = createSession();

    session.updateTotalSize("width", "3,250mm");
    session.updateTotalSize("height", "1625.5");

    expect(session.totalSize).toEqual({ width: "3250", height: "1625.5" });
    expect(session.scaleEstimate()).toMatchObject({
      widthMm: 3250,
      heightMm: 1625.5,
      mmPerPxX: 32.5,
      mmPerPxY: 32.51
    });
    expect(session.scaleSummary()).toMatchObject({
      widthMm: 3250,
      heightMm: 1626,
      widthPx: "100.00",
      heightPx: "50.00"
    });
  });

  it("calculates room estimates and allows manual room measurement overrides", () => {
    const session = createSession([
      roomFixture({ id: "room_1", points: [[0, 0], [100, 0], [100, 50], [0, 50]] }),
      roomFixture({ id: "room_2", points: [[100, 0], [150, 0], [150, 50], [100, 50]] })
    ]);

    session.updateTotalSize("width", "3000");
    session.updateTotalSize("height", "1000");
    session.updateMeasurement("room_2", "width", "875");
    session.calculate();

    expect(session.calculated).toBe(true);
    expect(session.message()).toBe("calculated 2");
    expect(session.estimates.room_1).toMatchObject({
      widthMm: 2000,
      heightMm: 1000,
      widthFromOuter: false,
      heightFromOuter: true
    });
    expect(session.estimates.room_2).toMatchObject({
      widthMm: 875,
      widthManual: true,
      manuallyEdited: true
    });
  });

  it("reports a required-size error only when asked to calculate without valid scale", () => {
    const session = createSession();

    session.updateTotalSize("width", "3000");

    expect(session.error).toBe("");
    expect(session.message()).toBe("input total size");

    session.calculate();

    expect(session.error).toBe("size required");
    expect(session.calculated).toBe(false);
    expect(session.estimates).toEqual({});
  });

  it("builds storage estimates from the current scale", () => {
    const session = createSession();

    session.updateTotalSize("width", "2000");
    session.updateTotalSize("height", "1000");

    expect(session.storageEstimates().room_1).toMatchObject({
      widthMm: 2000,
      heightMm: 1000
    });
  });

  it("uses an explicit manual outline instead of treating the first room as the floorplan bounds", () => {
    const session = createFloorplanRoomScaleSession({
      getRoomCandidates: () => [roomFixture({
        points: [[100, 100], [200, 100], [200, 150], [100, 150]]
      })],
      getOuterBounds: () => ({ x: 20, y: 30, width: 400, height: 250 }),
      getText: () => text,
      outerBoundsTouchTolerancePx: 8
    });

    session.updateTotalSize("width", "4000");
    session.updateTotalSize("height", "2500");

    expect(session.scaleEstimate()).toMatchObject({
      outerBounds: { x: 20, y: 30, width: 400, height: 250 },
      mmPerPxX: 10,
      mmPerPxY: 10
    });
  });
});
