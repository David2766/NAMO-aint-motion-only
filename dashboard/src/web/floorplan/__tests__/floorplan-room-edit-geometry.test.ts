import { describe, expect, it } from "vitest";
import {
  mergeRoomCandidatePoints,
  normalizeRect,
  rectFromPoints,
  rectPointsFromBounds,
  roundPoint,
  splitRoomCandidateByLine
} from "../floorplan-room-edit-geometry";

function rectCandidate(x: number, y: number, width: number, height: number) {
  return {
    shape: "rect",
    rect: { x, y, width, height }
  };
}

function polygonCandidate(points: Array<[number, number]>) {
  return {
    shape: "polygon",
    points,
    rect: rectFromPoints(points)
  };
}

function sortedBounds(polygons: Array<Array<[number, number]>>) {
  return polygons
    .map((points) => rectFromPoints(points))
    .sort((a, b) => a.x - b.x || a.y - b.y || a.width - b.width || a.height - b.height);
}

describe("floorplan room edit geometry", () => {
  it("normalizes rectangles and derives bounds from points", () => {
    expect(normalizeRect({ x: 30, y: 40, width: -20, height: -10 })).toEqual({
      x: 10,
      y: 30,
      width: 20,
      height: 10
    });
    expect(rectFromPoints([[10, 20], [40, 10], [30, 80]])).toEqual({
      x: 10,
      y: 10,
      width: 30,
      height: 70
    });
    expect(rectPointsFromBounds({ x: 1, y: 2, width: 3, height: 4 })).toEqual([[1, 2], [4, 2], [4, 6], [1, 6]]);
    expect(roundPoint(12.345)).toBe(12.35);
  });

  it("splits a rectangular room with a vertical line", () => {
    const result = splitRoomCandidateByLine(rectCandidate(0, 0, 100, 80), {
      x1: 50,
      y1: -20,
      x2: 50,
      y2: 120
    });

    expect(result).not.toBeNull();
    expect(result).toHaveLength(2);
    expect(sortedBounds(result ?? [])).toEqual([
      { x: 0, y: 0, width: 50, height: 80 },
      { x: 50, y: 0, width: 50, height: 80 }
    ]);
  });

  it("splits a polygon room with a horizontal line", () => {
    const result = splitRoomCandidateByLine(polygonCandidate([[0, 0], [120, 0], [120, 80], [0, 80]]), {
      x1: -20,
      y1: 40,
      x2: 140,
      y2: 40
    });

    expect(result).not.toBeNull();
    expect(result).toHaveLength(2);
    expect(sortedBounds(result ?? [])).toEqual([
      { x: 0, y: 0, width: 120, height: 40 },
      { x: 0, y: 40, width: 120, height: 40 }
    ]);
  });

  it("rejects split lines that do not cross the room", () => {
    const result = splitRoomCandidateByLine(rectCandidate(0, 0, 100, 80), {
      x1: 150,
      y1: -20,
      x2: 150,
      y2: 120
    });

    expect(result).toBeNull();
  });

  it("merges adjacent rectangular rooms into a single outline", () => {
    const points = mergeRoomCandidatePoints(rectCandidate(0, 0, 100, 80), rectCandidate(100, 0, 100, 80));

    expect(points.length).toBeGreaterThanOrEqual(4);
    expect(rectFromPoints(points)).toEqual({
      x: 0,
      y: 0,
      width: 200,
      height: 80
    });
  });
});
