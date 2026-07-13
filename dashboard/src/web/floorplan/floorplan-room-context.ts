import {
  LD2450_FOV_DEGREES,
  LD2450_MAX_DISTANCE_MM,
  MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS,
  RADAR_ROOM_CONTEXT_COVERAGE_STEPS
} from "../../core/constants";
import type { FloorplanStorageDocument, FloorplanStorageRoom } from "../../core/floorplan/floorplan-storage";
import {
  floorplanPxToRadarPoint,
  radarPointToFloorplanPx,
  type FloorplanRadarTransformOptions
} from "../../core/floorplan/radar-floorplan-transform";
import { pointInPolygon, type ZonePoint } from "../../core/geometry";
import type { WebDeviceConfig } from "../../core/types";

export type FloorplanRoomContext = NonNullable<NonNullable<WebDeviceConfig["floorplan"]>["room"]>;

interface Point {
  x: number;
  y: number;
}

type PointTuple = [number, number];

export function buildFloorplanRoomContext(document: FloorplanStorageDocument): FloorplanRoomContext | undefined {
  const room = findRadarRoom(document);
  if (!room) return undefined;

  const boundary = buildRoomCoverageBoundary(document, room);
  return {
    id: room.id,
    name: room.name,
    source: "stored_room",
    ...(boundary.length >= 3 ? { boundary } : {})
  };
}

export function findRadarRoom(document: FloorplanStorageDocument): FloorplanStorageRoom | null {
  const origin = {
    x: document.radar.originPx[0],
    y: document.radar.originPx[1]
  };
  return findRoomContainingPoint(document.rooms, origin, (room) => room.pointsPx);
}

export function findRoomContainingPoint<T>(
  rooms: T[],
  point: Point | PointTuple,
  pointsForRoom: (room: T) => PointTuple[],
  boundaryTolerancePx = 0.5
): T | null {
  const normalized = Array.isArray(point) ? { x: point[0], y: point[1] } : point;
  return rooms.find((room) => isPointInsideOrOnRoom(normalized, pointsForRoom(room), boundaryTolerancePx)) ?? null;
}

function isPointInsideOrOnRoom(point: Point, points: PointTuple[], boundaryTolerancePx: number): boolean {
  if (points.length < 3) return false;
  return pointOnPolygonBoundary(point, points, boundaryTolerancePx) || pointInPolygon(point, points);
}

function pointOnPolygonBoundary(point: Point, points: PointTuple[], tolerancePx: number): boolean {
  for (let index = 0; index < points.length; index += 1) {
    if (pointOnSegment(point, points[index], points[(index + 1) % points.length], tolerancePx)) return true;
  }
  return false;
}

function pointOnSegment(point: Point, start: PointTuple, end: PointTuple, tolerancePx: number): boolean {
  const [x1, y1] = start;
  const [x2, y2] = end;
  const dx = x2 - x1;
  const dy = y2 - y1;
  const lengthSquared = dx * dx + dy * dy;
  if (lengthSquared <= 0.0001) return Math.hypot(point.x - x1, point.y - y1) <= tolerancePx;
  const projection = Math.max(0, Math.min(1, ((point.x - x1) * dx + (point.y - y1) * dy) / lengthSquared));
  const closestX = x1 + dx * projection;
  const closestY = y1 + dy * projection;
  return Math.hypot(point.x - closestX, point.y - closestY) <= tolerancePx;
}

export function buildRoomCoverageBoundary(document: FloorplanStorageDocument, room: FloorplanStorageRoom): ZonePoint[] {
  if (room.pointsPx.length < 3) return [];

  const options = transformOptions(document);
  const coverage = radarCoveragePolygonPx(options);
  const clipped = clipPolygon(room.pointsPx.map(pointFromTuple), coverage);
  const normalized = simplifyPolygon(removeDuplicatePoints(clipped), MAX_FLOORPLAN_ROOM_BOUNDARY_POINTS);
  if (normalized.length < 3 || Math.abs(polygonArea(normalized)) < 1) return [];

  return normalized
    .map((point) => floorplanPxToRadarPoint(point, options))
    .map(([x, y]) => [Math.round(x), Math.round(y)] as ZonePoint);
}

