import { describe, expect, it } from "vitest";
import { localizedZoneDisplayName, normalizeSoftwareConfig, normalizeZoneType, stripPlaceholders } from "./zones";
import type { WebDeviceConfig, WebZone } from "./types";

const zoneBase = {
  id: "zone_1",
  name: "Zone 1",
  shape: "rect",
  points: [
    [-500, 1000],
    [500, 1000],
    [500, 2000],
    [-500, 2000]
  ]
} satisfies Omit<WebZone, "type">;

const englishLabels = {
  zoneLabel: (index: string) => `Zone ${index}`,
  exitPointLabel: (index: string) => `Exit Point ${index}`,
  calibrationZoneLabel: (index: string) => `Correction zone ${index}`
};

function configWithZone(zone: Partial<Omit<WebZone, "type">> & { type?: unknown }): WebDeviceConfig {
  return {
    version: 1,
    zones: [{ ...zoneBase, ...zone } as WebZone],
    calibrationZones: []
  };
}

describe("normalizeZoneType", () => {
  it("keeps supported software zone types", () => {
    expect(normalizeZoneType("detection")).toBe("detection");
    expect(normalizeZoneType("filter")).toBe("filter");
    expect(normalizeZoneType("reduced")).toBe("reduced");
    expect(normalizeZoneType("disabled")).toBe("disabled");
    expect(normalizeZoneType("exit")).toBe("exit");
  });

  it("defaults missing or invalid zone types to detection", () => {
    expect(normalizeZoneType(undefined)).toBe("detection");
    expect(normalizeZoneType("")).toBe("detection");
    expect(normalizeZoneType("unknown")).toBe("detection");
    expect(normalizeZoneType({ isTrusted: true })).toBe("detection");
  });
});

describe("normalizeSoftwareConfig", () => {
  it("repairs missing software zone types so radar CSS classes remain stable", () => {
    const normalized = normalizeSoftwareConfig(configWithZone({ type: undefined }));

    expect(normalized.zones[0]?.type).toBe("detection");
  });

  it("repairs event-like software zone types created by accidental click handler forwarding", () => {
    const normalized = normalizeSoftwareConfig(configWithZone({ type: { isTrusted: true } }));

    expect(normalized.zones[0]?.type).toBe("detection");
  });

  it("preserves valid software zone types", () => {
    const normalized = normalizeSoftwareConfig(configWithZone({ type: "filter" }));

    expect(normalized.zones[0]?.type).toBe("filter");
  });

  it("preserves user-created reduced zones for device saves", () => {
    const normalized = normalizeSoftwareConfig(configWithZone({ type: "reduced" }));

    expect(normalized.zones[0]?.type).toBe("reduced");
  });

  it("migrates a legacy local radar placement to pose version one", () => {
    const normalized = normalizeSoftwareConfig({
      version: 1,
      zones: [],
      floorplan: {
        enabled: true,
        hasImage: true,
        radar: {
          originX: 100,
          originY: 120,
          rotation: 15,
          scale: 1
        }
      }
    });

    expect(normalized.floorplan?.radar?.poseVersion).toBe(1);
  });
});

describe("localizedZoneDisplayName", () => {
  it("localizes legacy Korean default names", () => {
    expect(localizedZoneDisplayName({ ...zoneBase, type: "detection", name: "구역 1" }, englishLabels)).toBe("Zone 1");
    expect(localizedZoneDisplayName({ ...zoneBase, type: "exit", name: "퇴실지점 1" }, englishLabels)).toBe("Exit Point 1");
    expect(localizedZoneDisplayName({ ...zoneBase, id: "calibration_1", type: "filter", name: "보정 1" }, englishLabels)).toBe("Correction zone 1");
  });

  it("preserves user-defined names", () => {
    expect(localizedZoneDisplayName({ ...zoneBase, type: "detection", name: "Living room" }, englishLabels)).toBe("Living room");
  });

  it("uses the zone type when an unnamed zone needs a fallback label", () => {
    expect(localizedZoneDisplayName({ ...zoneBase, type: "exit", name: "" }, englishLabels)).toBe("Exit Point 1");
  });
});

describe("stripPlaceholders", () => {
  it("removes transient drafts and shared display zones from device payloads", () => {
    const payload = stripPlaceholders({
      version: 1,
      zones: [
        { ...zoneBase, id: "draft", type: "detection", placeholder: true },
        { ...zoneBase, id: "shared", type: "detection", shared: true },
        { ...zoneBase, id: "local", type: "filter" }
      ],
      calibrationZones: []
    });

    expect(payload.zones.map((zone) => zone.id)).toEqual(["local"]);
  });
});
