import { MAX_CALIBRATION_ZONES, MAX_SOFTWARE_ZONES, MAX_ZONE_NAME_LENGTH } from "./constants";
import { clampPointsToBounds, rectPoints } from "./geometry";
import type { WebDeviceConfig, WebZone, WebZoneType } from "./types";

export interface AdvancedZoneDisplayModel {
  id: string;
  name: string;
  type: string;
  points: Array<[number, number]>;
  calibration: boolean;
}

export function isEmptyZone(zone: Pick<WebZone, "points">): boolean {
  return zone.points.length === 0 || zone.points.every(([x, y]) => x === 0 && y === 0);
}

export function calibrationType(type: WebZoneType): Extract<WebZoneType, "filter" | "reduced" | "disabled"> {
  if (type === "reduced" || type === "disabled") return type;
  return "filter";
}

export function isCalibrationZoneId(zoneId: string): boolean {
  return /^calibration_\d+$/.test(zoneId);
}

export function zoneSlotLabel(zoneId: string): string {
  const match = /^zone_(\d+)$/.exec(zoneId);
  return match ? `구역 ${match[1]}` : zoneId;
}

export function zoneDisplayName(zone: Pick<WebZone, "id" | "name">): string {
  return zone.name || zoneSlotLabel(zone.id);
}

export function limitZoneName(name: string): string {
  return Array.from(name.trim()).slice(0, MAX_ZONE_NAME_LENGTH).join("");
}

export function clampZoneToHardwareBounds<T extends { points: Array<[number, number]> }>(zone: T): T {
  return {
    ...zone,
    points: clampPointsToBounds(zone.points)
  };
}

export function normalizeSoftwareConfig(config: WebDeviceConfig): WebDeviceConfig {
  return stripPlaceholders(config);
}

export function stripPlaceholders(config: WebDeviceConfig): WebDeviceConfig {
  return {
    ...config,
    zones: config.zones
      .filter((zone) => !isEmptyZone(zone))
      .slice(0, MAX_SOFTWARE_ZONES)
      .map((zone) => {
        const { placeholder: _placeholder, ...rest } = zone;
        return clampZoneToHardwareBounds(rest);
      }),
    calibrationZones: (config.calibrationZones || [])
      .filter((zone) => !isEmptyZone(zone))
      .slice(0, MAX_CALIBRATION_ZONES)
      .map((zone) => clampZoneToHardwareBounds({ ...zone, type: calibrationType(zone.type) }))
  };
}

export function upsertZone(zones: WebZone[], nextZone: WebZone): WebZone[] {
  const found = zones.some((zone) => zone.id === nextZone.id);
  const next = found ? zones.map((zone) => (zone.id === nextZone.id ? nextZone : zone)) : [...zones, nextZone];
  return next.sort((a, b) => zoneOrder(a.id) - zoneOrder(b.id)).slice(0, MAX_SOFTWARE_ZONES);
}

export function nextZoneId(zones: WebZone[]): string {
  const used = new Set(zones.map((zone) => zone.id));
  for (let index = 1; index <= MAX_SOFTWARE_ZONES; index += 1) {
    const id = `zone_${index}`;
    if (!used.has(id)) return id;
  }
  return `zone_${zones.length + 1}`;
}

export function defaultZonePoints(index: number): Array<[number, number]> {
  const offset = Math.min(index, MAX_SOFTWARE_ZONES - 1) * 180;
  return rectPoints(-900 + offset, 1000 + offset, 900 + offset, 2400 + offset);
}

export function zoneOrder(zoneId: string): number {
  const match = /^(?:zone|calibration)_(\d+)$/.exec(zoneId);
  return match ? Number(match[1]) : 99;
}

export function normalizeAdvancedZone(zone: {
  id?: unknown;
  name?: unknown;
  type?: unknown;
  points?: unknown;
}): AdvancedZoneDisplayModel | null {
  if (typeof zone.id !== "string") return null;
  if (!Array.isArray(zone.points)) return null;
  const points = zone.points
    .map((point): [number, number] | null => {
      if (!Array.isArray(point) || point.length < 2) return null;
      const x = Number(point[0]);
      const y = Number(point[1]);
      return Number.isFinite(x) && Number.isFinite(y) ? [x, y] : null;
    })
    .filter((point): point is [number, number] => point !== null)
    .slice(0, 8);
  if (points.length < 3) return null;
  return {
    id: zone.id,
    name: typeof zone.name === "string" ? zone.name : "",
    type: typeof zone.type === "string" ? zone.type : "detection",
    points,
    calibration: isCalibrationZoneId(zone.id)
  };
}