export function radarCoveragePolygonPx(options: FloorplanRadarTransformOptions): Point[] {
  const halfFov = LD2450_FOV_DEGREES / 2;
  const points: Point[] = [{ x: options.placement.originX, y: options.placement.originY }];
  for (let index = 0; index <= RADAR_ROOM_CONTEXT_COVERAGE_STEPS; index += 1) {
    const angle = -halfFov + (halfFov * 2 * index) / RADAR_ROOM_CONTEXT_COVERAGE_STEPS;
    const radians = (angle * Math.PI) / 180;
    const point = radarPointToFloorplanPx(
      [Math.sin(radians) * LD2450_MAX_DISTANCE_MM, Math.cos(radians) * LD2450_MAX_DISTANCE_MM],
      options
    );
    points.push(point);
  }
  return points;
}

export function clipPolygon(subject: Point[], clip: Point[]): Point[] {
  if (subject.length < 3 || clip.length < 3) return [];
  let output = subject;
  const orientation = Math.sign(polygonArea(clip)) || 1;

  for (let index = 0; index < clip.length; index += 1) {
    const edgeStart = clip[index];
    const edgeEnd = clip[(index + 1) % clip.length];
    const input = output;
    output = [];
    if (!input.length) break;

    let previous = input[input.length - 1];
    for (const current of input) {
      const currentInside = isInsideClipEdge(current, edgeStart, edgeEnd, orientation);
      const previousInside = isInsideClipEdge(previous, edgeStart, edgeEnd, orientation);
      if (currentInside) {
        if (!previousInside) output.push(lineIntersection(previous, current, edgeStart, edgeEnd));
        output.push(current);
      } else if (previousInside) {
        output.push(lineIntersection(previous, current, edgeStart, edgeEnd));
      }
      previous = current;
    }
  }

  return output;
}

function transformOptions(document: FloorplanStorageDocument): FloorplanRadarTransformOptions {
  return {
    placement: {
      originX: document.radar.originPx[0],
      originY: document.radar.originPx[1],
      rotation: document.radar.rotationDeg
    },
    scale: document.scale,
    scaleFactor: document.radar.scale
  };
}

function pointFromTuple([x, y]: [number, number]): Point {
  return { x, y };
}

function isInsideClipEdge(point: Point, edgeStart: Point, edgeEnd: Point, orientation: number): boolean {
  return orientation * cross(edgeStart, edgeEnd, point) >= -0.0001;
}

function lineIntersection(a: Point, b: Point, c: Point, d: Point): Point {
  const denominator = (a.x - b.x) * (c.y - d.y) - (a.y - b.y) * (c.x - d.x);
  if (Math.abs(denominator) < 0.0001) return b;
  const determinantA = a.x * b.y - a.y * b.x;
  const determinantB = c.x * d.y - c.y * d.x;
  return {
    x: (determinantA * (c.x - d.x) - (a.x - b.x) * determinantB) / denominator,
    y: (determinantA * (c.y - d.y) - (a.y - b.y) * determinantB) / denominator
  };
}

function cross(a: Point, b: Point, c: Point): number {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

function polygonArea(points: Point[]): number {
  let area = 0;
  for (let index = 0; index < points.length; index += 1) {
    const current = points[index];
    const next = points[(index + 1) % points.length];
    area += current.x * next.y - next.x * current.y;
  }
  return area / 2;
}

function removeDuplicatePoints(points: Point[]): Point[] {
  const result: Point[] = [];
  for (const point of points) {
    const rounded = {
      x: roundPoint(point.x),
      y: roundPoint(point.y)
    };
    const previous = result[result.length - 1];
    if (!previous || Math.hypot(previous.x - rounded.x, previous.y - rounded.y) >= 0.5) {
      result.push(rounded);
    }
  }
  if (result.length > 1) {
    const first = result[0];
    const last = result[result.length - 1];
    if (Math.hypot(first.x - last.x, first.y - last.y) < 0.5) result.pop();
  }
  return result;
}

function simplifyPolygon(points: Point[], maxPoints: number): Point[] {
  const simplified = [...points];
  while (simplified.length > maxPoints && simplified.length > 3) {
    let weakestIndex = 0;
    let weakestArea = Number.POSITIVE_INFINITY;
    for (let index = 0; index < simplified.length; index += 1) {
      const previous = simplified[(index - 1 + simplified.length) % simplified.length];
      const current = simplified[index];
      const next = simplified[(index + 1) % simplified.length];
      const area = Math.abs(cross(previous, current, next));
      if (area < weakestArea) {
        weakestArea = area;
        weakestIndex = index;
      }
    }
    simplified.splice(weakestIndex, 1);
  }
  return simplified;
}

function roundPoint(value: number): number {
  return Math.round(value * 100) / 100;
}
