import {
  LD2450_ZONE_MAX_X_MM,
  LD2450_ZONE_MAX_Y_MM,
  LD2450_ZONE_MIN_X_MM,
  LD2450_ZONE_MIN_Y_MM
} from "./constants";
import type { RadarScreenPoint } from "./types";

export type ZonePoint = [number, number];

export interface ZoneBounds {
  minX: number;
  maxX: number;
  minY: number;
  maxY: number;
  width: number;
  height: number;
}

export interface RectLike {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

export function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value));
}

export function clampShift(minValue: number, maxValue: number, minLimit: number, maxLimit: number): number {
  if (minValue < minLimit) return minLimit - minValue;
  if (maxValue > maxLimit) return maxLimit - maxValue;
  return 0;
}

export function rectPoints(x1: number, y1: number, x2: number, y2: number): ZonePoint[] {
  const minX = Math.min(x1, x2);
  const maxX = Math.max(x1, x2);
  const minY = Math.min(y1, y2);
  const maxY = Math.max(y1, y2);
  return [
    [minX, minY],
    [maxX, minY],
    [maxX, maxY],
    [minX, maxY]
  ];
}

export function boundsFromPoints(points: ZonePoint[]): ZoneBounds {
  const xs = points.map(([x]) => x);
  const ys = points.map(([, y]) => y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  return {
    minX,
    maxX,
    minY,
    maxY,
    width: maxX - minX,
    height: maxY - minY
  };
}

export function clampPointsToBounds(
  points: ZonePoint[],
  minX = LD2450_ZONE_MIN_X_MM,
  maxX = LD2450_ZONE_MAX_X_MM,
  minY = LD2450_ZONE_MIN_Y_MM,
  maxY = LD2450_ZONE_MAX_Y_MM
): ZonePoint[] {
  if (points.length === 0) return points;
  const bounds = boundsFromPoints(points);
  const shiftX = clampShift(bounds.minX, bounds.maxX, minX, maxX);
  const shiftY = clampShift(bounds.minY, bounds.maxY, minY, maxY);
  return points.map(([x, y]) => [
    clamp(Math.round(x + shiftX), minX, maxX),
    clamp(Math.round(y + shiftY), minY, maxY)
  ]);
}

export function pointInPolygon(point: RadarScreenPoint, points: ZonePoint[]): boolean {
  if (points.length < 3) return false;
  let inside = false;
  for (let index = 0, previous = points.length - 1; index < points.length; previous = index++) {
    const [xi, yi] = points[index];
    const [xj, yj] = points[previous];
    const intersects = yi > point.y !== yj > point.y && point.x < ((xj - xi) * (point.y - yi)) / (yj - yi || 0.0001) + xi;
    if (intersects) inside = !inside;
  }
  return inside;
}
