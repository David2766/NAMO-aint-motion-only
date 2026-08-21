import { describe, expect, it } from "vitest";
import {
  staticRadarCalibrationRecommendations,
  type StaticRadarCalibrationSample
} from "./static-radar-calibration";

function samples(
  gate: number,
  count: number,
  sensitivity: number,
  moveEnergy: number,
  stillEnergy: number
): StaticRadarCalibrationSample[] {
  return Array.from({ length: count }, () => ({ gate, sensitivity, moveEnergy, stillEnergy }));
}

describe("static radar false-positive calibration", () => {
  it("raises the threshold above persistent false-positive energy with one recommendation", () => {
    expect(staticRadarCalibrationRecommendations(samples(3, 8, 50, 48, 72))).toEqual([
      {
        gate: 3,
        sampleCount: 8,
        currentSensitivity: 50,
        recommendedSensitivity: 23,
        falsePositiveEnergy: 72
      }
    ]);
  });

  it("ignores short-lived gates and energy already below the active threshold", () => {
    const input = [
      ...samples(2, 4, 50, 70, 72),
      ...samples(4, 6, 40, 25, 30)
    ];

    expect(staticRadarCalibrationRecommendations(input)).toEqual([]);
  });

  it("keeps a safety floor instead of disabling a gate completely", () => {
    expect(staticRadarCalibrationRecommendations(samples(5, 6, 60, 98, 100))).toEqual([
      {
        gate: 5,
        sampleCount: 6,
        currentSensitivity: 60,
        recommendedSensitivity: 10,
        falsePositiveEnergy: 100
      }
    ]);
  });

  it("limits automatic correction to the two most persistent gates", () => {
    const input = [
      ...samples(1, 5, 50, 60, 65),
      ...samples(2, 8, 50, 65, 70),
      ...samples(3, 7, 50, 62, 68)
    ];

    expect(staticRadarCalibrationRecommendations(input).map((value) => value.gate)).toEqual([2, 3]);
  });
});
