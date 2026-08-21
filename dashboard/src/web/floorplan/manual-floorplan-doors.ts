import type { FloorplanWallSegment, RoomCandidate } from "../../core/floorplan/floorplan-types";
import type { FloorplanStorageDoor } from "../../core/floorplan/floorplan-storage";
import { roomCandidatePoints } from "./room-candidate-points";

export type ManualFloorplanDoorKind = "hinged" | "sliding";

export type ManualFloorplanDoor = FloorplanStorageDoor;

export type ManualFloorplanDoorPlacementFailure =
  | "wall_not_found"
  | "wall_too_short"
  | "door_overlap";

export type ManualFloorplanDoorPlacementResult =
  | { door: Omit<ManualFloorplanDoor, "id">; reason: null }
  | { door: null; reason: ManualFloorplanDoorPlacementFailure };

export type ManualFloorplanDoorResizeHandle = "start" | "end";

export type ManualFloorplanDoorResizeResult =
  | { door: ManualFloorplanDoor; reason: null }
  | { door: null; reason: ManualFloorplanDoorPlacementFailure };

export type ManualFloorplanDoorMoveResult = ManualFloorplanDoorResizeResult;

type PointTuple = [number, number];

interface WallEdge {
  start: PointTuple;
  end: PointTuple;
  length: number;
  angleDeg: number;
}

const DEFAULT_DOOR_WIDTH_MM: Record<ManualFloorplanDoorKind, number> = {
  hinged: 900,
  sliding: 1600
};

const MIN_DOOR_WIDTH_MM: Record<ManualFloorplanDoorKind, number> = {
  hinged: 600,
  sliding: 900
};

export function placeManualFloorplanDoor(
  point: { x: number; y: number },
  outline: PointTuple[],
  rooms: RoomCandidate[],
  doors: ManualFloorplanDoor[],
  kind: ManualFloorplanDoorKind,
  mmPerPx: number,
  maxDistancePx = 42
): ManualFloorplanDoorPlacementResult {
  return placeDoorOnEdges(point, floorplanWallEdges(outline, rooms), doors, kind, mmPerPx, maxDistancePx);
}

export function placeManualFloorplanDoorOnWalls(
  point: { x: number; y: number },
  walls: FloorplanWallSegment[],
  doors: ManualFloorplanDoor[],
  kind: ManualFloorplanDoorKind,
  mmPerPx: number,
  maxDistancePx = 42
): ManualFloorplanDoorPlacementResult {
  return placeDoorOnEdges(point, wallEdgesFromSegments(walls), doors, kind, mmPerPx, maxDistancePx);
}

function placeDoorOnEdges(
  point: { x: number; y: number },
  edges: WallEdge[],
  doors: ManualFloorplanDoor[],
  kind: ManualFloorplanDoorKind,
  mmPerPx: number,
  maxDistancePx: number
): ManualFloorplanDoorPlacementResult {
  let nearest: { edge: WallEdge; point: PointTuple; distance: number; position: number } | null = null;
  let shortWallFound = false;

  for (const edge of edges) {
    const minimumWidthPx = MIN_DOOR_WIDTH_MM[kind] / mmPerPx;
    const clearancePx = 100 / mmPerPx;
    if (edge.length < minimumWidthPx + clearancePx * 2) {
      const projection = projectToEdge(point, edge);
      if (projection.distance <= maxDistancePx) shortWallFound = true;
      continue;
    }
    const projection = projectToEdge(point, edge);
    if (projection.distance > maxDistancePx || (nearest && projection.distance >= nearest.distance)) continue;
    nearest = { edge, ...projection };
  }

  if (!nearest) {
    return { door: null, reason: shortWallFound ? "wall_too_short" : "wall_not_found" };
  }

  const clearancePx = 100 / mmPerPx;
  const widthPx = Math.min(
    DEFAULT_DOOR_WIDTH_MM[kind] / mmPerPx,
    nearest.edge.length - clearancePx * 2
  );
  const halfWidth = widthPx / 2;
  const position = Math.min(
    nearest.edge.length - clearancePx - halfWidth,
    Math.max(clearancePx + halfWidth, nearest.position)
  );
  const ratio = position / nearest.edge.length;
  const centerPx: PointTuple = [
    nearest.edge.start[0] + (nearest.edge.end[0] - nearest.edge.start[0]) * ratio,
    nearest.edge.start[1] + (nearest.edge.end[1] - nearest.edge.start[1]) * ratio
  ];
  const door = {
    kind,
    centerPx,
    widthPx,
    rotationDeg: nearest.edge.angleDeg
  };
  if (doors.some((existing) => doorsOverlap(existing, door, clearancePx))) {
    return { door: null, reason: "door_overlap" };
  }
  return { door, reason: null };
}

