import {
  advanceFloorplanAutomationAreasRevision,
  advanceFloorplanGeometryVersion,
  advanceFloorplanPoseVersion,
  ensureFloorplanStorageIdentity,
  type FloorplanStorageDocument,
  type FloorplanStorageOcclusion,
  type FloorplanStorageObject,
} from "../../core/floorplan/floorplan-storage";
import type { WebDeviceConfig } from "../types";
import { buildFloorplanRoomContext } from "../floorplan/floorplan-room-context";

type UpdateDeviceConfig = (mutator: (current: WebDeviceConfig) => WebDeviceConfig) => void;

interface StoredFloorplanSaveOptions {
  document: FloorplanStorageDocument;
  saveScopes?: StoredFloorplanSaveScopes;
  roomNamePatches?: Record<string, string>;
  saveDocument: (document: FloorplanStorageDocument) => Promise<void>;
  saveRoomName?: (roomId: string, name: string) => Promise<void>;
  saveOcclusion?: (occlusion: FloorplanStorageOcclusion) => Promise<void>;
  saveObjects?: (objects: FloorplanStorageObject[]) => Promise<void>;
  updateDeviceConfig: UpdateDeviceConfig;
  saveDeviceConfig: () => Promise<void>;
}

export interface StoredFloorplanSaveScopes {
  document?: boolean;
  coordinateFrame?: boolean;
  config?: boolean;
  roomContext?: boolean;
  radar?: boolean;
  roomName?: boolean;
  occlusion?: boolean;
  objects?: boolean;
  automationAreas?: boolean;
}

type StoredFloorplanRoomContext = NonNullable<NonNullable<WebDeviceConfig["floorplan"]>["room"]>;

export function withStoredFloorplanAvailability(
  current: WebDeviceConfig,
  hasImage: boolean,
  room: StoredFloorplanRoomContext | undefined
): WebDeviceConfig {
  return {
    ...current,
    floorplan: {
      ...(current.floorplan ?? {}),
      enabled: hasImage,
      hasImage,
      room
    }
  };
}

export function withStoredFloorplanRoomContext(
  current: WebDeviceConfig,
  document: FloorplanStorageDocument
): WebDeviceConfig {
  return {
    ...current,
    floorplan: {
      ...(current.floorplan ?? {}),
      enabled: true,
      hasImage: current.floorplan?.hasImage ?? true,
      scaleMmPerPx: (document.scale.mmPerPxX + document.scale.mmPerPxY) / 2,
      scaleMmPerPxX: document.scale.mmPerPxX,
      scaleMmPerPxY: document.scale.mmPerPxY,
      radar: {
        originX: document.radar.originPx[0],
        originY: document.radar.originPx[1],
        rotation: document.radar.rotationDeg,
        scale: document.radar.scale,
        poseVersion: document.radar.poseVersion
      },
      room: buildFloorplanRoomContext(document)
    }
  };
}

export async function saveStoredFloorplanWithConfig({
  document,
  saveScopes,
  roomNamePatches = {},
  saveDocument,
  saveRoomName,
  saveOcclusion,
  saveObjects,
  updateDeviceConfig,
  saveDeviceConfig
}: StoredFloorplanSaveOptions): Promise<FloorplanStorageDocument> {
  const scopes = saveScopes ?? { document: true, config: true, roomContext: true };
  const preparedDocument = prepareFloorplanDocumentForSave(document, scopes);
  const identityMigrationRequired = preparedDocument.floorplanId !== document.floorplanId ||
    preparedDocument.geometryVersion !== document.geometryVersion ||
    preparedDocument.version !== document.version;
  const needsPartialDocumentSave = scopes.roomName || scopes.occlusion || scopes.objects;
  const canPatchDocument =
    !identityMigrationRequired &&
    (!scopes.roomName || saveRoomName) &&
    (!scopes.occlusion || saveOcclusion) &&
    (!scopes.objects || saveObjects);

  if (scopes.document || scopes.automationAreas || identityMigrationRequired || (needsPartialDocumentSave && !canPatchDocument)) {
    await saveDocument(preparedDocument);
  } else {
    if (scopes.roomName) {
      for (const [roomId, name] of Object.entries(roomNamePatches)) {
        await saveRoomName?.(roomId, name);
      }
    }
    if (scopes.occlusion) await saveOcclusion?.(preparedDocument.occlusion);
    if (scopes.objects) await saveObjects?.(preparedDocument.objects ?? []);
  }
  if (scopes.radar || scopes.roomContext) {
    updateDeviceConfig((current) => withStoredFloorplanRoomContext(current, preparedDocument));
  }
  if (scopes.config || scopes.radar || scopes.roomContext) await saveDeviceConfig();
  return preparedDocument;
}

export function prepareFloorplanDocumentForSave(
  document: FloorplanStorageDocument,
  scopes: StoredFloorplanSaveScopes
): FloorplanStorageDocument {
  let prepared = ensureFloorplanStorageIdentity(document);
  if (scopes.coordinateFrame) prepared = advanceFloorplanGeometryVersion(prepared);
  if (scopes.automationAreas) prepared = advanceFloorplanAutomationAreasRevision(prepared);
  if (scopes.radar) prepared = advanceFloorplanPoseVersion(prepared);
  return prepared;
}
