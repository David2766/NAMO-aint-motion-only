import { describe, expect, it } from "vitest";
import { addSoftwareZone } from "./zone-geometry";
import type { WebDeviceConfig } from "../core/types";

function emptyConfig(): WebDeviceConfig {
  return {
    version: 1,
    zones: [],
    calibrationZones: []
  };
}

const koreanLabels = {
  zoneLabel: (index: string) => `구역 ${index}`,
  exitPointLabel: (index: string) => `퇴실지점 ${index}`
};

const englishLabels = {
  zoneLabel: (index: string) => `Zone ${index}`,
  exitPointLabel: (index: string) => `Exit Point ${index}`
};

describe("addSoftwareZone", () => {
  it("uses a detection-zone default name for normal zones", () => {
    const result = addSoftwareZone(emptyConfig(), "detection", koreanLabels);

    expect(result.config.zones[0]?.name).toBe("구역 1");
  });

  it("uses an exit-point default name for exit zones", () => {
    const result = addSoftwareZone(emptyConfig(), "exit", koreanLabels);

    expect(result.config.zones[0]?.name).toBe("퇴실지점 1");
  });

  it("uses the active language labels for generated names", () => {
    const detection = addSoftwareZone(emptyConfig(), "detection", englishLabels);
    const exit = addSoftwareZone(emptyConfig(), "exit", englishLabels);

    expect(detection.config.zones[0]?.name).toBe("Zone 1");
    expect(exit.config.zones[0]?.name).toBe("Exit Point 1");
  });
});