export function resizeManualFloorplanDoor(
  door: ManualFloorplanDoor,
  handle: ManualFloorplanDoorResizeHandle,
  point: { x: number; y: number },
  outline: PointTuple[],
  rooms: RoomCandidate[],
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorResizeResult {
  const tolerancePx = Math.max(1, 30 / mmPerPx);
  const edge = floorplanWallEdges(outline, rooms).find((candidate) =>
    doorFitsEdge(door, candidate, tolerancePx)
  );
  return resizeDoorOnEdge(door, handle, point, edge, doors, mmPerPx);
}

export function resizeManualFloorplanDoorOnWalls(
  door: ManualFloorplanDoor,
  handle: ManualFloorplanDoorResizeHandle,
  point: { x: number; y: number },
  walls: FloorplanWallSegment[],
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorResizeResult {
  const tolerancePx = Math.max(1, 30 / mmPerPx);
  const edge = wallEdgesFromSegments(walls).find((candidate) =>
    doorFitsEdge(door, candidate, tolerancePx)
  );
  return resizeDoorOnEdge(door, handle, point, edge, doors, mmPerPx);
}

function resizeDoorOnEdge(
  door: ManualFloorplanDoor,
  handle: ManualFloorplanDoorResizeHandle,
  point: { x: number; y: number },
  edge: WallEdge | undefined,
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorResizeResult {
  if (!edge) return { door: null, reason: "wall_not_found" };

  const center = projectToEdge({ x: door.centerPx[0], y: door.centerPx[1] }, edge).position;
  const halfWidth = door.widthPx / 2;
  const fixedPosition = handle === "start" ? center + halfWidth : center - halfWidth;
  const requestedPosition = projectToEdge(point, edge).position;
  const clearancePx = 100 / mmPerPx;
  const minimumWidthPx = MIN_DOOR_WIDTH_MM[door.kind] / mmPerPx;
  const minimumPosition = clearancePx;
  const maximumPosition = edge.length - clearancePx;
  const movingPosition = handle === "start"
    ? Math.min(fixedPosition - minimumWidthPx, Math.max(minimumPosition, requestedPosition))
    : Math.max(fixedPosition + minimumWidthPx, Math.min(maximumPosition, requestedPosition));
  const widthPx = Math.abs(fixedPosition - movingPosition);
  if (widthPx < minimumWidthPx - 0.001) return { door: null, reason: "wall_too_short" };

  const centerPosition = (fixedPosition + movingPosition) / 2;
  const ratio = centerPosition / edge.length;
  const resized: ManualFloorplanDoor = {
    ...door,
    centerPx: [
      edge.start[0] + (edge.end[0] - edge.start[0]) * ratio,
      edge.start[1] + (edge.end[1] - edge.start[1]) * ratio
    ],
    widthPx,
    rotationDeg: edge.angleDeg
  };
  if (doors.some((existing) => existing.id !== door.id && doorsOverlap(existing, resized, clearancePx))) {
    return { door: null, reason: "door_overlap" };
  }
  return { door: resized, reason: null };
}

export function moveManualFloorplanDoor(
  door: ManualFloorplanDoor,
  point: { x: number; y: number },
  outline: PointTuple[],
  rooms: RoomCandidate[],
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorMoveResult {
  const tolerancePx = Math.max(1, 30 / mmPerPx);
  const edge = floorplanWallEdges(outline, rooms).find((candidate) =>
    doorFitsEdge(door, candidate, tolerancePx)
  );
  return moveDoorOnEdge(door, point, edge, doors, mmPerPx);
}

export function moveManualFloorplanDoorOnWalls(
  door: ManualFloorplanDoor,
  point: { x: number; y: number },
  walls: FloorplanWallSegment[],
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorMoveResult {
  const tolerancePx = Math.max(1, 30 / mmPerPx);
  const edge = wallEdgesFromSegments(walls).find((candidate) =>
    doorFitsEdge(door, candidate, tolerancePx)
  );
  return moveDoorOnEdge(door, point, edge, doors, mmPerPx);
}

function moveDoorOnEdge(
  door: ManualFloorplanDoor,
  point: { x: number; y: number },
  edge: WallEdge | undefined,
  doors: ManualFloorplanDoor[],
  mmPerPx: number
): ManualFloorplanDoorMoveResult {
  if (!edge) return { door: null, reason: "wall_not_found" };

  const clearancePx = 100 / mmPerPx;
  const halfWidth = door.widthPx / 2;
  const requestedPosition = projectToEdge(point, edge).position;
  const position = Math.min(
    edge.length - clearancePx - halfWidth,
    Math.max(clearancePx + halfWidth, requestedPosition)
  );
  const ratio = position / edge.length;
  const moved: ManualFloorplanDoor = {
    ...door,
    centerPx: [
      edge.start[0] + (edge.end[0] - edge.start[0]) * ratio,
      edge.start[1] + (edge.end[1] - edge.start[1]) * ratio
    ],
    rotationDeg: edge.angleDeg
  };
  if (doors.some((existing) => existing.id !== door.id && doorsOverlap(existing, moved, clearancePx))) {
    return { door: null, reason: "door_overlap" };
  }
  return { door: moved, reason: null };
}

export function manualFloorplanDoorEndpoints(door: Pick<ManualFloorplanDoor, "centerPx" | "widthPx" | "rotationDeg">): {
  start: PointTuple;
  end: PointTuple;
} {
  const radians = door.rotationDeg * Math.PI / 180;
  const offsetX = Math.cos(radians) * door.widthPx / 2;
  const offsetY = Math.sin(radians) * door.widthPx / 2;
  return {
    start: [door.centerPx[0] - offsetX, door.centerPx[1] - offsetY],
    end: [door.centerPx[0] + offsetX, door.centerPx[1] + offsetY]
  };
}

export function manualFloorplanSlidingDoorOffsetPx(mmPerPx: number): number {
  return Math.max(2, 80 / mmPerPx);
}

export function manualFloorplanDoorsValid(
  doors: ManualFloorplanDoor[],
  outline: PointTuple[],
  rooms: RoomCandidate[],
  mmPerPx: number
): boolean {
  const edges = floorplanWallEdges(outline, rooms);
  const tolerancePx = Math.max(0.5, 20 / mmPerPx);
  return doors.every((door) => edges.some((edge) => doorFitsEdge(door, edge, tolerancePx)));
}

export function manualFloorplanDoorsValidOnWalls(
  doors: ManualFloorplanDoor[],
  walls: FloorplanWallSegment[],
  mmPerPx: number
): boolean {
  const edges = wallEdgesFromSegments(walls);
  const tolerancePx = Math.max(0.5, 20 / mmPerPx);
  return doors.every((door) => edges.some((edge) => doorFitsEdge(door, edge, tolerancePx)));
}

export function drawManualFloorplanDoors(
  context: CanvasRenderingContext2D,
  doors: ManualFloorplanDoor[],
  mmPerPx = 10
): void {
  for (const door of doors) {
    const halfWidth = door.widthPx / 2;
    context.save();
    context.translate(door.centerPx[0], door.centerPx[1]);
    context.rotate(door.rotationDeg * Math.PI / 180);
    context.lineCap = "round";
    context.lineJoin = "round";

    context.strokeStyle = "#6b7280";
    context.lineWidth = 7;
    context.beginPath();
    context.moveTo(-halfWidth, 0);
    context.lineTo(halfWidth, 0);
    context.stroke();

    context.strokeStyle = "#334155";
    context.lineWidth = 2.2;
    if (door.kind === "hinged") {
      context.beginPath();
      context.moveTo(-halfWidth, 0);
      context.lineTo(-halfWidth, -door.widthPx);
      context.stroke();
      context.strokeStyle = "#94a3b8";
      context.lineWidth = 1.6;
      context.beginPath();
      context.arc(-halfWidth, 0, door.widthPx, 0, -Math.PI / 2, true);
      context.stroke();
    } else {
      const offset = manualFloorplanSlidingDoorOffsetPx(mmPerPx);
      context.beginPath();
      context.moveTo(-halfWidth, -offset);
      context.lineTo(door.widthPx * 0.08, -offset);
      context.moveTo(-door.widthPx * 0.08, offset);
      context.lineTo(halfWidth, offset);
      context.stroke();
      context.strokeStyle = "#94a3b8";
      context.lineWidth = 1.4;
      context.beginPath();
      context.moveTo(-door.widthPx * 0.08, -offset * 1.7);
      context.lineTo(-door.widthPx * 0.08, offset * 1.7);
      context.moveTo(door.widthPx * 0.08, -offset * 1.7);
      context.lineTo(door.widthPx * 0.08, offset * 1.7);
      context.stroke();
    }

    context.fillStyle = "#334155";
    for (const x of [-halfWidth, halfWidth]) {
      context.beginPath();
      context.arc(x, 0, 3.2, 0, Math.PI * 2);
      context.fill();
    }
    context.restore();
  }
}

function floorplanWallEdges(outline: PointTuple[], rooms: RoomCandidate[]): WallEdge[] {
  const polygons = [
    outline,
    ...rooms
      .filter((room) => room.status !== "rejected")
      .map((room) => roomCandidatePoints(room))
  ].filter((points) => points.length >= 2);
  const seen = new Set<string>();
  const edges: WallEdge[] = [];
  for (const points of polygons) {
    for (let index = 0; index < points.length; index += 1) {
      const start = points[index];
      const end = points[(index + 1) % points.length];
      const length = Math.hypot(end[0] - start[0], end[1] - start[1]);
      if (length <= 0.001) continue;
      const key = edgeKey(start, end);
      if (seen.has(key)) continue;
      seen.add(key);
      edges.push({
        start,
        end,
        length,
        angleDeg: Math.atan2(end[1] - start[1], end[0] - start[0]) * 180 / Math.PI
      });
    }
  }
  return edges;
}

function wallEdgesFromSegments(walls: FloorplanWallSegment[]): WallEdge[] {
  const seen = new Set<string>();
  const edges: WallEdge[] = [];
  for (const wall of walls) {
    const start: PointTuple = [wall.x1, wall.y1];
    const end: PointTuple = [wall.x2, wall.y2];
    const key = edgeKey(start, end);
    if (seen.has(key)) continue;
    const length = Math.hypot(end[0] - start[0], end[1] - start[1]);
    if (length <= 0.001) continue;
    seen.add(key);
    edges.push({
      start,
      end,
      length,
      angleDeg: Math.atan2(end[1] - start[1], end[0] - start[0]) * 180 / Math.PI
    });
  }
  return edges;
}

function projectToEdge(point: { x: number; y: number }, edge: WallEdge) {
  const dx = edge.end[0] - edge.start[0];
  const dy = edge.end[1] - edge.start[1];
  const rawRatio = ((point.x - edge.start[0]) * dx + (point.y - edge.start[1]) * dy) /
    Math.max(Number.EPSILON, edge.length ** 2);
  const ratio = Math.min(1, Math.max(0, rawRatio));
  const projected: PointTuple = [edge.start[0] + dx * ratio, edge.start[1] + dy * ratio];
  return {
    point: projected,
    distance: Math.hypot(point.x - projected[0], point.y - projected[1]),
    position: ratio * edge.length
  };
}

function doorFitsEdge(door: ManualFloorplanDoor, edge: WallEdge, tolerancePx: number): boolean {
  if (angleDifference(door.rotationDeg, edge.angleDeg) > 0.5) return false;
  const projection = projectToEdge({ x: door.centerPx[0], y: door.centerPx[1] }, edge);
  return projection.distance <= tolerancePx &&
    projection.position >= door.widthPx / 2 - tolerancePx &&
    projection.position <= edge.length - door.widthPx / 2 + tolerancePx;
}

function doorsOverlap(
  existing: ManualFloorplanDoor,
  proposed: Omit<ManualFloorplanDoor, "id">,
  clearancePx: number
): boolean {
  if (angleDifference(existing.rotationDeg, proposed.rotationDeg) > 0.5) return false;
  const radians = proposed.rotationDeg * Math.PI / 180;
  const dx = existing.centerPx[0] - proposed.centerPx[0];
  const dy = existing.centerPx[1] - proposed.centerPx[1];
  const perpendicularDistance = Math.abs(-Math.sin(radians) * dx + Math.cos(radians) * dy);
  const alongDistance = Math.abs(Math.cos(radians) * dx + Math.sin(radians) * dy);
  return perpendicularDistance <= clearancePx &&
    alongDistance < (existing.widthPx + proposed.widthPx) / 2 + clearancePx;
}

function angleDifference(first: number, second: number): number {
  const difference = Math.abs(((first - second + 90) % 180 + 180) % 180 - 90);
  return difference;
}

function edgeKey(first: PointTuple, second: PointTuple): string {
  const firstKey = `${rounded(first[0])},${rounded(first[1])}`;
  const secondKey = `${rounded(second[0])},${rounded(second[1])}`;
  return firstKey < secondKey ? `${firstKey}:${secondKey}` : `${secondKey}:${firstKey}`;
}

function rounded(value: number): number {
  return Math.round(value * 100) / 100;
}
