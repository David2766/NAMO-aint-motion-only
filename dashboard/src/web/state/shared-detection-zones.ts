import {
  advanceFloorplanAutomationAreasRevision,
  FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION,
  type FloorplanAutomationArea,
  type FloorplanStorageDocument
} from "../../core/floorplan/floorplan-storage";
import {
  floorplanPolygonToRadarZonePoints,
  radarZoneToFloorplanPolygon,
  type FloorplanRadarTransformOptions
} from "../../core/floorplan/radar-floorplan-transform";
import { stripPlaceholders } from "../../core/zones";
import type { WebDeviceConfig, WebZone } from "../../core/types";
import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";

export function usesSharedDetectionZones(site: PlatformSiteConfigurationStatus | null): boolean {
  return Boolean(site && site.memberCount > 1 && site.floorplan.committed);
}

export function sharedDetectionZoneEditable(
  zone: WebZone | null | undefined,
  site: PlatformSiteConfigurationStatus | null
): boolean {
  return !zone || zone.placeholder || zone.type !== "detection" || !site ||
    !usesSharedDetectionZones(site) || site.localOwner;
}

export function mergeSharedDetectionZones(
  config: WebDeviceConfig,
  document: FloorplanStorageDocument,
  site: PlatformSiteConfigurationStatus
): WebDeviceConfig {
  if (!usesSharedDetectionZones(site)) return config;
  const placement = config.floorplan?.radar;
  const local = config.zones.filter((zone) => zone.type !== "detection");
  if (!validLocalRadarPlacement(placement)) {
    return { ...config, zones: local };
  }
  const spaceId = currentSpaceId(config, document);
  const canonical = (document.automationAreas?.areas ?? [])
    .filter((area) => area.enabled && area.spaceId === spaceId)
    .map((area): WebZone => ({
      id: sharedRuntimeId(area.areaId),
      name: area.name,
      type: "detection",
      shape: "polygon",
      points: floorplanPolygonToRadarZonePoints(
        area.pointsPx.map(([x, y]) => ({ x, y })),
        transformOptions(document, placement)
      ).map(([x, y]) => [Math.round(x), Math.round(y)]),
      shared: true,
      sharedAreaId: area.areaId
    }));
  const legacyOwnerDetection = site.localOwner && canonical.length === 0
    ? config.zones.filter((zone) => zone.type === "detection")
    : [];
  return { ...config, zones: [...canonical, ...legacyOwnerDetection, ...local] };
}

export function mergeSharedDetectionZonesAfterLocalSave(
  config: WebDeviceConfig,
  document: FloorplanStorageDocument,
  site: PlatformSiteConfigurationStatus
): WebDeviceConfig {
  return mergeSharedDetectionZones(
    stripPlaceholders(localZoneConfig(config, site)),
    document,
    site
  );
}

export function localZoneConfig(
  config: WebDeviceConfig,
  site: PlatformSiteConfigurationStatus | null
): WebDeviceConfig {
  if (!usesSharedDetectionZones(site)) return config;
  return {
    ...config,
    zones: config.zones.filter((zone) => zone.type !== "detection")
  };
}

export function withSharedDetectionZones(
  document: FloorplanStorageDocument,
  config: WebDeviceConfig
): FloorplanStorageDocument {
  const placement = config.floorplan?.radar;
  if (!validLocalRadarPlacement(placement)) return document;
  const spaceId = currentSpaceId(config, document);
  const retained = (document.automationAreas?.areas ?? []).filter((area) => area.spaceId !== spaceId);
  const detection = config.zones
    .filter((zone) => zone.type === "detection" && !zone.placeholder)
    .map((zone): FloorplanAutomationArea => ({
      areaId: zone.sharedAreaId ?? zone.id,
      name: zone.name,
      spaceId,
      enabled: true,
      pointsPx: radarZoneToFloorplanPolygon(zone, transformOptions(document, placement))
        .map(({ x, y }) => [roundPoint(x), roundPoint(y)])
    }));
  const current = document.automationAreas?.areas ?? [];
  const nextAreas = [...retained, ...detection];
  if (JSON.stringify(current) === JSON.stringify(nextAreas)) return document;
  return advanceFloorplanAutomationAreasRevision({
    ...document,
    automationAreas: {
      schemaVersion: FLOORPLAN_AUTOMATION_AREAS_SCHEMA_VERSION,
      revision: document.automationAreas?.revision ?? 0,
      areas: nextAreas
    }
  });
}

function currentSpaceId(config: WebDeviceConfig, document: FloorplanStorageDocument): string {
  return config.floorplan?.room?.id || document.rooms[0]?.id || "default_space";
}

function transformOptions(
  document: FloorplanStorageDocument,
  placement: NonNullable<NonNullable<WebDeviceConfig["floorplan"]>["radar"]>
): FloorplanRadarTransformOptions {
  return {
    placement: {
      originX: placement.originX,
      originY: placement.originY,
      rotation: placement.rotation
    },
    scale: document.scale,
    scaleFactor: placement.scale
  };
}

function validLocalRadarPlacement(
  placement: NonNullable<WebDeviceConfig["floorplan"]>["radar"] | null | undefined
): placement is NonNullable<NonNullable<WebDeviceConfig["floorplan"]>["radar"]> {
  return Boolean(placement &&
    Number.isFinite(placement.originX) &&
    Number.isFinite(placement.originY) &&
    Number.isFinite(placement.rotation) &&
    Number.isFinite(placement.scale) &&
    Math.abs(placement.originX) <= 100000 &&
    Math.abs(placement.originY) <= 100000 &&
    placement.scale > 0 && placement.scale <= 10 &&
    (placement.poseVersion === undefined ||
      (Number.isInteger(placement.poseVersion) && placement.poseVersion > 0)));
}

function sharedRuntimeId(areaId: string): string {
  return `shared:${areaId}`;
}

function roundPoint(value: number): number {
  return Math.round(value * 100) / 100;
}
