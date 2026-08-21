import { describe, expect, it } from "vitest";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { WebDeviceConfig } from "../types";
import {
  prepareFloorplanDocumentForSave,
  saveStoredFloorplanWithConfig,
  withStoredFloorplanAvailability
} from "./floorplan-save-coordinator";

function documentFixture(): FloorplanStorageDocument {
  return {
    version: 2,
    floorplanId: "fp-00112233445566778899aabbccddeeff",
    geometryVersion: 7,
    image: {
      path: "/floorplan.webp",
      mime: "image/webp",
      width: 200,
      height: 120
    },
    scale: {
      widthMm: 2000,
      heightMm: 1200,
      outerBoundsPx: [0, 0, 200, 120],
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: {
      originPx: [50, 100],
      rotationDeg: 0,
      scale: 1,
      poseVersion: 11
    },
    rooms: [
      {
        id: "room_1",
        name: "Bedroom",
        kind: "room",
        pointsPx: [[0, 0], [100, 0], [100, 120], [0, 120]],
        widthMm: 1000,
        heightMm: 1200,
        manualSize: false
      }
    ],
    occlusion: {
      ignoredEdges: []
    },
    objects: []
  };
}

describe("floorplan save coordinator", () => {
  it("marks replicated floorplan storage available without replacing Node-local pose or zones", () => {
    const config: WebDeviceConfig = {
      version: 1,
      zones: [{
        id: "zone_1",
        name: "Local zone",
        type: "detection",
        shape: "rect",
        points: [[10, 20], [30, 40]]
      }],
      calibrationZones: [{
        id: "calibration_1",
        name: "Local filter",
        type: "filter",
        shape: "rect",
        points: [[50, 60], [70, 80]]
      }],
      floorplan: {
        enabled: false,
        hasImage: false,
        radar: {
          originX: 91,
          originY: 47,
          rotation: 135,
          scale: 0.8,
          poseVersion: 22
        }
      }
    };

    const updated = withStoredFloorplanAvailability(config, true, undefined);

    expect(updated.floorplan).toMatchObject({
      enabled: true,
      hasImage: true,
      radar: config.floorplan?.radar
    });
    expect(updated.floorplan?.room).toBeUndefined();
    expect(updated.zones).toBe(config.zones);
    expect(updated.calibrationZones).toBe(config.calibrationZones);
    expect(config.floorplan?.enabled).toBe(false);
  });

  it("does not invent a Node-local radar pose while enabling replicated storage", () => {
    const config: WebDeviceConfig = {
      version: 1,
      zones: [],
      calibrationZones: [],
      floorplan: {
        enabled: false,
        hasImage: false
      }
    };

    const updated = withStoredFloorplanAvailability(config, true, undefined);

    expect(updated.floorplan?.enabled).toBe(true);
    expect(updated.floorplan?.hasImage).toBe(true);
    expect(updated.floorplan?.radar).toBeUndefined();
    expect(updated.floorplan?.room).toBeUndefined();
  });

  it("advances coordinate-frame and radar pose revisions independently", () => {
    const document = documentFixture();

    const geometry = prepareFloorplanDocumentForSave(document, { document: true, coordinateFrame: true });
    expect(geometry.geometryVersion).toBe(8);
    expect(geometry.radar.poseVersion).toBe(11);

    const pose = prepareFloorplanDocumentForSave(document, { radar: true });
    expect(pose.geometryVersion).toBe(7);
    expect(pose.radar.poseVersion).toBe(12);
  });

  it("keeps calibration geometry stable for room-layout document edits", () => {
    const document = documentFixture();

    const roomLayout = prepareFloorplanDocumentForSave(document, { document: true, roomContext: true });

    expect(roomLayout.geometryVersion).toBe(7);
    expect(roomLayout.radar.poseVersion).toBe(11);
  });

  it("advances shared area revision without changing geometry or radar pose", () => {
    const document = documentFixture();
    document.automationAreas = {
      schemaVersion: 1,
      revision: 4,
      areas: [{
        areaId: "area_1",
        name: "Desk",
        spaceId: "room_1",
        enabled: true,
        pointsPx: [[10, 10], [30, 10], [30, 30], [10, 30]]
      }]
    };

    const prepared = prepareFloorplanDocumentForSave(document, { automationAreas: true });

    expect(prepared.automationAreas?.revision).toBe(5);
    expect(prepared.geometryVersion).toBe(7);
    expect(prepared.radar.poseVersion).toBe(11);
  });

  it("keeps newly created shared areas when assigning their first revision", () => {
    const document = documentFixture();
    document.automationAreas = {
      schemaVersion: 1,
      revision: 0,
      areas: [{
        areaId: "area_1",
        name: "Desk",
        spaceId: "room_1",
        enabled: true,
        pointsPx: [[10, 10], [30, 10], [30, 30], [10, 30]]
      }]
    };

    const prepared = prepareFloorplanDocumentForSave(document, { automationAreas: true });

    expect(prepared.automationAreas?.revision).toBe(1);
    expect(prepared.automationAreas?.areas).toEqual(document.automationAreas.areas);
  });

  it("persists legacy identity migration with a full document save", async () => {
    const legacy = documentFixture();
    delete legacy.floorplanId;
    delete legacy.geometryVersion;
    delete legacy.radar.poseVersion;
    legacy.version = 1;
    const savedDocuments: FloorplanStorageDocument[] = [];
    let config: WebDeviceConfig = { version: 1, zones: [], calibrationZones: [] };
    let configSaveCount = 0;

    const result = await saveStoredFloorplanWithConfig({
      document: legacy,
      saveScopes: { radar: true },
      saveDocument: async (document) => {
        savedDocuments.push(document);
      },
      updateDeviceConfig: (mutator) => {
        config = mutator(config);
      },
      saveDeviceConfig: async () => {
        configSaveCount += 1;
      }
    });

    expect(savedDocuments).toHaveLength(1);
    expect(savedDocuments[0].version).toBe(2);
    expect(savedDocuments[0].floorplanId).toMatch(/^fp-[0-9a-f]{32}$/);
    expect(savedDocuments[0].geometryVersion).toBe(1);
    expect(savedDocuments[0].radar.poseVersion).toBe(2);
    expect(result).toBe(savedDocuments[0]);
    expect(config.floorplan?.radar?.poseVersion).toBe(2);
    expect(configSaveCount).toBe(1);
  });

  it("saves the document before updating and saving config", async () => {
    const calls: string[] = [];
    let config: WebDeviceConfig = {
      version: 1,
      zones: [],
      calibrationZones: [],
      floorplan: {
        enabled: true,
        hasImage: true
      }
    };

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: (mutator) => {
        calls.push("updateDeviceConfig");
        config = mutator(config);
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveDocument", "updateDeviceConfig", "saveDeviceConfig"]);
    expect(config.floorplan?.room).toMatchObject({
      id: "room_1",
      name: "Bedroom",
      source: "stored_room"
    });
  });

  it("does not touch config when document save fails", async () => {
    const calls: string[] = [];
    const config: WebDeviceConfig = {
      version: 1,
      zones: [],
      calibrationZones: []
    };

    await expect(
      saveStoredFloorplanWithConfig({
        document: documentFixture(),
        saveDocument: async () => {
          calls.push("saveDocument");
          throw new Error("document failed");
        },
        updateDeviceConfig: () => {
          calls.push("updateDeviceConfig");
        },
        saveDeviceConfig: async () => {
          calls.push("saveDeviceConfig");
        }
      })
    ).rejects.toThrow("document failed");

    expect(calls).toEqual(["saveDocument"]);
    expect(config.floorplan).toBeUndefined();
  });

  it("saves only config for config-scoped changes", async () => {
    const calls: string[] = [];
    let config: WebDeviceConfig = {
      version: 1,
      zones: [],
      calibrationZones: [],
      floorplan: {
        enabled: true,
        hasImage: true
      }
    };

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        config: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: (mutator) => {
        calls.push("updateDeviceConfig");
        config = mutator(config);
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveDeviceConfig"]);
    expect(config.floorplan?.room).toBeUndefined();
  });

  it("saves only the floorplan document for document-scoped changes", async () => {
    const calls: string[] = [];

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        document: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: () => {
        calls.push("updateDeviceConfig");
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveDocument"]);
  });

  it("updates room context and saves config when requested by the dirty scope", async () => {
    const calls: string[] = [];
    let config: WebDeviceConfig = {
      version: 1,
      zones: [],
      calibrationZones: [],
      floorplan: {
        enabled: true,
        hasImage: true
      }
    };

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        document: true,
        roomContext: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: (mutator) => {
        calls.push("updateDeviceConfig");
        config = mutator(config);
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveDocument", "updateDeviceConfig", "saveDeviceConfig"]);
    expect(config.floorplan?.room?.id).toBe("room_1");
  });

  it("saves radar placement as Node-local device config without patching the shared document", async () => {
    const calls: string[] = [];
    let config: WebDeviceConfig = { version: 1, zones: [], calibrationZones: [] };

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        radar: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: (mutator) => {
        calls.push("updateDeviceConfig");
        config = mutator(config);
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["updateDeviceConfig", "saveDeviceConfig"]);
    expect(config.floorplan?.radar).toEqual({
      originX: 50,
      originY: 100,
      rotation: 0,
      scale: 1,
      poseVersion: 12
    });
    expect(config.floorplan).toMatchObject({
      scaleMmPerPx: 10,
      scaleMmPerPxX: 10,
      scaleMmPerPxY: 10
    });
  });

  it("saves room-name patches without uploading the full document", async () => {
    const calls: string[] = [];

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        roomName: true
      },
      roomNamePatches: {
        room_1: "Living"
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      saveRoomName: async (roomId, name) => {
        calls.push(`saveRoomName:${roomId}:${name}`);
      },
      updateDeviceConfig: () => {
        calls.push("updateDeviceConfig");
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveRoomName:room_1:Living"]);
  });

  it("does not require a floorplan patch callback for Node-local radar placement", async () => {
    const calls: string[] = [];

    await saveStoredFloorplanWithConfig({
      document: documentFixture(),
      saveScopes: {
        radar: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      updateDeviceConfig: () => {
        calls.push("updateDeviceConfig");
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["updateDeviceConfig", "saveDeviceConfig"]);
  });

  it("saves furniture objects with a patch when only object scope is dirty", async () => {
    const calls: string[] = [];
    const document = {
      ...documentFixture(),
      objects: [{
        id: "object_1",
        asset: "desk",
        xPx: 10,
        yPx: 20,
        widthPx: 30,
        heightPx: 40,
        rotationDeg: 0
      }]
    };

    await saveStoredFloorplanWithConfig({
      document,
      saveScopes: {
        objects: true
      },
      saveDocument: async () => {
        calls.push("saveDocument");
      },
      saveObjects: async (objects) => {
        calls.push(`saveObjects:${objects.length}`);
      },
      updateDeviceConfig: () => {
        calls.push("updateDeviceConfig");
      },
      saveDeviceConfig: async () => {
        calls.push("saveDeviceConfig");
      }
    });

    expect(calls).toEqual(["saveObjects:1"]);
  });
});
