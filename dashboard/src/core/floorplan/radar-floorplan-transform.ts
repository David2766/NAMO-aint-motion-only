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
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const pxPerMmX = scaleFactor / scale.mmPerPxX;
  const pxPerMmY = scaleFactor / scale.mmPerPxY;
  const worldX = radarX * cos + radarY * sin;
  const worldY = radarX * sin - radarY * cos;
  return {
    x: placement.originX + worldX * pxPerMmX,
    y: placement.originY + worldY * pxPerMmY
  };
}

export function floorplanPxToRadarPoint(
  point: RadarScreenPoint,
  { placement, scale, scaleFactor = 1 }: FloorplanRadarTransformOptions
): ZonePoint {
  const angle = (placement.rotation * Math.PI) / 180;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const worldX = (point.x - placement.originX) * (scale.mmPerPxX / scaleFactor);
  const worldY = (point.y - placement.originY) * (scale.mmPerPxY / scaleFactor);
  return [
    worldX * cos + worldY * sin,
    worldX * sin - worldY * cos
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
