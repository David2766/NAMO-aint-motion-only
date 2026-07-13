import { describe, expect, it } from "vitest";
import { localizedZoneDisplayName, normalizeSoftwareConfig, normalizeZoneType } from "./zones";
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
