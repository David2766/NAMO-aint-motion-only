import { describe, expect, it } from "vitest";
import {
  calculateFloorplanScale,
  floorplanScaleSummary,
  parseDimensionInput,
  storedFloorplanScaleEstimate,
  storedFloorplanScaleInputValid,
  updateStoredFloorplanScaleFromInput
} from "../floorplan-scale";

describe("floorplan scale helpers", () => {
  it("parses only positive finite dimensions", () => {
    expect(parseDimensionInput("1200")).toBe(1200);
    expect(parseDimensionInput("12.5")).toBe(12.5);
    expect(parseDimensionInput("0")).toBeNull();
    expect(parseDimensionInput("-1")).toBeNull();
    expect(parseDimensionInput("abc")).toBeNull();
  });

  it("calculates new floorplan mm-per-pixel scale", () => {
    expect(calculateFloorplanScale({ x: 10, y: 20, width: 400, height: 200 }, { width: "8000", height: "3000" })).toEqual({
      outerBounds: { x: 10, y: 20, width: 400, height: 200 },
      widthMm: 8000,
      heightMm: 3000,
      mmPerPxX: 20,
      mmPerPxY: 15
    });
  });

  it("rejects invalid new floorplan scale inputs", () => {
    expect(calculateFloorplanScale(null, { width: "8000", height: "3000" })).toBeNull();
    expect(calculateFloorplanScale({ x: 0, y: 0, width: 0, height: 200 }, { width: "8000", height: "3000" })).toBeNull();
    expect(calculateFloorplanScale({ x: 0, y: 0, width: 400, height: 200 }, { width: "", height: "3000" })).toBeNull();
  });

  it("formats scale summary values for display", () => {
    expect(floorplanScaleSummary({
      outerBounds: { x: 0, y: 0, width: 333.333, height: 222.222 },
      widthMm: 8000.4,
      heightMm: 3000.4,
      mmPerPxX: 24.00012,
      mmPerPxY: 13.4999
    })).toEqual({
      widthMm: 8000,
      heightMm: 3000,
      widthPx: "333.33",
      heightPx: "222.22",
      mmPerPxX: "24.00",
      mmPerPxY: "13.50"
    });
  });

  it("reads stored scale objects without changing precision", () => {
    expect(storedFloorplanScaleEstimate({
      widthMm: 8000,
      heightMm: 3000,
      outerBoundsPx: [10, 20, 400, 200],
      mmPerPxX: 20,
      mmPerPxY: 15
    })).toEqual({
      outerBounds: { x: 10, y: 20, width: 400, height: 200 },
      widthMm: 8000,
      heightMm: 3000,
      mmPerPxX: 20,
      mmPerPxY: 15
    });
  });

  it("validates and recalculates stored scale from user input", () => {
    const current = {
      widthMm: 1000,
      heightMm: 1000,
      outerBoundsPx: [10, 20, 333, 222] as [number, number, number, number],
      mmPerPxX: 1,
      mmPerPxY: 1
    };
    expect(storedFloorplanScaleInputValid({ width: "8000", height: "3000" })).toBe(true);
    expect(storedFloorplanScaleInputValid({ width: "", height: "3000" })).toBe(false);
    expect(updateStoredFloorplanScaleFromInput(current, { width: "8000", height: "3000" })).toEqual({
      widthMm: 8000,
      heightMm: 3000,
      outerBoundsPx: [10, 20, 333, 222],
      mmPerPxX: 24.02,
      mmPerPxY: 13.51
    });
  });
});
