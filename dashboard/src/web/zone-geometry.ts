import { EXIT_LINE_STRIP_WIDTH_MM, MAX_SOFTWARE_ZONES, MAX_ZONE_POINTS } from "../core/constants";
import { boundsFromPoints, clampPointsToBounds, rectPoints } from "../core/geometry";
export { clamp, rectPoints } from "../core/geometry";
import {
  defaultZonePoints,
  limitZoneName,
  localizedDefaultZoneName,
  nextZoneId,
  upsertZone,
  type ZoneNameLabels
} from "../core/zones";
import type { WebDeviceConfig, WebZoneType } from "../core/types";
import type { RadarScreenPoint, WebZone } from "../core/types";

export type PolygonPoint = [number, number];

export interface PolygonPointBounds {
  width?: number;
  height?: number;
}

export interface PolygonPointMutationResult {
  points: PolygonPoint[];
  selectedPointIndex: number;
  changed: boolean;
  messageCode?: ZoneGeometryMessageCode;
  messageParams?: ZoneGeometryMessageParams;
}

export interface PolygonObjectMutationResult<T extends { points: PolygonPoint[] }> extends PolygonPointMutationResult {
  item: T;
}

export interface ZoneMutationResult {
  config: WebDeviceConfig;
  selectedZoneId?: string;
  selectedPointIndex?: number;
  changed: boolean;
  messageCode?: ZoneGeometryMessageCode;
  messageParams?: ZoneGeometryMessageParams;
}

export type ZoneGeometryMessageCode =
  | "polygon_max_points"
  | "polygon_min_points"
  | "zones_max_count"
  | "zone_max_points"
  | "zone_min_points";

export type ZoneGeometryMessageParams = Record<string, string | number | boolean | undefined>;

export interface ExitLine {
  start: PolygonPoint;
  end: PolygonPoint;
}

function normalizePolygonPoint(point: RadarScreenPoint, bounds?: PolygonPointBounds): PolygonPoint {
  const maxX = Number.isFinite(bounds?.width) ? Number(bounds?.width) : point.x;
  const maxY = Number.isFinite(bounds?.height) ? Number(bounds?.height) : point.y;
  return [
    Math.round(Math.min(maxX, Math.max(0, point.x))),
    Math.round(Math.min(maxY, Math.max(0, point.y)))
  ];
}

export function insertPolygonPoint(
  points: PolygonPoint[],
  edgeIndex: number,
  point: RadarScreenPoint,
  options: { maxPoints?: number; bounds?: PolygonPointBounds } = {}
): PolygonPointMutationResult {
  if (!points.length || edgeIndex < 0) return { points, selectedPointIndex: -1, changed: false };
  if (options.maxPoints && points.length >= options.maxPoints) {
    return {
      points,
      selectedPointIndex: -1,
      changed: false,
      messageCode: "polygon_max_points",
      messageParams: { maxPoints: options.maxPoints }
    };
  }
  const insertIndex = Math.min(edgeIndex + 1, points.length);
  return {
    points: [...points.slice(0, insertIndex), normalizePolygonPoint(point, options.bounds), ...points.slice(insertIndex)],
    selectedPointIndex: insertIndex,
    changed: true
  };
}

export function deletePolygonPoint(
  points: PolygonPoint[],
  pointIndex: number,
  options: { minPoints?: number } = {}
): PolygonPointMutationResult {
  const minPoints = options.minPoints ?? 3;
  if (points.length <= minPoints || pointIndex < 0 || pointIndex >= points.length) {
    return {
      points,
      selectedPointIndex: pointIndex,
      changed: false,
      messageCode: "polygon_min_points",
      messageParams: { minPoints }
    };
  }
  return {
    points: points.filter((_, index) => index !== pointIndex),
    selectedPointIndex: Math.min(pointIndex, points.length - 2),
    changed: true
  };
}

export function movePolygonPoint(
  points: PolygonPoint[],
  pointIndex: number,
  point: RadarScreenPoint,
  options: { bounds?: PolygonPointBounds } = {}
): PolygonPointMutationResult {
  if (pointIndex < 0 || pointIndex >= points.length) {
    return { points, selectedPointIndex: pointIndex, changed: false };
  }
  return {
    points: points.map((existing, index): PolygonPoint =>
      index === pointIndex ? normalizePolygonPoint(point, options.bounds) : existing
    ),
    selectedPointIndex: pointIndex,
    changed: true
  };
}

export function insertPolygonObjectPoint<T extends { points: PolygonPoint[] }>(
  item: T,
  edgeIndex: number,
  point: RadarScreenPoint,
  options: { maxPoints?: number; bounds?: PolygonPointBounds } = {}
): PolygonObjectMutationResult<T> {
  const result = insertPolygonPoint(item.points, edgeIndex, point, options);
  return {
    ...result,
    item: result.changed ? { ...item, points: result.points } : item
  };
}

