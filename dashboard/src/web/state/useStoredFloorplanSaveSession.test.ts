import { describe, expect, it, vi } from "vitest";
import type {
  FloorplanStorageDocument,
  FloorplanStorageObject,
  FloorplanStorageOcclusion
} from "../../core/floorplan/floorplan-storage";
import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { WebDeviceConfig } from "../types";
import type { FloorplanStorageClientOptions } from "../floorplan/floorplan-storage-client";
import { createStoredFloorplanSaveSession } from "./useStoredFloorplanSaveSession.svelte";
import type { StoredFloorplanDirtyScope } from "./useStoredFloorplanEditSession.svelte";

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
    rooms: [{
      id: "room_1",
      name: "Bedroom",
      kind: "room",
      pointsPx: [[0, 0], [100, 0], [100, 120], [0, 120]],
      widthMm: 1000,
      heightMm: 1200,
      manualSize: false
    }],
    occlusion: {
      ignoredEdges: []
    },
    objects: []
  };
}

function candidateFixture(): RoomCandidate {
  return {
    id: "room_1",
    name: "Bedroom",
    kind: "room",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: { x: 0, y: 0, width: 100, height: 120 },
    points: [[0, 0], [100, 0], [100, 120], [0, 120]]
  };
}

function editFixture(scopes: StoredFloorplanDirtyScope[] = ["document"]) {
  return {
    saveBusy: false,
    saveStatus: "",
    saveTone: "idle" as const,
    roomNamePatches: {
      room_1: "Living"
    },
    isDirty: (scope: StoredFloorplanDirtyScope) => scopes.includes(scope),
    clearDirty: vi.fn()
  };
}

