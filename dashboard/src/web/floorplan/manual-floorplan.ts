import type { RoomCandidate } from "../../core/floorplan/floorplan-types";
import {
  drawManualFloorplanDoors,
  type ManualFloorplanDoor
} from "./manual-floorplan-doors";
import { roomCandidatePoints } from "./room-candidate-points";

export type ManualFloorplanPoint = [number, number];

export const MANUAL_FLOORPLAN_WIDTH_PX = 3000;
export const MANUAL_FLOORPLAN_HEIGHT_PX = 2000;
export const MANUAL_FLOORPLAN_MM_PER_PX = 10;
export const MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT = 100;
export const MANUAL_FLOORPLAN_MIN_SCALE_PERCENT = 25;
export const MANUAL_FLOORPLAN_MAX_SCALE_PERCENT = 500;
export const MANUAL_FLOORPLAN_SCALE_STEP_PERCENT = 25;
export const MANUAL_FLOORPLAN_GRID_STEP_PX = 10;
export const MANUAL_FLOORPLAN_SNAP_STEP_PX = 50;
export const MANUAL_FLOORPLAN_ANGLE_SNAP_DEG = 6;

interface CanvasPoint {
  x: number;
  y: number;
}

interface ManualFloorplanRenderOptions {
  rooms?: RoomCandidate[];
  outline?: ManualFloorplanPoint[];
  doors?: ManualFloorplanDoor[];
  showGrid?: boolean;
  width?: number;
  height?: number;
  mmPerPx?: number;
}

export interface ManualFloorplanEdgeLabel {
  x: number;
  y: number;
  angleDeg: number;
  text: string;
}

export function snapManualFloorplanPoint(
  points: ManualFloorplanPoint[],
  point: CanvasPoint,
  width = MANUAL_FLOORPLAN_WIDTH_PX,
  height = MANUAL_FLOORPLAN_HEIGHT_PX,
  mmPerPx = MANUAL_FLOORPLAN_MM_PER_PX
): CanvasPoint {
  const snapStepPx = manualFloorplanSnapStepPx(mmPerPx);
  const clamped = clampPoint(point, width, height);
  const gridSnap = snapToExistingAxes(points, clamped, clampPoint({
    x: snapCoordinate(clamped.x, snapStepPx),
    y: snapCoordinate(clamped.y, snapStepPx)
  }, width, height), snapStepPx);
  const gridPoint = gridSnap.point;
  const previous = points[points.length - 1];
  if (!previous) return gridPoint;

  if (gridSnap.aligned) return gridPoint;

  const dx = gridPoint.x - previous[0];
  const dy = gridPoint.y - previous[1];
  const length = Math.hypot(dx, dy);
  if (length < 0.001) return { x: previous[0], y: previous[1] };

  const angle = Math.atan2(dy, dx);
  const snapStep = Math.PI / 4;
  const snappedAngle = Math.round(angle / snapStep) * snapStep;
  if (Math.abs(normalizeAngle(angle - snappedAngle)) > MANUAL_FLOORPLAN_ANGLE_SNAP_DEG * Math.PI / 180) {
    return gridPoint;
  }

  const directionX = Math.round(Math.cos(snappedAngle));
  const directionY = Math.round(Math.sin(snappedAngle));
  if (directionX === 0) return { x: previous[0], y: gridPoint.y };
  if (directionY === 0) return { x: gridPoint.x, y: previous[1] };

  const diagonalDistance = Math.max(
    snapStepPx,
    snapCoordinate((Math.abs(dx) + Math.abs(dy)) / 2, snapStepPx)
  );
  return clampPoint({
    x: previous[0] + directionX * diagonalDistance,
    y: previous[1] + directionY * diagonalDistance
  }, width, height);
}

export function manualFloorplanMmPerPx(scalePercent: number): number {
  const scale = Math.min(
    MANUAL_FLOORPLAN_MAX_SCALE_PERCENT,
    Math.max(MANUAL_FLOORPLAN_MIN_SCALE_PERCENT, scalePercent)
  );
  return MANUAL_FLOORPLAN_MM_PER_PX * MANUAL_FLOORPLAN_DEFAULT_SCALE_PERCENT / scale;
}

export function manualFloorplanSnapStepPx(mmPerPx: number): number {
  return 500 / Math.max(0.001, mmPerPx);
}

export function manualFloorplanGridStepPx(mmPerPx: number): number {
  return 100 / Math.max(0.001, mmPerPx);
}

export function manualFloorplanEdgeLabels(
  points: ManualFloorplanPoint[],
  closePolygon = false,
  mmPerPx = MANUAL_FLOORPLAN_MM_PER_PX
): ManualFloorplanEdgeLabel[] {
  if (points.length < 2) return [];
  const edgeCount = closePolygon && points.length >= 3 ? points.length : points.length - 1;
  return Array.from({ length: edgeCount }, (_, index) => {
    const start = points[index];
    const end = points[(index + 1) % points.length];
    const lengthMetres = Math.hypot(end[0] - start[0], end[1] - start[1]) * mmPerPx / 1000;
    return {
      x: (start[0] + end[0]) / 2,
      y: (start[1] + end[1]) / 2,
      angleDeg: Math.atan2(end[1] - start[1], end[0] - start[0]) * 180 / Math.PI,
      text: `${(Math.round(lengthMetres * 10) / 10).toFixed(1)} m`
    };
  });
}

