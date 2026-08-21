import {
  ensureFloorplanStorageIdentity,
  sanitizeFloorplanAutomationAreas,
  type FloorplanStorageDocument,
  type FloorplanStorageDoor,
  type FloorplanStorageObject,
  type FloorplanStorageRoom,
  type FloorplanStorageScale
} from "../../core/floorplan/floorplan-storage";
import { pointInPolygon } from "../../core/geometry";
import type { FloorplanWallSegment, RoomCandidate } from "../../core/floorplan/floorplan-types";
import { rectFromPoints } from "./floorplan-room-edit-geometry";
import { currentStoredFloorplanRooms as buildRoomsFromCandidates } from "./floorplan-storage-room";

interface StoredFloorplanRoomsInput {
  candidates: RoomCandidate[];
  scale: FloorplanStorageScale | null | undefined;
  previousRooms: FloorplanStorageRoom[];
  touchTolerancePx: number;
}

interface StoredFloorplanDocumentInput extends StoredFloorplanRoomsInput {
  document: FloorplanStorageDocument | null | undefined;
  objects: unknown[];
  wallSegments?: FloorplanWallSegment[];
}

export interface LocalFloorplanRadarPlacement {
  originX: number;
  originY: number;
  rotation: number;
  scale: number;
  poseVersion?: number;
}

export function withLocalFloorplanRadarPlacement(
  document: FloorplanStorageDocument,
  placement: LocalFloorplanRadarPlacement | null | undefined
): FloorplanStorageDocument {
  if (!placement) return document;
  return {
    ...document,
    radar: {
      ...document.radar,
      originPx: [placement.originX, placement.originY],
      rotationDeg: placement.rotation,
      scale: placement.scale,
      poseVersion: placement.poseVersion ?? document.radar.poseVersion
    }
  };
}

export function buildStoredFloorplanRooms({
  candidates,
  scale,
  previousRooms,
  touchTolerancePx
}: StoredFloorplanRoomsInput): FloorplanStorageRoom[] {
  if (!scale) return [];
  return buildRoomsFromCandidates(candidates, scale, previousRooms, touchTolerancePx);
}

export function storedRoomToCandidate(room: FloorplanStorageRoom): RoomCandidate {
  const points = room.pointsPx.map(([x, y]) => [x, y] as [number, number]);
  return {
    id: room.id,
    name: room.name,
    kind: room.kind ?? "unknown",
    confidence: 100,
    status: "confirmed",
    shape: "polygon",
    rect: rectFromPoints(points),
    points,
    debug: {
      reason: "stored",
      finalPoints: points.length,
      closedLoop: true
    }
  };
}

export function buildCurrentStoredFloorplanDocument({
  document,
  candidates,
  scale,
  previousRooms,
  touchTolerancePx,
  objects,
  wallSegments
}: StoredFloorplanDocumentInput): FloorplanStorageDocument | null {
  if (!document || !scale) return null;
  const rooms = buildStoredFloorplanRooms({
    candidates,
    scale,
    previousRooms,
    touchTolerancePx
  });
  return {
    ...document,
    rooms,
    occlusion: {
      ignoredEdges: document.occlusion?.ignoredEdges ?? []
    },
    wallSegments: wallSegments ?? document.wallSegments ?? [],
    objects: sanitizeStoredFloorplanObjects(objects, rooms)
  };
}

export function sanitizeStoredFloorplanDocument(document: FloorplanStorageDocument): FloorplanStorageDocument {
  const automationAreas = sanitizeFloorplanAutomationAreas(document.automationAreas);
  const normalized = ensureFloorplanStorageIdentity({
    ...document,
    rooms: Array.isArray(document.rooms) ? document.rooms : [],
    occlusion: {
      ignoredEdges: Array.isArray(document.occlusion?.ignoredEdges) ? document.occlusion.ignoredEdges : []
    },
    doors: Array.isArray(document.doors) ? sanitizeStoredFloorplanDoors(document.doors) : undefined,
    objects: sanitizeStoredFloorplanObjects(document.objects ?? [], Array.isArray(document.rooms) ? document.rooms : []),
    automationAreas
  });
  if (automationAreas) return normalized;
  const { automationAreas: _invalidAutomationAreas, ...withoutAutomationAreas } = normalized;
  return withoutAutomationAreas;
}

export function sanitizeStoredFloorplanDoors(doors: unknown[]): FloorplanStorageDoor[] {
  return doors.filter(isFloorplanStorageDoor).map((door) => ({
    id: door.id,
    kind: door.kind,
    centerPx: [door.centerPx[0], door.centerPx[1]],
    widthPx: door.widthPx,
    rotationDeg: door.rotationDeg
  }));
}

export function sanitizeStoredFloorplanObjects(
  objects: unknown[],
  rooms: FloorplanStorageRoom[] = []
): FloorplanStorageObject[] {
  return objects
    .filter(isFloorplanStorageObject)
    .map((object) => normalizeStoredFloorplanObject(object, rooms));
}

function normalizeStoredFloorplanObject(
  object: FloorplanStorageObject,
  rooms: FloorplanStorageRoom[]
): FloorplanStorageObject {
  const containingRoom = roomContainingObjectCenter(object, rooms);
  if (containingRoom) {
    return {
      ...object,
      roomId: containingRoom.id
    };
  }
  const currentRoomExists = rooms.some((room) => room.id === object.roomId);
  if (!currentRoomExists && object.roomId) {
    const { roomId: _roomId, ...withoutRoomId } = object;
    return withoutRoomId;
  }
  return object;
}

function roomContainingObjectCenter(
  object: FloorplanStorageObject,
  rooms: FloorplanStorageRoom[]
): FloorplanStorageRoom | null {
  const point = {
    x: object.xPx + object.widthPx / 2,
    y: object.yPx + object.heightPx / 2
  };
  return rooms.find((room) => pointInPolygon(point, room.pointsPx)) ?? null;
}

function isFloorplanStorageObject(value: unknown): value is FloorplanStorageObject {
  if (!value || typeof value !== "object") return false;
  const object = value as Partial<FloorplanStorageObject>;
  return typeof object.id === "string" &&
    object.id.length > 0 &&
    typeof object.asset === "string" &&
    object.asset.length > 0 &&
    isFiniteNumber(object.xPx) &&
    isFiniteNumber(object.yPx) &&
    isFiniteNumber(object.widthPx) &&
    isFiniteNumber(object.heightPx) &&
    object.widthPx > 0 &&
    object.heightPx > 0 &&
    (object.rotationDeg === undefined || isFiniteNumber(object.rotationDeg)) &&
    (object.roomId === undefined || typeof object.roomId === "string");
}

function isFloorplanStorageDoor(value: unknown): value is FloorplanStorageDoor {
  if (!value || typeof value !== "object") return false;
  const door = value as Partial<FloorplanStorageDoor>;
  return typeof door.id === "string" &&
    door.id.length > 0 &&
    (door.kind === "hinged" || door.kind === "sliding") &&
    Array.isArray(door.centerPx) &&
    door.centerPx.length === 2 &&
    isFiniteNumber(door.centerPx[0]) &&
    isFiniteNumber(door.centerPx[1]) &&
    isFiniteNumber(door.widthPx) &&
    door.widthPx > 0 &&
    isFiniteNumber(door.rotationDeg);
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}
