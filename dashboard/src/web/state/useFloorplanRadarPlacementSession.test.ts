import { describe, expect, it, vi } from "vitest";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { WebDeviceConfig } from "../../core/types";
import type { FloorplanOcclusionSegment } from "../floorplan/floorplan-occlusion";
import { createFloorplanRadarPlacementSession } from "./useFloorplanRadarPlacementSession.svelte";

function roomFixture(overrides: Partial<RoomCandidate> = {}): RoomCandidate {
  const points = overrides.points ?? [[0, 0], [100, 0], [100, 100], [0, 100]] as Array<[number, number]>;
  return {
    id: "room_1",
    name: "Room 1",
    kind: "unknown",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 100, height: 100 },
    points,
    ...overrides
  };
}

function createSession(options: {
  config?: WebDeviceConfig | null;
  rooms?: RoomCandidate[];
  physicalWalls?: FloorplanOcclusionSegment[];
  onUpdate?: (updater: (current: WebDeviceConfig) => WebDeviceConfig) => void;
} = {}) {
  let config = options.config ?? null;
  const emptyConfig: WebDeviceConfig = { version: 1, zones: [] };
  const onUpdate = options.onUpdate ?? vi.fn((updater: (current: WebDeviceConfig) => WebDeviceConfig) => {
    config = updater(config ?? emptyConfig);
  });
  const session = createFloorplanRadarPlacementSession({
    getDeviceConfig: () => config,
    getScaleEstimate: () => ({
      outerBounds: { x: 10, y: 20, width: 200, height: 120 },
      widthMm: 4000,
      heightMm: 2400,
      mmPerPxX: 20,
      mmPerPxY: 20
    }),
    getOuterBounds: () => ({ x: 10, y: 20, width: 200, height: 120 }),
    getImageSize: () => ({ width: 300, height: 200 }),
    getConfirmedRoomCandidates: () => options.rooms ?? [roomFixture()],
    getPhysicalWallSegments: () => options.physicalWalls ?? [],
    onUpdateDeviceConfig: onUpdate
  });
  return {
    session,
    onUpdate,
    get config() {
      return config;
    }
  };
}

describe("floorplan radar placement session", () => {
  it("uses the saved radar config when initializing", () => {
    const { session } = createSession({
      config: {
        version: 1,
        zones: [],
        floorplan: {
          enabled: true,
          hasImage: true,
          radarOcclusionIgnoredEdges: ["wall:a"],
          radar: {
            originX: 12,
            originY: 34,
            rotation: 178,
            scale: 1.2
          }
        }
      }
    });

    session.initialize();

    expect(session.placement).toEqual({ originX: 12, originY: 34, rotation: 178 });
    expect(session.scalePercent).toBe(105);
    expect(session.ignoredOcclusionEdges).toEqual(["wall:a"]);
  });

  it("falls back to the scale outer bounds for default placement", () => {
    const { session } = createSession();

    expect(session.defaultPlacement()).toEqual({
      originX: 110,
      originY: 124,
      rotation: 0
    });
  });

  it("commits rounded placement and scale into device config", () => {
    const harness = createSession();
    const { session } = harness;

    session.updateScalePercent("103");
    session.commitPlacement({
      originX: 12.345,
      originY: 67.891,
      rotation: 179.86
    });

    expect(harness.config?.floorplan).toMatchObject({
      enabled: true,
      hasImage: true,
      scaleMmPerPx: 20,
      scaleMmPerPxX: 20,
      scaleMmPerPxY: 20,
      radar: {
        originX: 12.35,
        originY: 67.89,
        rotation: 179.9,
        scale: 1.03
      }
    });
  });

  it("toggles occlusion edges through the existing grouping helper", () => {
    const harness = createSession({
      rooms: [roomFixture({ points: [[0, 0], [250, 0], [250, 200], [0, 200]] })]
    });
    const { session } = harness;
    session.initialize();
    const [segment] = session.boundarySegments();

    session.toggleOcclusionEdge(segment);

    expect(harness.config?.floorplan?.radarOcclusionIgnoredEdges).toEqual([segment.occlusionKey]);
  });

  it("returns only the boundary segments for the room containing the radar", () => {
    const roomA = roomFixture({ id: "a", points: [[0, 0], [100, 0], [100, 100], [0, 100]] });
    const roomB = roomFixture({ id: "b", points: [[120, 0], [220, 0], [220, 100], [120, 100]] });
    const { session } = createSession({ rooms: [roomA, roomB] });

    const segments = session.visibleRoomWallSegmentsForPlacement({ originX: 130, originY: 40, rotation: 0 }, "room");

    expect(segments).toHaveLength(4);
    expect(segments.every((segment) => segment.id.startsWith("room-b-"))).toBe(true);
  });

  it("returns no room wall segments when the radar is outside all confirmed rooms", () => {
    const { session } = createSession();

    expect(session.visibleRoomWallSegmentsForPlacement({ originX: 999, originY: 999, rotation: 0 }, "room")).toEqual([]);
  });

  it("uses physical walls instead of logical room boundaries when available", () => {
    const physicalWall: FloorplanOcclusionSegment = {
      id: "physical-1",
      occlusionKey: "wall:50,0-50,100",
      axis: "vertical",
      x1: 50,
      y1: 0,
      x2: 50,
      y2: 100
    };
    const { session } = createSession({ physicalWalls: [physicalWall] });

    expect(session.boundarySegments()).toEqual([physicalWall]);
    expect(session.visibleRoomWallSegmentsForPlacement({ originX: 999, originY: 999, rotation: 0 }, "room"))
      .toEqual([physicalWall]);
  });

  it("resets all occlusion exceptions through the same config path", () => {
    const harness = createSession({
      config: {
        version: 1,
        zones: [],
        floorplan: {
          enabled: true,
          hasImage: true,
          radarOcclusionIgnoredEdges: ["wall:a"]
        }
      }
    });
    harness.session.initialize();

    harness.session.resetOcclusionEdges();

    expect(harness.config?.floorplan?.radarOcclusionIgnoredEdges).toEqual([]);
  });
});