export function validManualFloorplanPolygon(points: ManualFloorplanPoint[]): boolean {
  if (points.length < 3) return false;
  if (points.some((point, index) => samePoint(point, points[(index + 1) % points.length]))) return false;
  if (Math.abs(polygonArea(points)) < 1) return false;

  for (let first = 0; first < points.length; first += 1) {
    const firstEnd = (first + 1) % points.length;
    for (let second = first + 1; second < points.length; second += 1) {
      const secondEnd = (second + 1) % points.length;
      if (first === second || firstEnd === second || secondEnd === first) continue;
      if (segmentsIntersect(points[first], points[firstEnd], points[second], points[secondEnd])) return false;
    }
  }
  return true;
}

export function manualFloorplanPolygonInsideOutline(
  points: ManualFloorplanPoint[],
  outline: ManualFloorplanPoint[]
): boolean {
  if (!validManualFloorplanPolygon(points) || !validManualFloorplanPolygon(outline)) return false;
  if (points.some((point) => !pointInsideOrOnPolygon(point, outline))) return false;

  return points.every((point, index) => {
    const next = points[(index + 1) % points.length];
    const midpoint: ManualFloorplanPoint = [(point[0] + next[0]) / 2, (point[1] + next[1]) / 2];
    return pointInsideOrOnPolygon(midpoint, outline) && !segmentCrossesPolygon(point, next, outline);
  });
}

export function manualFloorplanDraftPointInsideOutline(
  points: ManualFloorplanPoint[],
  point: ManualFloorplanPoint,
  outline: ManualFloorplanPoint[]
): boolean {
  if (!validManualFloorplanPolygon(outline) || !pointInsideOrOnPolygon(point, outline)) return false;
  const previous = points[points.length - 1];
  if (!previous) return true;
  const midpoint: ManualFloorplanPoint = [(previous[0] + point[0]) / 2, (previous[1] + point[1]) / 2];
  return pointInsideOrOnPolygon(midpoint, outline) && !segmentCrossesPolygon(previous, point, outline);
}

export function manualFloorplanBounds(points: ManualFloorplanPoint[]) {
  if (!points.length) return null;
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  return { x: minX, y: minY, width: maxX - minX, height: maxY - minY };
}

export async function renderManualFloorplanImage({
  rooms = [],
  outline = [],
  doors = [],
  showGrid = false,
  width = MANUAL_FLOORPLAN_WIDTH_PX,
  height = MANUAL_FLOORPLAN_HEIGHT_PX,
  mmPerPx = MANUAL_FLOORPLAN_MM_PER_PX
}: ManualFloorplanRenderOptions = {}): Promise<Blob> {
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("canvas_unavailable");

  context.fillStyle = "#f4f6f3";
  context.fillRect(0, 0, width, height);
  if (showGrid) drawMetricGrid(context, width, height, mmPerPx);

  const visibleRooms = rooms.filter((room) => room.status !== "rejected");
  for (const room of visibleRooms) {
    const points = roomCandidatePoints(room);
    if (points.length < 3) continue;
    // The first manual save freezes these edges as physical walls. Logical
    // room fills and labels remain live overlays so they can be edited later.
    drawPolygon(context, points, "transparent", "#000000", 4);
  }

  if (outline.length >= 3) drawPolygon(context, outline, "transparent", "#000000", 7);
  drawManualFloorplanDoors(context, doors, mmPerPx);

  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/webp", 0.86));
  if (!blob) throw new Error("webp_failed");
  return blob;
}

function drawMetricGrid(
  context: CanvasRenderingContext2D,
  width: number,
  height: number,
  mmPerPx: number
): void {
  const gridStepPx = manualFloorplanGridStepPx(mmPerPx);
  const snapStepPx = manualFloorplanSnapStepPx(mmPerPx);
  const majorStepPx = 1000 / Math.max(0.001, mmPerPx);
  context.save();
  for (let value = 0; value <= Math.max(width, height); value += gridStepPx) {
    const major = nearMultiple(value, majorStepPx);
    const snapLine = nearMultiple(value, snapStepPx);
    context.strokeStyle = major ? "#b8c4bd" : snapLine ? "#d2dad5" : "#e5e9e6";
    context.lineWidth = major ? 1.4 : snapLine ? 0.9 : 0.5;
    if (value <= width) {
      context.beginPath();
      context.moveTo(value, 0);
      context.lineTo(value, height);
      context.stroke();
    }
    if (value <= height) {
      context.beginPath();
      context.moveTo(0, value);
      context.lineTo(width, value);
      context.stroke();
    }
  }
  context.restore();
}