export function deletePolygonObjectPoint<T extends { points: PolygonPoint[] }>(
  item: T,
  pointIndex: number,
  options: { minPoints?: number } = {}
): PolygonObjectMutationResult<T> {
  const result = deletePolygonPoint(item.points, pointIndex, options);
  return {
    ...result,
    item: result.changed ? { ...item, points: result.points } : item
  };
}

export function movePolygonObjectPoint<T extends { points: PolygonPoint[] }>(
  item: T,
  pointIndex: number,
  point: RadarScreenPoint,
  options: { bounds?: PolygonPointBounds } = {}
): PolygonObjectMutationResult<T> {
  const result = movePolygonPoint(item.points, pointIndex, point, options);
  return {
    ...result,
    item: result.changed ? { ...item, points: result.points } : item
  };
}

export function addSoftwareZone(
  config: WebDeviceConfig,
  type: WebZoneType,
  labels: Pick<ZoneNameLabels, "zoneLabel" | "exitPointLabel">
): ZoneMutationResult {
  if (config.zones.length >= MAX_SOFTWARE_ZONES) {
    return {
      config,
      changed: false,
      messageCode: "zones_max_count",
      messageParams: { max: MAX_SOFTWARE_ZONES }
    };
  }
  const id = nextZoneId(config.zones);
  const zone: WebZone = {
    id,
    name: localizedDefaultZoneName(id, type, labels),
    type,
    shape: type === "exit" ? "polygon" : "rect",
    points: type === "exit" ? defaultExitLineStripPoints(config.zones.length) : defaultZonePoints(config.zones.length)
  };
  return {
    config: { ...config, zones: upsertZone(config.zones, zone) },
    selectedZoneId: id,
    selectedPointIndex: -1,
    changed: true
  };
}

export function exitLineFromZone(zone: Pick<WebZone, "points">): ExitLine {
  const points = zone.points;
  if (points.length >= 4) {
    return {
      start: midpoint(points[0], points[3]),
      end: midpoint(points[1], points[2])
    };
  }
  if (points.length >= 2) {
    return { start: points[0], end: points[1] };
  }
  return { start: [-800, 2200], end: [800, 2200] };
}

export function exitLineStripPoints(start: PolygonPoint, end: PolygonPoint, widthMm = EXIT_LINE_STRIP_WIDTH_MM): PolygonPoint[] {
  const dx = end[0] - start[0];
  const dy = end[1] - start[1];
  const length = Math.hypot(dx, dy);
  if (length < 1) return exitLineStripPoints(start, [start[0] + 1, start[1]], widthMm);
  const halfWidth = widthMm / 2;
  const nx = (-dy / length) * halfWidth;
  const ny = (dx / length) * halfWidth;
  return clampPointsToBounds([
    [Math.round(start[0] + nx), Math.round(start[1] + ny)],
    [Math.round(end[0] + nx), Math.round(end[1] + ny)],
    [Math.round(end[0] - nx), Math.round(end[1] - ny)],
    [Math.round(start[0] - nx), Math.round(start[1] - ny)]
  ]);
}

export function moveExitLineZone(zone: WebZone, startPoint: RadarScreenPoint, currentPoint: RadarScreenPoint): WebZone {
  const line = exitLineFromZone(zone);
  const dx = currentPoint.x - startPoint.x;
  const dy = currentPoint.y - startPoint.y;
  return {
    ...zone,
    shape: "polygon",
    points: exitLineStripPoints(
      [Math.round(line.start[0] + dx), Math.round(line.start[1] + dy)],
      [Math.round(line.end[0] + dx), Math.round(line.end[1] + dy)]
    )
  };
}

export function updateExitLineEndpoint(zone: WebZone, pointIndex: number | undefined, point: RadarScreenPoint): WebZone {
  const line = exitLineFromZone(zone);
  if (pointIndex === 0) {
    return { ...zone, shape: "polygon", points: exitLineStripPoints([Math.round(point.x), Math.round(point.y)], line.end) };
  }
  if (pointIndex === 1) {
    return { ...zone, shape: "polygon", points: exitLineStripPoints(line.start, [Math.round(point.x), Math.round(point.y)]) };
  }
  return zone;
}

function defaultExitLineStripPoints(index: number): PolygonPoint[] {
  const offset = Math.min(index, MAX_SOFTWARE_ZONES - 1) * 160;
  return exitLineStripPoints([-900 + offset, 2200 + offset], [900 + offset, 2200 + offset]);
}

function midpoint(a: PolygonPoint, b: PolygonPoint): PolygonPoint {
  return [Math.round((a[0] + b[0]) / 2), Math.round((a[1] + b[1]) / 2)];
}

