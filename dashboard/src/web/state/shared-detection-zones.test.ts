import { describe, expect, it } from "vitest";
import { buildFloorplanStorageDocument } from "../../core/floorplan/floorplan-storage";
import { radarPointsToFloorplanPx } from "../../core/floorplan/radar-floorplan-transform";
import type { WebDeviceConfig, WebZone } from "../../core/types";
import type { PlatformSiteConfigurationStatus } from "../api/platform-pairing";
import {
  localZoneConfig,
  mergeSharedDetectionZones,
  mergeSharedDetectionZonesAfterLocalSave,
  sharedDetectionZoneEditable,
  usesSharedDetectionZones,
  withSharedDetectionZones
} from "./shared-detection-zones";

function documentFixture() {
  return buildFloorplanStorageDocument({
    image: { width: 200, height: 120 },
    scale: {
      widthMm: 2000,
      heightMm: 1200,
      outerBounds: { x: 0, y: 0, width: 200, height: 120 },
      mmPerPxX: 10,
      mmPerPxY: 10
    },
    radar: { originX: 100, originY: 100, rotation: 0, scale: 1 },
    rooms: [{
      id: "room_1",
      name: "Room",
      kind: "room",
      confidence: 100,
      status: "confirmed",
      shape: "polygon",
      rect: { x: 0, y: 0, width: 200, height: 120 },
      points: [[0, 0], [200, 0], [200, 120], [0, 120]]
    }]
  });
}

function site(localOwner: boolean): PlatformSiteConfigurationStatus {
  return {
    memberCount: 2,
    localOwner,
    floorplan: { committed: true, floorplanId: "fp-test", geometryVersion: 1, configHash: 1, imageHash: 1 }
  } as PlatformSiteConfigurationStatus;
}

function zone(id: string, type: WebZone["type"]): WebZone {
  return {
    id,
    name: id,
    type,
    shape: "polygon",
    points: [[-500, 500], [500, 500], [500, 1000], [-500, 1000]]
  };
}

function config(zones: WebZone[]): WebDeviceConfig {
  return {
    version: 1,
    zones,
    calibrationZones: [],
    floorplan: {
      enabled: true,
      hasImage: true,
      radar: { originX: 100, originY: 100, rotation: 0, scale: 1, poseVersion: 3 },
      room: { id: "room_1", name: "Room", source: "stored_room" }
    }
  };
}

describe("shared detection zones", () => {
  it("keeps standalone detection zones local", () => {
    const standalone = { ...site(true), memberCount: 1 };
    const source = config([zone("desk", "detection")]);

    expect(usesSharedDetectionZones(standalone)).toBe(false);
    expect(localZoneConfig(source, standalone).zones).toHaveLength(1);
  });

  it("stores owner detection zones in the Floorplan and loads them read-only on a Node", () => {
    const source = config([zone("desk", "detection"), zone("curtain", "filter")]);
    const document = withSharedDetectionZones(documentFixture(), source);
    const nodeConfig = mergeSharedDetectionZones(config([zone("curtain", "filter")]), document, site(false));

    expect(document.automationAreas?.revision).toBe(1);
    expect(document.automationAreas?.areas.map((area) => area.areaId)).toEqual(["desk"]);
    expect(nodeConfig.zones.map((item) => item.id)).toEqual(["shared:desk", "curtain"]);
    expect(nodeConfig.zones[0]).toMatchObject({
      type: "detection",
      shared: true,
      sharedAreaId: "desk"
    });
    expect(sharedDetectionZoneEditable(nodeConfig.zones[0], site(false))).toBe(false);
    expect(sharedDetectionZoneEditable(nodeConfig.zones[0], site(true))).toBe(true);
  });

  it("keeps legacy owner detection zones available for one-time promotion", () => {
    const source = config([zone("desk", "detection"), zone("curtain", "filter")]);
    const merged = mergeSharedDetectionZones(source, documentFixture(), site(true));

    expect(merged.zones.map((item) => item.id)).toEqual(["desk", "curtain"]);
    expect(merged.zones[0]?.shared).toBeUndefined();
  });

  it("does not duplicate shared detection zones in the local config payload", () => {
    const source = config([
      { ...zone("desk", "detection"), shared: true },
      zone("curtain", "filter")
    ]);

    expect(localZoneConfig(source, site(true)).zones.map((item) => item.id)).toEqual(["curtain"]);
  });

  it("keeps shared and local Zone identifiers distinct at runtime", () => {
    const document = withSharedDetectionZones(documentFixture(), config([zone("zone_3", "detection")]));
    const merged = mergeSharedDetectionZones(config([zone("zone_3", "filter")]), document, site(false));

    expect(merged.zones.map((item) => item.id)).toEqual(["shared:zone_3", "zone_3"]);
    expect(new Set(merged.zones.map((item) => item.id)).size).toBe(merged.zones.length);
    expect(withSharedDetectionZones(document, merged).automationAreas?.areas[0]?.areaId).toBe("zone_3");
  });

  it("fails closed instead of projecting through the replicated owner's radar pose", () => {
    const document = withSharedDetectionZones(documentFixture(), config([zone("desk", "detection")]));
    const node = config([zone("curtain", "filter")]);
    delete node.floorplan?.radar;

    const merged = mergeSharedDetectionZones(node, document, site(false));

    expect(merged.zones.map((item) => item.id)).toEqual(["curtain"]);
  });

  it("projects one canonical area through the receiving Node's own pose and scale", () => {
    const document = withSharedDetectionZones(documentFixture(), config([zone("desk", "detection")]));
    const node = config([]);
    node.floorplan!.radar = {
      originX: 40,
      originY: 30,
      rotation: 90,
      scale: 0.95,
      poseVersion: 7
    };

    const merged = mergeSharedDetectionZones(node, document, site(false));
    const shared = merged.zones[0];
    const projected = radarPointsToFloorplanPx(shared.points, {
      placement: { originX: 40, originY: 30, rotation: 90 },
      scale: document.scale,
      scaleFactor: 0.95
    });
    const canonical = document.automationAreas!.areas[0].pointsPx;

    projected.forEach((point, index) => {
      expect(point.x).toBeCloseTo(canonical[index][0], 0);
      expect(point.y).toBeCloseTo(canonical[index][1], 0);
    });
    expect(shared.points).not.toEqual(zone("desk", "detection").points);
  });

  it("keeps an untyped draft editable on a non-owner Node", () => {
    expect(sharedDetectionZoneEditable(
      { ...zone("draft", "detection"), placeholder: true },
      site(false)
    )).toBe(true);
  });

  it("drops an untyped Node draft after saving local changes before a shared refresh", () => {
    const document = withSharedDetectionZones(documentFixture(), config([zone("desk", "detection")]));
    const nodeConfig = config([
      { ...zone("draft", "detection"), placeholder: true },
      zone("curtain", "filter"),
      zone("sofa", "reduced")
    ]);

    const merged = mergeSharedDetectionZonesAfterLocalSave(nodeConfig, document, site(false));

    expect(merged.zones.map((item) => item.id)).toEqual([
      "shared:desk",
      "curtain",
      "sofa"
    ]);
    expect(merged.zones.some((item) => item.placeholder)).toBe(false);
  });
});
