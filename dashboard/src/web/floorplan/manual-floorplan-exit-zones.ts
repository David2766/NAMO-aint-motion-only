import { MAX_SOFTWARE_ZONES } from "../../core/constants";
import { floorplanPxToRadarPoint } from "../../core/floorplan/radar-floorplan-transform";
import type { FloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import type { WebDeviceConfig, WebZone } from "../../core/types";
import { localizedDefaultZoneName, nextZoneId } from "../../core/zones";
import { exitLineStripPoints } from "../zone-geometry";
import {
  manualFloorplanDoorEndpoints,
  type ManualFloorplanDoor
} from "./manual-floorplan-doors";

export interface ManualDoorExitZoneLabels {
  zoneLabel: (index: string) => string;
  exitPointLabel: (index: string) => string;
}

export type ManualDoorExitZoneResult =
  | { config: WebDeviceConfig; added: number; reason: null }
  | { config: WebDeviceConfig; added: 0; reason: "zone_limit" };

export type ManualDoorExitZoneSyncResult =
  | { config: WebDeviceConfig; added: number; updated: number; removed: number; reason: null }
  | { config: WebDeviceConfig; added: 0; updated: 0; removed: 0; reason: "zone_limit" };

export function addManualDoorExitZones(
  config: WebDeviceConfig,
  document: FloorplanStorageDocument,
  doors: ManualFloorplanDoor[],
  labels: ManualDoorExitZoneLabels
): ManualDoorExitZoneResult {
  const result = synchronizeManualDoorExitZones(config, document, document, [], doors, labels);
  return result.reason
    ? { config, added: 0, reason: result.reason }
    : { config: result.config, added: result.added, reason: null };
}

export function synchronizeManualDoorExitZones(
  config: WebDeviceConfig,
  previousDocument: FloorplanStorageDocument,
  nextDocument: FloorplanStorageDocument,
  previousDoors: ManualFloorplanDoor[],
  nextDoors: ManualFloorplanDoor[],
  labels: ManualDoorExitZoneLabels
): ManualDoorExitZoneSyncResult {
  const previousPoints = new Map(
    previousDoors.map((door) => [door.id, doorExitPoints(previousDocument, door)])
  );
  const claimedZoneIds = new Set<string>();
  const previousZones = new Map<string, WebZone>();
  for (const door of previousDoors) {
    const points = previousPoints.get(door.id);
    const zone = points && config.zones.find((candidate) =>
      !claimedZoneIds.has(candidate.id) && candidate.type === "exit" && samePoints(candidate.points, points)
    );
    if (!zone) continue;
    claimedZoneIds.add(zone.id);
    previousZones.set(door.id, zone);
  }

  const zones = config.zones.filter((zone) => !claimedZoneIds.has(zone.id));
  let added = 0;
  let updated = 0;
  const generated: WebZone[] = [];
  for (const door of nextDoors) {
    const points = doorExitPoints(nextDocument, door);
    const previous = previousZones.get(door.id);
    if (previous) {
      if (!samePoints(previous.points, points)) updated += 1;
      generated.push({ ...previous, points });
      continue;
    }
    if ([...zones, ...generated].some((zone) => zone.type === "exit" && samePoints(zone.points, points))) {
      continue;
    }
    const currentZones = [...zones, ...generated];
    const id = nextZoneId(currentZones);
    generated.push({
      id,
      name: localizedDefaultZoneName(id, "exit", labels),
      type: "exit",
      shape: "polygon",
      points
    });
    added += 1;
  }

  if (zones.length + generated.length > MAX_SOFTWARE_ZONES) {
    return { config, added: 0, updated: 0, removed: 0, reason: "zone_limit" };
  }
  const nextZones = [...zones, ...generated];
  return {
    config: sameZones(config.zones, nextZones) ? config : { ...config, zones: nextZones },
    added,
    updated,
    removed: [...previousZones.keys()].filter((doorId) => !nextDoors.some((door) => door.id === doorId)).length,
    reason: null
  };
}

function doorExitPoints(document: FloorplanStorageDocument, door: ManualFloorplanDoor): Array<[number, number]> {
  const options = {
    placement: {
      originX: document.radar.originPx[0],
      originY: document.radar.originPx[1],
      rotation: document.radar.rotationDeg
    },
    scale: document.scale,
    scaleFactor: document.radar.scale
  };
  const endpoints = manualFloorplanDoorEndpoints(door);
  const start = floorplanPxToRadarPoint({ x: endpoints.start[0], y: endpoints.start[1] }, options);
  const end = floorplanPxToRadarPoint({ x: endpoints.end[0], y: endpoints.end[1] }, options);
  return exitLineStripPoints(start, end);
}

function samePoints(first: Array<[number, number]>, second: Array<[number, number]>): boolean {
  return first.length === second.length && first.every(([x, y], index) =>
    x === second[index][0] && y === second[index][1]
  );
}

function sameZones(first: WebZone[], second: WebZone[]): boolean {
  return JSON.stringify(first) === JSON.stringify(second);
}
