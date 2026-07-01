import type { RadarScreenPoint, RadarViewport } from "./types";
import { LD2450_FOV_DEGREES } from "./constants";

export {
  LD2450_FOV_DEGREES,
  LD2450_MAX_DISTANCE_MM,
  LD2450_ZONE_MAX_X_MM,
  LD2450_ZONE_MAX_Y_MM,
  LD2450_ZONE_MIN_X_MM,
  LD2450_ZONE_MIN_Y_MM
} from "./constants";

export function toScreenPoint(x: number, y: number, viewport: RadarViewport): RadarScreenPoint {
  const usableWidth = viewport.width - viewport.pad * 2;
  const usableHeight = viewport.height - viewport.pad * 2;
  const centerX = viewport.width / 2;
  const bottomY = viewport.height - viewport.pad;

  return {
    x: centerX + (x / viewport.rangeX) * (usableWidth / 2),
    y: bottomY - (y / viewport.rangeY) * usableHeight
  };
}

export function toRadarPoint(x: number, y: number, viewport: RadarViewport): RadarScreenPoint {
  const usableWidth = viewport.width - viewport.pad * 2;
  const usableHeight = viewport.height - viewport.pad * 2;
  const centerX = viewport.width / 2;
  const bottomY = viewport.height - viewport.pad;

  return {
    x: ((x - centerX) / (usableWidth / 2)) * viewport.rangeX,
    y: ((bottomY - y) / usableHeight) * viewport.rangeY
  };
}

export function radarViewportRangeX(rangeY: number, fovDegrees = LD2450_FOV_DEGREES): number {
  const halfFovRad = ((fovDegrees / 2) * Math.PI) / 180;
  return Math.sin(halfFovRad) * rangeY;
}

export function pointAtPolar(distance: number, angleDegrees: number): RadarScreenPoint {
  const angleRad = (angleDegrees * Math.PI) / 180;
  return {
    x: Math.sin(angleRad) * distance,
    y: Math.cos(angleRad) * distance
  };
}

export function isPointInRadarCoverage(
  x: number,
  y: number,
  viewport: RadarViewport
): boolean {
  if (y < 0) return false;
  const distance = Math.sqrt(x * x + y * y);
  if (distance > viewport.rangeY) return false;
  const angle = Math.atan2(x, y) * (180 / Math.PI);
  return Math.abs(angle) <= viewport.fovDegrees / 2;
}

export function distanceMeters(x: number, y: number): number {
  return Math.sqrt(x * x + y * y) / 1000;
}

export function distanceLabel(
  x: number,
  y: number,
  showDistance: boolean,
  decimals: number
): string {
  if (!showDistance) return "";
  const safeDecimals = Math.max(0, Math.floor(Number(decimals)));
  return `${distanceMeters(x, y).toFixed(safeDecimals)}m`;
}
