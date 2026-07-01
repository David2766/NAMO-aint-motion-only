import type { RoomCandidate } from "./floorplan-types";

export const FLOORPLAN_STORAGE_VERSION = 1;
export const FLOORPLAN_IMAGE_PATH = "/floorplan.webp";
export const FLOORPLAN_CONFIG_PATH = "/floorplan.json";

export interface FloorplanStorageImage {
  path: string;
  mime: string;
  width: number;
  height: number;
  bytes?: number;
  name?: string;
}

export interface FloorplanStorageScale {
  widthMm: number;
  heightMm: number;
  outerBoundsPx: [number, number, number, number];
  mmPerPxX: number;
  mmPerPxY: number;
}

export interface FloorplanStorageRadar {
  originPx: [number, number];
  rotationDeg: number;
  scale: number;
}

export interface FloorplanStorageRoom {
  id: string;
  name: string;
  kind: RoomCandidate["kind"];
  pointsPx: Array<[number, number]>;
  widthMm?: number;
  heightMm?: number;
  manualSize?: boolean;
}

export interface FloorplanStorageOcclusion {
  ignoredEdges: string[];
}

export interface FloorplanStorageObject {
  id: string;
  asset: string;
  xPx: number;
  yPx: number;
  widthPx: number;
  heightPx: number;
  rotationDeg: number;
}

export interface FloorplanStorageDocument {
  version: number;
  image: FloorplanStorageImage;
  scale: FloorplanStorageScale;
  radar: FloorplanStorageRadar;
  rooms: FloorplanStorageRoom[];
  occlusion: FloorplanStorageOcclusion;
  objects?: FloorplanStorageObject[];
}

export interface FloorplanStorageBuildInput {
  image: {
    width: number;
    height: number;
    bytes?: number;
    name?: string;
    mime?: string;
    path?: string;
  };
  scale: {
    widthMm: number;
    heightMm: number;
    outerBounds: {
      x: number;
      y: number;
      width: number;
      height: number;
    };
    mmPerPxX: number;
    mmPerPxY: number;
  };
  radar: {
    originX: number;
    originY: number;
    rotation: number;
    scale: number;
  };
  rooms: RoomCandidate[];
  roomMeasurements?: Record<string, { width?: string; height?: string }>;
  roomSizeEstimates?: Record<string, { widthMm?: number; heightMm?: number; manuallyEdited?: boolean }>;
  ignoredOcclusionEdges?: string[];
  objects?: FloorplanStorageObject[];
}

export function buildFloorplanStorageDocument(input: FloorplanStorageBuildInput): FloorplanStorageDocument {
  return {
    version: FLOORPLAN_STORAGE_VERSION,
    image: {
      path: input.image.path ?? FLOORPLAN_IMAGE_PATH,
      mime: input.image.mime ?? "image/webp",
      width: Math.round(input.image.width),
      height: Math.round(input.image.height),
      bytes: finiteOptionalInteger(input.image.bytes),
      name: input.image.name
    },
    scale: {
      widthMm: roundInteger(input.scale.widthMm),
      heightMm: roundInteger(input.scale.heightMm),
      outerBoundsPx: [
        roundPoint(input.scale.outerBounds.x),
        roundPoint(input.scale.outerBounds.y),
        roundPoint(input.scale.outerBounds.width),
        roundPoint(input.scale.outerBounds.height)
      ],
      mmPerPxX: roundPoint(input.scale.mmPerPxX),
      mmPerPxY: roundPoint(input.scale.mmPerPxY)
    },
    radar: {
      originPx: [roundPoint(input.radar.originX), roundPoint(input.radar.originY)],
      rotationDeg: roundPoint(input.radar.rotation),
      scale: roundPoint(input.radar.scale)
    },
    rooms: input.rooms
      .filter((room) => room.status !== "rejected")
      .map((room) => storageRoomFromCandidate(room, input.roomMeasurements ?? {}, input.roomSizeEstimates ?? {})),
    occlusion: {
      ignoredEdges: [...new Set(input.ignoredOcclusionEdges ?? [])]
    },
    objects: (input.objects ?? []).map((object) => ({
      id: object.id,
      asset: object.asset,
      xPx: roundPoint(object.xPx),
      yPx: roundPoint(object.yPx),
      widthPx: roundPoint(object.widthPx),
      heightPx: roundPoint(object.heightPx),
      rotationDeg: roundPoint(object.rotationDeg)
    }))
  };
}

export function floorplanStorageJson(document: FloorplanStorageDocument): string {
  return JSON.stringify(document);
}

export function floorplanStoragePrettyJson(document: FloorplanStorageDocument): string {
  return JSON.stringify(document, null, 2);
}

function storageRoomFromCandidate(
  room: RoomCandidate,
  measurements: Record<string, { width?: string; height?: string }>,
  estimates: Record<string, { widthMm?: number; heightMm?: number; manuallyEdited?: boolean }>
): FloorplanStorageRoom {
  const measurement = measurements[room.id] ?? {};
  const estimate = estimates[room.id] ?? {};
  const widthMm = parsePositiveNumber(measurement.width) ?? estimate.widthMm;
  const heightMm = parsePositiveNumber(measurement.height) ?? estimate.heightMm;
  return {
    id: room.id,
    name: room.name,
    kind: room.kind,
    pointsPx: candidatePoints(room).map(([x, y]) => [roundPoint(x), roundPoint(y)]),
    widthMm: finiteOptionalInteger(widthMm),
    heightMm: finiteOptionalInteger(heightMm),
    manualSize: Boolean(measurement.width || measurement.height || estimate.manuallyEdited)
  };
}

function candidatePoints(room: RoomCandidate): Array<[number, number]> {
  if (room.shape === "polygon" && room.points?.length) return room.points;
  const x1 = room.rect.x;
  const y1 = room.rect.y;
  const x2 = room.rect.x + room.rect.width;
  const y2 = room.rect.y + room.rect.height;
  return [[x1, y1], [x2, y1], [x2, y2], [x1, y2]];
}

function parsePositiveNumber(value: string | undefined): number | undefined {
  if (!value) return undefined;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : undefined;
}

function finiteOptionalInteger(value: number | undefined): number | undefined {
  return typeof value === "number" && Number.isFinite(value) ? Math.round(value) : undefined;
}

function roundInteger(value: number): number {
  return Math.round(value);
}

function roundPoint(value: number): number {
  return Math.round(value * 100) / 100;
}
