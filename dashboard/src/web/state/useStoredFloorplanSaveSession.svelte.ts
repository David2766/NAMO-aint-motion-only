import type {
  FloorplanStorageDocument,
  FloorplanStorageObject,
  FloorplanStorageOcclusion,
  FloorplanStorageRoom,
  FloorplanStorageScale
} from "../../core/floorplan/floorplan-storage";
import type { FloorplanWallSegment, RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { WebDeviceConfig } from "../types";
import {
  buildCurrentStoredFloorplanDocument,
  buildStoredFloorplanRooms
} from "../floorplan/stored-floorplan-document";
import {
  saveFloorplanObjectsPatch,
  saveFloorplanOcclusionPatch,
  saveFloorplanRoomNamePatch,
  saveFloorplanStorageDocument,
  type FloorplanStorageClientOptions
} from "../floorplan/floorplan-storage-client";
import { deviceStorageQueue } from "../api/device-storage-queue";
import {
  saveStoredFloorplanWithConfig,
  type StoredFloorplanSaveScopes
} from "./floorplan-save-coordinator";
import type { StoredFloorplanDirtyScope } from "./useStoredFloorplanEditSession.svelte";

type SaveTone = "idle" | "saving" | "ok" | "error";
type UpdateDeviceConfig = (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;

interface StoredFloorplanSaveEditSession {
  saveBusy: boolean;
  saveStatus: string;
  saveTone: SaveTone;
  readonly roomNamePatches: Record<string, string>;
  isDirty(scope: StoredFloorplanDirtyScope): boolean;
  clearDirty(): void;
}

interface StoredFloorplanSaveText {
  storedSaveInvalid: string;
  storedSaveInProgress: string;
  storedSaveDone: string;
}

interface StoredFloorplanSaveSessionOptions {
  getDocument: () => FloorplanStorageDocument | null;
  setDocument: (document: FloorplanStorageDocument) => void;
  getVisibleRoomCandidates: () => RoomCandidate[];
  getObjects: () => unknown[];
  getWallSegments: () => FloorplanWallSegment[];
  getEditSession: () => StoredFloorplanSaveEditSession | undefined;
  getText: () => StoredFloorplanSaveText;
  isScaleInputValid: () => boolean;
  isSaveBlocked: () => boolean;
  getClientOptions: () => FloorplanStorageClientOptions;
  updateDeviceConfig?: UpdateDeviceConfig;
  saveDeviceConfig?: () => Promise<void>;
  beforeSharedSave?: () => Promise<void>;
  publishSharedFloorplan?: (document: FloorplanStorageDocument) => Promise<void>;
  errorMessage: (error: unknown) => string;
  touchTolerancePx: number;
  runStorageTask?: <T>(task: () => Promise<T>) => Promise<T>;
  saveDocument?: (document: FloorplanStorageDocument, options: FloorplanStorageClientOptions) => Promise<void>;
  saveRoomName?: (roomId: string, name: string, options: FloorplanStorageClientOptions) => Promise<void>;
  saveOcclusion?: (occlusion: FloorplanStorageOcclusion, options: FloorplanStorageClientOptions) => Promise<void>;
  saveObjects?: (objects: FloorplanStorageObject[], options: FloorplanStorageClientOptions) => Promise<void>;
}

export function createStoredFloorplanSaveSession({
  getDocument,
  setDocument,
  getVisibleRoomCandidates,
  getObjects,
  getWallSegments,
  getEditSession,
  getText,
  isScaleInputValid,
  isSaveBlocked,
  getClientOptions,
  updateDeviceConfig,
  saveDeviceConfig,
  beforeSharedSave,
  publishSharedFloorplan,
  errorMessage,
  touchTolerancePx,
  runStorageTask = (task) => deviceStorageQueue.run("floorplan", task),
  saveDocument = saveFloorplanStorageDocument,
  saveRoomName = saveFloorplanRoomNamePatch,
  saveOcclusion = saveFloorplanOcclusionPatch,
  saveObjects = saveFloorplanObjectsPatch
}: StoredFloorplanSaveSessionOptions) {
  function currentRooms(scale: FloorplanStorageScale | null | undefined = getDocument()?.scale): FloorplanStorageRoom[] {
    const document = getDocument();
    return buildStoredFloorplanRooms({
      candidates: getVisibleRoomCandidates(),
      scale,
      previousRooms: document?.rooms ?? [],
      touchTolerancePx
    });
  }

  function currentDocument(): FloorplanStorageDocument | null {
    const document = getDocument();
    return buildCurrentStoredFloorplanDocument({
      document,
      candidates: getVisibleRoomCandidates(),
      scale: document?.scale,
      previousRooms: document?.rooms ?? [],
      touchTolerancePx,
      objects: getObjects(),
      wallSegments: getWallSegments()
    });
  }

  function canSave(): boolean {
    return Boolean(getDocument() && isScaleInputValid() && !isSaveBlocked());
  }

  async function save(): Promise<void> {
    const edit = getEditSession();
    if (!edit || isSaveBlocked()) return;
    const document = currentDocument();
    if (!document || !isScaleInputValid()) {
      edit.saveStatus = getText().storedSaveInvalid;
      edit.saveTone = "error";
      return;
    }

    edit.saveBusy = true;
    edit.saveTone = "saving";
    edit.saveStatus = getText().storedSaveInProgress;
    try {
      const scopes = saveScopes(edit);
      const sharedSave = hasSharedFloorplanChanges(scopes);
      if (sharedSave) await beforeSharedSave?.();
      const clientOptions = getClientOptions();
      const savedDocument = await saveStoredFloorplanWithConfig({
        document,
        saveScopes: scopes,
        roomNamePatches: edit.roomNamePatches,
        saveDocument: (nextDocument) => runStorageTask(() => saveDocument(nextDocument, clientOptions)),
        saveRoomName: (roomId, name) => runStorageTask(() => saveRoomName(roomId, name, clientOptions)),
        saveOcclusion: (occlusion) => runStorageTask(() => saveOcclusion(occlusion, clientOptions)),
        saveObjects: (objects) => runStorageTask(() => saveObjects(objects, clientOptions)),
        updateDeviceConfig: updateDeviceConfig ?? noopUpdateDeviceConfig,
        saveDeviceConfig: saveDeviceConfig ?? noopSaveDeviceConfig
      });
      if (sharedSave) await publishSharedFloorplan?.(savedDocument);
      setDocument(savedDocument);
      edit.clearDirty();
      edit.saveTone = "ok";
      edit.saveStatus = getText().storedSaveDone;
    } catch (error) {
      edit.saveTone = "error";
      edit.saveStatus = errorMessage(error);
    } finally {
      edit.saveBusy = false;
    }
  }

  return {
    currentRooms,
    currentDocument,
    canSave,
    save
  };
}

export function hasSharedFloorplanChanges(scopes: StoredFloorplanSaveScopes): boolean {
  return Boolean(scopes.document || scopes.roomName || scopes.occlusion || scopes.objects || scopes.automationAreas);
}

function saveScopes(edit: StoredFloorplanSaveEditSession): StoredFloorplanSaveScopes {
  return {
    document: edit.isDirty("document"),
    coordinateFrame: edit.isDirty("coordinateFrame"),
    config: edit.isDirty("config"),
    roomContext: edit.isDirty("roomContext"),
    radar: edit.isDirty("radar"),
    roomName: edit.isDirty("roomName"),
    occlusion: edit.isDirty("occlusion"),
    objects: edit.isDirty("objects"),
    automationAreas: edit.isDirty("automationAreas")
  };
}

function noopUpdateDeviceConfig(): void {
}

async function noopSaveDeviceConfig(): Promise<void> {
}
