import type { FloorplanWallSegment, RoomCandidate } from "./floorplan-types";

export const FLOORPLAN_STORAGE_VERSION = 2;
export const FLOORPLAN_IMAGE_PATH = "/floorplan.webp";
export const FLOORPLAN_CONFIG_PATH = "/floorplan.json";
export const FLOORPLAN_ID_PREFIX = "fp-";
export const FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION = 1;
export const MAX_AUTOMATION_AREAS_PER_SPACE = 6;
export const MAX_AUTOMATION_AREAS_PER_FLOORPLAN = 48;
export const MAX_AUTOMATION_AREA_POINTS = 8;
export const MAX_AUTOMATION_AREA_NAME_BYTES = 48;
const MAX_FLOORPLAN_REVISION = 0xffffffff;

export interface FloorplanStorageIdentity {
  floorplanId: string;
  geometryVersion: number;
}

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
  poseVersion?: number;
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

export interface FloorplanStorageOutline {
  pointsPx: Array<[number, number]>;
}

export interface FloorplanStorageOcclusion {
  ignoredEdges: string[];
}

export type FloorplanStorageWallSegment = FloorplanWallSegment;

export interface FloorplanStorageDoor {
  id: string;
  kind: "hinged" | "sliding";
  centerPx: [number, number];
  widthPx: number;
  rotationDeg: number;
}

export interface FloorplanStorageObject {
  id: string;
  asset: string;
  roomId?: string;
  xPx: number;
  yPx: number;
  widthPx: number;
  heightPx: number;
  rotationDeg: number;
}

export interface FloorplanAutomationArea {
  areaId: string;
  name: string;
  spaceId: string;
  enabled: boolean;
  pointsPx: Array<[number, number]>;
}

export interface FloorplanAutomationAreas {
  schemaVersion: typeof FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION;
  revision: number;
  areas: FloorplanAutomationArea[];
}

export interface FloorplanStorageDocument extends Partial<FloorplanStorageIdentity> {
  version: number;
  source?: "image" | "manual";
  image: FloorplanStorageImage;
  scale: FloorplanStorageScale;
  radar: FloorplanStorageRadar;
  outline?: FloorplanStorageOutline;
  rooms: FloorplanStorageRoom[];
  wallSegments?: FloorplanStorageWallSegment[];
  doors?: FloorplanStorageDoor[];
  occlusion: FloorplanStorageOcclusion;
  objects?: FloorplanStorageObject[];
  automationAreas?: FloorplanAutomationAreas;
}

export interface FloorplanStorageBuildInput {
  source?: "image" | "manual";
  outline?: Array<[number, number]>;
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
  wallSegments?: FloorplanWallSegment[];
  doors?: FloorplanStorageDoor[];
  roomMeasurements?: Record<string, { width?: string; height?: string }>;
  roomSizeEstimates?: Record<string, { widthMm?: number; heightMm?: number; manuallyEdited?: boolean }>;
  ignoredOcclusionEdges?: string[];
  objects?: FloorplanStorageObject[];
}

export function buildFloorplanStorageDocument(input: FloorplanStorageBuildInput): FloorplanStorageDocument {
  return {
    version: FLOORPLAN_STORAGE_VERSION,
    source: input.source,
    floorplanId: createFloorplanId(),
    geometryVersion: 1,
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
      scale: roundPoint(input.radar.scale),
      poseVersion: 1
    },
    outline: input.outline?.length
      ? { pointsPx: input.outline.map(([x, y]) => [roundPoint(x), roundPoint(y)]) }
      : undefined,
    rooms: input.rooms
      .filter((room) => room.status !== "rejected")
      .map((room) => storageRoomFromCandidate(room, input.roomMeasurements ?? {}, input.roomSizeEstimates ?? {})),
    wallSegments: (input.wallSegments ?? []).map(storageWallSegmentFromSegment),
    doors: input.doors?.map((door) => ({
      id: door.id,
      kind: door.kind,
      centerPx: [roundPoint(door.centerPx[0]), roundPoint(door.centerPx[1])],
      widthPx: roundPoint(door.widthPx),
      rotationDeg: roundPoint(door.rotationDeg)
    })),
    occlusion: {
      ignoredEdges: [...new Set(input.ignoredOcclusionEdges ?? [])]
    },
    objects: (input.objects ?? []).map((object) => ({
      id: object.id,
      asset: object.asset,
      roomId: object.roomId,
      xPx: roundPoint(object.xPx),
      yPx: roundPoint(object.yPx),
      widthPx: roundPoint(object.widthPx),
      heightPx: roundPoint(object.heightPx),
      rotationDeg: roundPoint(object.rotationDeg)
    }))
  };
}

export function createFloorplanId(): string {
  const cryptoApi = globalThis.crypto;
  if (!cryptoApi?.getRandomValues) {
    throw new Error("secure_random_unavailable");
  }
  const bytes = new Uint8Array(16);
  cryptoApi.getRandomValues(bytes);
  return `${FLOORPLAN_ID_PREFIX}${Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("")}`;
}

