import { describe, expect, it } from "vitest";
import fixtureCatalog from "../../../../tools/platform-sim/fixtures-v1.json";
import { floorplanPxToRadarPoint, radarPointToFloorplanPx } from "./radar-floorplan-transform";

interface CoordinateFixture {
  name: string;
  placement: { originX: number; originY: number; rotation: number };
  scale: { mmPerPxX: number; mmPerPxY: number };
  scaleFactor: number;
  radarPointMm: number[];
  floorplanPointPx: { x: number; y: number };
}

const fixtures: { coordinateCases: CoordinateFixture[] } = fixtureCatalog;

describe("radar floorplan coordinate contract", () => {
  for (const fixture of fixtures.coordinateCases) {
    it(`matches the shared ${fixture.name} fixture`, () => {
      expect(fixture.radarPointMm).toHaveLength(2);
      const radarPointMm: [number, number] = [fixture.radarPointMm[0], fixture.radarPointMm[1]];
      const options = {
        placement: fixture.placement,
        scale: fixture.scale,
        scaleFactor: fixture.scaleFactor
      };
      const floorplanPoint = radarPointToFloorplanPx(radarPointMm, options);
      expect(floorplanPoint.x).toBeCloseTo(fixture.floorplanPointPx.x, 6);
      expect(floorplanPoint.y).toBeCloseTo(fixture.floorplanPointPx.y, 6);

      const radarPoint = floorplanPxToRadarPoint(fixture.floorplanPointPx, options);
      expect(radarPoint[0]).toBeCloseTo(radarPointMm[0], 6);
      expect(radarPoint[1]).toBeCloseTo(radarPointMm[1], 6);
    });
  }
});
