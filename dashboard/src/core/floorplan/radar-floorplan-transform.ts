import type { RadarScreenPoint, WebZone } from "../types";
import type { ZonePoint } from "../geometry";

export interface FloorplanRadarPlacement {
  originX: number;
  originY: number;
  rotation: number;
}

export interface FloorplanRadarScale {
  mmPerPxX: number;
  mmPerPxY: number;
}

export interface FloorplanRadarTransformOptions {
  placement: FloorplanRadarPlacement;
  scale: FloorplanRadarScale;
  scaleFactor?: number;
}

export function radarPointToFloorplanPx(
  [radarX, radarY]: ZonePoint,
  { placement, scale, scaleFactor = 1 }: FloorplanRadarTransformOptions
): RadarScreenPoint {
  const angle = (placement.rotation * Math.PI) / 180;
  const pxPerMmX = scaleFactor / scale.mmPerPxX;
  const pxPerMmY = scaleFactor / scale.mmPerPxY;
  const dx = radarX * pxPerMmX;
  const dy = -radarY * pxPerMmY;
  return {
    x: placement.originX + dx * Math.cos(angle) - dy * Math.sin(angle),
    y: placement.originY + dx * Math.sin(angle) + dy * Math.cos(angle)
  };
}

export function floorplanPxToRadarPoint(
  point: RadarScreenPoint,
  { placement, scale, scaleFactor = 1 }: FloorplanRadarTransformOptions
): ZonePoint {
  const angle = (placement.rotation * Math.PI) / 180;
  const dx = point.x - placement.originX;
  const dy = point.y - placement.originY;
  const unrotatedX = dx * Math.cos(angle) + dy * Math.sin(angle);
  const unrotatedY = -dx * Math.sin(angle) + dy * Math.cos(angle);
  return [
    unrotatedX * (scale.mmPerPxX / scaleFactor),
    -unrotatedY * (scale.mmPerPxY / scaleFactor)
  ];
}

export function radarPointsToFloorplanPx(points: ZonePoint[], options: FloorplanRadarTransformOptions): RadarScreenPoint[] {
  return points.map((point) => radarPointToFloorplanPx(point, options));
}

export function floorplanPxToRadarPoints(points: RadarScreenPoint[], options: FloorplanRadarTransformOptions): ZonePoint[] {
  return points.map((point) => floorplanPxToRadarPoint(point, options));
}

export function radarZoneToFloorplanPolygon(zone: Pick<WebZone, "points">, options: FloorplanRadarTransformOptions): RadarScreenPoint[] {
  return radarPointsToFloorplanPx(zone.points ?? [], options);
}

export function floorplanPolygonToRadarZonePoints(points: RadarScreenPoint[], options: FloorplanRadarTransformOptions): ZonePoint[] {
  return floorplanPxToRadarPoints(points, options);
}

