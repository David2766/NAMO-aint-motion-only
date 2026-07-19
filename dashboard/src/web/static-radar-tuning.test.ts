import { describe, expect, it } from "vitest";
import { radarSceneViewport } from "./canvas/radar-view";
import {
  staticRadarDetectedGateNumber,
  staticRadarGateLabelPoint,
  staticRadarGatePath
} from "./static-radar-tuning";

describe("static radar tuning geometry", () => {
  it("projects a distance gate through the radar viewport without invalid coordinates", () => {
    const viewport = radarSceneViewport();
    const gate = { startMm: 1500, endMm: 2250 };
    const path = staticRadarGatePath(gate, viewport);
    const label = staticRadarGateLabelPoint(gate, viewport);

    expect(path).toMatch(/^M/);
    expect(path).toMatch(/ Z$/);
    expect(path).not.toMatch(/NaN|Infinity/);
    expect(label.x).toBe(viewport.width / 2);
    expect(label.y).toBeGreaterThan(viewport.pad);
    expect(label.y).toBeLessThan(viewport.height - viewport.pad);
  });

  it("clips gates beyond the visible radar range", () => {
    const viewport = radarSceneViewport();
    const path = staticRadarGatePath({ startMm: viewport.rangeY, endMm: viewport.rangeY + 750 }, viewport);

    expect(path).toBe("");
  });

  it("uses the single detection distance reported by the sensor", () => {
    const gates = Array.from({ length: 9 }, (_, gate) => ({
      gate,
      startMm: gate * 750,
      endMm: (gate + 1) * 750
    }));

    expect(
      staticRadarDetectedGateNumber(gates, {
        presence: true,
        detectionDistanceMm: 960,
        moving: true,
        still: true,
        movingDistanceMm: 3520,
        stillDistanceMm: 5000
      })
    ).toBe(1);
  });

  it("does not infer detection from inactive or invalid sensor distances", () => {
    const gates = [{ gate: 1, startMm: 750, endMm: 1500 }];

    expect(
      staticRadarDetectedGateNumber(gates, {
        presence: false,
        detectionDistanceMm: 1000
      })
    ).toBeNull();
    expect(staticRadarDetectedGateNumber(gates, { presence: true, detectionDistanceMm: Number.NaN })).toBeNull();
  });

  it("uses the final gate boundary without inventing another target", () => {
    const gates = [{ gate: 2, startMm: 1500, endMm: 2250 }];

    expect(
      staticRadarDetectedGateNumber(gates, {
        presence: true,
        detectionDistanceMm: 2250
      })
    ).toBe(2);
  });
});