function createHarness(options: {
  document?: FloorplanStorageDocument | null;
  candidates?: RoomCandidate[];
  scopes?: StoredFloorplanDirtyScope[];
  scaleValid?: boolean;
  saveBlocked?: boolean;
  saveDocument?: (document: FloorplanStorageDocument, options: FloorplanStorageClientOptions) => Promise<void>;
  saveRoomName?: (roomId: string, name: string, options: FloorplanStorageClientOptions) => Promise<void>;
  saveOcclusion?: (occlusion: FloorplanStorageOcclusion, options: FloorplanStorageClientOptions) => Promise<void>;
  saveObjects?: (objects: FloorplanStorageObject[], options: FloorplanStorageClientOptions) => Promise<void>;
  saveDeviceConfig?: () => Promise<void>;
  beforeSharedSave?: () => Promise<void>;
  publishSharedFloorplan?: (document: FloorplanStorageDocument) => Promise<void>;
  updateDeviceConfig?: (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;
} = {}) {
  let document = options.document === undefined ? documentFixture() : options.document;
  const edit = editFixture(options.scopes);
  const calls: string[] = [];
  let config: WebDeviceConfig = { version: 1, zones: [], calibrationZones: [] };
  const session = createStoredFloorplanSaveSession({
    getDocument: () => document,
    setDocument: (nextDocument) => {
      document = nextDocument;
      calls.push("setDocument");
    },
    getVisibleRoomCandidates: () => options.candidates ?? [candidateFixture()],
    getObjects: () => [{
      id: "object_1",
      asset: "desk",
      xPx: 10,
      yPx: 20,
      widthPx: 30,
      heightPx: 40,
      rotationDeg: 0
    }],
    getWallSegments: () => [],
    getEditSession: () => edit,
    getText: () => ({
      storedSaveInvalid: "invalid",
      storedSaveInProgress: "saving",
      storedSaveDone: "done"
    }),
    isScaleInputValid: () => options.scaleValid ?? true,
    isSaveBlocked: () => options.saveBlocked ?? false,
    getClientOptions: () => ({ baseUrl: "/device" }),
    updateDeviceConfig: options.updateDeviceConfig ?? vi.fn((mutator: (current: WebDeviceConfig) => WebDeviceConfig) => {
      calls.push("updateDeviceConfig");
      config = mutator(config);
    }),
    saveDeviceConfig: options.saveDeviceConfig ?? vi.fn(async () => {
      calls.push("saveDeviceConfig");
    }),
    beforeSharedSave: options.beforeSharedSave,
    publishSharedFloorplan: options.publishSharedFloorplan,
    errorMessage: (error) => error instanceof Error ? error.message : "error",
    touchTolerancePx: 8,
    runStorageTask: async (task) => {
      calls.push("queue");
      return task();
    },
    saveDocument: options.saveDocument ?? vi.fn(async () => {
      calls.push("saveDocument");
    }),
    saveRoomName: options.saveRoomName ?? vi.fn(async () => {
      calls.push("saveRoomName");
    }),
    saveOcclusion: options.saveOcclusion ?? vi.fn(async () => {
      calls.push("saveOcclusion");
    }),
    saveObjects: options.saveObjects ?? vi.fn(async () => {
      calls.push("saveObjects");
    })
  });
  return {
    session,
    edit,
    calls,
    get document() {
      return document;
    },
    get config() {
      return config;
    }
  };
}

describe("stored floorplan save session", () => {
  it("builds the current stored floorplan document from rooms, objects, and wall segments", () => {
    const { session } = createHarness();

    const document = session.currentDocument();

    expect(document?.rooms).toHaveLength(1);
    expect(document?.objects).toEqual([expect.objectContaining({ id: "object_1", roomId: "room_1" })]);
  });

  it("reports save availability from document, scale validity, and storage busy state", () => {
    expect(createHarness().session.canSave()).toBe(true);
    expect(createHarness({ document: null }).session.canSave()).toBe(false);
    expect(createHarness({ scaleValid: false }).session.canSave()).toBe(false);
    expect(createHarness({ saveBlocked: true }).session.canSave()).toBe(false);
  });

  it("saves full document changes through the floorplan queue and clears dirty state", async () => {
    const harness = createHarness({ scopes: ["document", "roomContext"] });

    await harness.session.save();

    expect(harness.calls).toEqual([
      "queue",
      "saveDocument",
      "updateDeviceConfig",
      "saveDeviceConfig",
      "setDocument"
    ]);
    expect(harness.config.floorplan?.room?.id).toBe("room_1");
    expect(harness.edit.clearDirty).toHaveBeenCalled();
    expect(harness.edit.saveTone).toBe("ok");
    expect(harness.edit.saveStatus).toBe("done");
  });

  it("stores radar locally and uses patch callbacks for shared scoped edits", async () => {
    const harness = createHarness({ scopes: ["radar", "roomName", "occlusion", "objects"] });

    await harness.session.save();

    expect(harness.calls).toEqual([
      "queue",
      "saveRoomName",
      "queue",
      "saveOcclusion",
      "queue",
      "saveObjects",
      "updateDeviceConfig",
      "saveDeviceConfig",
      "setDocument"
    ]);
    expect(harness.config.floorplan?.radar?.poseVersion).toBe(12);
  });

  it("checks ownership and publishes exactly once for shared changes", async () => {
    const beforeSharedSave = vi.fn(async () => undefined);
    const publishSharedFloorplan = vi.fn(async () => undefined);
    const harness = createHarness({
      scopes: ["document", "coordinateFrame", "objects"],
      beforeSharedSave,
      publishSharedFloorplan
    });

    await harness.session.save();

    expect(harness.calls).toEqual(["queue", "saveDocument", "setDocument"]);
    expect(beforeSharedSave).toHaveBeenCalledOnce();
    expect(publishSharedFloorplan).toHaveBeenCalledWith(
      expect.objectContaining({ geometryVersion: 8 })
    );
  });

  it("does not publish local radar-only changes", async () => {
    const beforeSharedSave = vi.fn(async () => undefined);
    const publishSharedFloorplan = vi.fn(async () => undefined);
    const harness = createHarness({
      scopes: ["radar"],
      beforeSharedSave,
      publishSharedFloorplan
    });

    await harness.session.save();

    expect(beforeSharedSave).not.toHaveBeenCalled();
    expect(publishSharedFloorplan).not.toHaveBeenCalled();
  });

  it("keeps shared changes dirty when manifest publication fails", async () => {
    const harness = createHarness({
      scopes: ["objects"],
      publishSharedFloorplan: vi.fn(async () => {
        throw new Error("publish failed");
      })
    });

    await harness.session.save();

    expect(harness.calls).toEqual(["queue", "saveObjects"]);
    expect(harness.edit.clearDirty).not.toHaveBeenCalled();
    expect(harness.edit.saveTone).toBe("error");
    expect(harness.edit.saveStatus).toBe("publish failed");
  });

  it("does not save and reports invalid state when the document cannot be built", async () => {
    const harness = createHarness({ document: null });

    await harness.session.save();

    expect(harness.calls).toEqual([]);
    expect(harness.edit.saveTone).toBe("error");
    expect(harness.edit.saveStatus).toBe("invalid");
  });

  it("keeps dirty state and reports the error when saving fails", async () => {
    const harness = createHarness({
      saveDocument: vi.fn(async () => {
        throw new Error("failed");
      })
    });

    await harness.session.save();

    expect(harness.edit.clearDirty).not.toHaveBeenCalled();
    expect(harness.edit.saveBusy).toBe(false);
    expect(harness.edit.saveTone).toBe("error");
    expect(harness.edit.saveStatus).toBe("failed");
  });
});
