import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../../../core/floorplan/floorplan-storage";
import type { WebDeviceConfig } from "../../../core/types";
import {
  addManualDoorExitZones,
  synchronizeManualDoorExitZones
} from "../manual-floorplan-exit-zones";
import type { ManualFloorplanDoor } from "../manual-floorplan-doors";

const labels = {
  zoneLabel: (index: string) => `Zone ${index}`,
  exitPointLabel: (index: string) => `Exit Point ${index}`
};

function document(): FloorplanStorageDocument {
  return {
    version: 1,
    image: { path: "/floorplan.webp", mime: "image/webp", width: 400, height: 300 },
    scale: {
      widthMm: 4000,
      heightMm: 3000,
      outerBoundsPx: [0, 0, 400, 300],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: { originPx: [100, 200], rotationDeg: 0, scale: 1, poseVersion: 1 },
    rooms: [],
    occlusion: { ignoredEdges: [] }
  };
}

function door(): ManualFloorplanDoor {
  return {
    id: "door_1",
    kind: "hinged",
    centerPx: [100, 100],
    widthPx: 100,
    rotationDeg: 0
  };
}

function config(): WebDeviceConfig {
  return { version: 1, zones: [], calibrationZones: [] };
}

describe("manual floorplan door exit zones", () => {
  it("maps each door opening into the existing radar-local exit zone format", () => {
    const result = addManualDoorExitZones(config(), document(), [door()], labels);

    expect(result).toMatchObject({ reason: null, added: 1 });
    expect(result.config.zones[0]).toEqual({
      id: "zone_1",
      name: "Exit Point 1",
      type: "exit",
      shape: "polygon",
      points: [[-500, 1150], [500, 1150], [500, 850], [-500, 850]]
    });
  });

  it("is idempotent and refuses to silently discard doors at the six-zone limit", () => {
    const first = addManualDoorExitZones(config(), document(), [door()], labels);
    expect(addManualDoorExitZones(first.config, document(), [door()], labels).added).toBe(0);

    const full = {
      ...config(),
      zones: Array.from({ length: 6 }, (_, index) => ({
        id: `zone_${index + 1}`,
        name: `Zone ${index + 1}`,
        type: "detection" as const,
        shape: "rect" as const,
        points: [[0, 0], [10, 0], [10, 10], [0, 10]] as Array<[number, number]>
      }))
    };
    expect(addManualDoorExitZones(full, document(), [door()], labels).reason).toBe("zone_limit");
  });

  it("moves and removes only the exit zones generated from edited doors", () => {
    const initial = addManualDoorExitZones(config(), document(), [door()], labels);
    const unrelated = {
      id: "zone_2",
      name: "Manual exit",
      type: "exit" as const,
      shape: "polygon" as const,
      points: [[10, 10], [20, 10], [20, 20], [10, 20]] as Array<[number, number]>
    };
    const movedDoor = { ...door(), centerPx: [200, 100] as [number, number] };
    const moved = synchronizeManualDoorExitZones(
      { ...initial.config, zones: [...initial.config.zones, unrelated] },
      document(),
      document(),
      [door()],
      [movedDoor],
      labels
    );

    expect(moved).toMatchObject({ reason: null, added: 0, updated: 1, removed: 0 });
    expect(moved.config.zones.find((zone) => zone.id === "zone_1")?.points)
      .toEqual([[500, 1150], [1500, 1150], [1500, 850], [500, 850]]);
    expect(moved.config.zones).toContainEqual(unrelated);

    const removed = synchronizeManualDoorExitZones(
      moved.config,
      document(),
      document(),
      [movedDoor],
      [],
      labels
    );
    expect(removed).toMatchObject({ reason: null, added: 0, updated: 0, removed: 1 });
    expect(removed.config.zones).toEqual([unrelated]);
  });

  it("finds the previous generated zone in the previous radar coordinate frame", () => {
    const previousDocument = document();
    const initial = addManualDoorExitZones(config(), previousDocument, [door()], labels);
    const nextDocument = {
      ...previousDocument,
      radar: { ...previousDocument.radar, originPx: [150, 200] as [number, number] }
    };

    const synchronized = synchronizeManualDoorExitZones(
      initial.config,
      previousDocument,
      nextDocument,
      [door()],
      [door()],
      labels
    );

    expect(synchronized).toMatchObject({ reason: null, added: 0, updated: 1, removed: 0 });
    expect(synchronized.config.zones).toHaveLength(1);
    expect(synchronized.config.zones[0].id).toBe("zone_1");
  });
});
