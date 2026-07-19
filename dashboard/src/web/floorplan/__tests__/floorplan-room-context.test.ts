import { describe, expect, it } from "vitest";
import { MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS } from "../../../core/constants";
import type { FloorplanStorageDocument } from "../../../core/floorplan/floorplan-storage";
import { buildFloorplanRoomContext, clipPolygon, findRadarRoom, findRoomContainingPoint } from "../floorplan-room-context";

function documentWithRoom(
  pointsPx: Array<[number, number]>,
  originPx: [number, number] = [100, 180],
  overrides: Partial<FloorplanStorageDocument["radar"]> = {}
): FloorplanStorageDocument {
  return {
    version: 1,
    image: {
      path: "/floorplan.webp",
      mime: "image/webp",
      width: 400,
      height: 400
    },
    scale: {
      widthMm: 4000,
      heightMm: 4000,
      outerBoundsPx: [0, 0, 400, 400],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: {
      originPx,
      rotationDeg: 0,
      scale: 1,
      ...overrides
    },
    rooms: [
      {
        id: "room_1",
        name: "Bedroom",
        kind: "room",
        pointsPx
      }
    ],
    occlusion: {
      ignoredEdges: []
    }
  };
}

describe("floorplan room context", () => {
  it("selects the room containing the radar origin", () => {
    const document = documentWithRoom([[20, 20], [220, 20], [220, 260], [20, 260]]);

    expect(findRadarRoom(document)?.id).toBe("room_1");
  });

  it("selects the room containing a live radar placement point", () => {
    const rooms = [
      {
        id: "left",
        pointsPx: [[0, 0], [100, 0], [100, 100], [0, 100]] as Array<[number, number]>
      },
      {
        id: "right",
        pointsPx: [[120, 0], [220, 0], [220, 100], [120, 100]] as Array<[number, number]>
      }
    ];

    expect(findRoomContainingPoint(rooms, { x: 150, y: 50 }, (room) => room.pointsPx)?.id).toBe("right");
  });

  it("treats a radar placement on a room boundary as inside that room", () => {
    const rooms = [{
      id: "room_1",
      pointsPx: [[20, 20], [220, 20], [220, 260], [20, 260]] as Array<[number, number]>
    }];

    expect(findRoomContainingPoint(rooms, { x: 220, y: 120 }, (room) => room.pointsPx)?.id).toBe("room_1");
  });

  it("builds a compact radar-space boundary for the selected room", () => {
    const document = documentWithRoom([[20, 20], [220, 20], [220, 260], [20, 260]]);

    const context = buildFloorplanRoomContext(document);

    expect(context).toMatchObject({
      id: "room_1",
      name: "Bedroom",
      source: "stored_room"
    });
    expect(context?.boundary?.length).toBeGreaterThanOrEqual(3);
    expect(context?.boundary?.length).toBeLessThanOrEqual(MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS);
  });

  it("omits room context when the radar origin is outside every room", () => {
    const document = documentWithRoom([[20, 20], [120, 20], [120, 120], [20, 120]], [300, 300]);

    expect(buildFloorplanRoomContext(document)).toBeUndefined();
  });

  it("keeps room identity and omits boundary when coverage overlap is too small", () => {
    const document = documentWithRoom([[99.8, 99.8], [100.2, 99.8], [100.2, 100.2], [99.8, 100.2]], [100, 100]);

    const context = buildFloorplanRoomContext(document);

    expect(context).toMatchObject({
      id: "room_1",
      name: "Bedroom",
      source: "stored_room"
    });
    expect(context?.boundary).toBeUndefined();
  });

  it("clips a room that only partially overlaps the radar coverage polygon", () => {
    const subject = [
      { x: -10, y: 0 },
      { x: 10, y: 0 },
      { x: 10, y: 10 },
      { x: -10, y: 10 }
    ];
    const clip = [
      { x: 0, y: -5 },
      { x: 15, y: -5 },
      { x: 15, y: 15 },
      { x: 0, y: 15 }
    ];

    const clipped = clipPolygon(subject, clip);

    expect(clipped.length).toBeGreaterThanOrEqual(4);
    expect(clipped.every((point) => point.x >= -0.001 && point.x <= 10.001)).toBe(true);
    expect(clipped.every((point) => point.y >= -0.001 && point.y <= 10.001)).toBe(true);
  });

  it("keeps generated boundaries within the configured point limit", () => {
    const manyPointRoom: Array<[number, number]> = [
      [40, 80],
      [80, 50],
      [130, 44],
      [185, 52],
      [235, 86],
      [258, 145],
      [248, 210],
      [206, 260],
      [142, 280],
      [78, 248],
      [42, 188],
      [30, 128]
    ];
    const document = documentWithRoom(manyPointRoom, [130, 150]);

    const context = buildFloorplanRoomContext(document);

    expect(context?.boundary?.length).toBeGreaterThanOrEqual(3);
    expect(context?.boundary?.length).toBeLessThanOrEqual(MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS);
  });
});
