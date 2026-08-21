import { describe, expect, it } from "vitest";
import { createConfigBackup, validateConfigBackupText } from "./config-backup";
import type { WebDeviceConfig } from "./types";

const reducedZoneConfig: WebDeviceConfig = {
  version: 1,
  zones: [
    {
      id: "zone_1",
      name: "Desk echo",
      type: "reduced",
      shape: "rect",
      points: [
        [-500, 1000],
        [500, 1000],
        [500, 2000],
        [-500, 2000]
      ]
    }
  ],
  calibrationZones: []
};

describe("config backup zone types", () => {
  it("round-trips a user-created reduced zone", async () => {
    const backup = await createConfigBackup(reducedZoneConfig);
    const result = await validateConfigBackupText(JSON.stringify(backup));

    expect(result.errors).toEqual([]);
    expect(result.config?.zones[0]?.type).toBe("reduced");
    expect(result.summary.reducedZones).toBe(1);
  });
});