export function deleteZone(config: WebDeviceConfig, zoneId: string): ZoneMutationResult {
  if (!zoneId) return { config, changed: false };
  const calibrationZones = config.calibrationZones ?? [];
  if (calibrationZones.some((zone) => zone.id === zoneId)) {
    const nextCalibrationZones = calibrationZones.filter((zone) => zone.id !== zoneId);
    return {
      config: { ...config, calibrationZones: nextCalibrationZones },
      selectedZoneId: config.zones[0]?.id ?? nextCalibrationZones[0]?.id ?? "",
      selectedPointIndex: -1,
      changed: true
    };
  }
  if (!config.zones.some((zone) => zone.id === zoneId)) return { config, changed: false };
  const nextZones = config.zones.filter((zone) => zone.id !== zoneId);
  return {
    config: { ...config, zones: nextZones },
    selectedZoneId: nextZones[0]?.id ?? calibrationZones[0]?.id ?? "",
    selectedPointIndex: -1,
    changed: true
  };
}

export function renameZone(config: WebDeviceConfig, zoneId: string, name: string): ZoneMutationResult {
  const nextName = limitZoneName(name);
  if (!config.zones.some((zone) => zone.id === zoneId)) return { config, changed: false };
  return {
    config: {
      ...config,
      zones: config.zones.map((zone) => zone.id === zoneId ? { ...zone, name: nextName } : zone)
    },
    changed: true
  };
}

export function setZoneType(config: WebDeviceConfig, zoneId: string, type: WebZoneType): ZoneMutationResult {
  if (!config.zones.some((zone) => zone.id === zoneId)) return { config, changed: false };
  return {
    config: {
      ...config,
      zones: config.zones.map((zone) => zone.id === zoneId ? { ...zone, type } : zone)
    },
    changed: true
  };
}

export function setCalibrationZoneType(
  config: WebDeviceConfig,
  zoneId: string,
  type: Extract<WebZoneType, "filter" | "reduced" | "disabled">
): ZoneMutationResult {
  const calibrationZones = config.calibrationZones ?? [];
  if (!calibrationZones.some((zone) => zone.id === zoneId)) return { config, changed: false };
  return {
    config: {
      ...config,
      calibrationZones: calibrationZones.map((zone) => zone.id === zoneId ? { ...zone, type } : zone)
    },
    changed: true
  };
}

export function convertZoneToRect(zone: WebZone): WebZone {
  if (zone.type === "exit") return zone;
  if (zone.points.length < 3) return zone;
  const bounds = boundsFromPoints(zone.points);
  return {
    ...zone,
    shape: "rect",
    points: rectPoints(bounds.minX, bounds.minY, bounds.maxX, bounds.maxY)
  };
}

export function convertZoneToRectInConfig(config: WebDeviceConfig, zoneId: string): ZoneMutationResult {
  const zone = config.zones.find((item) => item.id === zoneId);
  if (!zone || zone.shape === "rect") return { config, changed: false };
  return {
    config: {
      ...config,
      zones: config.zones.map((item) => item.id === zoneId ? convertZoneToRect(item) : item)
    },
    selectedPointIndex: -1,
    changed: true
  };
}

export function insertZonePoint(zone: WebZone, edgeIndex: number, point: RadarScreenPoint): WebZone {
  const polygonZone: WebZone = { ...zone, shape: "polygon" };
  const result = insertPolygonObjectPoint(polygonZone, edgeIndex, point, { maxPoints: MAX_ZONE_POINTS });
  if (!result.changed) return zone;
  return clampZoneToHardwareBounds(result.item);
}

export function insertZonePointInConfig(
  config: WebDeviceConfig,
  zoneId: string,
  edgeIndex: number,
  point: RadarScreenPoint
): ZoneMutationResult {
  const zone = config.zones.find((item) => item.id === zoneId);
  if (!zone) return { config, changed: false };
  if (zone.type === "exit") return { config, changed: false };
  if (zone.points.length >= MAX_ZONE_POINTS) {
    return {
      config,
      changed: false,
      messageCode: "zone_max_points",
      messageParams: { maxPoints: MAX_ZONE_POINTS }
    };
  }
  const insertIndex = Math.min(edgeIndex + 1, zone.points.length);
  const nextZone = insertZonePoint(zone, edgeIndex, point);
  return {
    config: { ...config, zones: upsertZone(config.zones, nextZone) },
    selectedZoneId: zoneId,
    selectedPointIndex: insertIndex,
    changed: true
  };
}

export function deleteZonePoint(zone: WebZone, pointIndex: number): WebZone {
  if (zone.type === "exit") return zone;
  if (zone.shape !== "polygon") return zone;
  const result = deletePolygonObjectPoint(zone, pointIndex);
  if (!result.changed) return zone;
  return result.item;
}

