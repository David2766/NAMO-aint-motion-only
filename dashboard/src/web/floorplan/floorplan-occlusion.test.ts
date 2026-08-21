import { describe, expect, it } from "vitest";
import {
  buildBoundaryOcclusionSegments,
  nextIgnoredOcclusionEdges,
  radarVisiblePolygonPoints,
  wallSegmentGeometryKey
} from "./floorplan-occlusion";

describe("floorplan occlusion helpers", () => {
  it("builds stable wall keys independent of point order", () => {
    expect(wallSegmentGeometryKey(10, 20, 30, 20)).toBe("wall:10,20-30,20");
    expect(wallSegmentGeometryKey(30, 20, 10, 20)).toBe("wall:10,20-30,20");
  });

  it("builds room boundary segments with geometry keys", () => {
    const segments = buildBoundaryOcclusionSegments([{
      id: "room-a",
      segmentPrefix: "room",
      points: [[0, 0], [100, 0], [100, 80], [0, 80]]
    }]);

    expect(segments).toHaveLength(4);
    expect(segments[0]).toMatchObject({
      id: "room-room-a-0",
      occlusionKey: "wall:0,0-100,0",
      axis: "horizontal"
    });
    expect(segments[1]).toMatchObject({
      id: "room-room-a-1",
      occlusionKey: "wall:100,0-100,80",
      axis: "vertical"
    });
  });

  it("toggles all touching wall segments as one ignored group", () => {
    const segments = [
      {
        id: "a",
        occlusionKey: "wall:0,0-50,0",
        x1: 0,
        y1: 0,
        x2: 50,
        y2: 0,
        axis: "horizontal" as const
      },
      {
        id: "b",
        occlusionKey: "wall:50,0-100,0",
        x1: 50,
        y1: 0,
        x2: 100,
        y2: 0,
        axis: "horizontal" as const
      }
    ];

    const ignored = nextIgnoredOcclusionEdges([], segments[0], segments);
    expect(ignored).toEqual(["wall:0,0-50,0", "wall:50,0-100,0"]);

    expect(nextIgnoredOcclusionEdges(ignored, segments[1], segments)).toEqual([]);
  });

  it("stops radar visibility at the nearest physical wall", () => {
    const segments = buildBoundaryOcclusionSegments([{
      id: "outer",
      segmentPrefix: "wall",
      points: [[0, 0], [100, 0], [100, 100], [0, 100]]
    }]);

    expect(radarVisiblePolygonPoints(
      { x: 50, y: 50 },
      [{ x: 150, y: 50 }, { x: 50, y: -50 }],
      segments
    )).toEqual([
      { x: 50, y: 50 },
      { x: 100, y: 50 },
      { x: 50, y: 0 }
    ]);
  });

  it("lets a ray pass only when that physical wall is explicitly ignored", () => {
    const segments = buildBoundaryOcclusionSegments([{
      id: "outer",
      segmentPrefix: "wall",
      points: [[0, 0], [100, 0], [100, 100], [0, 100]]
    }]);
    const rightWall = segments.find((segment) => segment.x1 === 100 && segment.x2 === 100);

    expect(radarVisiblePolygonPoints(
      { x: 50, y: 50 },
      [{ x: 150, y: 50 }],
      segments,
      rightWall ? [rightWall.occlusionKey] : []
    )).toEqual([{ x: 50, y: 50 }, { x: 150, y: 50 }]);
  });
});