export function ensureFloorplanStorageIdentity(document: FloorplanStorageDocument): FloorplanStorageDocument {
  const floorplanId = validFloorplanId(document.floorplanId) ? document.floorplanId : createFloorplanId();
  const geometryVersion = positiveVersion(document.geometryVersion);
  const poseVersion = positiveVersion(document.radar?.poseVersion);
  if (document.version === FLOORPLAN_STORAGE_VERSION &&
      document.floorplanId === floorplanId &&
      document.geometryVersion === geometryVersion &&
      document.radar?.poseVersion === poseVersion) {
    return document;
  }
  return {
    ...document,
    version: FLOORPLAN_STORAGE_VERSION,
    floorplanId,
    geometryVersion,
    radar: {
      ...document.radar,
      poseVersion
    }
  };
}

export function advanceFloorplanGeometryVersion(document: FloorplanStorageDocument): FloorplanStorageDocument {
  const normalized = ensureFloorplanStorageIdentity(document);
  return {
    ...normalized,
    geometryVersion: nextVersion(normalized.geometryVersion)
  };
}

export function advanceFloorplanPoseVersion(document: FloorplanStorageDocument): FloorplanStorageDocument {
  const normalized = ensureFloorplanStorageIdentity(document);
  return {
    ...normalized,
    radar: {
      ...normalized.radar,
      poseVersion: nextVersion(normalized.radar.poseVersion)
    }
  };
}

export function advanceFloorplanAutomationAreasRevision(
  document: FloorplanStorageDocument
): FloorplanStorageDocument {
  const normalized = ensureFloorplanStorageIdentity(document);
  const persisted = sanitizeFloorplanAutomationAreas(normalized.automationAreas);
  const draft = normalized.automationAreas?.revision === 0
    ? sanitizeFloorplanAutomationAreas({ ...normalized.automationAreas, revision: 1 })
    : undefined;
  const current = persisted ?? (draft ? { ...draft, revision: 0 } : undefined) ?? {
    schemaVersion: FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION,
    revision: 0,
    areas: []
  };
  return {
    ...normalized,
    automationAreas: {
      ...current,
      revision: current.revision === 0 ? 1 : nextVersion(current.revision)
    }
  };
}

export function sanitizeFloorplanAutomationAreas(value: unknown): FloorplanAutomationAreas | undefined {
  if (!value || typeof value !== "object") return undefined;
  const source = value as Partial<FloorplanAutomationAreas>;
  if (source.schemaVersion !== FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION ||
      typeof source.revision !== "number" || !Number.isInteger(source.revision) || source.revision <= 0 ||
      !Array.isArray(source.areas) || source.areas.length > MAX_AUTOMATION_AREAS_PER_FLOORPLAN) {
    return undefined;
  }

  const perSpace = new Map<string, number>();
  const areaIds = new Set<string>();
  const areas: FloorplanAutomationArea[] = [];
  for (const candidate of source.areas) {
    if (!isFloorplanAutomationArea(candidate) || areaIds.has(candidate.areaId)) return undefined;
    const count = (perSpace.get(candidate.spaceId) ?? 0) + (candidate.enabled ? 1 : 0);
    if (count > MAX_AUTOMATION_AREAS_PER_SPACE) return undefined;
    perSpace.set(candidate.spaceId, count);
    areaIds.add(candidate.areaId);
    areas.push({
      areaId: candidate.areaId,
      name: candidate.name,
      spaceId: candidate.spaceId,
      enabled: candidate.enabled,
      pointsPx: candidate.pointsPx.map(([x, y]) => [roundPoint(x), roundPoint(y)])
    });
  }
  return {
    schemaVersion: FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION,
    revision: source.revision,
    areas
  };
}

function isFloorplanAutomationArea(value: unknown): value is FloorplanAutomationArea {
  if (!value || typeof value !== "object") return false;
  const area = value as Partial<FloorplanAutomationArea>;
  if (typeof area.areaId !== "string" || !/^[A-Za-z0-9._:-]{1,40}$/.test(area.areaId) ||
      typeof area.name !== "string" || utf8Length(area.name) > MAX_AUTOMATION_AREA_NAME_BYTES ||
      typeof area.spaceId !== "string" || !/^[A-Za-z0-9._:-]{1,40}$/.test(area.spaceId) ||
      typeof area.enabled !== "boolean" || !Array.isArray(area.pointsPx) ||
      area.pointsPx.length < 3 || area.pointsPx.length > MAX_AUTOMATION_AREA_POINTS) {
    return false;
  }
  return area.pointsPx.every((point) =>
    Array.isArray(point) && point.length === 2 &&
    point.every((coordinate) => typeof coordinate === "number" && Number.isFinite(coordinate) && Math.abs(coordinate) <= 100000)
  );
}

function utf8Length(value: string): number {
  return new TextEncoder().encode(value).length;
}

function storageWallSegmentFromSegment(segment: FloorplanWallSegment): FloorplanStorageWallSegment {
  return {
    id: segment.id,
    axis: segment.axis,
    x1: roundPoint(segment.x1),
    y1: roundPoint(segment.y1),
    x2: roundPoint(segment.x2),
    y2: roundPoint(segment.y2),
    length: roundPoint(segment.length)
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

function validFloorplanId(value: string | undefined): value is string {
  return typeof value === "string" && /^fp-[0-9a-f]{32}$/.test(value);
}

function positiveVersion(value: number | undefined): number {
  return typeof value === "number" && Number.isSafeInteger(value) && value > 0 && value <= MAX_FLOORPLAN_REVISION
    ? value
    : 1;
}

function nextVersion(value: number | undefined): number {
  const current = positiveVersion(value);
  return current < MAX_FLOORPLAN_REVISION ? current + 1 : current;
}