function drawPolygon(
  context: CanvasRenderingContext2D,
  points: ManualFloorplanPoint[],
  fill: string,
  stroke: string,
  lineWidth: number
): void {
  context.save();
  context.beginPath();
  context.moveTo(points[0][0], points[0][1]);
  for (const [x, y] of points.slice(1)) context.lineTo(x, y);
  context.closePath();
  if (fill !== "transparent") {
    context.fillStyle = fill;
    context.fill();
  }
  context.strokeStyle = stroke;
  context.lineWidth = lineWidth;
  context.lineJoin = "round";
  context.stroke();
  context.restore();
}

function snapCoordinate(value: number, stepPx: number): number {
  return Math.round(value / stepPx) * stepPx;
}

function snapToExistingAxes(
  points: ManualFloorplanPoint[],
  point: CanvasPoint,
  gridPoint: CanvasPoint,
  snapStepPx: number
): { point: CanvasPoint; aligned: boolean } {
  const threshold = snapStepPx / 2;
  const closestX = closestAxis(points, point.x, 0, threshold);
  const closestY = closestAxis(points, point.y, 1, threshold);
  return {
    point: {
      x: closestX ?? gridPoint.x,
      y: closestY ?? gridPoint.y
    },
    aligned: closestX !== null || closestY !== null
  };
}

function nearMultiple(value: number, step: number): boolean {
  return Math.abs(value / step - Math.round(value / step)) < 0.001;
}

function closestAxis(
  points: ManualFloorplanPoint[],
  value: number,
  axis: 0 | 1,
  threshold: number
): number | null {
  let closest: number | null = null;
  let distance = threshold + 0.001;
  for (const point of points) {
    const nextDistance = Math.abs(point[axis] - value);
    if (nextDistance >= distance) continue;
    closest = point[axis];
    distance = nextDistance;
  }
  return closest;
}

function clampPoint(point: CanvasPoint, width: number, height: number): CanvasPoint {
  return {
    x: Math.min(width, Math.max(0, point.x)),
    y: Math.min(height, Math.max(0, point.y))
  };
}

function normalizeAngle(angle: number): number {
  return Math.atan2(Math.sin(angle), Math.cos(angle));
}

function samePoint(first: ManualFloorplanPoint, second: ManualFloorplanPoint): boolean {
  return Math.abs(first[0] - second[0]) < 0.001 && Math.abs(first[1] - second[1]) < 0.001;
}

function polygonArea(points: ManualFloorplanPoint[]): number {
  return points.reduce((sum, point, index) => {
    const next = points[(index + 1) % points.length];
    return sum + point[0] * next[1] - next[0] * point[1];
  }, 0) / 2;
}

function segmentsIntersect(
  a: ManualFloorplanPoint,
  b: ManualFloorplanPoint,
  c: ManualFloorplanPoint,
  d: ManualFloorplanPoint
): boolean {
  const abC = orientation(a, b, c);
  const abD = orientation(a, b, d);
  const cdA = orientation(c, d, a);
  const cdB = orientation(c, d, b);
  if (abC * abD < 0 && cdA * cdB < 0) return true;
  return (abC === 0 && pointOnSegment(c, a, b)) ||
    (abD === 0 && pointOnSegment(d, a, b)) ||
    (cdA === 0 && pointOnSegment(a, c, d)) ||
    (cdB === 0 && pointOnSegment(b, c, d));
}

function orientation(a: ManualFloorplanPoint, b: ManualFloorplanPoint, c: ManualFloorplanPoint): number {
  const cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  return Math.abs(cross) < 0.001 ? 0 : Math.sign(cross);
}

function pointOnSegment(point: ManualFloorplanPoint, start: ManualFloorplanPoint, end: ManualFloorplanPoint): boolean {
  return point[0] >= Math.min(start[0], end[0]) - 0.001 &&
    point[0] <= Math.max(start[0], end[0]) + 0.001 &&
    point[1] >= Math.min(start[1], end[1]) - 0.001 &&
    point[1] <= Math.max(start[1], end[1]) + 0.001;
}

function pointInsideOrOnPolygon(point: ManualFloorplanPoint, polygon: ManualFloorplanPoint[]): boolean {
  let inside = false;
  for (let index = 0, previous = polygon.length - 1; index < polygon.length; previous = index, index += 1) {
    const start = polygon[previous];
    const end = polygon[index];
    if (orientation(start, end, point) === 0 && pointOnSegment(point, start, end)) return true;
    const crosses = (end[1] > point[1]) !== (start[1] > point[1]) &&
      point[0] < ((start[0] - end[0]) * (point[1] - end[1])) / (start[1] - end[1]) + end[0];
    if (crosses) inside = !inside;
  }
  return inside;
}

function segmentCrossesPolygon(
  start: ManualFloorplanPoint,
  end: ManualFloorplanPoint,
  polygon: ManualFloorplanPoint[]
): boolean {
  return polygon.some((edgeStart, index) => {
    const edgeEnd = polygon[(index + 1) % polygon.length];
    const segmentStart = orientation(start, end, edgeStart);
    const segmentEnd = orientation(start, end, edgeEnd);
    const edgeToStart = orientation(edgeStart, edgeEnd, start);
    const edgeToEnd = orientation(edgeStart, edgeEnd, end);
    return segmentStart * segmentEnd < 0 && edgeToStart * edgeToEnd < 0;
  });
}