export function deleteZonePointInConfig(config: WebDeviceConfig, zoneId: string, pointIndex: number): ZoneMutationResult {
  const zone = config.zones.find((item) => item.id === zoneId);
  if (!zone || zone.shape !== "polygon" || zone.points.length <= 3 || pointIndex < 0 || pointIndex >= zone.points.length) {
    return {
      config,
      changed: false,
      messageCode: "zone_min_points",
      messageParams: { minPoints: 3 }
    };
  }
  const nextPointIndex = Math.min(pointIndex, zone.points.length - 2);
  return {
    config: {
      ...config,
      zones: config.zones.map((item) => item.id === zoneId ? deleteZonePoint(item, pointIndex) : item)
    },
    selectedZoneId: zoneId,
    selectedPointIndex: nextPointIndex,
    changed: true
  };
}

export function moveZone(zone: WebZone, startPoint: RadarScreenPoint, currentPoint: RadarScreenPoint): WebZone {
  const dx = currentPoint.x - startPoint.x;
  const dy = currentPoint.y - startPoint.y;
  return clampZoneToHardwareBounds({
    ...zone,
    points: zone.points.map(([x, y]): [number, number] => [Math.round(x + dx), Math.round(y + dy)])
  });
}

export function updateZonePoint(zone: WebZone, pointIndex: number | undefined, point: RadarScreenPoint): WebZone {
  if (pointIndex === undefined || pointIndex < 0 || pointIndex >= zone.points.length) return zone;
  if (zone.shape === "rect") {
    return resizeRectZone(zone, pointIndex, point);
  }
  const result = movePolygonObjectPoint(zone, pointIndex, point);
  if (!result.changed) return zone;
  return clampZoneToHardwareBounds(result.item);
}

export function resizeRectZone(zone: WebZone, pointIndex: number, point: RadarScreenPoint): WebZone {
  if (zone.shape !== "rect" || zone.points.length < 4) return zone;
  const xs = zone.points.map(([x]) => x);
  const ys = zone.points.map(([, y]) => y);
  let minX = Math.min(...xs);
  let maxX = Math.max(...xs);
  let minY = Math.min(...ys);
  let maxY = Math.max(...ys);

  if (pointIndex === 0) {
    minX = point.x;
    minY = point.y;
  } else if (pointIndex === 1) {
    maxX = point.x;
    minY = point.y;
  } else if (pointIndex === 2) {
    maxX = point.x;
    maxY = point.y;
  } else if (pointIndex === 3) {
    minX = point.x;
    maxY = point.y;
  }

  return clampZoneToHardwareBounds({
    ...zone,
    points: rectPoints(minX, minY, maxX, maxY)
  });
}

export function resizeCalibrationZone(
  zone: WebZone,
  pointIndex: number | undefined,
  point: RadarScreenPoint,
  allowShrink: boolean
): WebZone {
  if (pointIndex === undefined || pointIndex < 0 || zone.points.length < 4) return zone;
  const bounds = zoneBounds(zone);
  let nextMinX = bounds.minX;
  let nextMaxX = bounds.maxX;
  let nextMinY = bounds.minY;
  let nextMaxY = bounds.maxY;

  if (pointIndex === 0) {
    nextMinX = allowShrink ? point.x : Math.min(bounds.minX, point.x);
    nextMinY = allowShrink ? point.y : Math.min(bounds.minY, point.y);
  } else if (pointIndex === 1) {
    nextMaxX = allowShrink ? point.x : Math.max(bounds.maxX, point.x);
    nextMinY = allowShrink ? point.y : Math.min(bounds.minY, point.y);
  } else if (pointIndex === 2) {
    nextMaxX = allowShrink ? point.x : Math.max(bounds.maxX, point.x);
    nextMaxY = allowShrink ? point.y : Math.max(bounds.maxY, point.y);
  } else if (pointIndex === 3) {
    nextMinX = allowShrink ? point.x : Math.min(bounds.minX, point.x);
    nextMaxY = allowShrink ? point.y : Math.max(bounds.maxY, point.y);
  }

  return clampZoneToHardwareBounds({
    ...zone,
    shape: "rect",
    points: rectPoints(nextMinX, nextMinY, nextMaxX, nextMaxY)
  });
}

export function calibrationResizeShrinks(zone: WebZone, pointIndex: number | undefined, point: RadarScreenPoint): boolean {
  if (pointIndex === undefined || pointIndex < 0 || zone.points.length < 4) return false;
  const current = zoneBounds(zone);
  const resized = zoneBounds(resizeCalibrationZone(zone, pointIndex, point, true));
  return resized.width < current.width || resized.height < current.height;
}

export function zoneBounds(zone: WebZone): { minX: number; maxX: number; minY: number; maxY: number; width: number; height: number } {
  return boundsFromPoints(zone.points);
}

export function clampZoneToHardwareBounds(zone: WebZone): WebZone {
  return {
    ...zone,
    points: clampPointsToBounds(zone.points)
  };
}
